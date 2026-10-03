idf_component_register(SRCS "main.c" "panel_lcd.c" "hw_fault.c" "hw_fault_diag.c" "hw_core.c" "hw_core_diag.c" "hw_main.c" "hw_main_diag.c" "hw_wdbg.c" "hw_wdbg_diag.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_timer)
