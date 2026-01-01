/**
 * @file esp_hardware_discovery.c
 * @brief Hardware capability discovery with 4-byte IC descriptors
 * @version 1.0.0
 * 
 * Implementation for reading/writing board capabilities to 24AA02E64 EEPROM
 */

#include "esp_hardware_discovery.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>

static const char *TAG = "EEPROM_CAP";

// I2C parameters
#define I2C_MASTER_TIMEOUT_MS       100
#define EEPROM_WRITE_DELAY_MS       5       // Page write time

// ============================================================================
// LOW-LEVEL I2C FUNCTIONS
// ============================================================================

/**
 * @brief Write bytes to EEPROM
 */
static esp_err_t eeprom_write_bytes(uint8_t i2c_addr, uint8_t mem_addr, 
                                     const uint8_t *data, size_t len) {
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, mem_addr, true);
    i2c_master_write(cmd, data, len, true);
    i2c_master_stop(cmd);
    
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 
                                          pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(EEPROM_WRITE_DELAY_MS));
    }
    
    return ret;
}

/**
 * @brief Read bytes from EEPROM
 */
static esp_err_t eeprom_read_bytes(uint8_t i2c_addr, uint8_t mem_addr, 
                                    uint8_t *data, size_t len) {
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    
    // Set address
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, mem_addr, true);
    
    // Read data
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (i2c_addr << 1) | I2C_MASTER_READ, true);
    
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, &data[len - 1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 
                                          pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    
    return ret;
}

/**
 * @brief Check if EEPROM is present at address
 */
static bool eeprom_probe(uint8_t i2c_addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    
    esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 
                                          pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    
    return (ret == ESP_OK);
}

// ============================================================================
// PUBLIC API FUNCTIONS
// ============================================================================

bool eeprom_is_programmed(uint8_t i2c_addr) {
    uint8_t magic;
    esp_err_t ret = eeprom_read_bytes(i2c_addr, CAP_OFFSET_MAGIC, &magic, 1);
    
    if (ret != ESP_OK) {
        return false;
    }
    
    return (magic >= CAP_MAGIC_MIN && magic <= CAP_MAGIC_MAX);
}

bool eeprom_read_unique_id(uint8_t i2c_addr, uint8_t *unique_id) {
    if (unique_id == NULL) {
        return false;
    }
    
    esp_err_t ret = eeprom_read_bytes(i2c_addr, EEPROM_UNIQUE_ID_START, 
                                       unique_id, EEPROM_UNIQUE_ID_SIZE);
    
    return (ret == ESP_OK);
}

bool eeprom_read_capabilities(uint8_t i2c_addr, eeprom_capabilities_t *caps) {
    if (caps == NULL) {
        ESP_LOGE(TAG, "NULL capabilities pointer");
        return false;
    }
    
    memset(caps, 0, sizeof(eeprom_capabilities_t));
    caps->i2c_address = i2c_addr;
    
    // Read header (8 bytes)
    uint8_t header[8];
    esp_err_t ret = eeprom_read_bytes(i2c_addr, CAP_OFFSET_MAGIC, header, 8);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read header from 0x%02X", i2c_addr);
        return false;
    }
    
    caps->magic = header[0];
    caps->project_id = header[1];
    caps->pcb_id = header[2];
    caps->revision = header[3];
    caps->reserved[0] = header[4];
    caps->reserved[1] = header[5];
    caps->reserved[2] = header[6];
    caps->component_count = header[7];
    
    // Validate magic byte
    if (caps->magic < CAP_MAGIC_MIN || caps->magic > CAP_MAGIC_MAX) {
        ESP_LOGW(TAG, "Invalid magic byte: 0x%02X", caps->magic);
        caps->is_valid = false;
        return false;
    }
    
    caps->is_valid = true;
    
    // Validate component count
    if (caps->component_count > CAP_MAX_COMPONENTS) {
        ESP_LOGW(TAG, "Component count too high: %d (max %d)", 
                 caps->component_count, CAP_MAX_COMPONENTS);
        caps->component_count = CAP_MAX_COMPONENTS;
    }
    
    // Read IC descriptors (4 bytes each)
    if (caps->component_count > 0) {
        size_t ic_data_len = caps->component_count * CAP_BYTES_PER_IC;
        uint8_t *ic_data = malloc(ic_data_len);
        
        if (ic_data == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %d bytes for IC data", ic_data_len);
            return false;
        }
        
        ret = eeprom_read_bytes(i2c_addr, CAP_OFFSET_IC_LIST, 
                                 ic_data, ic_data_len);
        
        if (ret == ESP_OK) {
            // Parse IC descriptors
            for (int i = 0; i < caps->component_count; i++) {
                int offset = i * CAP_BYTES_PER_IC;
                caps->components[i].category = ic_data[offset];
                caps->components[i].id = ic_data[offset + 1];
                caps->components[i].i2c_address = ic_data[offset + 2];
                caps->components[i].status = ic_data[offset + 3];
            }
        } else {
            ESP_LOGE(TAG, "Failed to read IC descriptors");
            free(ic_data);
            return false;
        }
        
        free(ic_data);
    }
    
    // Read unique ID
    eeprom_read_unique_id(i2c_addr, caps->unique_id);
    
    ESP_LOGI(TAG, "Read capabilities from 0x%02X: %s PCB v%d.%d, %d components",
             i2c_addr, eeprom_project_name(caps->project_id), 
             caps->pcb_id, caps->revision, caps->component_count);
    
    return true;
}

bool eeprom_write_capabilities(uint8_t i2c_addr, 
                                const eeprom_capabilities_t *caps, 
                                bool force) {
    if (caps == NULL) {
        ESP_LOGE(TAG, "NULL capabilities pointer");
        return false;
    }
    
    // Check if already programmed
    if (!force && eeprom_is_programmed(i2c_addr)) {
        ESP_LOGW(TAG, "EEPROM already programmed at 0x%02X (use force=true to override)", 
                 i2c_addr);
        return false;
    }
    
    // Validate magic byte
    if (caps->magic < CAP_MAGIC_MIN || caps->magic > CAP_MAGIC_MAX) {
        ESP_LOGE(TAG, "Invalid magic byte: 0x%02X (must be %d-%d)", 
                 caps->magic, CAP_MAGIC_MIN, CAP_MAGIC_MAX);
        return false;
    }
    
    // Validate component count
    if (caps->component_count > CAP_MAX_COMPONENTS) {
        ESP_LOGE(TAG, "Too many components: %d (max %d)", 
                 caps->component_count, CAP_MAX_COMPONENTS);
        return false;
    }
    
    ESP_LOGI(TAG, "Writing capabilities to 0x%02X: %s PCB v%d.%d, %d components",
             i2c_addr, eeprom_project_name(caps->project_id),
             caps->pcb_id, caps->revision, caps->component_count);
    
    // Write header (8 bytes)
    uint8_t header[8] = {
        caps->magic,
        caps->project_id,
        caps->pcb_id,
        caps->revision,
        0,  // reserved
        0,  // reserved
        0,  // reserved
        caps->component_count
    };
    
    esp_err_t ret = eeprom_write_bytes(i2c_addr, CAP_OFFSET_MAGIC, header, 8);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write header");
        return false;
    }
    
    // Write IC descriptors (4 bytes each)
    if (caps->component_count > 0) {
        size_t ic_data_len = caps->component_count * CAP_BYTES_PER_IC;
        uint8_t *ic_data = malloc(ic_data_len);
        
        if (ic_data == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %d bytes for IC data", ic_data_len);
            return false;
        }
        
        // Pack IC descriptors
        for (int i = 0; i < caps->component_count; i++) {
            int offset = i * CAP_BYTES_PER_IC;
            ic_data[offset] = caps->components[i].category;
            ic_data[offset + 1] = caps->components[i].id;
            ic_data[offset + 2] = caps->components[i].i2c_address;
            ic_data[offset + 3] = caps->components[i].status;
        }
        
        ret = eeprom_write_bytes(i2c_addr, CAP_OFFSET_IC_LIST, 
                                  ic_data, ic_data_len);
        
        free(ic_data);
        
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write IC descriptors");
            return false;
        }
    }
    
    ESP_LOGI(TAG, "Successfully wrote capabilities to 0x%02X", i2c_addr);
    return true;
}

int eeprom_scan_bus(eeprom_capabilities_t *caps, int max_devices) {
    if (caps == NULL || max_devices <= 0) {
        return 0;
    }
    
    ESP_LOGI(TAG, "Scanning I2C bus for 24AA02E64 EEPROMs...");
    
    int found = 0;
    
    for (uint8_t addr = EEPROM_I2C_ADDR_0; 
         addr <= EEPROM_I2C_ADDR_7 && found < max_devices; 
         addr++) {
        
        if (eeprom_probe(addr)) {
            ESP_LOGI(TAG, "Found EEPROM at 0x%02X", addr);
            
            if (eeprom_is_programmed(addr)) {
                if (eeprom_read_capabilities(addr, &caps[found])) {
                    found++;
                } else {
                    ESP_LOGW(TAG, "Failed to read capabilities from 0x%02X", addr);
                }
            } else {
                ESP_LOGI(TAG, "EEPROM at 0x%02X is not programmed", addr);
            }
        }
    }
    
    ESP_LOGI(TAG, "Scan complete: found %d programmed EEPROM(s)", found);
    return found;
}

bool eeprom_has_ic(const eeprom_capabilities_t *caps, 
                   uint8_t category, uint8_t id) {
    if (caps == NULL || !caps->is_valid) {
        return false;
    }
    
    for (int i = 0; i < caps->component_count; i++) {
        if (caps->components[i].category == category &&
            caps->components[i].id == id &&
            caps->components[i].status == IC_STATUS_INSTALLED) {
            return true;
        }
    }
    
    return false;
}

int eeprom_count_category(const eeprom_capabilities_t *caps, uint8_t category) {
    if (caps == NULL || !caps->is_valid) {
        return 0;
    }
    
    int count = 0;
    
    for (int i = 0; i < caps->component_count; i++) {
        if (caps->components[i].category == category &&
            caps->components[i].status == IC_STATUS_INSTALLED) {
            count++;
        }
    }
    
    return count;
}

eeprom_ic_descriptor_t* eeprom_find_category(const eeprom_capabilities_t *caps, 
                                              uint8_t category) {
    if (caps == NULL || !caps->is_valid) {
        return NULL;
    }
    
    for (int i = 0; i < caps->component_count; i++) {
        if (caps->components[i].category == category &&
            caps->components[i].status == IC_STATUS_INSTALLED) {
            return (eeprom_ic_descriptor_t*)&caps->components[i];
        }
    }
    
    return NULL;
}

bool eeprom_update_ic_status(uint8_t i2c_addr, 
                              uint8_t category, uint8_t id, 
                              uint8_t new_status) {
    eeprom_capabilities_t caps;
    if (!eeprom_read_capabilities(i2c_addr, &caps)) {
        ESP_LOGE(TAG, "Failed to read capabilities");
        return false;
    }
    
    bool found = false;
    int ic_index = -1;
    
    for (int i = 0; i < caps.component_count; i++) {
        if (caps.components[i].category == category &&
            caps.components[i].id == id) {
            found = true;
            ic_index = i;
            break;
        }
    }
    
    if (!found) {
        ESP_LOGW(TAG, "IC not found: category=%d, id=%d", category, id);
        return false;
    }
    
    uint8_t old_status = caps.components[ic_index].status;
    caps.components[ic_index].status = new_status;
    
    uint8_t mem_addr = CAP_OFFSET_IC_LIST + (ic_index * CAP_BYTES_PER_IC) + 3;
    
    esp_err_t ret = eeprom_write_bytes(i2c_addr, mem_addr, &new_status, 1);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Updated IC status: %s -> %s",
                 eeprom_status_name(old_status),
                 eeprom_status_name(new_status));
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to write status byte");
        return false;
    }
}

// ============================================================================
// STRING CONVERSION FUNCTIONS
// ============================================================================

const char* eeprom_project_name(uint8_t project_id) {
    switch (project_id) {
        case PROJECT_GNSS:      return "GNSS";
        case PROJECT_SHEPHERD:  return "Shepherd";
        case PROJECT_MIDI:      return "MIDI";
        case PROJECT_TEST:      return "Test";
        default:                return "Unknown";
    }
}

const char* eeprom_category_name(uint8_t category) {
    switch (category) {
        case CAT_RTC:           return "RTC";
        case CAT_GPS:           return "GPS";
        case CAT_IMU:           return "IMU";
        case CAT_CRYPTO:        return "Crypto";
        case CAT_DISPLAY:       return "Display";
        case CAT_COMM:          return "Communication";
        case CAT_USB_SERIAL:    return "USB/Serial";
        case CAT_MOTOR:         return "Motor";
        case CAT_TEMP:          return "Temperature";
        case CAT_PRESSURE:      return "Pressure";
        case CAT_SENSOR:        return "Sensor";
        case CAT_AUDIO:         return "Audio";
        case CAT_POWER:         return "Power";
        case CAT_LED:           return "LED";
        case CAT_IO_EXPANDER:   return "I/O Expander";
        case CAT_MEMORY:        return "Memory";
        case CAT_MCU:           return "MCU";
        case CAT_CONNECTOR:     return "Connector";
        case CAT_BUTTON:        return "Button/Switch";
        case CAT_BATTERY:       return "Battery/Power";
        case CAT_ACTUATOR:      return "Actuator";
        case CAT_ANTENNA:       return "Antenna";
        case CAT_USB_HUB:       return "USB Hub";
        case CAT_MISC:          return "Misc";
        default:                return "Unknown";
    }
}

const char* eeprom_ic_name(const eeprom_ic_descriptor_t *ic) {
    if (ic == NULL) {
        return "NULL";
    }
    
    if (ic->category == CAT_RTC) {
        switch (ic->id) {
            case RTC_MCP79412: return "MCP79412";
            case RTC_MCP79410: return "MCP79410";
            case RTC_DS3231:   return "DS3231";
            case RTC_MAX31343: return "MAX31343";
            case RTC_PCF8523:  return "PCF8523";
            case RTC_RV3028:   return "RV3028";
        }
    }
    
    if (ic->category == CAT_GPS) {
        switch (ic->id) {
            case GPS_ZED_F9P: return "ZED-F9P";
            case GPS_NEO_M8P: return "NEO-M8P";
            case GPS_NEO_M9N: return "NEO-M9N";
            case GPS_NEO_M9P: return "NEO-M9P";
            case GPS_SAM_M8Q: return "SAM-M8Q";
            case GPS_NEO_7M:  return "NEO-7M";
        }
    }
    
    if (ic->category == CAT_IMU) {
        switch (ic->id) {
            case IMU_ICM20948:  return "ICM-20948";
            case IMU_BNO086:    return "BNO086";
            case IMU_LSM6DSO32: return "LSM6DSO32";
            case IMU_MPU6050:   return "MPU6050";
            case IMU_BNO055:    return "BNO055";
            case IMU_LSM9DS1:   return "LSM9DS1";
        }
    }
    
    if (ic->category == CAT_CRYPTO) {
        switch (ic->id) {
            case CRYPTO_ATECC608C: return "ATECC608C";
            case CRYPTO_ATECC608A: return "ATECC608A";
            case CRYPTO_ATSHA204A: return "ATSHA204A";
            case CRYPTO_ATECC508A: return "ATECC508A";
        }
    }
    
    if (ic->category == CAT_DISPLAY) {
        switch (ic->id) {
            case DISPLAY_SSD1306:   return "SSD1306";
            case DISPLAY_SH1106:    return "SH1106";
            case DISPLAY_ST7789:    return "ST7789";
            case DISPLAY_ILI9341:   return "ILI9341";
            case DISPLAY_E_INK_2_9: return "E-Ink 2.9\"";
        }
    }
    
    if (ic->category == CAT_COMM) {
        switch (ic->id) {
            case COMM_ESP32_C6:  return "ESP32-C6";
            case COMM_NRF52840:  return "nRF52840";
            case COMM_SX1262:    return "SX1262";
            case COMM_RFM95W:    return "RFM95W";
            case COMM_ESP32:     return "ESP32";
        }
    }
    
    if (ic->category == CAT_USB_SERIAL) {
        switch (ic->id) {
            case USB_SERIAL_CP2102:     return "CP2102";
            case USB_SERIAL_CH340:      return "CH340";
            case USB_SERIAL_CH341:      return "CH341";
            case USB_SERIAL_FT232RL:    return "FT232RL";
            case USB_SERIAL_FT230X:     return "FT230X";
            case USB_SERIAL_FT231X:     return "FT231X";
            case USB_SERIAL_NATIVE:     return "Native USB";
            case USB_SERIAL_CP2112:     return "CP2112";
            case USB_SERIAL_CY7C65213:  return "CY7C65213";
            case USB_SERIAL_CY7C65215:  return "CY7C65215";
            case USB_SERIAL_MCP2200:    return "MCP2200";
            case USB_SERIAL_FT4232H:    return "FT4232H";
        }
    }
    
    if (ic->category == CAT_MOTOR) {
        switch (ic->id) {
            case MOTOR_TB6612:     return "TB6612";
            case MOTOR_DRV8833:    return "DRV8833";
            case MOTOR_L298N:      return "L298N";
            case MOTOR_WAVE_ROVER: return "WAVE ROVER";
        }
    }
    
    if (ic->category == CAT_TEMP) {
        switch (ic->id) {
            case TEMP_MCP9808: return "MCP9808";
            case TEMP_SHT35:   return "SHT35";
            case TEMP_DS18B20: return "DS18B20";
            case TEMP_BME280:  return "BME280";
            case TEMP_SI7021:  return "Si7021";
        }
    }
    
    if (ic->category == CAT_PRESSURE) {
        switch (ic->id) {
            case PRESSURE_BMP280: return "BMP280";
            case PRESSURE_BMP388: return "BMP388";
            case PRESSURE_MS5611: return "MS5611";
        }
    }
    
    if (ic->category == CAT_SENSOR) {
        switch (ic->id) {
            case SENSOR_TOF_VL53L0X:  return "VL53L0X";
            case SENSOR_TOF_VL53L4CD: return "VL53L4CD";
            case SENSOR_LIDAR_TF:     return "TF Lidar";
            case SENSOR_ULTRASONIC:   return "Ultrasonic";
            case SENSOR_HALL_EFFECT:  return "Hall Effect";
            case SENSOR_LIGHT_TSL25911: return "TSL25911";
            case SENSOR_THERMOCOUPLE_MAX31855: return "MAX31855";
        }
    }
    
    if (ic->category == CAT_AUDIO) {
        switch (ic->id) {
            case AUDIO_MAX98357: return "MAX98357";
            case AUDIO_PCM5102:  return "PCM5102";
            case AUDIO_UDA1334:  return "UDA1334";
            case AUDIO_PCM4222:  return "PCM4222";
            case AUDIO_MAX9814:  return "MAX9814";
        }
    }
    
    if (ic->category == CAT_POWER) {
        switch (ic->id) {
            case POWER_INA219:  return "INA219";
            case POWER_INA3221: return "INA3221";
            case POWER_BQ25895: return "BQ25895";
            case POWER_LTC4150: return "LTC4150";
        }
    }
    
    if (ic->category == CAT_LED) {
        switch (ic->id) {
            case LED_WS2812B:   return "WS2812B";
            case LED_APA102:    return "APA102";
            case LED_SK6812:    return "SK6812";
            case LED_PCA9685:   return "PCA9685";
            case LED_TLC5916:   return "TLC5916";
            case LED_TLC5940:   return "TLC5940";
            case LED_TLC5925:   return "TLC5925";
            case LED_MAX7219:   return "MAX7219";
            case LED_MAX7221:   return "MAX7221";
            case LED_TPIC6B595: return "TPIC6B595";
            case LED_AS1107:    return "AS1107";
        }
    }
    
    if (ic->category == CAT_IO_EXPANDER) {
        switch (ic->id) {
            case IO_MCP23008: return "MCP23008";
            case IO_MCP23017: return "MCP23017";
            case IO_TCA9548A: return "TCA9548A";
            case IO_PCA9685:  return "PCA9685";
        }
    }
    
    if (ic->category == CAT_MEMORY) {
        switch (ic->id) {
            case MEMORY_24AA02E64: return "24AA02E64";
            case MEMORY_24LC256:   return "24LC256";
            case MEMORY_AT24C32:   return "AT24C32";
            case MEMORY_25LC640:   return "25LC640";
            case MEMORY_W25Q128:   return "W25Q128";
        }
    }
    
    if (ic->category == CAT_MCU) {
        switch (ic->id) {
            case MCU_ESP32_C6: return "ESP32-C6";
            case MCU_ESP32_S3: return "ESP32-S3";
            case MCU_ESP32:    return "ESP32";
            case MCU_RP2040:   return "RP2040";
            case MCU_STM32F4:  return "STM32F4";
        }
    }
    
    if (ic->category == CAT_CONNECTOR) {
        switch (ic->id) {
            case CONNECTOR_USB_UART:      return "USB-UART";
            case CONNECTOR_USB_OTG:       return "USB-OTG";
            case CONNECTOR_ETHERNET_RJ45: return "Ethernet RJ45";
            case CONNECTOR_MIKROBUS:      return "mikroBUS";
            case CONNECTOR_QWIIC:         return "Qwiic";
            case CONNECTOR_GROVE:         return "Grove";
            case CONNECTOR_STEMMA_QT:     return "STEMMA QT";
        }
    }
    
    if (ic->category == CAT_BUTTON) {
        switch (ic->id) {
            case BUTTON_RESET:       return "Reset Button";
            case BUTTON_BOOT:        return "Boot Button";
            case BUTTON_WIFI_DEFAULT: return "WiFi Default Button";
            case BUTTON_USER_2:      return "User Button 2";
            case SWITCH_DIP_4POS:    return "DIP Switch 4-pos";
            case SWITCH_MODE_SELECT: return "Mode Select Switch";
        }
    }
    
    if (ic->category == CAT_BATTERY) {
        switch (ic->id) {
            case BATTERY_LIPO_1S:    return "LiPo 1S";
            case BATTERY_LIPO_2S:    return "LiPo 2S";
            case BATTERY_LIPO_3S:    return "LiPo 3S";
            case BATTERY_18650_2S:   return "18650 2S";
            case BATTERY_SOLAR_6V:   return "Solar 6V";
            case POWER_POE:          return "PoE";
        }
    }
    
    if (ic->category == CAT_ACTUATOR) {
        switch (ic->id) {
            case ACTUATOR_RELAY_SPDT:  return "Relay SPDT";
            case ACTUATOR_SERVO_SG90:  return "Servo SG90";
            case ACTUATOR_MOTOR_TB6612: return "Motor TB6612";
            case ACTUATOR_SOLENOID:    return "Solenoid";
            case ACTUATOR_BUZZER:      return "Buzzer";
        }
    }
    
    if (ic->category == CAT_ANTENNA) {
        switch (ic->id) {
            case ANTENNA_PCB_2_4GHZ:   return "PCB 2.4GHz";
            case ANTENNA_EXTERNAL_915: return "External 915MHz";
            case ANTENNA_DIPOLE_GPS:   return "Dipole GPS";
            case ANTENNA_PATCH_GPS:    return "Patch GPS";
        }
    }
    
    if (ic->category == CAT_USB_HUB) {
        switch (ic->id) {
            case USB_HUB_CY7C65621: return "CY7C65621";
            case USB_HUB_CY7C65631: return "CY7C65631";
        }
    }
    
    return "Unknown IC";
}

const char* eeprom_status_name(uint8_t status) {
    switch (status) {
        case IC_STATUS_UNKNOWN:       return "Unknown";
        case IC_STATUS_INSTALLED:     return "Installed";
        case IC_STATUS_NOT_POPULATED: return "Not Populated";
        case IC_STATUS_FAILED:        return "Failed";
        case IC_STATUS_DISABLED:      return "Disabled";
        case IC_STATUS_TESTING:       return "Testing";
        case IC_STATUS_DEPRECATED:    return "Deprecated";
        case IC_STATUS_RESERVED:      return "Reserved";
        default:                      return "Custom";
    }
}

bool eeprom_validate_self_reference(const eeprom_capabilities_t *caps) {
    if (caps == NULL || !caps->is_valid) {
        return false;
    }
    
    if (caps->component_count == 0) {
        ESP_LOGW(TAG, "No components defined - cannot validate self-reference");
        return false;
    }
    
    const eeprom_ic_descriptor_t *first = &caps->components[0];
    
    if (first->category != CAT_MEMORY) {
        ESP_LOGW(TAG, "Component[0] is not MEMORY category (convention: EEPROM should be first)");
        return false;
    }
    
    if (first->id != MEMORY_24AA02E64) {
        ESP_LOGW(TAG, "Component[0] is not 24AA02E64 (found: %s)", 
                 eeprom_ic_name(first));
        return false;
    }
    
    if (first->i2c_address != caps->i2c_address) {
        ESP_LOGE(TAG, "EEPROM self-reference address mismatch! Says 0x%02X, actually at 0x%02X",
                 first->i2c_address, caps->i2c_address);
        return false;
    }
    
    if (first->status != IC_STATUS_INSTALLED) {
        ESP_LOGW(TAG, "EEPROM self-reference status is not INSTALLED: %s",
                 eeprom_status_name(first->status));
        return false;
    }
    
    ESP_LOGI(TAG, "✓ EEPROM self-reference validated at 0x%02X", caps->i2c_address);
    return true;
}

void eeprom_print_capabilities(const eeprom_capabilities_t *caps) {
    if (caps == NULL) {
        ESP_LOGE(TAG, "NULL capabilities");
        return;
    }
    
    if (!caps->is_valid) {
        ESP_LOGW(TAG, "Invalid capabilities data");
        return;
    }
    
    ESP_LOGI(TAG, "=== EEPROM Capabilities (0x%02X) ===", caps->i2c_address);
    ESP_LOGI(TAG, "Magic:    0x%02X", caps->magic);
    ESP_LOGI(TAG, "Project:  %s (%d)", eeprom_project_name(caps->project_id), 
             caps->project_id);
    ESP_LOGI(TAG, "PCB:      %d", caps->pcb_id);
    ESP_LOGI(TAG, "Revision: %d", caps->revision);
    
    // Display timestamp like kernel compile date
    if (caps->timestamp > 0) {
        time_t ts = (time_t)caps->timestamp;
        struct tm timeinfo;
        gmtime_r(&ts, &timeinfo);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
        ESP_LOGI(TAG, "Programmed: %s (timestamp: %llu)", time_str, caps->timestamp);
    } else {
        ESP_LOGI(TAG, "Programmed: [timestamp not set]");
    }
    
    ESP_LOGI(TAG, "Unique ID: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             caps->unique_id[0], caps->unique_id[1], caps->unique_id[2], 
             caps->unique_id[3], caps->unique_id[4], caps->unique_id[5], 
             caps->unique_id[6], caps->unique_id[7]);
    
    ESP_LOGI(TAG, "Components: %d", caps->component_count);
    
    for (int i = 0; i < caps->component_count; i++) {
        const eeprom_ic_descriptor_t *ic = &caps->components[i];
        
        ESP_LOGI(TAG, "  [%2d] %-12s %-15s I2C:0x%02X Status:%s",
                 i,
                 eeprom_category_name(ic->category),
                 eeprom_ic_name(ic),
                 ic->i2c_address,
                 eeprom_status_name(ic->status));
    }
    
    ESP_LOGI(TAG, "====================================");
}
