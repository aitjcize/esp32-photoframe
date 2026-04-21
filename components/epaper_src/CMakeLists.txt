set(src_dirs  
                  ./ 
                  ./Fonts)
set(include_dirs  
                  ./
                  ./Fonts)

idf_component_register(
  SRC_DIRS 
  ${src_dirs}
  PRIV_REQUIRES driver fatfs espressif__libpng espressif__zlib
  INCLUDE_DIRS 
  ${include_dirs})

# Disable warnings-as-errors for this component (Chinese font files have string truncation warnings in ESP-IDF v6.0)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
