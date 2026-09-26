/**
 * Intsat board templates: the manufacturing manifest each board ID and revision
 * carries (esp_hardware_discovery.h, eeprom_intsat_template). Pure data and
 * copying; no bus access.
 *
 * Each list is the manufacturing truth for that assembly, taken from the board's
 * netlist and BOM. GPIO-valued descriptors identify the two same-part LED
 * drivers by their output-enable pins and the gated LDOs by their EN pins. A BOM
 * or netlist change to a listed part needs a new revision here, never an edit to
 * a released list: programmed EEPROMs keep the list they were written with.
 */

#include "esp_hardware_discovery.h"

#include <string.h>

// Placeholders filled per board: the manifest EEPROM's own part (from the
// profile) and the batch's humidity sensor (from the options). ID 0 is unused in
// both the memory and the sensor catalogs.
#define TEMPLATE_SELF       IC_EEPROM_SELF_TYPE(0, EEPROM_I2C_ADDR_0)
#define TEMPLATE_HUMIDITY   IC_I2C(CAT_SENSOR, 0, 0x40)

// NEO first spin, revision A.
static const eeprom_ic_descriptor_t s_neo_a[] = {
    TEMPLATE_SELF,                                      // U28
    IC_BOARD(CAT_INTSAT, INTSAT_NEO, 1),
    IC_INSTALLED(CAT_MCU, MCU_ESP32_S3),
    IC_INSTALLED(CAT_GPS, GPS_NEO_M9N),
    IC_I2C(CAT_RTC, RTC_MCP79412, 0x6F),
    IC_I2C(CAT_CRYPTO, CRYPTO_ATECC608C, 0x60),
    IC_I2C(CAT_TEMP, TEMP_MCP9808, 0x18),
    IC_I2C(CAT_PRESSURE, PRESSURE_BMP388, 0x76),
    TEMPLATE_HUMIDITY,
    IC_GPIO(CAT_POWER, POWER_ADM7150, 38),              // 3V3_GNSS
    IC_GPIO(CAT_POWER, POWER_RT9193, 21),               // 3V3_SENS
    IC_GPIO(CAT_LED, LED_TLC5916, 47),
    IC_GPIO(CAT_LED, LED_TLC5916, 48),
    IC_INSTALLED(CAT_BATTERY, BATTERY_CR123A),
    IC_INSTALLED(CAT_CONNECTOR, CONNECTOR_USB_OTG),
    IC_INSTALLED(CAT_CONNECTOR, CONNECTOR_QWIIC),
    IC_GPIO(CAT_BUTTON, BUTTON_BOOT, 0),
};

// ZED-X20P square, revision A.
static const eeprom_ic_descriptor_t s_x20_a[] = {
    TEMPLATE_SELF,                                      // U28
    IC_BOARD(CAT_INTSAT, INTSAT_X20, 1),
    IC_INSTALLED(CAT_MCU, MCU_ESP32_S3),
    IC_INSTALLED(CAT_GPS, GPS_ZED_X20P),
    IC_I2C(CAT_RTC, RTC_MAX31328, 0x68),
    IC_I2C(CAT_CRYPTO, CRYPTO_ATECC608C, 0x60),
    IC_I2C(CAT_TEMP, TEMP_MCP9808, 0x18),
    IC_I2C(CAT_PRESSURE, PRESSURE_BMP581, 0x46),        // U2
    TEMPLATE_HUMIDITY,
    IC_INSTALLED(CAT_COMM, COMM_W5500),                 // SPI Ethernet, U34
    IC_GPIO(CAT_POWER, POWER_ADM7150, 38),              // 3V3_GNSS, U27
    IC_GPIO(CAT_POWER, POWER_TPS7A20, 21),              // 3V3_SENS, U30
    IC_I2C(CAT_POWER, POWER_INA3221, 0x41),             // +5V, 3V3_GNSS and 3V3_SYS monitor, U37
    IC_GPIO(CAT_LED, LED_TLC5916, 47),
    IC_GPIO(CAT_LED, LED_TLC5916, 48),
    IC_INSTALLED(CAT_BATTERY, BATTERY_CR2032),          // RTC backup only, BT1
    IC_INSTALLED(CAT_BATTERY, BATTERY_CR123A),          // GNSS backup carrier on JBAT1
    IC_INSTALLED(CAT_CONNECTOR, CONNECTOR_USB_OTG),
    IC_INSTALLED(CAT_CONNECTOR, CONNECTOR_QWIIC),
    IC_INSTALLED(CAT_CONNECTOR, CONNECTOR_ETHERNET_RJ45),
    IC_GPIO(CAT_BUTTON, BUTTON_BOOT, 0),
    IC_GPIO(CAT_BUTTON, BUTTON_USER_2, 18),             // brightness preset, SW3
};

// MAX-M10S mobile, revision A.
static const eeprom_ic_descriptor_t s_max_a[] = {
    TEMPLATE_SELF,                                      // U28
    IC_BOARD(CAT_INTSAT, INTSAT_MAX, 1),
    IC_INSTALLED(CAT_MCU, MCU_ESP32_S3),
    IC_INSTALLED(CAT_GPS, GPS_MAX_M10S),
    IC_I2C(CAT_RTC, RTC_MCP79412, 0x6F),
    IC_I2C(CAT_CRYPTO, CRYPTO_ATECC608C, 0x60),
    IC_I2C(CAT_TEMP, TEMP_MCP9808, 0x18),
    IC_I2C(CAT_PRESSURE, PRESSURE_MS5607, 0x77),
    TEMPLATE_HUMIDITY,
    IC_I2C(CAT_IMU, IMU_ICM45686, 0x69),
    IC_I2C(CAT_SENSOR, SENSOR_MAG_MMC34160PJ, 0x30),
    IC_INSTALLED(CAT_SENSOR, SENSOR_THERMOCOUPLE_MAX31856), // SPI, U39
    IC_GPIO(CAT_POWER, POWER_TPS7A20, 38),              // 3V3_GNSS, U27
    IC_GPIO(CAT_POWER, POWER_TPS7A20, 21),              // 3V3_SENS, U30
    IC_GPIO(CAT_LED, LED_TLC5916, 47),
    IC_GPIO(CAT_LED, LED_TLC5916, 48),
    IC_INSTALLED(CAT_BATTERY, BATTERY_CR2032),          // RTC backup only, BT1
    IC_NOT_POP(CAT_BATTERY, BATTERY_CR123A),            // JBAT1 takes an optional external cell
    IC_INSTALLED(CAT_CONNECTOR, CONNECTOR_USB_OTG),
    IC_INSTALLED(CAT_CONNECTOR, CONNECTOR_QWIIC),
    IC_GPIO(CAT_BUTTON, BUTTON_BOOT, 0),
    IC_GPIO(CAT_BUTTON, BUTTON_USER_2, 18),             // brightness preset, SW3
};

#define TEMPLATE(board, rev, list) { (board), (rev), (list), sizeof(list) / sizeof((list)[0]) }
static const struct {
    uint8_t board_id, revision;
    const eeprom_ic_descriptor_t *components;
    size_t count;
} s_templates[] = {
    TEMPLATE(INTSAT_NEO, 1, s_neo_a),
    TEMPLATE(INTSAT_X20, 1, s_x20_a),
    TEMPLATE(INTSAT_MAX, 1, s_max_a),
};

_Static_assert(sizeof(s_neo_a) / sizeof(s_neo_a[0]) <= CAP_MAX_COMPONENTS, "NEO list exceeds capacity");
_Static_assert(sizeof(s_x20_a) / sizeof(s_x20_a[0]) <= CAP_MAX_COMPONENTS, "X20 list exceeds capacity");
_Static_assert(sizeof(s_max_a) / sizeof(s_max_a[0]) <= CAP_MAX_COMPONENTS, "MAX list exceeds capacity");

static bool same_descriptor(eeprom_ic_descriptor_t a, eeprom_ic_descriptor_t b) {
    return a.category == b.category && a.id == b.id &&
           a.i2c_address == b.i2c_address && a.status == b.status;
}

bool eeprom_intsat_template(uint8_t board_id, uint8_t revision,
                            const eeprom_intsat_options_t *options,
                            eeprom_profile_t profile, eeprom_capabilities_t *caps) {
    if (caps == NULL) {
        return false;
    }
    memset(caps, 0, sizeof(*caps));
    uint8_t memory_id = eeprom_profile_memory_id(profile);
    if (options == NULL || memory_id == 0 ||
        (options->humidity_id != SENSOR_HDC2080 && options->humidity_id != SENSOR_HDC2022)) {
        return false;
    }
    for (size_t t = 0; t < sizeof(s_templates) / sizeof(s_templates[0]); t++) {
        if (s_templates[t].board_id != board_id || s_templates[t].revision != revision) {
            continue;
        }
        caps->magic = CAP_MAGIC_PREFERRED;
        caps->project_id = PROJECT_GNSS;
        caps->pcb_id = GNSS_PCB_MAIN;
        caps->revision = revision;
        caps->i2c_address = EEPROM_I2C_ADDR_0;
        caps->component_count = (uint8_t)s_templates[t].count;
        for (size_t i = 0; i < s_templates[t].count; i++) {
            eeprom_ic_descriptor_t ic = s_templates[t].components[i];
            if (same_descriptor(ic, TEMPLATE_SELF)) {
                ic.id = memory_id;
            } else if (same_descriptor(ic, TEMPLATE_HUMIDITY)) {
                ic.id = options->humidity_id;
            }
            caps->components[i] = ic;
        }
        caps->is_valid = true;
        return true;
    }
    return false;
}
