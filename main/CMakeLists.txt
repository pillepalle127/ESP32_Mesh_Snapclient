idf_component_register(
    SRCS
        "main.c"
        "net_mesh.c"
        "snapclient_glue.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        audio_i2s
        source_arbiter
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        mesh_lite
        espressif__iot_bridge
        78__esp-opus
)