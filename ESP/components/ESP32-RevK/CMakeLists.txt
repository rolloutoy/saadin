cmake_minimum_required(VERSION 3.5...3.25)

set(SOURCES "revk.c" "jo.c" "lwmqtt.c" "settings_lib.c" "settings_old.c" "halib.c" "led.c" "ota_confirm.c")
set(RECS nvs_flash app_update bootloader_support esp_http_client esp-tls esp_http_server esp_driver_spi spi_flash esp_wifi esp_timer esp_system driver bt esp_driver_gpio esp_driver_pcnt esp_driver_usb_serial_jtag)

# Add extra dependencies

# We should be able to do if(CONFIG_IDF_TARGET_ESP8266), and the IDF documentation
# is quite clear that we should be able to do that, but it does not seem to work.

if(EXISTS $ENV{IDF_PATH}/components/esp8266)
	list(APPEND SOURCES "esp8266_compat.c")
	set(RECS nvs_flash app_update esp_http_client esp-tls esp_http_server esp8266 spi_flash)
endif()

if(EXISTS "../managed_components/espressif__mdns")
	list(APPEND RECS mdns)
endif()

if(EXISTS "../managed_components/espressif__led_strip")
	list(APPEND RECS led_strip)
endif()

if(EXISTS "../managed_components/espressif__ksz8851snl")
	list(APPEND RECS espressif__ksz8851snl esp_eth esp_driver_ledc)
endif()


idf_component_register(
		SRCS ${SOURCES}
		INCLUDE_DIRS "include"
		REQUIRES ${RECS}
#		EMBED_FILES "../../build/partition_table/partition-table.bin"
		)

add_definitions(-DBUILD_ESP32_USING_CMAKE)

# Keep the firmware probation marker (revk.c), which nothing references
if(CONFIG_REVK_OTA_CONFIRM)
	target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ota_confirm_magic")
endif()
