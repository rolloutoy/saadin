set (COMPONENT_SRCS "cn_wired_driver.c" "Faikout.c" "bleenv.c" "settings.c" "faikout_i2c.c" "heatctl.c" "heatlog.c")
set (COMPONENT_REQUIRES "ESP32-RevK" "esp_driver_rmt" "esp_driver_uart" "esp_driver_i2c" "esp_driver_usb_serial_jtag")
set (COMPONENT_EMBED_FILES "favicon.ico" "apple-touch-icon.png")
register_component ()

# The heating graph page's script: d3 7.9.0 (dist/d3.min.js from the d3 npm package, ISC, see d3.min.js.LICENSE.md),
# then the page itself, graph.js. Joined in that order and gzipped at build time.
set (graphjs "${CMAKE_CURRENT_BINARY_DIR}/graph.js")
set (graphjs_src "${CMAKE_CURRENT_SOURCE_DIR}/d3.min.js" "${CMAKE_CURRENT_SOURCE_DIR}/graph.js")
string (REPLACE ";" "|" graphjs_in "${graphjs_src}")
add_custom_command (OUTPUT "${graphjs}.gz"
    COMMAND ${CMAKE_COMMAND} "-DOUT=${graphjs}" "-DIN=${graphjs_in}" -P "${CMAKE_CURRENT_SOURCE_DIR}/joinjs.cmake"
    COMMAND gzip -9 -n -f "${graphjs}"
    DEPENDS ${graphjs_src} "${CMAKE_CURRENT_SOURCE_DIR}/joinjs.cmake"
    VERBATIM)
add_custom_target (graphjs_gz DEPENDS "${graphjs}.gz")
add_dependencies (${COMPONENT_LIB} graphjs_gz)
target_add_binary_data (${COMPONENT_LIB} "${graphjs}.gz" BINARY)
