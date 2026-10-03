idf_component_register(SRCS "main.c" "ina226.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lvgl_port esp_lcd driver esp_timer)
