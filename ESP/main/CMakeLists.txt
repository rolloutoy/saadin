set (COMPONENT_SRCS "cn_wired_driver.c" "Faikout.c" "bleenv.c" "settings.c" "faikout_i2c.c" "heatctl.c" "heatlog.c")
set (COMPONENT_REQUIRES "ESP32-RevK" "esp_driver_rmt" "esp_driver_uart" "esp_driver_i2c" "esp_driver_usb_serial_jtag")
set (COMPONENT_EMBED_FILES "favicon.ico" "apple-touch-icon.png")
register_component ()

# Chart.js 4.5.1 (chart.umd.min.js from the chart.js npm package, MIT, see chart.umd.min.js.LICENSE.md) for the graph
# page, gzipped at build time
set (chartjs "${CMAKE_CURRENT_BINARY_DIR}/chart.umd.min.js")
add_custom_command (OUTPUT "${chartjs}.gz"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/chart.umd.min.js" "${chartjs}"
    COMMAND gzip -9 -n -f "${chartjs}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/chart.umd.min.js"
    VERBATIM)
add_custom_target (chartjs_gz DEPENDS "${chartjs}.gz")
add_dependencies (${COMPONENT_LIB} chartjs_gz)
target_add_binary_data (${COMPONENT_LIB} "${chartjs}.gz" BINARY)
