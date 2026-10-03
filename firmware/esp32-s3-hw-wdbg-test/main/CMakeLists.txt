idf_component_register(SRCS "app_main.c" "panel_lcd.c" "hw_wdbg.c" "hw_wdbg_diag.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_timer)
