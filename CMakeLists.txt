# CMakeLists.txt
idf_component_register(
    SRCS "src/esp_hardware_discovery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)
