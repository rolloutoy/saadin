set (COMPONENT_SRCS "cn_wired_driver.c" "Faikout.c" "bleenv.c" "settings.c" "faikout_i2c.c" "heatctl.c" "heatlog.c")
set (COMPONENT_REQUIRES "ESP32-RevK" "esp_driver_rmt" "esp_driver_uart" "esp_driver_i2c" "esp_driver_usb_serial_jtag")
set (COMPONENT_EMBED_FILES "favicon.ico" "apple-touch-icon.png")
register_component ()

# Chart.js 4.5.1 (chart.umd.min.js from the chart.js npm package), then Hammer.js 2.0.8 (hammer.min.js from the
# hammerjs npm package) and chartjs-plugin-zoom 2.2.0 (chartjs-plugin-zoom.min.js from its npm package), all MIT, see
# the *.LICENSE.md beside each. Joined in that order (the zoom plugin needs both) and gzipped at build time.
set (chartjs "${CMAKE_CURRENT_BINARY_DIR}/chart.umd.min.js")
set (chartjs_src "${CMAKE_CURRENT_SOURCE_DIR}/chart.umd.min.js" "${CMAKE_CURRENT_SOURCE_DIR}/hammer.min.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/chartjs-plugin-zoom.min.js")
string (REPLACE ";" "|" chartjs_in "${chartjs_src}")
add_custom_command (OUTPUT "${chartjs}.gz"
    COMMAND ${CMAKE_COMMAND} "-DOUT=${chartjs}" "-DIN=${chartjs_in}" -P "${CMAKE_CURRENT_SOURCE_DIR}/joinjs.cmake"
    COMMAND gzip -9 -n -f "${chartjs}"
    DEPENDS ${chartjs_src} "${CMAKE_CURRENT_SOURCE_DIR}/joinjs.cmake"
    VERBATIM)
add_custom_target (chartjs_gz DEPENDS "${chartjs}.gz")
add_dependencies (${COMPONENT_LIB} chartjs_gz)
target_add_binary_data (${COMPONENT_LIB} "${chartjs}.gz" BINARY)
