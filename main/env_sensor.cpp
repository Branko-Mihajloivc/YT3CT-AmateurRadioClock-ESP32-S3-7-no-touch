#include "env_sensor.h"
#include "esp_lv_adapter.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_rom_sys.h" // esp_rom_delay_us
#include "esp_timer.h" // esp_timer_get_time
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "env_sensor";

static const gpio_num_t DHT_PIN = GPIO_NUM_6; // board's labeled "GPIO" header: 3V3 / GND / GP6
static const uint32_t READ_INTERVAL_MS = 30UL * 1000UL; // ambient temp/humidity doesn't change fast
static const uint32_t FIRST_READ_DELAY_MS = 10UL * 1000UL; // stagger vs the other core-0 tasks' first firings
static const int SENSOR_TASK_CORE = 0; // NOT core 1 -- see env_sensor.h

static lv_obj_t *s_label;

// While tuning the raw-symbol decode below, dump every captured symbol
// so the actual hardware alignment can be read off the serial log
// instead of guessed. Turn off once the decode is confirmed correct.
#define DIAG_DUMP_SYMBOLS 0

// --- RMT-based read -----------------------------------------------------
//
// An earlier version bit-banged this with a tight CPU polling loop,
// which needed portDISABLE_INTERRUPTS() around the timing-critical part
// to stay accurate -- that starved the RGB panel driver's PSRAM
// bounce-buffer refill ISR (also on core 0) and caused visible display
// jitter every ~30s. Removing the interrupt-disable fixed the jitter but
// made reads unreliable: WiFi's own latency spikes routinely blew past
// the microsecond-scale timing margins.
//
// The RMT peripheral sidesteps both problems: it timestamps the DHT11's
// edge transitions in hardware, independent of CPU scheduling/interrupt
// state, so there's nothing here for WiFi activity to corrupt and
// nothing that needs interrupts disabled.
static rmt_channel_handle_t s_rx_channel;
static QueueHandle_t s_receive_queue;
static const uint32_t RMT_RESOLUTION_HZ = 1000000; // 1 tick = 1us
static const size_t RMT_MEM_BLOCK_SYMBOLS = 64;
static rmt_symbol_word_t s_rx_symbols[RMT_MEM_BLOCK_SYMBOLS];

static bool IRAM_ATTR rmt_rx_done_cb(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data) {
    (void)channel;
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)user_data;
    xQueueSendFromISR(queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static void env_sensor_rmt_init(void) {
    rmt_rx_channel_config_t rx_chan_config = {};
    rx_chan_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_chan_config.resolution_hz = RMT_RESOLUTION_HZ;
    rx_chan_config.mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS;
    rx_chan_config.gpio_num = DHT_PIN;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_chan_config, &s_rx_channel));

    s_receive_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = rmt_rx_done_cb;
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(s_rx_channel, &cbs, s_receive_queue));

    ESP_ERROR_CHECK(rmt_enable(s_rx_channel));
}

// Reads one DHT11 frame (5 bytes: humidity int, humidity frac, temp int,
// temp frac, checksum). Returns false on any timing/checksum failure,
// leaving *temp_c/*humidity untouched.
static bool read_dht11(float *temp_c, float *humidity) {
    // Arm the RMT receiver before releasing the line, so it's already
    // watching when the sensor starts its ACK -- signal_range_max_ns is
    // the idle threshold RMT uses to decide the frame is over (well
    // above the longest expected single pulse, ~80us for the ACK high
    // phase; well below waiting a full second for nothing).
    rmt_receive_config_t rx_config = {};
    rx_config.signal_range_min_ns = 1000;     // 1us floor -- filters glitches, real pulses are >=26us
    rx_config.signal_range_max_ns = 150000;   // 150us idle -> end of frame

    // Start signal: pull low >=18ms, then release and let the external
    // pull-up bring the line high before switching to input to listen.
    gpio_set_direction(DHT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    gpio_set_level(DHT_PIN, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(DHT_PIN, GPIO_MODE_INPUT);

    esp_err_t err = rmt_receive(s_rx_channel, s_rx_symbols, sizeof(s_rx_symbols), &rx_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_receive arm failed: %s", esp_err_to_name(err));
        return false;
    }

    rmt_rx_done_event_data_t rx_data;
    if (xQueueReceive(s_receive_queue, &rx_data, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "fail: no RMT rx-done event (sensor not responding?)");
        return false;
    }

#if DIAG_DUMP_SYMBOLS
    ESP_LOGI(TAG, "RMT captured %d symbols:", (int)rx_data.num_symbols);
    for (size_t i = 0; i < rx_data.num_symbols; i++) {
        rmt_symbol_word_t s = rx_data.received_symbols[i];
        ESP_LOGI(TAG, "  [%2d] level0=%d dur0=%4d  level1=%d dur1=%4d",
                 (int)i, s.level0, s.duration0, s.level1, s.duration1);
    }
#endif

    // Expect 1 ACK symbol (low ~80us, high ~80us) + 40 bit symbols (low
    // ~50us, high ~26-28us for a 0 bit or ~70us for a 1 bit) = 41 total,
    // each rmt_symbol_word_t naturally holding one low+high pair.
    if (rx_data.num_symbols < 41) {
        ESP_LOGW(TAG, "fail: only %d symbols captured, expected 41", (int)rx_data.num_symbols);
        return false;
    }

    // Empirically confirmed via the DIAG_DUMP_SYMBOLS dump: we arm the
    // RMT receiver while the line is still high (mid-ACK, before the
    // sensor's own ~20-40us reaction time has elapsed), so the pairing
    // is offset by one field from what the raw protocol phases suggest.
    // symbols[0].duration1 is bit 0's preceding low phase; each bit i's
    // value is symbols[i+1].duration0 (its HIGH phase width -- ~24us for
    // a 0 bit, ~71us for a 1), not duration1 (which is uniformly ~54us,
    // just the low phase before the *next* bit, carrying no data).
    uint8_t data[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 40; i++) {
        rmt_symbol_word_t s = rx_data.received_symbols[i + 1]; // [0] is the ACK
        data[i / 8] <<= 1;
        if (s.duration0 > 40) data[i / 8] |= 1;
    }

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        ESP_LOGW(TAG, "checksum mismatch: data=%02x %02x %02x %02x %02x computed=%02x",
                 data[0], data[1], data[2], data[3], data[4], checksum);
        return false;
    }

    // DHT11 (unlike the higher-precision DHT22) only reports whole-number
    // humidity/temperature -- the "decimal" bytes are always 0.
    *humidity = (float)data[0];
    *temp_c = (float)data[2];
    return true;
}

static void read_and_update() {
    // Read happens outside the LVGL lock, same reasoning as
    // solar_conditions.cpp's HTTP fetch -- only the label update itself
    // needs it.
    int64_t t0 = esp_timer_get_time();
    float temp_c = 0, humidity = 0;
    bool ok = read_dht11(&temp_c, &humidity);
    int64_t us = esp_timer_get_time() - t0;
    if (!ok) {
        ESP_LOGW(TAG, "DHT11 read failed after %lld us, keeping last value", (long long)us);
        return;
    }
    ESP_LOGI(TAG, "DHT11 read ok in %lld us: %.1fC %.0f%%", (long long)us, temp_c, humidity);

    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f" "\xC2\xB0" "C  %.0f%%", temp_c, humidity);
    if (esp_lv_adapter_lock(200) == ESP_OK) {
        lv_label_set_text(s_label, buf);
        esp_lv_adapter_unlock();
    }
}

static void sensor_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(FIRST_READ_DELAY_MS));
    while (true) {
        read_and_update();
        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
    }
}

void env_sensor_init(lv_obj_t *parent, int x, int y) {
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << DHT_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(DHT_PIN, 1); // idle-high, matching the protocol's expected resting state

    env_sensor_rmt_init();

    s_label = lv_label_create(parent);
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    // Match main.cpp's top-bar font sizing (falls back the same way if
    // 30/48 aren't enabled in the LVGL Kconfig).
#if LV_FONT_MONTSERRAT_30
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_30, 0);
#endif
    lv_label_set_text(s_label, "--.-" "\xC2\xB0" "C  --%"); // \xC2\xB0 = UTF-8 degree sign
    lv_obj_set_pos(s_label, x, y);
    // Same dark translucent highlight box as the rest of the top bar and
    // the bottom bar's city labels.
    lv_obj_set_style_bg_color(s_label, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(s_label, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_label, 4, 0);
    lv_obj_set_style_pad_hor(s_label, 6, 0);
    lv_obj_set_style_pad_ver(s_label, 2, 0);

    xTaskCreatePinnedToCore(sensor_task, "dht11", 4096, NULL, 1, NULL, SENSOR_TASK_CORE);
}
