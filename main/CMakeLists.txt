idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "app_state.c"
         "app_protocol.c"
         "app_events.c"
         "app_sound.c"
         "app_ui.c"
         "nvs_settings.c"
         "wifi_app.c"
         "ws_client.c"
         "audio_streamer.c"
         "ble_hid_keyboard.c"
         "hid_keymap.c"
         "console_cmds.c"
    INCLUDE_DIRS "."
    REQUIRES bsp
             json
             nvs_flash
             esp_websocket_client
             esp_wifi
             esp_netif
             esp_event
             esp_console
             bt
)
