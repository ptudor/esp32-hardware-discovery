# ESP-IDF 5.2 provides the new I2C API through driver; 5.3 split it out.
if(IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR LESS 3)
    set(discovery_i2c_component driver)
else()
    set(discovery_i2c_component esp_driver_i2c)
endif()

idf_component_register(
    SRCS "src/esp_hardware_discovery.c"
    INCLUDE_DIRS "include"
    REQUIRES ${discovery_i2c_component}
)
