set(src_dirs  ./
              ./src)
set(include_dirs  ./ 
                  ./src
                  ./src/REG)
idf_component_register(
  SRC_DIRS ${src_dirs}
  INCLUDE_DIRS ${include_dirs}
  REQUIRES driver esp_driver_gpio esp_driver_i2c
)
add_compile_definitions(XPOWERS_CHIP_AXP2101 CONFIG_XPOWERS_ESP_IDF_NEW_API)
##REQUIRES