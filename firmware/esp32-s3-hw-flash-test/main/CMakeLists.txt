idf_component_register(SRCS "app_main.c" "hw_flash.c" "hw_flash_diag.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_timer)
