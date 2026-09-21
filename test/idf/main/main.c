// Build and link the public API without requiring attached hardware.
#include "esp_hardware_discovery.h"

void app_main(void) {
    eeprom_capabilities_t caps = {0};
    eeprom_factory_id_t identity;

    // No bus has been initialized: these calls must fail without I2C traffic.
    (void)eeprom_discovery_init(NULL);
    (void)eeprom_set_profile(EEPROM_I2C_ADDR_0, EEPROM_PROFILE_M24128_U);
    (void)eeprom_read_factory_id(EEPROM_I2C_ADDR_0, &identity);
    (void)eeprom_get_program_state(EEPROM_I2C_ADDR_0);
    (void)eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps);
}
