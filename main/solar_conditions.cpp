#include "solar_conditions.h"
#include "esp_lv_adapter.h"

#include <string.h>
#include <stdio.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_tls.h" // esp_tls_get_and_clear_last_error
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "solar_conditions";

// esp_http_client_open()'s own return value only says "connect failed" --
// the plain global errno read right after it returns is unreliable (it
// can get clobbered by internal cleanup calls made between the real
// failure and esp_http_client_open()'s return). esp-tls tracks the real
// underlying socket/TLS error separately for exactly this reason; this
// handler reads it out on HTTP_EVENT_ERROR, where evt->data is the
// transport's esp_tls_error_handle_t.
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ERROR && evt->data) {
        esp_tls_error_handle_t tls_err = (esp_tls_error_handle_t)evt->data;
        int esp_tls_code = 0, esp_tls_flags = 0;
        // The return value is the categorized esp_err_t (which stage
        // failed -- DNS/socket/connect/handshake); esp_tls_code's meaning
        // depends on that category (raw errno for SYSTEM, raw mbedtls
        // code for MBEDTLS, etc.) -- log both.
        esp_err_t last_err = esp_tls_get_and_clear_last_error(tls_err, &esp_tls_code, &esp_tls_flags);
        ESP_LOGW(TAG, "HTTP_EVENT_ERROR: %s (0x%x), code=%d (0x%x), tls_flags=0x%x",
                 esp_err_to_name(last_err), last_err, esp_tls_code, esp_tls_code, esp_tls_flags);
    }
    return ESP_OK;
}

// getaddrinfo("hamqsl.com") fails 100% of the time on this device (lwIP's
// embedded DNS resolver, EAI_FAIL) even though: it's not a bad/blocked DNS
// server (same failure with the router's DNS AND an explicit 8.8.8.8
// override, readback-verified); not a network-wide DNS outage (NTP, a
// different hostname, resolves via the same override in under 2s every
// boot); not IPv6/dual-stack confusion (restricted to AF_INET, same
// failure); not a CNAME-chain limitation (apex "hamqsl.com" has a direct A
// record, no CNAME, same failure); and not specific to this WiFi network
// (identical failure on a phone's mobile hotspot, a completely different
// carrier path). Every other tool on the same networks -- ping, PowerShell
// Resolve-DnsName against 8.8.8.8, a plain HTTPS fetch -- resolves and
// fetches this exact host instantly. So this connects straight to the
// known IP (bypassing the DNS lookup that's the one thing consistently
// failing) while still sending the real hostname for TLS SNI/cert
// validation and the HTTP Host header, via common_name below and an
// explicit Host header in poll_once(). Only real downside: if hamqsl.com's
// IP ever changes, this needs updating -- worth revisiting hostname-based
// resolution if the underlying DNS failure ever gets root-caused.
static const char *SOLAR_XML_URL = "https://192.124.249.177/solarxml.php";
static const char *SOLAR_HOSTNAME = "hamqsl.com";
static const uint32_t HTTP_TIMEOUT_MS = 10000;

// hamqsl.com's own guidance: "please only select to update every hour --
// that is the update period for the flux parameters."
static const uint32_t POLL_INTERVAL_MS = 60UL * 60UL * 1000UL;
static const uint32_t FIRST_POLL_DELAY_MS = 15UL * 1000UL;
static const int SOLAR_TASK_CORE = 0; // NOT core 1 -- see solar_conditions.h

static void solar_task(void *arg); // defined below solar_conditions_init(), which starts it

// Static task creation with a PSRAM-allocated stack (see
// CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY in sdkconfig.defaults) --
// internal SRAM was too fragmented for xTaskCreatePinnedToCore()'s
// dynamic 8KB stack allocation to reliably succeed. The task's small
// control block (StaticTask_t) still lives in internal RAM as required;
// only the stack itself moves to PSRAM.
static const uint32_t SOLAR_TASK_STACK_BYTES = 8192;
static StaticTask_t s_solar_task_tcb;
static StackType_t *s_solar_task_stack;

static const char *BAND_NAMES[4] = { "80m-40m", "30m-20m", "17m-15m", "12m-10m" };

static lv_obj_t *s_sfi_label;
static lv_obj_t *s_sn_label;
static lv_obj_t *s_a_label;
static lv_obj_t *s_k_label;
static lv_obj_t *s_band_day_label[4];
static lv_obj_t *s_band_night_label[4];

static lv_color_t color_green()  { return lv_color_make(90, 210, 90); }
static lv_color_t color_yellow() { return lv_color_make(230, 200, 60); }
static lv_color_t color_red()    { return lv_color_make(220, 70, 70); }

// Good/Fair/Poor -> green/yellow/red; anything unrecognized falls back to
// plain white rather than guessing.
static lv_color_t band_value_color(const char *val) {
    if (strcmp(val, "Good") == 0) return color_green();
    if (strcmp(val, "Fair") == 0) return color_yellow();
    if (strcmp(val, "Poor") == 0) return color_red();
    return lv_color_white();
}

// Same dark translucent highlight box style as the top bar (main.cpp's
// apply_highlight) and the bottom bar's city labels (city_clocks.cpp) --
// duplicated locally rather than shared across files, matching how
// city_clocks.cpp already does it, since each of these UI files is kept
// self-contained.
static void apply_highlight(lv_obj_t *label) {
    lv_obj_set_style_bg_color(label, lv_color_make(20, 20, 20), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_80, 0);
    lv_obj_set_style_radius(label, 4, 0);
    lv_obj_set_style_pad_hor(label, 6, 0);
    lv_obj_set_style_pad_ver(label, 2, 0);
}

// Creates a throwaway label just to measure how wide `text` renders in
// this column's font, then deletes it -- column positions below are based
// on this real measurement instead of a guessed fraction of the column
// width, which is exactly what caused the top bar's overlap bugs earlier
// in this project (see main.cpp's build_ui()).
static int measure_width(lv_obj_t *parent, const char *text) {
    lv_obj_t *tmp = lv_label_create(parent);
    lv_label_set_text(tmp, text);
    lv_obj_update_layout(tmp);
    int w = lv_obj_get_width(tmp);
    lv_obj_del(tmp);
    return w;
}

// Same idea, but measured WITH apply_highlight()'s styling applied first
// -- for any label that's going to become a highlighted box, this must
// be used instead of the plain measure_width() above. lv_obj_set_width()
// sets the outer box width, and apply_highlight() adds 6px of horizontal
// padding per side; sizing a box from an unpadded measurement leaves 12px
// too little room for the text, which silently wraps it onto a second
// line that then overflows the box into the row below (found on real
// hardware -- see the photo that prompted this).
static int measure_box_width(lv_obj_t *parent, const char *text) {
    lv_obj_t *tmp = lv_label_create(parent);
    apply_highlight(tmp);
    lv_label_set_text(tmp, text);
    lv_obj_update_layout(tmp);
    int w = lv_obj_get_width(tmp);
    lv_obj_del(tmp);
    return w;
}

// Same idea, but for a highlighted row's real rendered height (text line
// height plus apply_highlight()'s own vertical padding) -- a first
// version guessed a fixed row step matching city_clocks.cpp's, which
// turned out shorter than this column's actual box height and made
// consecutive rows overlap on real hardware.
static int measure_row_height(lv_obj_t *parent, const char *text) {
    lv_obj_t *tmp = lv_label_create(parent);
    apply_highlight(tmp);
    lv_label_set_text(tmp, text);
    lv_obj_update_layout(tmp);
    int h = lv_obj_get_height(tmp);
    lv_obj_del(tmp);
    return h;
}

// Finds "<tag>value</tag>" (no attributes) anywhere in xml and copies the
// value (trimmed to out_size) into out. Returns false if the tag isn't
// present, leaving out untouched.
static bool extract_tag(const char *xml, const char *tag, char *out, size_t out_size) {
    char open_tag[32];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    const char *start = strstr(xml, open_tag);
    if (!start) return false;
    start += strlen(open_tag);
    const char *end = strchr(start, '<');
    if (!end) return false;
    size_t len = (size_t)(end - start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

// Finds a specific <band name="NAME" time="TIME">VALUE</band> entry.
// Matches by attribute value rather than position in the document, so
// this keeps working even if hamqsl ever reorders the band list.
static bool extract_band(const char *xml, const char *name, const char *time_of_day,
                          char *out, size_t out_size) {
    char needle[64];
    snprintf(needle, sizeof(needle), "name=\"%s\" time=\"%s\">", name, time_of_day);
    const char *start = strstr(xml, needle);
    if (!start) return false;
    start += strlen(needle);
    const char *end = strchr(start, '<');
    if (!end) return false;
    size_t len = (size_t)(end - start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

void solar_conditions_init(lv_obj_t *parent, int x, int y, int w, int h) {
    // Dedicated screen region now (not a floating overlay panel), so no
    // background box needed -- labels sit straight on the screen's black
    // background.
    const int pad = 14;
    const int gap = 16;
    // Real rendered row height (text line + apply_highlight()'s vertical
    // padding), not a guessed constant -- a fixed 22px (city_clocks.cpp's
    // own row step) turned out shorter than this column's actual box
    // height and made consecutive rows overlap on real hardware. A small
    // 2px breathing gap is added on top so rows sit close but don't touch.
    const int row_step = measure_row_height(parent, "80m-40m") + 2;
    int cx = x + pad;
    int cy = y + pad;

    // Vertically center the whole stat+table block in the column (no
    // title above it anymore). Block height is 7 back-to-back row-steps:
    // 2 stat rows, a DAY/NIGHT header row, and 4 band rows -- the header
    // now sits immediately below the stat rows, no extra gap between them.
    int block_h = row_step * 7;
    int remaining_h = (y + h) - cy;
    int content_top = cy + (remaining_h > block_h ? (remaining_h - block_h) / 2 : 0);
    cy = content_top;

    // Stat row: SFI / SN on the left column, A / K on the right column.
    int stat_col1_w = measure_box_width(parent, "SFI 300");
    int sn_w = measure_box_width(parent, "SN 300");
    if (sn_w > stat_col1_w) stat_col1_w = sn_w;
    int stat_col2_w = measure_box_width(parent, "A 300");
    int k_w = measure_box_width(parent, "K 300");
    if (k_w > stat_col2_w) stat_col2_w = k_w;
    int stat_col2_x = cx + stat_col1_w + gap;

    // Recolored inline (same "#rrggbb text#" span technique city_clocks.cpp
    // uses for its sun-times) so only the number is green -- the "SFI"/
    // "SN"/"A"/"K" tag stays the label's plain white base color. Each gets
    // the same dark highlight box as the top/bottom bars, sized to its
    // column so same-column boxes line up.
    s_sfi_label = lv_label_create(parent);
    lv_obj_set_style_text_color(s_sfi_label, lv_color_white(), 0);
    apply_highlight(s_sfi_label);
    lv_obj_set_width(s_sfi_label, stat_col1_w);
    lv_label_set_recolor(s_sfi_label, true);
    lv_label_set_text(s_sfi_label, "SFI #5ad25a --#");
    lv_obj_set_pos(s_sfi_label, cx, cy);

    s_sn_label = lv_label_create(parent);
    lv_obj_set_style_text_color(s_sn_label, lv_color_white(), 0);
    apply_highlight(s_sn_label);
    lv_obj_set_width(s_sn_label, stat_col1_w);
    lv_label_set_recolor(s_sn_label, true);
    lv_label_set_text(s_sn_label, "SN #5ad25a --#");
    lv_obj_set_pos(s_sn_label, cx, cy + row_step);

    s_a_label = lv_label_create(parent);
    lv_obj_set_style_text_color(s_a_label, lv_color_white(), 0);
    apply_highlight(s_a_label);
    lv_obj_set_width(s_a_label, stat_col2_w);
    lv_label_set_recolor(s_a_label, true);
    lv_label_set_text(s_a_label, "A #5ad25a --#");
    lv_obj_set_pos(s_a_label, stat_col2_x, cy);

    s_k_label = lv_label_create(parent);
    lv_obj_set_style_text_color(s_k_label, lv_color_white(), 0);
    apply_highlight(s_k_label);
    lv_obj_set_width(s_k_label, stat_col2_w);
    lv_label_set_recolor(s_k_label, true);
    lv_label_set_text(s_k_label, "K #5ad25a --#");
    lv_obj_set_pos(s_k_label, stat_col2_x, cy + row_step);

    // Band table: name column + DAY/NIGHT value columns. Header row stays
    // plain (unboxed) text; only the data cells get the highlight-box
    // treatment. Sits immediately below the stat rows, no gap.
    const int table_top = cy + 2 * row_step;
    const int col_name_x = cx;
    int name_w = measure_box_width(parent, "80m-40m") + 10; // a bit of extra breathing room beyond the bare fit
    const int col_day_x = col_name_x + name_w + gap;
    int val_w = measure_box_width(parent, "Good");
    const int col_night_x = col_day_x + val_w + gap;

    lv_obj_t *hdr_day = lv_label_create(parent);
    lv_obj_set_style_text_color(hdr_day, color_yellow(), 0);
    lv_label_set_text(hdr_day, "DAY");
    lv_obj_set_pos(hdr_day, col_day_x, table_top);

    lv_obj_t *hdr_night = lv_label_create(parent);
    lv_obj_set_style_text_color(hdr_night, color_yellow(), 0);
    lv_label_set_text(hdr_night, "NIGHT");
    lv_obj_set_pos(hdr_night, col_night_x, table_top);

    for (int i = 0; i < 4; i++) {
        int row_y = table_top + row_step * (i + 1);

        lv_obj_t *name_label = lv_label_create(parent);
        lv_obj_set_style_text_color(name_label, lv_color_make(190, 190, 190), 0);
        apply_highlight(name_label);
        lv_obj_set_width(name_label, name_w);
        lv_label_set_text(name_label, BAND_NAMES[i]);
        lv_obj_set_pos(name_label, col_name_x, row_y);

        s_band_day_label[i] = lv_label_create(parent);
        lv_obj_set_style_text_color(s_band_day_label[i], lv_color_white(), 0);
        apply_highlight(s_band_day_label[i]);
        lv_obj_set_width(s_band_day_label[i], val_w);
        lv_label_set_text(s_band_day_label[i], "--");
        lv_obj_set_pos(s_band_day_label[i], col_day_x, row_y);

        s_band_night_label[i] = lv_label_create(parent);
        lv_obj_set_style_text_color(s_band_night_label[i], lv_color_white(), 0);
        apply_highlight(s_band_night_label[i]);
        lv_obj_set_width(s_band_night_label[i], val_w);
        lv_label_set_text(s_band_night_label[i], "--");
        lv_obj_set_pos(s_band_night_label[i], col_night_x, row_y);
    }

    ESP_LOGI(TAG, "before solar_task create: free internal RAM %u bytes (largest block %u), free PSRAM %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    s_solar_task_stack = (StackType_t *)heap_caps_malloc(SOLAR_TASK_STACK_BYTES, MALLOC_CAP_SPIRAM);
    TaskHandle_t created = NULL;
    if (s_solar_task_stack) {
        created = xTaskCreateStaticPinnedToCore(solar_task, "solar", SOLAR_TASK_STACK_BYTES, NULL, 1,
                                                 s_solar_task_stack, &s_solar_task_tcb, SOLAR_TASK_CORE);
    }
    ESP_LOGI(TAG, "solar_task create result: %s", created ? "OK" : "FAILED");
}

static void poll_once() {
    ESP_LOGI(TAG, "poll_once: starting, free internal RAM %u bytes (largest block %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    esp_http_client_config_t config = {};
    config.url = SOLAR_XML_URL;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    // Tested skipping cert verification (matching the Arduino build's
    // setInsecure()) to see if that's what hamqsl.com's WAF keys on --
    // identical ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT either way, so it isn't.
    // Reverted to full validation since the weaker mode bought nothing.
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = http_event_handler;
    // hamqsl.com is behind a Sucuri/Cloudproxy WAF. It accepts our TLS
    // connection and GET, then closes cleanly (FIN) without ever sending
    // a response -- no error, no timeout, just silence. A plain curl from
    // this machine (same request, User-Agent included) gets a normal 200
    // with no such behavior, so it's not the request content -- setting
    // a User-Agent alone didn't fix it either. Explicitly offering the
    // same ALPN ("http/1.1") curl negotiated, in case the WAF treats a
    // no-ALPN TLS handshake as suspicious/bot-like.
    config.user_agent = "GeochronClock/1.0 (ESP32-S3)";
    static const char *alpn_list[] = {"http/1.1", NULL};
    config.alpn_protos = alpn_list;
    // SOLAR_XML_URL is a raw IP (see comment above) -- common_name tells
    // esp-tls to still send SNI="hamqsl.com" and validate the cert against
    // that hostname, exactly as if we'd connected via DNS.
    config.common_name = SOLAR_HOSTNAME;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "esp_http_client_init failed");
        return;
    }
    // The URL's host is a raw IP, so esp_http_client would otherwise send
    // "Host: 192.124.249.177" -- set the real hostname explicitly so the
    // CDN/WAF in front of hamqsl.com routes the request correctly.
    esp_http_client_set_header(client, "Host", SOLAR_HOSTNAME);
    ESP_LOGI(TAG, "poll_once: client init ok, opening connection");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        // The real reason is logged by http_event_handler() above (global
        // errno here is unreliable -- see its comment).
        ESP_LOGW(TAG, "connection failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }
    ESP_LOGI(TAG, "poll_once: connection open, fetching headers");

    errno = 0;
    int64_t content_length = esp_http_client_fetch_headers(client);
    int fetch_errno = errno; // fetch_headers()'s read loop does `errno = 0` right before each esp_transport_read(), so this is reliable (unlike the connect-path errno, not clobbered by intervening calls)
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "poll_once: headers fetched, status=%d content_length=%lld, errno=%d (%s)",
             status, (long long)content_length, fetch_errno, strerror(fetch_errno));
    if (status != 200) {
        ESP_LOGW(TAG, "GET failed, HTTP status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    // The feed is a few KB of XML -- 8KB is comfortable headroom even if
    // hamqsl ever grows it a bit. content_length can be -1 for chunked
    // responses; either way esp_http_client_read_response() stops at
    // whichever comes first, end-of-body or the buffer filling up.
    static char xml[8192];
    (void)content_length;
    int read_len = esp_http_client_read_response(client, xml, sizeof(xml) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        ESP_LOGW(TAG, "empty or failed response body");
        return;
    }
    xml[read_len] = '\0';
    ESP_LOGI(TAG, "fetched %d bytes, status %d", read_len, status);

    char sfi[16] = "", sn[16] = "", a_idx[16] = "", k_idx[16] = "";
    bool have_sfi = extract_tag(xml, "solarflux", sfi, sizeof(sfi));
    bool have_sn  = extract_tag(xml, "sunspots", sn, sizeof(sn));
    bool have_a   = extract_tag(xml, "aindex", a_idx, sizeof(a_idx));
    bool have_k   = extract_tag(xml, "kindex", k_idx, sizeof(k_idx));
    ESP_LOGI(TAG, "parsed sfi=%d sn=%d a=%d k=%d", have_sfi, have_sn, have_a, have_k);

    char day_val[4][16], night_val[4][16];
    bool have_day[4], have_night[4];
    for (int i = 0; i < 4; i++) {
        day_val[i][0] = '\0';
        night_val[i][0] = '\0';
        have_day[i] = extract_band(xml, BAND_NAMES[i], "day", day_val[i], sizeof(day_val[i]));
        have_night[i] = extract_band(xml, BAND_NAMES[i], "night", night_val[i], sizeof(night_val[i]));
    }

    // Only the label-update step needs the LVGL lock -- deliberately not
    // held across the network call above, so a slow/dead server can't
    // stall the UI.
    char buf[32]; // room for the "#5ad25a ...#" recolor markup around the number, not just the number itself
    if (esp_lv_adapter_lock(200) == ESP_OK) {
        if (have_sfi) { snprintf(buf, sizeof(buf), "SFI #5ad25a %s#", sfi); lv_label_set_text(s_sfi_label, buf); }
        if (have_sn)  { snprintf(buf, sizeof(buf), "SN #5ad25a %s#", sn); lv_label_set_text(s_sn_label, buf); }
        if (have_a)   { snprintf(buf, sizeof(buf), "A #5ad25a %s#", a_idx); lv_label_set_text(s_a_label, buf); }
        if (have_k)   { snprintf(buf, sizeof(buf), "K #5ad25a %s#", k_idx); lv_label_set_text(s_k_label, buf); }
        for (int i = 0; i < 4; i++) {
            if (have_day[i]) {
                lv_label_set_text(s_band_day_label[i], day_val[i]);
                lv_obj_set_style_text_color(s_band_day_label[i], band_value_color(day_val[i]), 0);
            }
            if (have_night[i]) {
                lv_label_set_text(s_band_night_label[i], night_val[i]);
                lv_obj_set_style_text_color(s_band_night_label[i], band_value_color(night_val[i]), 0);
            }
        }
        esp_lv_adapter_unlock();
    }
}

static void solar_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "solar_task: entered, waiting %u ms before first poll", (unsigned)FIRST_POLL_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(FIRST_POLL_DELAY_MS));
    while (true) {
        poll_once();
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}
