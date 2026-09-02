idf_component_register(
    SRCS "main.cpp" "daynight_map.cpp" "city_clocks.cpp" "sun_position.cpp" "solar_conditions.cpp" "env_sensor.cpp" "wifi_manager.cpp" "beacon_panel.cpp" "range_rings.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash lwip esp_http_client esp-tls mbedtls driver esp_driver_rmt rgb_lcd_port sd io_extension
    WHOLE_ARCHIVE
    )

idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl" IN_LIST build_components)
    idf_component_get_property(lvgl_lib lvgl COMPONENT_LIB)
elseif("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
endif()

if(lvgl_lib)
    target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
endif()
