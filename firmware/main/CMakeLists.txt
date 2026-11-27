idf_component_register(SRCS "app_main.c" "port_esp.c" INCLUDE_DIRS "."
                       REQUIRES sct_core
                       PRIV_REQUIRES driver esp_driver_uart esp_driver_gpio esp_driver_usb_serial_jtag esp_adc esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
