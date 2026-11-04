idf_component_register(
    SRCS "main.c" "display.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES e3d esp_lcd esp_driver_ppa esp_driver_ledc esp_mm esp_hw_support esp_timer
)
