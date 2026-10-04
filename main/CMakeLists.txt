idf_component_register(
    SRCS
        "main.cpp"
        "web_assets.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_server
        esp_http_client
        app_update
        esp_timer
        esp_system
        esp_app_format
        spi_flash
)
