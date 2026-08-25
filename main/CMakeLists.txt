idf_component_register(
    SRCS        "main.c"
                "net_mesh.c"
                "snapclient_glue.c"
                "a2dp_sink_glue.c"
    INCLUDE_DIRS "."
    REQUIRES    audio_i2s
                source_arbiter
                nvs_flash
                esp_wifi
                esp_netif
                esp_event
                bt
                mesh_lite
                iot_bridge
)
