/**
 * @file esp_hardware_discovery.h
 * @brief Hardware capability discovery with 4-byte IC descriptors
 * @version 1.0.0
 * 
 * Each IC: [category][id][i2c_address][status]
 * - Category: Type of IC (RTC, GPS, etc)
 * - ID: Specific chip within category
 * - I2C Address: Where to find it (0 if not I2C) - creative use for GPIO pins, PWM channels, etc
 * - Status: Installed/failed/unpopulated (field-updateable!)
 *
 * CRITICAL CONVENTION: components[0] should ALWAYS be the EEPROM itself for sanity checking
 *
 * USAGE: Create an I2C master bus, then call eeprom_discovery_init() once before
 * any function that touches the bus. After initialization, all bus-touching
 * functions are thread-safe (serialized by an internal mutex).
 */

#ifndef ESP_HARDWARE_DISCOVERY_H
#define ESP_HARDWARE_DISCOVERY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// EEPROM HARDWARE
// ============================================================================

#define EEPROM_24AAXXE64_SIZE           256
#define EEPROM_24AAXXE64_WRITABLE_SIZE  248
#define EEPROM_24AA02E64_PAGE_SIZE        8
#define EEPROM_24AA025E64_PAGE_SIZE      16
#define EEPROM_24CS128_SIZE          16384
#define EEPROM_24CS128_PAGE_SIZE        64
#define EEPROM_24CS128_SERIAL_SIZE      16
#define EEPROM_24CS128_SERIAL_START 0x0800  // Same security word on 24CS256/24CS512

#define EEPROM_24CS256_SIZE          32768
#define EEPROM_24CS256_PAGE_SIZE        64
#define EEPROM_24CS512_SIZE          65536
#define EEPROM_24CS512_PAGE_SIZE       128

#define EEPROM_M24128_U_SIZE       16384
#define EEPROM_M24128_U_PAGE_SIZE      64
#define EEPROM_M24128_U_UID_START  0x0000

// Select from known board/assembly information before accessing the device.
// The default preserves the common 24AA02E64/24AA025E64 wire protocol.
typedef enum {
    EEPROM_PROFILE_24AAXXE64 = 0,
    EEPROM_PROFILE_24CS128 = 1,
    EEPROM_PROFILE_M24128_U = 2,
    EEPROM_PROFILE_24CS256 = 3,
    EEPROM_PROFILE_24CS512 = 4,
} eeprom_profile_t;

typedef enum {
    EEPROM_FACTORY_ID_NONE = 0,
    EEPROM_FACTORY_ID_EUI64 = 1,
    EEPROM_FACTORY_ID_SERIAL128 = 2,
    EEPROM_FACTORY_ID_ST_UID128 = 3,
} eeprom_factory_id_kind_t;

typedef struct {
    eeprom_factory_id_kind_t kind;
    uint8_t length;
    uint8_t bytes[EEPROM_24CS128_SERIAL_SIZE];
} eeprom_factory_id_t;

// Backward-compatible part-specific size names. Both variants use the same
// 256-byte array and reserve the final eight bytes for the factory EUI-64.
#define EEPROM_24AA02E64_SIZE        EEPROM_24AAXXE64_SIZE
#define EEPROM_24AA025E64_SIZE       EEPROM_24AAXXE64_SIZE

// I2C address block. The 24AA02E64 ignores A2/A1/A0 in the control byte and
// therefore aliases across all eight addresses. The 24AA025E64 compares those
// bits with its three address pins and responds at one strapped address.
#define EEPROM_I2C_ADDR_BASE        0x50
#define EEPROM_I2C_ADDR_0           0x50
#define EEPROM_I2C_ADDR_1           0x51
#define EEPROM_I2C_ADDR_2           0x52
#define EEPROM_I2C_ADDR_3           0x53
#define EEPROM_I2C_ADDR_4           0x54
#define EEPROM_I2C_ADDR_5           0x55
#define EEPROM_I2C_ADDR_6           0x56
#define EEPROM_I2C_ADDR_7           0x57

// I2C bus speed used for the EEPROM device (Hz). All supported variants
// support 100 kHz and 400 kHz. Override at compile time if needed.
#ifndef EEPROM_DISCOVERY_I2C_SPEED_HZ
#define EEPROM_DISCOVERY_I2C_SPEED_HZ   100000
#endif

// ============================================================================
// MANIFEST LAYOUT (first 256 bytes; unchanged on 24CS128/256/512 and M24128-U)
// ============================================================================

// HEADER (16 bytes):
// Byte 0:       Magic (1-254, production uses 79)
// Byte 1:       Project ID
// Byte 2:       PCB ID
// Byte 3:       Revision
// Bytes 4-6:    Reserved (0) - Future use: checksum, flags, extended features
// Byte 7:       Component count (N, max 56)
// Bytes 8-15:   64-bit Unix timestamp, little-endian (when programmed)
//
// COMPONENTS (224 bytes):
// Bytes 16-239: N × 4-byte IC descriptors (max 56 components)
//               [category][id][i2c_addr][status]
//
// FOOTER (16 bytes):
// Bytes 240-247: Reserved (random from QA testing - future: crypto seed, batch code, etc.)
// Bytes 248-255: 8-byte unique ID (factory programmed, read-only)
// On 24CS128/256/512 and M24128-U, bytes 248-255 are unused by this format. Their factory
// identity is separate from the main array; use eeprom_read_factory_id().

#define CAP_OFFSET_MAGIC        0
#define CAP_OFFSET_PROJECT      1
#define CAP_OFFSET_PCB          2
#define CAP_OFFSET_REVISION     3
#define CAP_OFFSET_RESERVED_1   4       // Reserved for future: CRC8 checksum?
#define CAP_OFFSET_RESERVED_2   5       // Reserved for future: Feature flags?
#define CAP_OFFSET_RESERVED_3   6       // Reserved for future: Extended count?
#define CAP_OFFSET_IC_COUNT     7
#define CAP_OFFSET_TIMESTAMP    8       // 64-bit Unix timestamp, little-endian (8 bytes)
#define CAP_OFFSET_COMPONENTS   16      // Start of component array

#define CAP_MAX_COMPONENTS      56      // (240-16)/4 = 56 component slots
#define CAP_BYTES_PER_IC        4       // [cat][id][addr][status]

#define CAP_OFFSET_FOOTER       240     // Reserved footer bytes 240-247 (8 bytes);
                                        // full footer region incl. unique ID is 240-255
#define EEPROM_UNIQUE_ID_START  248     // Factory unique ID (8 bytes)
#define EEPROM_UNIQUE_ID_SIZE   8

// DEPRECATED: legacy alias from a pre-release layout where components started
// at byte 8. Use CAP_OFFSET_COMPONENTS. Will be removed in a future version.
#define CAP_OFFSET_IC_LIST      CAP_OFFSET_COMPONENTS

// Magic byte
#define CAP_MAGIC_MIN           1
#define CAP_MAGIC_MAX           254
#define CAP_MAGIC_PREFERRED     79      // Production standard

#define CAP_MAGIC_UNPROGRAMMED  0
#define CAP_MAGIC_ERASED        255

// ============================================================================
// RESERVED BYTES - FUTURE USE IDEAS
// ============================================================================
// Reserved byte 4: Could be CRC8 checksum of entire structure
// Reserved byte 5: Feature flags (battery present, USB host, etc)
// Reserved byte 6: Extended component count (high byte for >255 components)
//
// Current design: Keep at 0 for forward compatibility
// Future firmware can check these bytes and add features without breaking old boards
// ============================================================================

// ============================================================================
// IC STATUS FLAGS (4th byte of descriptor)
// ============================================================================

typedef enum {
    IC_STATUS_UNKNOWN       = 0,        // Status not set/unknown
    IC_STATUS_INSTALLED     = 1,        // Installed and working
    IC_STATUS_NOT_POPULATED = 2,        // Footprint exists, not populated
    IC_STATUS_FAILED        = 3,        // Installed but failed
    IC_STATUS_DISABLED      = 4,        // Installed but disabled in software
    IC_STATUS_TESTING       = 5,        // Under test/calibration
    IC_STATUS_DEPRECATED    = 6,        // Installed but firmware moved on
    // 7-254 available for custom statuses
    IC_STATUS_RESERVED      = 255       // Reserved
} eeprom_ic_status_t;

// ============================================================================
// PROJECT IDs
// ============================================================================

typedef enum {
    PROJECT_UNKNOWN     = 0,
    PROJECT_GNSS        = 1,        // GNSS/GPS monitoring and logging
    PROJECT_SHEPHERD    = 2,        // Swarm robotics
    PROJECT_MIDI        = 3,        // Audio hardware
    PROJECT_TEST        = 255
} eeprom_project_id_t;

// ============================================================================
// PCB IDs
// ============================================================================

typedef enum {
    GNSS_PCB_MAIN         = 1,
    GNSS_PCB_SENSOR       = 2,
    GNSS_PCB_DISPLAY      = 3,
} gnss_pcb_id_t;

typedef enum {
    SHEPHERD_PCB_ROVER      = 1,
    SHEPHERD_PCB_BASE       = 2,
    SHEPHERD_PCB_CORNER     = 3,
    SHEPHERD_PCB_LED_BUTTON = 4,
} shepherd_pcb_id_t;

typedef enum {
    MIDI_PCB_MAIN           = 1,
    MIDI_PCB_AUDIO_IO       = 2,
    MIDI_PCB_CONTROLLER     = 3,
} midi_pcb_id_t;

// ============================================================================
// IC CATEGORIES
// ============================================================================

typedef enum {
    CAT_NONE                = 0,
    CAT_RTC                 = 1,
    CAT_GPS                 = 2,
    CAT_IMU                 = 3,
    CAT_CRYPTO              = 4,
    CAT_DISPLAY             = 5,
    CAT_COMM                = 6,
    CAT_USB_SERIAL          = 7,
    CAT_MOTOR               = 8,
    CAT_TEMP                = 9,
    CAT_PRESSURE            = 10,
    CAT_SENSOR              = 11,
    CAT_AUDIO               = 12,
    CAT_POWER               = 13,
    CAT_LED                 = 14,
    CAT_IO_EXPANDER         = 15,
    CAT_MEMORY              = 16,       // EEPROM should define itself here!
    CAT_MCU                 = 17,
    CAT_CONNECTOR           = 18,       // Firmware-relevant connectors
    CAT_BUTTON              = 19,       // User input (addr = GPIO pin)
    CAT_BATTERY             = 20,       // Power sources
    CAT_ACTUATOR            = 21,       // Motors, servos, relays (addr = PWM channel)
    CAT_ANTENNA             = 22,       // RF components
    CAT_USB_HUB             = 23,       // USB Hub controllers
    CAT_MISC                = 255,
} eeprom_ic_category_t;

// ============================================================================
// IC IDs WITHIN CATEGORIES
// ============================================================================

// RTCs (CAT_RTC = 1)
typedef enum {
    RTC_MCP79412    = 1,
    RTC_MCP79410    = 2,
    RTC_DS3231      = 3,
    RTC_MAX31343    = 4,
    RTC_PCF8523     = 5,
    RTC_RV3028      = 6,
    RTC_MAX31328    = 7,        // TCXO with integrated crystal; fixed address 0x68
} eeprom_rtc_id_t;

// GPS (CAT_GPS = 2)
typedef enum {
    GPS_ZED_F9P     = 1,
    GPS_NEO_M8P     = 2,
    GPS_NEO_M9N     = 3,
    GPS_NEO_M9P     = 4,
    GPS_SAM_M8Q     = 5,
    GPS_NEO_7M      = 6,
    GPS_NEO_M10     = 7,
    GPS_NEO_F10N    = 8,
    GPS_NEO_F10T    = 9,
    GPS_ZED_F9T     = 10,
    GPS_MAX_M10S    = 11,       // MAX form factor, 18-pad LCC, from here down
    GPS_MAX_M10N    = 12,
    GPS_MAX_F10S    = 13,
    GPS_ZED_X20P    = 14,
} eeprom_gps_id_t;

// IMU (CAT_IMU = 3)
typedef enum {
    IMU_ICM20948    = 1,
    IMU_BNO086      = 2,
    IMU_LSM6DSO32   = 3,
    IMU_MPU6050     = 4,
    IMU_BNO055      = 5,
    IMU_LSM9DS1     = 6,
    IMU_ICM45686    = 7,
    IMU_LSM6DSRX    = 8,        // Six-axis IMU; I2C 0x6A/0x6B or SPI
} eeprom_imu_id_t;

// Crypto (CAT_CRYPTO = 4)
typedef enum {
    CRYPTO_ATECC608C = 1,
    CRYPTO_ATECC608A = 2,
    CRYPTO_ATSHA204A = 3,
    CRYPTO_ATECC508A = 4,
} eeprom_crypto_id_t;

// Display (CAT_DISPLAY = 5)
typedef enum {
    DISPLAY_SSD1306     = 1,
    DISPLAY_SH1106      = 2,
    DISPLAY_ST7789      = 3,
    DISPLAY_ILI9341     = 4,
    DISPLAY_E_INK_2_9   = 5,
} eeprom_display_id_t;

// Communication (CAT_COMM = 6)
typedef enum {
    COMM_ESP32_C6       = 1,        // Thread/Zigbee radio
    COMM_NRF52840       = 2,        // BLE/Thread
    COMM_SX1262         = 3,        // LoRa
    COMM_RFM95W         = 4,        // LoRa 915MHz
    COMM_ESP32          = 5,        // WiFi/BLE
} eeprom_comm_id_t;

// USB/Serial (CAT_USB_SERIAL = 7)
typedef enum {
    USB_SERIAL_CP2102      = 1,
    USB_SERIAL_CH340       = 2,
    USB_SERIAL_CH341       = 3,
    USB_SERIAL_FT232RL     = 4,
    USB_SERIAL_FT230X      = 5,
    USB_SERIAL_FT231X      = 6,
    USB_SERIAL_NATIVE      = 7,
    USB_SERIAL_CP2112      = 8,
    USB_SERIAL_CY7C65213   = 9,
    USB_SERIAL_CY7C65215   = 10,
    USB_SERIAL_MCP2200     = 11,
    USB_SERIAL_FT4232H     = 12,
} eeprom_usb_serial_id_t;

// Motor Controllers (CAT_MOTOR = 8)
typedef enum {
    MOTOR_TB6612        = 1,
    MOTOR_DRV8833       = 2,
    MOTOR_L298N         = 3,
    MOTOR_WAVE_ROVER    = 4,        // WAVE ROVER built-in
} eeprom_motor_id_t;

// Temperature Sensors (CAT_TEMP = 9)
typedef enum {
    TEMP_MCP9808    = 1,
    TEMP_SHT35      = 2,
    TEMP_DS18B20    = 3,
    TEMP_BME280     = 4,
    TEMP_SI7021     = 5,
    TEMP_MCP9804    = 6,        // I2C 0x18-0x1F
    TEMP_STS35      = 7,        // STS35-DIS; I2C 0x4A/0x4B
    TEMP_STS31A     = 8,        // STS31A-DIS; I2C 0x4A/0x4B
    TEMP_LM75A_NXP  = 9,        // NXP LM75AD: 11-bit; I2C 0x48-0x4F
    TEMP_SHT21      = 10,       // Humidity + temperature; I2C 0x40 (not SHT21P)
    TEMP_LM35       = 11,       // Analog output, 10 mV/degree C
    TEMP_LM34       = 12,       // Analog output, 10 mV/degree F
} eeprom_temp_id_t;

// Pressure Sensors (CAT_PRESSURE = 10)
typedef enum {
    PRESSURE_BMP280     = 1,
    PRESSURE_BMP388     = 2,
    PRESSURE_MS5611     = 3,
    PRESSURE_BMP390     = 4,
    PRESSURE_MS5607     = 5,    // 10-1200 mbar extended range; not an MS5611
    PRESSURE_BMP390L    = 6,    // Preserve exact part identity; I2C 0x76/0x77 or SPI
} eeprom_pressure_id_t;

// Generic Sensors (CAT_SENSOR = 11)
typedef enum {
    SENSOR_TOF_VL53L0X     = 1,
    SENSOR_TOF_VL53L4CD    = 2,
    SENSOR_LIDAR_TF        = 3,
    SENSOR_ULTRASONIC      = 4,
    SENSOR_HALL_EFFECT     = 5,
    SENSOR_LIGHT_TSL25911  = 6,
    SENSOR_THERMOCOUPLE_MAX31855 = 7,
    SENSOR_HDC2080         = 8,
    SENSOR_MAG_MMC34160PJ  = 9, // Three-axis magnetometer
    SENSOR_HDC2022         = 10, // Humidity + temperature; I2C 0x40/0x41
    SENSOR_HIH8121         = 11, // Humidity + temperature; I2C
    SENSOR_PROX_VCNL4200   = 12, // Proximity + ambient light; I2C 0x51
    SENSOR_TOUCH_AT42QT1070 = 13, // Capacitive touch; I2C 0x1B in comms mode
    SENSOR_LIGHT_NJL7502L  = 14, // Analog phototransistor
    SENSOR_LIGHT_SFH3310   = 15, // Analog phototransistor
} eeprom_sensor_id_t;

// Audio (CAT_AUDIO = 12)
typedef enum {
    AUDIO_MAX98357      = 1,        // I2S amplifier
    AUDIO_PCM5102       = 2,        // DAC
    AUDIO_UDA1334       = 3,        // DAC
    AUDIO_PCM4222       = 4,        // 24-bit ADC
    AUDIO_MAX9814       = 5,        // Mic preamp with AGC
    AUDIO_ICS43434      = 6,        // I2S digital MEMS microphone
} eeprom_audio_id_t;

// Power Management (CAT_POWER = 13)
typedef enum {
    POWER_INA219        = 1,        // Current/voltage monitor
    POWER_INA3221       = 2,        // 3-channel monitor
    POWER_BQ25895       = 3,        // Battery charger
    POWER_LTC4150       = 4,        // Coulomb counter
    POWER_ADM7150       = 5,        // GPIO-gated low-noise LDO
    POWER_RT9193        = 6,        // GPIO-gated low-noise LDO
    POWER_INA260        = 7,        // Current/voltage/power; I2C 0x40-0x4F
    POWER_LTC2990       = 8,        // Voltage/current/temperature; I2C 0x4C-0x4F
    POWER_TPS7A20       = 9,        // GPIO-gated low-noise LDO
} eeprom_power_id_t;

// LED Drivers (CAT_LED = 14)
typedef enum {
    LED_WS2812B     = 1,
    LED_APA102      = 2,
    LED_SK6812      = 3,
    LED_PCA9685     = 4,
    LED_TLC5916     = 5,
    LED_TLC5940     = 6,
    LED_TLC5925     = 7,
    LED_MAX7219     = 8,
    LED_MAX7221     = 9,
    LED_TPIC6B595   = 10,
    LED_AS1107      = 11,
} eeprom_led_id_t;

// I/O Expanders (CAT_IO_EXPANDER = 15)
typedef enum {
    IO_MCP23008     = 1,
    IO_MCP23017     = 2,
    IO_TCA9548A     = 3,        // I2C multiplexer
    IO_PCA9685      = 4,        // PWM driver
} eeprom_io_id_t;

// Memory (CAT_MEMORY = 16)
typedef enum {
    MEMORY_24AA02E64    = 1,        // This EEPROM!
    MEMORY_24LC256      = 2,
    MEMORY_AT24C32      = 3,
    MEMORY_25LC640      = 4,        // SPI EEPROM
    MEMORY_W25Q128      = 5,        // SPI Flash
    MEMORY_24AA025E64   = 6,        // Addressable EUI-64 manifest EEPROM
    MEMORY_24CS128      = 7,        // 16 KiB EEPROM with separate 128-bit serial
    MEMORY_M24128_U     = 8,        // ST 16 KiB EEPROM with read-only 128-bit UID
    MEMORY_24CS256      = 9,        // 32 KiB EEPROM with separate 128-bit serial
    MEMORY_24CS512      = 10,       // 64 KiB EEPROM with separate 128-bit serial
} eeprom_memory_id_t;

// MCU (CAT_MCU = 17)
typedef enum {
    MCU_ESP32_C6        = 1,
    MCU_ESP32_S3        = 2,
    MCU_ESP32           = 3,
    MCU_RP2040          = 4,
    MCU_STM32F4         = 5,
} eeprom_mcu_id_t;

// Connectors (CAT_CONNECTOR = 18) - Firmware-relevant only
typedef enum {
    CONNECTOR_USB_UART      = 1,
    CONNECTOR_USB_OTG       = 2,
    CONNECTOR_ETHERNET_RJ45 = 3,
    CONNECTOR_MIKROBUS      = 4,
    CONNECTOR_QWIIC         = 5,
    CONNECTOR_GROVE         = 6,
    CONNECTOR_STEMMA_QT     = 7,
} eeprom_connector_id_t;

// Buttons/Switches (CAT_BUTTON = 19)
// NOTE: i2c_address field = GPIO pin number
typedef enum {
    BUTTON_RESET            = 1,
    BUTTON_BOOT             = 2,
    BUTTON_WIFI_DEFAULT     = 3,    // WiFi config reset button
    BUTTON_USER_2           = 4,
    SWITCH_DIP_4POS         = 5,
    SWITCH_MODE_SELECT      = 6,
} eeprom_button_id_t;

// Battery/Power Source (CAT_BATTERY = 20)
typedef enum {
    BATTERY_LIPO_1S         = 1,
    BATTERY_LIPO_2S         = 2,
    BATTERY_LIPO_3S         = 3,
    BATTERY_18650_2S        = 4,
    BATTERY_SOLAR_6V        = 5,
    POWER_POE               = 6,    // Power over Ethernet
    BATTERY_CR123A          = 7,
    BATTERY_CR2032          = 8,
    BATTERY_CR1220          = 9,
} eeprom_battery_id_t;

// Actuators (CAT_ACTUATOR = 21)
// NOTE: i2c_address field = PWM channel or GPIO
typedef enum {
    ACTUATOR_RELAY_SPDT     = 1,
    ACTUATOR_SERVO_SG90     = 2,
    ACTUATOR_MOTOR_TB6612   = 3,
    ACTUATOR_SOLENOID       = 4,
    ACTUATOR_BUZZER         = 5,
} eeprom_actuator_id_t;

// Antennas (CAT_ANTENNA = 22)
typedef enum {
    ANTENNA_PCB_2_4GHZ      = 1,
    ANTENNA_EXTERNAL_915    = 2,
    ANTENNA_DIPOLE_GPS      = 3,
    ANTENNA_PATCH_GPS       = 4,
} eeprom_antenna_id_t;

// USB Hubs (CAT_USB_HUB = 23)
typedef enum {
    USB_HUB_CY7C65621       = 1,    // Cypress 4-port USB 2.0 hub
    USB_HUB_CY7C65631       = 2,    // Cypress 4-port USB 2.0 hub
} eeprom_usb_hub_id_t;

// ============================================================================
// 4-BYTE IC DESCRIPTOR
// ============================================================================

/**
 * @brief Complete IC descriptor with runtime status
 */
typedef struct {
    uint8_t category;       // IC category (CAT_xxx)
    uint8_t id;             // ID within category
    uint8_t i2c_address;    // I2C address, OR GPIO pin, OR PWM channel, OR 0
    uint8_t status;         // Runtime status (IC_STATUS_xxx)
} eeprom_ic_descriptor_t;

// ============================================================================
// BOARD CAPABILITIES
// ============================================================================

typedef struct {
    // Identity
    uint8_t magic;
    uint8_t project_id;
    uint8_t pcb_id;
    uint8_t revision;
    
    // Reserved for future use
    uint8_t reserved[3];
    
    // Components
    uint8_t component_count;
    uint64_t timestamp;          // Unix timestamp when programmed
                                 // (persisted at bytes 8-15, little-endian)
    eeprom_ic_descriptor_t components[CAP_MAX_COMPONENTS];

    // 24AA 64-bit EUI only; zero on 24CS128/256/512 and M24128-U. For board identity on
    // any profile use eeprom_read_factory_id(), preserving its kind and length.
    uint8_t unique_id[8];

    // Reserved footer (random from QA testing; populated from bytes 240-247 on read)
    uint8_t reserved_footer[8];
    
    // Runtime
    uint8_t i2c_address;    // EEPROM's own address
    bool is_valid;          // Magic byte valid?
} eeprom_capabilities_t;

// ============================================================================
// HELPER MACROS
// ============================================================================

// Create IC descriptor: IC(category, id, i2c_addr, status)
// (The id parameter is named ic_id so it cannot capture the .id designator.)
#define IC(cat, ic_id, addr, stat) \
    ((eeprom_ic_descriptor_t){.category=(cat), .id=(ic_id), .i2c_address=(addr), .status=(stat)})

// Convenience for installed I2C devices
#define IC_I2C(cat, id, addr) \
    IC(cat, id, addr, IC_STATUS_INSTALLED)

// Convenience for non-I2C devices (SPI, 1-Wire, GPIO, etc)
#define IC_INSTALLED(cat, id) \
    IC(cat, id, 0, IC_STATUS_INSTALLED)

// For GPIO-connected devices (buttons, LEDs on GPIO)
#define IC_GPIO(cat, id, gpio_pin) \
    IC(cat, id, gpio_pin, IC_STATUS_INSTALLED)

// For PWM-connected devices (servos, motors)
#define IC_PWM(cat, id, pwm_channel) \
    IC(cat, id, pwm_channel, IC_STATUS_INSTALLED)

// Mark as not populated (for field updates)
#define IC_NOT_POP(cat, id) \
    IC(cat, id, 0, IC_STATUS_NOT_POPULATED)

// EEPROM self-reference helpers (should be component[0]). IC_EEPROM_SELF is
// retained as the backward-compatible spelling for a 24AA02E64.
#define IC_EEPROM_SELF_TYPE(memory_id, addr) \
    IC_I2C(CAT_MEMORY, memory_id, addr)

#define IC_EEPROM_SELF_24AA02E64(addr) \
    IC_EEPROM_SELF_TYPE(MEMORY_24AA02E64, addr)

#define IC_EEPROM_SELF_24AA025E64(addr) \
    IC_EEPROM_SELF_TYPE(MEMORY_24AA025E64, addr)

#define IC_EEPROM_SELF_M24128_U(addr) \
    IC_EEPROM_SELF_TYPE(MEMORY_M24128_U, addr)

#define IC_EEPROM_SELF_24CS128(addr) \
    IC_EEPROM_SELF_TYPE(MEMORY_24CS128, addr)

#define IC_EEPROM_SELF_24CS256(addr) \
    IC_EEPROM_SELF_TYPE(MEMORY_24CS256, addr)

#define IC_EEPROM_SELF_24CS512(addr) \
    IC_EEPROM_SELF_TYPE(MEMORY_24CS512, addr)

#define IC_EEPROM_SELF(addr) \
    IC_EEPROM_SELF_24AA02E64(addr)

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

/**
 * @brief Result of reading the EEPROM magic byte
 *
 * A bus error and a partial/dirty image are intentionally distinct from a
 * blank device. Provisioning software may offer initialization only for
 * EEPROM_PROGRAM_STATE_BLANK.
 */
typedef enum {
    EEPROM_PROGRAM_STATE_BLANK = 0,
    EEPROM_PROGRAM_STATE_PROGRAMMED = 1,
    EEPROM_PROGRAM_STATE_BUS_ERROR = 2,
    EEPROM_PROGRAM_STATE_INVALID = 3,
} eeprom_program_state_t;

/**
 * @brief Initialize the discovery module
 *
 * Must be called once before any function that touches the I2C bus.
 * Creates the internal mutex: after successful initialization, every
 * bus-touching function in this module is thread-safe.
 *
 * @param bus_handle Handle of an already-created I2C master bus
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if bus_handle is NULL,
 *         ESP_ERR_INVALID_STATE if already initialized, ESP_ERR_NO_MEM
 */
esp_err_t eeprom_discovery_init(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Select the wire protocol at one main-array address (0x50-0x57)
 *
 * Call after init and before scanning, reading or provisioning a 24CS128, 24CS256,
 * 24CS512 or M24128-U.
 * Other addresses retain the default 24AA profile. No I2C traffic is sent;
 * an ACK or a manifest cannot safely select the word-address width.
 * Selection persists until changed. Configure before starting worker tasks.
 * Returns ESP_ERR_INVALID_ARG for an invalid address/profile, or
 * ESP_ERR_INVALID_STATE before init.
 */
esp_err_t eeprom_set_profile(uint8_t i2c_addr, eeprom_profile_t profile);

/**
 * @brief Read the selected EEPROM's complete factory board identity
 *
 * Returns EUI64/8 bytes on 24AA, SERIAL128/16 bytes on 24CS128/256/512 or
 * ST_UID128/16 bytes on M24128-U. Both 128-bit identities use an additional
 * I2C address at main address + 8. The word address is 0x0800 on the 24CS
 * parts and 0x0000 on M24128-U. Use every byte; SERIAL128 is not an EUI or UUID.
 * Independent of manifest contents; the RTC identity is not read or used.
 * On failure the output is cleared (kind NONE, length zero).
 */
bool eeprom_read_factory_id(uint8_t i2c_addr, eeprom_factory_id_t *identity);

eeprom_program_state_t eeprom_get_program_state(uint8_t i2c_addr);

/**
 * @brief Backward-compatible boolean programming check
 *
 * Returns true only for EEPROM_PROGRAM_STATE_PROGRAMMED. Blank, invalid and
 * bus-error states return false; provisioning code should use
 * eeprom_get_program_state() so it can distinguish those cases.
 */
bool eeprom_is_programmed(uint8_t i2c_addr);

bool eeprom_read_capabilities(uint8_t i2c_addr, eeprom_capabilities_t *caps);

// 8-byte EUI API: returns false without modifying the output on
// 24CS128/256/512 or M24128-U. Use eeprom_read_factory_id() for all identity types.
bool eeprom_read_unique_id(uint8_t i2c_addr, uint8_t *unique_id);

/**
 * @brief Write capabilities to the EEPROM
 *
 * Set caps->timestamp (Unix seconds) before calling if you want the
 * programming time recorded; it is stored little-endian at bytes 8-15.
 *
 * Commit order: component descriptors and timestamp are written first, the
 * header page containing the magic byte is written last, and everything is
 * verified by read-back. On blank EEPROMs, failure before the final header
 * write leaves the magic byte unprogrammed. This is not an atomic update:
 * when force-reprogramming, the old header remains valid until the final
 * page is committed. Power loss during a page write can leave partial data.
 *
 * @param force Overwrite even if the EEPROM is already programmed. With
 *              force=false, a bus error or partial/dirty image during the
 *              guard check aborts the write (neither is treated as "blank").
 */
bool eeprom_write_capabilities(uint8_t i2c_addr,
                                const eeprom_capabilities_t *caps,
                                bool force);

/**
 * @brief Scan the 0x50-0x57 manifest EEPROM address block
 *
 * Addressable 24AA025E64 and configured 24CS128/256/512 or M24128-U devices
 * are returned independently. Security addresses 0x58-0x5F are not scanned.
 * A 24AA02E64
 * ignores the three select bits and ACKs every address in the block, so the
 * scan records that physical device once and stops.
 */
int eeprom_scan_bus(eeprom_capabilities_t *caps, int max_devices);

void eeprom_print_capabilities(const eeprom_capabilities_t *caps);

// Check if specific IC is present AND installed
bool eeprom_has_ic(const eeprom_capabilities_t *caps,
                   uint8_t category, uint8_t id);

// Count installed ICs in category (status = INSTALLED)
int eeprom_count_category(const eeprom_capabilities_t *caps, uint8_t category);

// Find first installed IC in category
const eeprom_ic_descriptor_t* eeprom_find_category(const eeprom_capabilities_t *caps,
                                                    uint8_t category);

/**
 * @brief Update status of specific IC (useful for field updates!)
 *
 * Updates the FIRST descriptor matching (category, id). If a board carries
 * two identical parts (same category and id at different addresses), use
 * eeprom_update_ic_status_at() to disambiguate.
 */
bool eeprom_update_ic_status(uint8_t i2c_addr,
                              uint8_t category, uint8_t id,
                              uint8_t new_status);

/**
 * @brief Update status of the IC matching (category, id, address)
 *
 * Like eeprom_update_ic_status(), but also matches the descriptor's
 * address byte (I2C address / GPIO pin / PWM channel), so boards with two
 * identical parts can target the right one.
 */
bool eeprom_update_ic_status_at(uint8_t i2c_addr,
                                 uint8_t category, uint8_t id,
                                 uint8_t ic_address,
                                 uint8_t new_status);

// Get name strings
const char* eeprom_project_name(uint8_t project_id);
const char* eeprom_category_name(uint8_t category);
const char* eeprom_ic_name(const eeprom_ic_descriptor_t *ic);
const char* eeprom_status_name(uint8_t status);

// Validation helpers
bool eeprom_validate_self_reference(const eeprom_capabilities_t *caps);

#ifdef __cplusplus
}
#endif

#endif // ESP_HARDWARE_DISCOVERY_H
