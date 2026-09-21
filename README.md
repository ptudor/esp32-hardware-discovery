# ESP Hardware Discovery Component

ESP-IDF managed component for hardware capability discovery using 4-byte IC
descriptors stored in Microchip 24AA02E64, 24AA025E64 or 24CS128 EEPROM.

## Features

- **4-byte IC descriptors**: `[category][id][i2c_address][status]`
- **Runtime hardware discovery**: Know exactly what's on your PCB
- **Field-updateable status**: Mark failed components without full reprogram
- **56 IC capacity**: 224 bytes for component data
- **Factory board identity**: 64-bit EUI on 24AA or full 128-bit serial on 24CS128
- **EEPROM selection**: Non-addressable 24AA02E64, addressable 24AA025E64 or 24CS128
- **Multiple projects**: Support different PCB variants
- **Thread-safe**: All bus operations serialized by an internal mutex

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
git clone https://github.com/ptudor/esp32-hardware-discovery.git
```

## Quick Start

### 1. Include in Your Code
```c
#include "esp_hardware_discovery.h"
```

### 2. Initialize I2C and the Discovery Module
```c
#include "driver/i2c_master.h"

i2c_master_bus_config_t bus_config = {
    .i2c_port = -1,                     // Auto-select
    .sda_io_num = GPIO_NUM_21,
    .scl_io_num = GPIO_NUM_22,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
};
i2c_master_bus_handle_t bus = NULL;
ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));
ESP_ERROR_CHECK(eeprom_discovery_init(bus));
```

`eeprom_discovery_init()` must be called once before any function that
touches the bus. The EEPROM device is clocked at 100 kHz by default;
define `EEPROM_DISCOVERY_I2C_SPEED_HZ` at compile time to override.

### 3. Manufacturing - Program Board
```c
eeprom_capabilities_t caps = {
    .magic = CAP_MAGIC_PREFERRED,
    .project_id = PROJECT_GNSS,
    .pcb_id = GNSS_PCB_MAIN,
    .revision = 1,
    .component_count = 5,
    .timestamp = (uint64_t)time(NULL)   // Programming time, stored on-chip
};

// This board has an MCP79412 EEPROM at 0x57, so use the addressable variant.
// A0/A1/A2 are strapped low, selecting 0x50.
caps.components[0] = IC_EEPROM_SELF_24AA025E64(EEPROM_I2C_ADDR_0);
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

### 5. Select 24CS128 and Read the Board Identity

After initializing discovery, select the fitted part from known assembly
information **before any scan, read or write**. An ACK at `0x50` does not
identify the part, and the manifest cannot select its own address protocol.
The default `EEPROM_PROFILE_24AAXXE64` supports both 24AA parts unchanged.

```c
// MAX assembly with a 24CS128 at 0x50; call once during application setup.
ESP_ERROR_CHECK(eeprom_set_profile(0x50, EEPROM_PROFILE_24CS128));

// When constructing its manufacturing manifest:
caps.components[0] = IC_EEPROM_SELF_24CS128(0x50);

// Works before provisioning too. Use this as the board identity.
eeprom_factory_id_t board_id;
if (eeprom_read_factory_id(0x50, &board_id)) {
    // SERIAL128, length 16 on CS128; EUI64, length 8 on 24AA.
    // Store/compare kind, length, and every byte in board_id.bytes.
}
```

For MAX, the full CS128 serial is the board identity. The RTC's EUI is a
separate component identifier. Do not truncate the serial to 64 bits or
treat it as an IEEE EUI or a UUID. `eeprom_read_unique_id()` remains an
8-byte EUI API and returns false on CS128; the existing
`eeprom_capabilities_t.unique_id[8]` is zero for CS128. The new typed API
preserves the existing capabilities struct and provides an explicit failure
result: `kind = NONE`, `length = 0`, and cleared bytes.

Only the EEPROM transport changes. The manifest still has 56 descriptors,
the same offsets and timestamp, and an 8-byte reserved footer. CS128 bytes
`248–16383` and its security memory are untouched by manifest provisioning.
Blank checks examine only the established `0–247` region. The driver checks
software write protection, ACK-polls writes, and verifies provisioning and
CS128 status updates by read-back. It never writes configuration or issues
permanent lock commands. Keep WP low when using hardware write protection.

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
  Bytes 8-15:   64-bit Unix timestamp, little-endian (when programmed)

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

### Board Revision and Individual Identity

Identical component lists do not make two physical boards the same board.
The manifest has three separate design fields, each one byte:

| Field | Offset | Meaning |
|---|---|---|
| `project_id` | 1 | Product/project family |
| `pcb_id` | 2 | PCB design within that project |
| `revision` | 3 | Hardware revision of that design (0–255) |

Two boards built from the same design and revision share those fields and
their component list. Their factory EEPROM identities distinguish the
individual units: 64-bit EUI on 24AA, full 128-bit serial on CS128. Replacing
the identity EEPROM changes that physical unit's reported identity.

For repeated chips on one board, use separate descriptors with their actual
addresses and `eeprom_update_ic_status_at()` to target a particular one.
The fixed descriptor format has no extra instance/serial field for devices
that share the same category, part ID and address (for example, behind
different multiplexer channels).

### EEPROM Self-Reference

**Always** make component[0] the EEPROM itself, using the macro matching the
installed silicon:
```c
caps.components[0] = IC_EEPROM_SELF_24AA02E64(EEPROM_I2C_ADDR_0);
// or
caps.components[0] = IC_EEPROM_SELF_24AA025E64(EEPROM_I2C_ADDR_0);
// or, after selecting EEPROM_PROFILE_24CS128:
caps.components[0] = IC_EEPROM_SELF_24CS128(EEPROM_I2C_ADDR_0);
```

`IC_EEPROM_SELF(addr)` remains a backward-compatible alias for
`IC_EEPROM_SELF_24AA02E64(addr)`.

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

### Initialization
```c
esp_err_t eeprom_discovery_init(i2c_master_bus_handle_t bus_handle);
```

### Core Functions
```c
bool eeprom_is_programmed(uint8_t i2c_addr);
eeprom_program_state_t eeprom_get_program_state(uint8_t i2c_addr);
bool eeprom_read_capabilities(uint8_t i2c_addr, eeprom_capabilities_t *caps);
bool eeprom_write_capabilities(uint8_t i2c_addr, const eeprom_capabilities_t *caps, bool force);
bool eeprom_read_unique_id(uint8_t i2c_addr, uint8_t *unique_id);
int eeprom_scan_bus(eeprom_capabilities_t *caps, int max_devices);
```

Use `eeprom_get_program_state()` in provisioning code. It returns distinct
`EEPROM_PROGRAM_STATE_BLANK`, `EEPROM_PROGRAM_STATE_PROGRAMMED`,
`EEPROM_PROGRAM_STATE_INVALID`, and `EEPROM_PROGRAM_STATE_BUS_ERROR` states.
Blank means the complete writable region has a uniform `0xFF` erased or `0x00`
unprogrammed fill; an erased-looking magic byte in front of partial/dirty data
is invalid. Only `BLANK` is permission to initialize. `eeprom_is_programmed()`
is the backward-compatible convenience check and returns false for all states
except `PROGRAMMED`.

The scanner reports independently addressed 24AA025E64 and configured 24CS128
devices normally; it does not scan the CS128 security interfaces. A
24AA02E64 aliases the complete `0x50`-`0x57` block, so it is returned once and
the scan stops.

### Query Functions
```c
bool eeprom_has_ic(const eeprom_capabilities_t *caps, uint8_t category, uint8_t id);
int eeprom_count_category(const eeprom_capabilities_t *caps, uint8_t category);
const eeprom_ic_descriptor_t* eeprom_find_category(const eeprom_capabilities_t *caps, uint8_t category);
```

### Field Update
```c
// Updates the FIRST descriptor matching (category, id)
bool eeprom_update_ic_status(uint8_t i2c_addr, uint8_t category, uint8_t id, uint8_t new_status);

// Also matches the address byte - for boards with two identical parts
bool eeprom_update_ic_status_at(uint8_t i2c_addr, uint8_t category, uint8_t id,
                                uint8_t ic_address, uint8_t new_status);
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

1. **Always use the part-specific EEPROM self-reference** as component[0]
2. **Validate after reading** with `eeprom_validate_self_reference()`
3. **Skip component[0]** when iterating (it's the EEPROM itself)
4. **Use field updates** instead of full reprogramming when possible
5. **Check status** before initializing components

## Hardware

### EEPROM

| Property | 24AA02E64 | 24AA025E64 | 24CS128 |
|---|---|---|---|
| Main address | Aliases `0x50–0x57` | A0/A1/A2 select `0x50–0x57` | A0/A1/A2 select `0x50–0x57` |
| Array capacity | 256 bytes | 256 bytes | 16,384 bytes |
| Word address | 1 byte | 1 byte | 2 bytes, MSB first |
| Page size | 8 bytes | 16 bytes | 64 bytes |
| Factory identity | 64-bit EUI at `0xF8` | 64-bit EUI at `0xF8` | 128-bit serial at security word `0x0800` |
| Extra interface | None | None | Security/configuration at main address + 8 |
| SOIC pin 7 | NC | NC | WP; connect low for hardware write access |
| Self-reference | `IC_EEPROM_SELF_24AA02E64(addr)` | `IC_EEPROM_SELF_24AA025E64(addr)` | `IC_EEPROM_SELF_24CS128(addr)` |

Both 24AA parts use the same 256-byte array: bytes `0x00`-`0xF7` are writable and
the factory EUI-64 occupies read-only bytes `0xF8`-`0xFF`. The component
writes these parts at the 8-byte common denominator and ACK-polls after each chunk,
so the same storage implementation is safe on either part. Both support
100/400 kHz I2C, a maximum 5 ms write cycle, and at least 1,000,000 erase/write
cycles. See the [Microchip family datasheet](https://www.microchip.com/content/dam/mchp/documents/MPD/ProductDocuments/DataSheets/24AA02E48-24AA025E48-24AA02E64-24AA025E64-2-Kbit-I2C-Serial-EEPROMs-Data-Sheet-DS20002124.pdf).

Do not use a 24AA02E64 on a bus containing any other device in
`0x50`-`0x57`. In particular, it conflicts with the MCP79412's fixed EEPROM/EUI
address at `0x57`; a 24AA025E64 or 24CS128 at `0x50` can coexist with it.

The SOIC-8 24CS128T-I/SN is suitable for the MAX swap at 3.3 V with pin 7
grounded, but requires the CS128 firmware profile. At A0/A1/A2 = 0 its
interfaces are `0x50` and `0x58`, distinct from the RTC at `0x57`/`0x6F`.
The driver retains 100 kHz by default and does not enter high-speed mode.
See [Microchip DS20006913B](https://ww1.microchip.com/downloads/aemDocuments/documents/MPD/ProductDocuments/DataSheets/24CS128-128-Kbit-3.4-MHz-I2C-Serial-EEPROM-DS20006913.pdf),
sections 2, 6, 9 and 10.2. Bench validation remains necessary for serial
stability, writes/read-back, power-cycle retention and shared-bus operation.

### Supported Targets
ESP32, ESP32-S2, ESP32-S3, ESP32-C2, ESP32-C3, ESP32-C6, ESP32-H2

### Framework
ESP-IDF >= 5.2.0 (uses the `i2c_master` driver API)

## File Structure

```
esp32-hardware-discovery/
├── include/
│   └── esp_hardware_discovery.h    # Public API and type definitions
├── src/
│   └── esp_hardware_discovery.c    # Implementation
├── test/
│   └── host/                       # Host-side unit tests (mock I2C, no ESP-IDF needed)
├── esp_hardware_discovery_examples.c  # Usage examples (reference only)
├── CMakeLists.txt                  # ESP-IDF component registration
├── idf_component.yml               # ESP Component Registry metadata
├── README.md                       # User documentation
├── OVERVIEW.md                     # Architecture summary
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
