/**
 * @file esp_hardware_discovery.c
 * @brief Hardware capability discovery with 4-byte IC descriptors
 * @version 1.0.0
 *
 * Implementation for 24AA02E64, 24AA025E64, 24CS128, 24CS256, 24CS512 and
 * M24128-U manifest EEPROMs
 */

#include "esp_hardware_discovery.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "EEPROM_CAP";

// I2C parameters
#define I2C_MASTER_TIMEOUT_MS           100
#define EEPROM_PAGE_SIZE                EEPROM_24AA02E64_PAGE_SIZE // 24AA common denominator
#define EEPROM_WRITE_CYCLE_TIMEOUT_MS   6   // Twc max is 5ms; +1ms margin
#define EEPROM_ADDR_COUNT               8   // 24AA025E64 address-pin range
#define EEPROM_SECURITY_ADDR_BASE      0x58
#define EEPROM_24CS_CONFIG_START       0x8800  // Same register map on 24CS128/256/512
#define EEPROM_24CS_PROTECTION_ZONES   8       // Equal enhanced-protection zones

/**
 * Wire geometry for each profile. The 24CS parts share one security and
 * configuration register map and differ only in capacity, page size and
 * therefore protection-zone size (capacity / 8).
 */
typedef struct {
    const char *name;
    size_t capacity;
    size_t page_size;
    uint8_t memory_id;      // Required components[0] ID; 0 accepts either 24AA
    bool microchip_24cs;
} eeprom_geometry_t;

static const eeprom_geometry_t s_geometry[] = {
    [EEPROM_PROFILE_24AAXXE64] = { "24AA02E64/24AA025E64", EEPROM_24AAXXE64_SIZE,
                                   EEPROM_PAGE_SIZE, 0, false },
    [EEPROM_PROFILE_24CS128] = { "24CS128", EEPROM_24CS128_SIZE,
                                 EEPROM_24CS128_PAGE_SIZE, MEMORY_24CS128, true },
    [EEPROM_PROFILE_M24128_U] = { "M24128-U", EEPROM_M24128_U_SIZE,
                                  EEPROM_M24128_U_PAGE_SIZE, MEMORY_M24128_U, false },
    [EEPROM_PROFILE_24CS256] = { "24CS256", EEPROM_24CS256_SIZE,
                                 EEPROM_24CS256_PAGE_SIZE, MEMORY_24CS256, true },
    [EEPROM_PROFILE_24CS512] = { "24CS512", EEPROM_24CS512_SIZE,
                                 EEPROM_24CS512_PAGE_SIZE, MEMORY_24CS512, true },
};

#define EEPROM_PROFILE_COUNT    (sizeof(s_geometry) / sizeof(s_geometry[0]))
#define EEPROM_MAX_PAGE_SIZE    EEPROM_24CS512_PAGE_SIZE

// ============================================================================
// MODULE STATE (set up by eeprom_discovery_init)
// ============================================================================

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_devices[2 * EEPROM_ADDR_COUNT] = { NULL };
static eeprom_profile_t s_profiles[EEPROM_ADDR_COUNT] = { EEPROM_PROFILE_24AAXXE64 };
static SemaphoreHandle_t s_lock = NULL;

esp_err_t eeprom_discovery_init(i2c_master_bus_handle_t bus_handle) {
    if (bus_handle == NULL) {
        ESP_LOGE(TAG, "NULL bus handle");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_bus != NULL) {
        ESP_LOGE(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_bus = bus_handle;
    return ESP_OK;
}

static bool eeprom_check_init(void) {
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "Not initialized - call eeprom_discovery_init() first");
        return false;
    }
    return true;
}

static void eeprom_lock(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void eeprom_unlock(void) {
    xSemaphoreGive(s_lock);
}

static bool eeprom_valid_address(uint8_t i2c_addr) {
    return i2c_addr >= EEPROM_I2C_ADDR_0 && i2c_addr <= EEPROM_I2C_ADDR_7;
}

static const eeprom_geometry_t *eeprom_geometry(uint8_t addr) {
    return &s_geometry[eeprom_valid_address(addr) ?
                       s_profiles[addr - EEPROM_I2C_ADDR_BASE] : EEPROM_PROFILE_24AAXXE64];
}

static bool eeprom_is_24cs(uint8_t addr) {
    return eeprom_geometry(addr)->microchip_24cs;
}

static bool eeprom_is_st(uint8_t addr) {
    return eeprom_valid_address(addr) &&
           s_profiles[addr - EEPROM_I2C_ADDR_BASE] == EEPROM_PROFILE_M24128_U;
}

// Two-byte word addresses and a factory identity outside the main array.
static bool eeprom_is_large(uint8_t addr) {
    return eeprom_geometry(addr)->memory_id != 0;
}

// True for a self-reference ID that is valid only under its explicit profile.
static bool eeprom_memory_id_requires_profile(uint8_t memory_id) {
    if (memory_id == 0) {
        return false;
    }
    for (size_t i = 0; i < EEPROM_PROFILE_COUNT; i++) {
        if (s_geometry[i].memory_id == memory_id) {
            return true;
        }
    }
    return false;
}

static bool eeprom_profile_matches(uint8_t addr, const eeprom_capabilities_t *caps) {
    uint8_t self_id = caps->component_count && caps->components[0].category == CAT_MEMORY ?
                      caps->components[0].id : 0;
    uint8_t required = eeprom_geometry(addr)->memory_id;
    return required ? self_id == required : !eeprom_memory_id_requires_profile(self_id);
}

esp_err_t eeprom_set_profile(uint8_t i2c_addr, eeprom_profile_t profile) {
    if (!eeprom_valid_address(i2c_addr) || (unsigned)profile >= EEPROM_PROFILE_COUNT) {
        ESP_LOGE(TAG, "Invalid EEPROM address/profile selection");
        return ESP_ERR_INVALID_ARG;
    }
    if (!eeprom_check_init()) {
        return ESP_ERR_INVALID_STATE;
    }
    eeprom_lock();
    s_profiles[i2c_addr - EEPROM_I2C_ADDR_BASE] = profile;
    eeprom_unlock();
    return ESP_OK;
}

// ============================================================================
// LOW-LEVEL I2C FUNCTIONS (callers must hold the module lock)
// ============================================================================

/**
 * @brief Get (lazily creating) the device handle for an EEPROM address
 */
static i2c_master_dev_handle_t eeprom_get_device(uint8_t i2c_addr) {
    if (i2c_addr < EEPROM_I2C_ADDR_0 || i2c_addr >= EEPROM_SECURITY_ADDR_BASE + EEPROM_ADDR_COUNT) {
        ESP_LOGE(TAG, "Address 0x%02X outside EEPROM interfaces 0x50-0x5F", i2c_addr);
        return NULL;
    }

    int slot = i2c_addr - EEPROM_I2C_ADDR_BASE;

    if (s_devices[slot] == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = i2c_addr,
            .scl_speed_hz = EEPROM_DISCOVERY_I2C_SPEED_HZ,
        };

        esp_err_t ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_devices[slot]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add I2C device at 0x%02X: %d", i2c_addr, ret);
            s_devices[slot] = NULL;
            return NULL;
        }
    }

    return s_devices[slot];
}

/**
 * @brief ACK-poll until the EEPROM finishes its internal write cycle
 *
 * After a page write each supported EEPROM NACKs its own address until the
 * write cycle (Twc, max 5ms) completes.
 */
static esp_err_t eeprom_wait_write_complete(uint8_t i2c_addr) {
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(EEPROM_WRITE_CYCLE_TIMEOUT_MS) + 1;

    do {
        if (i2c_master_probe(s_bus, i2c_addr, 10) == ESP_OK) {
            return ESP_OK;
        }
    } while ((TickType_t)(xTaskGetTickCount() - start) <= timeout_ticks);

    ESP_LOGE(TAG, "EEPROM at 0x%02X did not complete write cycle", i2c_addr);
    return ESP_ERR_TIMEOUT;
}

// Combined transactions end with STOP; the address phase and read are joined
// by a repeated START, as required for security/configuration random reads.
static esp_err_t eeprom_read_security(uint8_t i2c_addr, uint16_t word_addr,
                                            uint8_t *data, size_t len) {
    i2c_master_dev_handle_t dev = eeprom_get_device(i2c_addr + 8);
    if (dev == NULL) {
        return ESP_FAIL;
    }
    uint8_t address[2] = { (uint8_t)(word_addr >> 8), (uint8_t)word_addr };
    return i2c_master_transmit_receive(dev, address, sizeof(address), data, len,
                                       I2C_MASTER_TIMEOUT_MS);
}

// Inspect enhanced protection; never change configuration or lock registers.
// In hardware protection mode WP must be low on the board; verify writes too.
static esp_err_t eeprom_24cs_check_write(uint8_t i2c_addr, uint16_t mem_addr, size_t len) {
    const eeprom_geometry_t *geometry = eeprom_geometry(i2c_addr);
    uint8_t config[2];
    esp_err_t ret = eeprom_read_security(i2c_addr, EEPROM_24CS_CONFIG_START,
                                               config, sizeof(config));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot read %s write protection at 0x%02X", geometry->name, i2c_addr);
        return ret;
    }
    if (config[0] & 0x02) { // EWPM, bit 9 of the 16-bit register
        size_t zone_size = geometry->capacity / EEPROM_24CS_PROTECTION_ZONES;
        for (size_t zone = mem_addr / zone_size; zone <= (mem_addr + len - 1) / zone_size; zone++) {
            if (config[1] & (1U << zone)) {
                ESP_LOGE(TAG, "%s zone %u is write-protected", geometry->name, (unsigned)zone);
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    return ESP_OK;
}

/** Write within 8-byte 24AA or the selected part's pages, ACK-polling each chunk. */
static esp_err_t eeprom_write_bytes(uint8_t i2c_addr, uint16_t mem_addr,
                                     const uint8_t *data, size_t len) {
    if (data == NULL || len == 0 || !eeprom_valid_address(i2c_addr)) {
        return ESP_ERR_INVALID_ARG;
    }

    const eeprom_geometry_t *geometry = eeprom_geometry(i2c_addr);
    bool large = eeprom_is_large(i2c_addr);
    size_t capacity = geometry->capacity;
    if (mem_addr >= capacity || len > capacity - mem_addr) {
        ESP_LOGE(TAG, "Write out of bounds: addr %u + len %zu > %zu",
                 mem_addr, len, capacity);
        return ESP_ERR_INVALID_SIZE;
    }

    if (!large && (size_t)mem_addr + len > EEPROM_UNIQUE_ID_START) {
        ESP_LOGE(TAG, "Write overlaps factory unique ID region (0x%02X-0xFF)",
                 EEPROM_UNIQUE_ID_START);
        return ESP_ERR_INVALID_ARG;
    }

    if (eeprom_is_24cs(i2c_addr)) {
        esp_err_t ret = eeprom_24cs_check_write(i2c_addr, mem_addr, len);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    i2c_master_dev_handle_t dev = eeprom_get_device(i2c_addr);
    if (dev == NULL) {
        return ESP_FAIL;
    }

    size_t written = 0;
    size_t page_size = geometry->page_size;
    size_t address_size = large ? 2 : 1;

    while (written < len) {
        uint16_t addr = mem_addr + written;
        size_t page_remaining = page_size - (addr % page_size);
        size_t chunk = len - written;
        if (chunk > page_remaining) {
            chunk = page_remaining;
        }

        uint8_t buf[2 + EEPROM_MAX_PAGE_SIZE];
        buf[0] = large ? (uint8_t)(addr >> 8) : (uint8_t)addr;
        if (large) {
            buf[1] = (uint8_t)addr;
        }
        memcpy(&buf[address_size], &data[written], chunk);

        esp_err_t ret = i2c_master_transmit(dev, buf, address_size + chunk,
                                            I2C_MASTER_TIMEOUT_MS);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = eeprom_wait_write_complete(i2c_addr);
        if (ret != ESP_OK) {
            return ret;
        }

        written += chunk;
    }

    return ESP_OK;
}

/**
 * @brief Read bytes from EEPROM
 *
 * Sequential reads are not limited by the page buffer; a single transaction
 * may cover any in-bounds range.
 */
static esp_err_t eeprom_read_bytes(uint8_t i2c_addr, uint16_t mem_addr,
                                    uint8_t *data, size_t len) {
    if (data == NULL || len == 0 || !eeprom_valid_address(i2c_addr)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool large = eeprom_is_large(i2c_addr);
    size_t capacity = eeprom_geometry(i2c_addr)->capacity;
    if (mem_addr >= capacity || len > capacity - mem_addr) {
        ESP_LOGE(TAG, "Read out of bounds: addr %u + len %zu > %zu",
                 mem_addr, len, capacity);
        return ESP_ERR_INVALID_SIZE;
    }

    i2c_master_dev_handle_t dev = eeprom_get_device(i2c_addr);
    if (dev == NULL) {
        return ESP_FAIL;
    }

    uint8_t address[2] = { (uint8_t)(mem_addr >> 8), (uint8_t)mem_addr };
    return i2c_master_transmit_receive(dev, large ? address : &address[1],
                                       large ? 2 : 1, data, len,
                                       I2C_MASTER_TIMEOUT_MS);
}

/**
 * @brief Check if EEPROM is present at address
 */
static bool eeprom_probe(uint8_t i2c_addr) {
    return (i2c_master_probe(s_bus, i2c_addr, I2C_MASTER_TIMEOUT_MS) == ESP_OK);
}

/**
 * @brief Read the magic byte and classify the device state
 *
 * "Blank" means every byte in the existing 248-byte manifest region has
 * the same erased/unprogrammed fill (extra capacity on larger parts is left alone).
 * If the magic says blank (0xFF or 0x00) but any other byte in that region
 * differs, a write was interrupted or an old image was only partly erased;
 * provisioning must require explicit recovery rather than overwrite it.
 */
static eeprom_program_state_t eeprom_get_prog_state(uint8_t i2c_addr) {
    uint8_t magic;
    esp_err_t ret = eeprom_read_bytes(i2c_addr, CAP_OFFSET_MAGIC, &magic, 1);

    if (ret != ESP_OK) {
        return EEPROM_PROGRAM_STATE_BUS_ERROR;
    }

    if (magic >= CAP_MAGIC_MIN && magic <= CAP_MAGIC_MAX) {
        return EEPROM_PROGRAM_STATE_PROGRAMMED;
    }

    uint8_t image[EEPROM_24AAXXE64_WRITABLE_SIZE];
    ret = eeprom_read_bytes(i2c_addr, 0, image, sizeof(image));
    if (ret != ESP_OK) {
        return EEPROM_PROGRAM_STATE_BUS_ERROR;
    }

    for (size_t i = 0; i < sizeof(image); i++) {
        if (image[i] != magic) {
            return EEPROM_PROGRAM_STATE_INVALID;
        }
    }

    return EEPROM_PROGRAM_STATE_BLANK;
}

// ============================================================================
// UNLOCKED INTERNALS (callers must hold the module lock)
// ============================================================================

static bool eeprom_read_unique_id_unlocked(uint8_t i2c_addr, uint8_t *unique_id) {
    if (eeprom_is_large(i2c_addr)) {
        ESP_LOGW(TAG, "Selected EEPROM has no EUI-64; use eeprom_read_factory_id()");
        return false;
    }
    esp_err_t ret = eeprom_read_bytes(i2c_addr, EEPROM_UNIQUE_ID_START,
                                       unique_id, EEPROM_UNIQUE_ID_SIZE);

    return (ret == ESP_OK);
}

static bool eeprom_read_capabilities_unlocked(uint8_t i2c_addr,
                                              eeprom_capabilities_t *caps) {
    memset(caps, 0, sizeof(eeprom_capabilities_t));
    caps->i2c_address = i2c_addr;

    // Read header including timestamp (16 bytes)
    uint8_t header[16];
    esp_err_t ret = eeprom_read_bytes(i2c_addr, CAP_OFFSET_MAGIC,
                                       header, sizeof(header));
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

    // Decode timestamp (bytes 8-15, little-endian)
    uint64_t ts = 0;
    for (int i = 0; i < 8; i++) {
        ts |= ((uint64_t)header[CAP_OFFSET_TIMESTAMP + i]) << (8 * i);
    }
    caps->timestamp = ts;

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
            ESP_LOGE(TAG, "Failed to allocate %zu bytes for IC data", ic_data_len);
            return false;
        }

        ret = eeprom_read_bytes(i2c_addr, CAP_OFFSET_COMPONENTS,
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

    // Transport and manifest must identify the same qualified model.
    if (!eeprom_profile_matches(i2c_addr, caps)) {
        ESP_LOGE(TAG, "EEPROM profile/self-reference mismatch at 0x%02X", i2c_addr);
        return false;
    }

    // Read reserved footer (bytes 240-247)
    ret = eeprom_read_bytes(i2c_addr, CAP_OFFSET_FOOTER,
                             caps->reserved_footer, sizeof(caps->reserved_footer));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read reserved footer from 0x%02X", i2c_addr);
    }

    // Read unique ID
    if (!eeprom_is_large(i2c_addr) &&
        !eeprom_read_unique_id_unlocked(i2c_addr, caps->unique_id)) {
        ESP_LOGW(TAG, "Failed to read unique ID from 0x%02X", i2c_addr);
    }

    // Everything the struct claims to hold has now been read successfully
    caps->is_valid = true;

    ESP_LOGI(TAG, "Read capabilities from 0x%02X: %s PCB v%d.%d, %d components",
             i2c_addr, eeprom_project_name(caps->project_id),
             caps->pcb_id, caps->revision, caps->component_count);

    return true;
}

static bool eeprom_write_capabilities_unlocked(uint8_t i2c_addr,
                                               const eeprom_capabilities_t *caps,
                                               bool force) {
    if (!eeprom_profile_matches(i2c_addr, caps) || (eeprom_is_large(i2c_addr) &&
        (caps->components[0].i2c_address != i2c_addr ||
         caps->components[0].status != IC_STATUS_INSTALLED))) {
        ESP_LOGE(TAG, "EEPROM profile/self-reference mismatch at 0x%02X", i2c_addr);
        return false;
    }
    // Check if already programmed. A bus error is NOT "blank": refusing to
    // write is the only safe response, otherwise a transient glitch would
    // bypass the overwrite guard.
    if (!force) {
        eeprom_program_state_t state = eeprom_get_prog_state(i2c_addr);

        if (state == EEPROM_PROGRAM_STATE_BUS_ERROR) {
            ESP_LOGE(TAG, "Cannot verify programming state of 0x%02X (bus error) - refusing to write",
                     i2c_addr);
            return false;
        }

        if (state == EEPROM_PROGRAM_STATE_PROGRAMMED) {
            ESP_LOGW(TAG, "EEPROM already programmed at 0x%02X (use force=true to override)",
                     i2c_addr);
            return false;
        }

        if (state == EEPROM_PROGRAM_STATE_INVALID) {
            ESP_LOGE(TAG, "EEPROM at 0x%02X has a partial/dirty image - refusing to write",
                     i2c_addr);
            return false;
        }
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

    // Pack and write IC descriptors first: the header page containing the
    // magic byte is committed last, so an interrupted write never leaves a
    // valid-looking magic in front of unwritten descriptors.
    uint8_t *ic_data = NULL;
    size_t ic_data_len = 0;

    if (caps->component_count > 0) {
        ic_data_len = (size_t)caps->component_count * CAP_BYTES_PER_IC;
        ic_data = malloc(ic_data_len);

        if (ic_data == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes for IC data", ic_data_len);
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

        esp_err_t ret = eeprom_write_bytes(i2c_addr, CAP_OFFSET_COMPONENTS,
                                            ic_data, ic_data_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write IC descriptors");
            free(ic_data);
            return false;
        }
    }

    // Build header image (bytes 0-15): identity plus little-endian timestamp
    uint8_t header[16] = {
        caps->magic,
        caps->project_id,
        caps->pcb_id,
        caps->revision,
        0,  // reserved
        0,  // reserved
        0,  // reserved
        caps->component_count
    };
    for (int i = 0; i < 8; i++) {
        header[CAP_OFFSET_TIMESTAMP + i] = (uint8_t)(caps->timestamp >> (8 * i));
    }

    // Commit order: timestamp page (8-15) first, then the header page (0-7)
    // containing the magic byte last.
    esp_err_t ret = eeprom_write_bytes(i2c_addr, CAP_OFFSET_TIMESTAMP,
                                        &header[CAP_OFFSET_TIMESTAMP], 8);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write timestamp");
        free(ic_data);
        return false;
    }

    ret = eeprom_write_bytes(i2c_addr, CAP_OFFSET_MAGIC, &header[0], 8);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write header");
        free(ic_data);
        return false;
    }

    // Read back and verify everything we wrote
    uint8_t verify_header[16];
    ret = eeprom_read_bytes(i2c_addr, CAP_OFFSET_MAGIC,
                             verify_header, sizeof(verify_header));
    if (ret != ESP_OK || memcmp(verify_header, header, sizeof(header)) != 0) {
        ESP_LOGE(TAG, "Header read-back verification failed at 0x%02X", i2c_addr);
        free(ic_data);
        return false;
    }

    if (ic_data != NULL) {
        uint8_t *verify_data = malloc(ic_data_len);
        if (verify_data == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes for verification", ic_data_len);
            free(ic_data);
            return false;
        }

        ret = eeprom_read_bytes(i2c_addr, CAP_OFFSET_COMPONENTS,
                                 verify_data, ic_data_len);
        bool match = (ret == ESP_OK && memcmp(verify_data, ic_data, ic_data_len) == 0);

        free(verify_data);
        free(ic_data);

        if (!match) {
            ESP_LOGE(TAG, "IC descriptor read-back verification failed at 0x%02X",
                     i2c_addr);
            return false;
        }
    }

    ESP_LOGI(TAG, "Successfully wrote capabilities to 0x%02X", i2c_addr);
    return true;
}

/**
 * @brief Shared implementation for the status update functions
 *
 * @param match_addr If true, the descriptor's address byte must also equal
 *                   ic_address; if false, the first (category, id) match wins.
 */
static bool eeprom_update_ic_status_internal(uint8_t i2c_addr,
                                             uint8_t category, uint8_t id,
                                             bool match_addr, uint8_t ic_address,
                                             uint8_t new_status) {
    eeprom_capabilities_t caps;
    if (!eeprom_read_capabilities_unlocked(i2c_addr, &caps)) {
        ESP_LOGE(TAG, "Failed to read capabilities");
        return false;
    }

    int ic_index = -1;

    for (int i = 0; i < caps.component_count; i++) {
        if (caps.components[i].category == category &&
            caps.components[i].id == id &&
            (!match_addr || caps.components[i].i2c_address == ic_address)) {
            ic_index = i;
            break;
        }
    }

    if (ic_index < 0) {
        if (match_addr) {
            ESP_LOGW(TAG, "IC not found: category=%d, id=%d, address=0x%02X",
                     category, id, ic_address);
        } else {
            ESP_LOGW(TAG, "IC not found: category=%d, id=%d", category, id);
        }
        return false;
    }

    uint8_t old_status = caps.components[ic_index].status;

    uint8_t mem_addr = CAP_OFFSET_COMPONENTS + (ic_index * CAP_BYTES_PER_IC) + 3;

    esp_err_t ret = eeprom_write_bytes(i2c_addr, mem_addr, &new_status, 1);

    if (ret == ESP_OK && eeprom_is_large(i2c_addr)) {
        uint8_t verify;
        ret = eeprom_read_bytes(i2c_addr, mem_addr, &verify, 1);
        if (ret == ESP_OK && verify != new_status) {
            ESP_LOGE(TAG, "EEPROM status read-back verification failed");
            ret = ESP_FAIL;
        }
    }

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
// PUBLIC API FUNCTIONS
// ============================================================================

eeprom_program_state_t eeprom_get_program_state(uint8_t i2c_addr) {
    if (!eeprom_check_init()) {
        return EEPROM_PROGRAM_STATE_BUS_ERROR;
    }

    eeprom_lock();
    eeprom_program_state_t state = eeprom_get_prog_state(i2c_addr);
    eeprom_unlock();

    return state;
}

bool eeprom_is_programmed(uint8_t i2c_addr) {
    return eeprom_get_program_state(i2c_addr) == EEPROM_PROGRAM_STATE_PROGRAMMED;
}

bool eeprom_read_unique_id(uint8_t i2c_addr, uint8_t *unique_id) {
    if (unique_id == NULL) {
        return false;
    }

    if (!eeprom_check_init()) {
        return false;
    }

    eeprom_lock();
    bool ok = eeprom_read_unique_id_unlocked(i2c_addr, unique_id);
    eeprom_unlock();

    return ok;
}

bool eeprom_read_factory_id(uint8_t i2c_addr, eeprom_factory_id_t *identity) {
    if (identity == NULL) {
        return false;
    }
    memset(identity, 0, sizeof(*identity));
    if (!eeprom_valid_address(i2c_addr) || !eeprom_check_init()) {
        return false;
    }
    eeprom_factory_id_t result = {0};
    eeprom_lock();
    bool st = eeprom_is_st(i2c_addr), large = eeprom_is_large(i2c_addr);
    bool ok = large ?
        eeprom_read_security(i2c_addr, st ? EEPROM_M24128_U_UID_START : EEPROM_24CS128_SERIAL_START,
                             result.bytes, 16) == ESP_OK :
        eeprom_read_unique_id_unlocked(i2c_addr, result.bytes);
    eeprom_unlock();
    // ST's identification page aliases upper address bits; validate its header.
    if (!ok || (st && memcmp(result.bytes, "\x20\xe0\x0e\xff", 4))) {
        ESP_LOGW(TAG, "Failed to read qualified factory identity from 0x%02X", i2c_addr);
        return false;
    }
    result.kind = st ? EEPROM_FACTORY_ID_ST_UID128 :
                  large ? EEPROM_FACTORY_ID_SERIAL128 : EEPROM_FACTORY_ID_EUI64;
    result.length = large ? 16 : EEPROM_UNIQUE_ID_SIZE;
    *identity = result;
    return true;
}

bool eeprom_read_capabilities(uint8_t i2c_addr, eeprom_capabilities_t *caps) {
    if (caps == NULL) {
        ESP_LOGE(TAG, "NULL capabilities pointer");
        return false;
    }

    if (!eeprom_check_init()) {
        return false;
    }

    eeprom_lock();
    bool ok = eeprom_read_capabilities_unlocked(i2c_addr, caps);
    eeprom_unlock();

    return ok;
}

bool eeprom_write_capabilities(uint8_t i2c_addr,
                                const eeprom_capabilities_t *caps,
                                bool force) {
    if (caps == NULL) {
        ESP_LOGE(TAG, "NULL capabilities pointer");
        return false;
    }

    if (!eeprom_check_init()) {
        return false;
    }

    eeprom_lock();
    bool ok = eeprom_write_capabilities_unlocked(i2c_addr, caps, force);
    eeprom_unlock();

    return ok;
}

int eeprom_scan_bus(eeprom_capabilities_t *caps, int max_devices) {
    if (caps == NULL || max_devices <= 0) {
        return 0;
    }

    if (!eeprom_check_init()) {
        return 0;
    }

    ESP_LOGI(TAG, "Scanning I2C bus for manifest EEPROMs...");

    eeprom_lock();

    int found = 0;

    for (uint8_t addr = EEPROM_I2C_ADDR_0;
         addr <= EEPROM_I2C_ADDR_7 && found < max_devices;
         addr++) {

        if (eeprom_probe(addr)) {
            ESP_LOGI(TAG, "Found EEPROM at 0x%02X", addr);

            eeprom_program_state_t state = eeprom_get_prog_state(addr);

            if (state == EEPROM_PROGRAM_STATE_PROGRAMMED) {
                if (eeprom_read_capabilities_unlocked(addr, &caps[found])) {
                    bool valid_self = eeprom_validate_self_reference(&caps[found]);
                    if (!valid_self) {
                        ESP_LOGW(TAG, "Device at 0x%02X has no valid self-reference - foreign EEPROM?",
                                 addr);
                    }

                    // The non-addressable 24AA02E64 ignores A2/A1/A0 in the
                    // control byte and the same physical chip ACKs 0x50-0x57.
                    // Its self-descriptor is enough to identify that profile;
                    // after recording it once, continuing would return seven
                    // aliases and could never reveal another usable device in
                    // the electrically-conflicting address block.
                    bool aliases_address_block =
                        caps[found].component_count > 0 &&
                        caps[found].components[0].category == CAT_MEMORY &&
                        caps[found].components[0].id == MEMORY_24AA02E64 &&
                        caps[found].components[0].status == IC_STATUS_INSTALLED;

                    found++;

                    if (aliases_address_block) {
                        ESP_LOGI(TAG, "24AA02E64 aliases 0x50-0x57; stopping after one physical EEPROM");
                        break;
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to read capabilities from 0x%02X", addr);
                }
            } else if (state == EEPROM_PROGRAM_STATE_BUS_ERROR) {
                ESP_LOGW(TAG, "Failed to read magic byte from 0x%02X", addr);
            } else if (state == EEPROM_PROGRAM_STATE_INVALID) {
                ESP_LOGW(TAG, "EEPROM at 0x%02X has a partial/dirty image", addr);
            } else {
                ESP_LOGI(TAG, "EEPROM at 0x%02X is not programmed", addr);
            }
        }
    }

    eeprom_unlock();

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

const eeprom_ic_descriptor_t* eeprom_find_category(const eeprom_capabilities_t *caps,
                                                    uint8_t category) {
    if (caps == NULL || !caps->is_valid) {
        return NULL;
    }

    for (int i = 0; i < caps->component_count; i++) {
        if (caps->components[i].category == category &&
            caps->components[i].status == IC_STATUS_INSTALLED) {
            return &caps->components[i];
        }
    }

    return NULL;
}

bool eeprom_update_ic_status(uint8_t i2c_addr,
                              uint8_t category, uint8_t id,
                              uint8_t new_status) {
    if (!eeprom_check_init()) {
        return false;
    }

    eeprom_lock();
    bool ok = eeprom_update_ic_status_internal(i2c_addr, category, id,
                                               false, 0, new_status);
    eeprom_unlock();

    return ok;
}

bool eeprom_update_ic_status_at(uint8_t i2c_addr,
                                 uint8_t category, uint8_t id,
                                 uint8_t ic_address,
                                 uint8_t new_status) {
    if (!eeprom_check_init()) {
        return false;
    }

    eeprom_lock();
    bool ok = eeprom_update_ic_status_internal(i2c_addr, category, id,
                                               true, ic_address, new_status);
    eeprom_unlock();

    return ok;
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
            case RTC_MAX31328: return "MAX31328";
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
            case GPS_NEO_M10: return "NEO-M10";
            case GPS_NEO_F10N: return "NEO-F10N";
            case GPS_NEO_F10T: return "NEO-F10T";
            case GPS_ZED_F9T: return "ZED-F9T";
            case GPS_MAX_M10S: return "MAX-M10S";
            case GPS_MAX_M10N: return "MAX-M10N";
            case GPS_MAX_F10S: return "MAX-F10S";
            case GPS_ZED_X20P: return "ZED-X20P";
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
            case IMU_ICM45686:  return "ICM-45686";
            case IMU_LSM6DSRX:  return "LSM6DSRX";
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
            case TEMP_MCP9804: return "MCP9804";
            case TEMP_STS35:   return "STS35";
            case TEMP_STS31A:  return "STS31A";
            case TEMP_LM75A_NXP: return "LM75A (NXP)";
            case TEMP_SHT21:   return "SHT21";
            case TEMP_LM35:    return "LM35";
            case TEMP_LM34:    return "LM34";
        }
    }

    if (ic->category == CAT_PRESSURE) {
        switch (ic->id) {
            case PRESSURE_BMP280: return "BMP280";
            case PRESSURE_BMP388: return "BMP388";
            case PRESSURE_MS5611: return "MS5611";
            case PRESSURE_BMP390: return "BMP390";
            case PRESSURE_MS5607: return "MS5607";
            case PRESSURE_BMP390L: return "BMP390L";
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
            case SENSOR_HDC2080: return "HDC2080";
            case SENSOR_MAG_MMC34160PJ: return "MMC34160PJ";
            case SENSOR_HDC2022: return "HDC2022";
            case SENSOR_HIH8121: return "HIH8121";
            case SENSOR_PROX_VCNL4200: return "VCNL4200";
            case SENSOR_TOUCH_AT42QT1070: return "AT42QT1070";
            case SENSOR_LIGHT_NJL7502L: return "NJL7502L";
            case SENSOR_LIGHT_SFH3310: return "SFH 3310";
        }
    }

    if (ic->category == CAT_AUDIO) {
        switch (ic->id) {
            case AUDIO_MAX98357: return "MAX98357";
            case AUDIO_PCM5102:  return "PCM5102";
            case AUDIO_UDA1334:  return "UDA1334";
            case AUDIO_PCM4222:  return "PCM4222";
            case AUDIO_MAX9814:  return "MAX9814";
            case AUDIO_ICS43434: return "ICS-43434";
        }
    }

    if (ic->category == CAT_POWER) {
        switch (ic->id) {
            case POWER_INA219:  return "INA219";
            case POWER_INA3221: return "INA3221";
            case POWER_BQ25895: return "BQ25895";
            case POWER_LTC4150: return "LTC4150";
            case POWER_ADM7150: return "ADM7150";
            case POWER_RT9193:  return "RT9193";
            case POWER_INA260:  return "INA260";
            case POWER_LTC2990: return "LTC2990";
            case POWER_TPS7A20: return "TPS7A20";
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
            case MEMORY_24AA025E64: return "24AA025E64";
            case MEMORY_24CS128:   return "24CS128";
            case MEMORY_M24128_U: return "M24128-U";
            case MEMORY_24CS256:   return "24CS256";
            case MEMORY_24CS512:   return "24CS512";
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
            case BATTERY_CR123A:     return "CR123A";
            case BATTERY_CR2032:     return "CR2032";
            case BATTERY_CR1220:     return "CR1220";
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

    if (first->id != MEMORY_24AA02E64 && first->id != MEMORY_24AA025E64 &&
        !eeprom_memory_id_requires_profile(first->id)) {
        ESP_LOGW(TAG, "Component[0] is not a supported manifest EEPROM (found: %s)",
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
        ESP_LOGI(TAG, "Programmed: %s (timestamp: %" PRIu64 ")", time_str, caps->timestamp);
    } else {
        ESP_LOGI(TAG, "Programmed: [timestamp not set]");
    }

    if (caps->component_count > 0 && caps->components[0].category == CAT_MEMORY &&
        eeprom_memory_id_requires_profile(caps->components[0].id)) {
        ESP_LOGI(TAG, "Board ID: 128-bit serial via eeprom_read_factory_id()");
    } else {
        ESP_LOGI(TAG, "64-bit EUI: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             caps->unique_id[0], caps->unique_id[1], caps->unique_id[2],
             caps->unique_id[3], caps->unique_id[4], caps->unique_id[5],
             caps->unique_id[6], caps->unique_id[7]);
    }

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
