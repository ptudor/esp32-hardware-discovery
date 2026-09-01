# CLAUDE.md — ESP Hardware Discovery

## What This Is

ESP Hardware Discovery is an ESP-IDF component for runtime hardware capability
discovery. It uses 4-byte IC descriptors stored in a Microchip 24AA02E64 or
addressable 24AA025E64 EEPROM to describe what's installed on a PCB.

**The soul**: Program once at manufacturing. Discover at runtime. Know exactly what hardware you're talking to without compile-time configuration.

See OVERVIEW.md for complete architecture and API documentation.

---

## CRITICAL: Code Change Rules

**READ THIS BEFORE EVERY CHANGE.**

This is a shared component used across multiple projects. Breaking changes affect all downstream users.

### Never Do These Things

1. **Never delete a function without explicit permission.** If something seems unused, ASK. It might be used by a project you don't have visibility into.

2. **Never change the EEPROM memory layout.** Boards in the field have programmed EEPROMs. Layout changes require migration paths.

3. **Never modify IC descriptor format.** The 4-byte format `[category][id][address][status]` is fixed.

4. **Never change enum values.** Adding new values is fine. Changing existing `CAT_GPS = 2` to something else breaks all programmed boards.

5. **Never remove error handling or logging.** ESP_LOGE/LOGW/LOGI calls are essential for debugging hardware issues.

### Always Do These Things

1. **Before changing a file, state what you're changing and why.** Wait for confirmation on significant changes.

2. **When fixing a bug, preserve all existing functionality.** The fix should be minimal and surgical.

3. **When adding new IC types, use the next available ID.** Check existing enums first.

4. **When unsure, ask.** "I could solve this by X or Y — which approach do you prefer?"

5. **Test your changes.** At minimum: `idf.py build`. Better: test on actual hardware.

---

## Development Environment

**Toolchain:**
- ESP-IDF 5.0.0 or later
- CMake-based build system
- VSCode with ESP-IDF extension (recommended)

**Targets:**
ESP32, ESP32-S2, ESP32-S3, ESP32-C2, ESP32-C3, ESP32-C6, ESP32-H2

**Hardware:**
- Microchip 24AA02E64-I/SN or 24AA025E64-I/SN EEPROM
- I2C bus at 100kHz or 400kHz
- Address block: 0x50-0x57; 24AA02E64 aliases all eight addresses, while
  24AA025E64 responds only at its A0/A1/A2-selected address

**Build commands:**
```bash
idf.py set-target esp32s3    # or esp32, esp32c6, etc.
idf.py build
idf.py flash monitor
```

---

## Key Conventions

### EEPROM Self-Reference

**Always** make `components[0]` the EEPROM itself with the part-specific macro:
```c
caps.components[0] = IC_EEPROM_SELF_24AA02E64(EEPROM_I2C_ADDR_0);
// or
caps.components[0] = IC_EEPROM_SELF_24AA025E64(EEPROM_I2C_ADDR_0);
```

This enables validation that the stored address matches where we actually found the device.
The legacy `IC_EEPROM_SELF(addr)` spelling continues to mean 24AA02E64.

### Component Iteration

Skip index 0 when iterating over components:
```c
for (int i = 1; i < caps.component_count; i++) {  // Start at 1
    if (caps.components[i].status == IC_STATUS_INSTALLED) {
        // Initialize this component
    }
}
```

### Status Codes

Use the predefined status codes:
```c
IC_STATUS_UNKNOWN       = 0    // Not set
IC_STATUS_INSTALLED     = 1    // Working
IC_STATUS_NOT_POPULATED = 2    // Footprint exists, not installed
IC_STATUS_FAILED        = 3    // Installed but not working
IC_STATUS_DISABLED      = 4    // Disabled in software
IC_STATUS_TESTING       = 5    // Under test/calibration
IC_STATUS_DEPRECATED    = 6    // Firmware moved on
```

### Adding New IC Types

1. Find the appropriate category enum (e.g., `eeprom_rtc_id_t`)
2. Add the new IC with the next available ID
3. Add the name mapping in `eeprom_ic_name()`
4. Document in README.md

```c
// In header - add to enum
typedef enum {
    RTC_MCP79412    = 1,
    RTC_DS3231      = 2,
    RTC_NEW_CHIP    = 3,  // NEW: Add at end with next ID
} eeprom_rtc_id_t;

// In source - add name mapping
if (ic->category == CAT_RTC) {
    switch (ic->id) {
        case RTC_MCP79412: return "MCP79412";
        case RTC_DS3231:   return "DS3231";
        case RTC_NEW_CHIP: return "NEW-CHIP";  // NEW
    }
}
```

### Adding New Categories

1. Add to `eeprom_ic_category_t` with next available ID
2. Create new `eeprom_<category>_id_t` enum
3. Add category name to `eeprom_category_name()`
4. Add IC name handling to `eeprom_ic_name()`

---

## C Style Guidelines

### Formatting

```c
// Function definitions: return type on same line
static esp_err_t eeprom_write_bytes(uint8_t i2c_addr, uint8_t mem_addr,
                                     const uint8_t *data, size_t len) {
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // ...
}

// Braces: opening brace on same line
if (condition) {
    // code
} else {
    // code
}

// Switch statements: case at same indent as switch
switch (category) {
    case CAT_RTC:
        return "RTC";
    case CAT_GPS:
        return "GPS";
    default:
        return "Unknown";
}
```

### Naming

```c
// Functions: snake_case with module prefix
bool eeprom_read_capabilities(...)
static esp_err_t eeprom_write_bytes(...)

// Types: snake_case with _t suffix
typedef struct { ... } eeprom_capabilities_t;
typedef enum { ... } eeprom_ic_status_t;

// Constants/Macros: SCREAMING_SNAKE_CASE
#define CAP_MAGIC_PREFERRED     79
#define EEPROM_I2C_ADDR_0       0x50

// Local variables: snake_case
uint8_t header[8];
int found = 0;
```

### Error Handling

```c
// Check parameters at function entry
if (caps == NULL) {
    ESP_LOGE(TAG, "NULL capabilities pointer");
    return false;
}

// Log meaningful messages
ESP_LOGI(TAG, "Read capabilities from 0x%02X: %s PCB v%d.%d, %d components",
         i2c_addr, eeprom_project_name(caps->project_id),
         caps->pcb_id, caps->revision, caps->component_count);

// Use ESP_LOG levels appropriately
ESP_LOGE(TAG, "...");  // Errors that prevent operation
ESP_LOGW(TAG, "...");  // Warnings about unexpected conditions
ESP_LOGI(TAG, "...");  // Normal operational info
```

### Memory Management

```c
// Always check malloc results
uint8_t *ic_data = malloc(ic_data_len);
if (ic_data == NULL) {
    ESP_LOGE(TAG, "Failed to allocate %d bytes for IC data", ic_data_len);
    return false;
}

// Free in all paths
if (ret != ESP_OK) {
    free(ic_data);
    return false;
}
free(ic_data);
```

### Documentation

```c
/**
 * @brief Brief one-line description
 *
 * Longer description if needed.
 *
 * @param param_name Description of parameter
 * @return Description of return value
 */
bool function_name(type param_name);
```

---

## File Organization

```
esp32-hardware-discovery/
├── include/
│   └── esp_hardware_discovery.h    # All public API and types
├── src/
│   └── esp_hardware_discovery.c    # Implementation
├── esp_hardware_discovery_examples.c  # Reference implementations
├── CMakeLists.txt                  # ESP-IDF component config
├── idf_component.yml               # Component registry metadata
├── README.md                       # User documentation
├── OVERVIEW.md                     # Architecture summary
├── CLAUDE.md                       # This file
├── CHANGELOG.md                    # Version history
└── LICENSE.txt                     # MIT License
```

**Header structure:**
1. Include guards
2. Includes
3. Constants and macros
4. Type definitions (enums first, then structs)
5. Helper macros
6. Function prototypes

**Source structure:**
1. Includes
2. Static constants
3. Static (private) functions
4. Public API functions
5. String conversion functions

---

## Reserved Bytes

Bytes 4-6 in the header are reserved for future use:

| Byte | Potential Use |
|------|---------------|
| 4 | CRC8 checksum |
| 5 | Feature flags |
| 6 | Extended component count |

Current policy: Keep at 0 for forward compatibility. Future firmware can check these bytes and add features without breaking old boards.

---

## When You're Unsure

Before making changes, ask:

- "This would change the memory layout — should I show you a migration plan?"
- "I can add this IC to category X or create new category Y — which do you prefer?"
- "This fix requires changing the public API — is that acceptable?"
- "Should this be a compile-time option or runtime configurable?"

---

## Common Tasks

### Adding a new project

1. Add to `eeprom_project_id_t` enum with next available ID
2. Create PCB ID enum for the project (e.g., `myproject_pcb_id_t`)
3. Add project name to `eeprom_project_name()`

### Adding support for a new IC

1. Identify the correct category
2. Add IC ID to the category's enum
3. Add name mapping to `eeprom_ic_name()`
4. Document typical I2C address if applicable

### Debugging a programmed board

```c
eeprom_capabilities_t caps;
if (eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps)) {
    eeprom_print_capabilities(&caps);

    if (!eeprom_validate_self_reference(&caps)) {
        ESP_LOGW(TAG, "Self-reference validation failed!");
    }
}
```

### Marking a failed component in the field

```c
eeprom_update_ic_status(EEPROM_I2C_ADDR_0,
                        CAT_IMU, IMU_ICM20948,
                        IC_STATUS_FAILED);
```

---

## The Philosophy

This component exists because hardware diversity is a fact of life:
- Same firmware may run on multiple PCB revisions
- Components may have substitutes (chip shortages)
- Boards may be partially populated
- Field failures happen

The EEPROM becomes the source of truth for "what's on this board." Firmware adapts to reality rather than assuming a fixed configuration.

Build for flexibility. Fail gracefully. Make debugging easy.
