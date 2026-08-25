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
         "audio_streamer.c"
         "ble_audio.c"
         "console_cmds.c"
         "wifi_app.c"
         "ws_client.c"
         "mdns_resolver.c"
         "mode.c"
         "usb_link.c"
         "usb_link_framing.c"
    INCLUDE_DIRS "."
    REQUIRES bsp
             json
             nvs_flash
             esp_console
             bt
             esp_wifi
             esp_netif
             esp_event
             mdns
             esp_websocket_client
             esp_driver_usb_serial_jtag
)
