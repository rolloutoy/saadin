set (COMPONENT_SRCS "cn_wired_driver.c" "Faikout.c" "bleenv.c" "settings.c" "faikout_i2c.c")
set (COMPONENT_REQUIRES "ESP32-RevK" "esp_driver_rmt" "esp_driver_uart" "esp_driver_i2c" "esp_driver_usb_serial_jtag")
set (COMPONENT_EMBED_FILES "favicon.ico" "apple-touch-icon.png")
register_component ()
