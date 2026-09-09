idf_component_register(
    SRCS        "source_arbiter.c"
                "resampler.c"
    INCLUDE_DIRS "include"
    REQUIRES    audio_i2s esp_ringbuf
	PRIV_REQUIRES esp_timer
)
