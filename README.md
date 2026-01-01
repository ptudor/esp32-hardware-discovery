# ESP Hardware Discovery Component

ESP-IDF managed component for hardware capability discovery using 4-byte IC descriptors stored in Microchip 24AA02E64 EEPROM.

## Features

- **4-byte IC descriptors**: `[category][id][i2c_address][status]`
- **Runtime hardware discovery**: Know exactly what's on your PCB
- **Field-updateable status**: Mark failed components without full reprogram
- **60 IC capacity**: 240 bytes for component data
- **Unique 64-bit ID**: Factory-programmed unique identifier
- **Multiple projects**: Support different PCB variants

## Installation

### Using ESP Component Registry (Recommended)

Add to your project's `idf_component.yml`:
```yaml
dependencies:
  ptudor/esp_hardware_discovery: "^1.0.0"
```

### Manual Installation
```bash
cd your-project/components
git clone https://github.com/ptudor/esp_hardware_discovery.git
```

## Quick Start

### 1. Include in Your Code
```c
#include "esp_hardware_discovery.h"
```

### 2. Initialize I2C
```c
i2c_config_t conf = {
    .mode = I2C_MODE_MASTER,
    .sda_io_num = GPIO_NUM_21,
    .scl_io_num = GPIO_NUM_22,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master.clk_speed = 100000,
};
i2c_param_config(I2C_NUM_0, &conf);
i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
```

### 3. Manufacturing - Program Board
```c
eeprom_capabilities_t caps = {
    .magic = CAP_MAGIC_PREFERRED,
    .project_id = PROJECT_GNSS,
    .pcb_id = GNSS_PCB_MAIN,
    .revision = 1,
    .component_count = 5
};

// Define components with EEPROM self-reference as first entry
caps.components[0] = IC_EEPROM_SELF(EEPROM_I2C_ADDR_0);
caps.components[1] = IC_I2C(CAT_RTC, RTC_MCP79412, 0x6F);
caps.components[2] = IC_INSTALLED(CAT_GPS, GPS_ZED_F9P);
caps.components[3] = IC_I2C(CAT_IMU, IMU_ICM20948, 0x68);
caps.components[4] = IC_I2C(CAT_TEMP, TEMP_MCP9808, 0x18);

eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false);
```

### 4. Runtime - Discover Hardware
```c
eeprom_capabilities_t caps;

if (eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps)) {
    // Validate self-reference
    if (!eeprom_validate_self_reference(&caps)) {
        ESP_LOGW(TAG, "Self-reference validation failed");
    }
    
    // Initialize components
    for (int i = 1; i < caps.component_count; i++) {  // Start at 1 (skip EEPROM)
        if (caps.components[i].status == IC_STATUS_INSTALLED) {
            // Initialize based on category/id
        }
    }
}
```

## Usage Across Multiple Projects

### Project Structure
```
your-workspace/
├── gnss-project/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   └── main/
│       └── gnss_main.c
├── shepherd-project/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   └── main/
│       └── shepherd_main.c
└── midi-project/
    ├── CMakeLists.txt
    ├── idf_component.yml
    └── main/
        └── midi_main.c
```

### Each Project's `idf_component.yml`
```yaml
dependencies:
  ptudor/esp_hardware_discovery: "^1.0.0"
```

### Project-Specific Usage

**GNSS Project:**
```c
caps.project_id = PROJECT_GNSS;
caps.pcb_id = GNSS_PCB_MAIN;
caps.components[1] = IC_I2C(CAT_GPS, GPS_ZED_F9P, 0x42);
```

**Shepherd Project:**
```c
caps.project_id = PROJECT_SHEPHERD;
caps.pcb_id = SHEPHERD_PCB_ROVER;
caps.components[1] = IC_I2C(CAT_MOTOR, MOTOR_TB6612, 0x00);
```

**MIDI Project:**
```c
caps.project_id = PROJECT_MIDI;
caps.pcb_id = MIDI_PCB_MAIN;
caps.components[1] = IC_I2C(CAT_AUDIO, AUDIO_MAX98357, 0x00);
```

## Architecture

### Memory Layout (256 bytes)

```
Header (16 bytes):
  Byte 0:       Magic (79 for production)
  Byte 1:       Project ID
  Byte 2:       PCB ID
  Byte 3:       Revision
  Bytes 4-6:    Reserved (future: CRC, flags, extended count)
  Byte 7:       Component count (N, max 56)
  Bytes 8-15:   64-bit Unix timestamp (when programmed)

Components (224 bytes):
  Bytes 16-239: N × 4-byte IC descriptors (max 56 components)
                [category][id][i2c_addr][status]

Footer (16 bytes):
  Bytes 240-247: Reserved
  Bytes 248-255: 8-byte unique ID (factory programmed, read-only)
```

### IC Descriptor Format (4 bytes each)

| Byte | Purpose | Example |
|------|---------|---------|
| 0 | Category | `CAT_GPS` (2) |
| 1 | IC ID within category | `GPS_ZED_F9P` (1) |
| 2 | Address/pin/channel | I2C: `0x42`, GPIO: `9`, PWM: `0` |
| 3 | Runtime status | `IC_STATUS_INSTALLED` (1) |

## Key Concepts

### EEPROM Self-Reference

**Always** make component[0] the EEPROM itself:
```c
caps.components[0] = IC_EEPROM_SELF(EEPROM_I2C_ADDR_0);
```

This allows validation:
```c
bool eeprom_validate_self_reference(const eeprom_capabilities_t *caps);
```

### Creative Address Field Usage

The `i2c_address` field is versatile:
- **I2C devices**: Actual I2C address
- **GPIO pins**: Pin number for buttons/LEDs
- **PWM channels**: Channel number for servos/motors
- **Zero**: For SPI, UART, or other interfaces
```c
IC_I2C(CAT_RTC, RTC_MCP79412, 0x6F);          // I2C at 0x6F
IC_GPIO(CAT_BUTTON, BUTTON_BOOT, 9);           // GPIO 9
IC_PWM(CAT_ACTUATOR, ACTUATOR_SERVO_SG90, 0);  // PWM channel 0
IC_INSTALLED(CAT_GPS, GPS_ZED_F9P);            // UART (addr=0)
```

### Status Codes

Field-updateable without reprogramming:
```c
IC_STATUS_INSTALLED     // Working
IC_STATUS_FAILED        // Detected but not working
IC_STATUS_NOT_POPULATED // Footprint exists, not installed
IC_STATUS_DISABLED      // Disabled in software
IC_STATUS_TESTING       // Under test/calibration
```

Update in the field:
```c
eeprom_update_ic_status(EEPROM_I2C_ADDR_0, CAT_IMU, IMU_ICM20948, IC_STATUS_FAILED);
```

## API Reference

### Core Functions
```c
bool eeprom_is_programmed(uint8_t i2c_addr);
bool eeprom_read_capabilities(uint8_t i2c_addr, eeprom_capabilities_t *caps);
bool eeprom_write_capabilities(uint8_t i2c_addr, const eeprom_capabilities_t *caps, bool force);
bool eeprom_read_unique_id(uint8_t i2c_addr, uint8_t *unique_id);
int eeprom_scan_bus(eeprom_capabilities_t *caps, int max_devices);
```

### Query Functions
```c
bool eeprom_has_ic(const eeprom_capabilities_t *caps, uint8_t category, uint8_t id);
int eeprom_count_category(const eeprom_capabilities_t *caps, uint8_t category);
eeprom_ic_descriptor_t* eeprom_find_category(const eeprom_capabilities_t *caps, uint8_t category);
```

### Field Update
```c
bool eeprom_update_ic_status(uint8_t i2c_addr, uint8_t category, uint8_t id, uint8_t new_status);
```

### Utilities
```c
void eeprom_print_capabilities(const eeprom_capabilities_t *caps);
bool eeprom_validate_self_reference(const eeprom_capabilities_t *caps);
const char* eeprom_project_name(uint8_t project_id);
const char* eeprom_category_name(uint8_t category);
const char* eeprom_ic_name(const eeprom_ic_descriptor_t *ic);
const char* eeprom_status_name(uint8_t status);
```

## Categories

23 component categories:
- Core: RTC, GPS, IMU, Crypto, Display, Comm, USB/Serial, Motor
- Sensors: Temperature, Pressure, Generic sensors
- Peripherals: Audio, Power, LED, I/O Expander, Memory, MCU
- Physical: Connector, Button, Battery, Actuator, Antenna

Each category supports 255 unique IC IDs.

## Best Practices

1. **Always use EEPROM self-reference** as component[0]
2. **Validate after reading** with `eeprom_validate_self_reference()`
3. **Skip component[0]** when iterating (it's the EEPROM itself)
4. **Use field updates** instead of full reprogramming when possible
5. **Check status** before initializing components

## Hardware

### EEPROM
**Part**: Microchip 24AA02E64-I/SN
**Interface**: I2C (100/400 kHz)
**Addresses**: 0x50-0x57 (A0-A2 pins)
**Memory**: 256 bytes user + 64-bit unique ID
**Write Time**: 5ms per page
**Endurance**: 1,000,000 cycles

### Supported Targets
ESP32, ESP32-S2, ESP32-S3, ESP32-C2, ESP32-C3, ESP32-C6, ESP32-H2

### Framework
ESP-IDF >= 5.0.0

## File Structure

```
esp32-hardware-discovery/
├── include/
│   └── esp_hardware_discovery.h    # Public API and type definitions
├── src/
│   └── esp_hardware_discovery.c    # Implementation
├── esp_hardware_discovery_examples.c  # Usage examples
├── CMakeLists.txt                  # ESP-IDF component registration
├── idf_component.yml               # ESP Component Registry metadata
├── README.md                       # User documentation
├── CHANGELOG.md                    # Version history
├── LICENSE.txt                     # MIT License
└── CLAUDE.md                       # AI assistant context
```

## Projects Using This Component

| Project ID | Name | Description |
|------------|------|-------------|
| 1 | GNSS | GPS monitoring and logging |
| 2 | Shepherd | Swarm robotics |
| 3 | MIDI | Audio hardware |  

## License

MIT License - See LICENSE file

## Author

Patrick Tudor (www.ptudor.net)

## Changelog

See CHANGELOG.md for version history.
