/**
 * Host-side unit tests for esp_hardware_discovery, driven through a simulated
 * 24AA02E64/24AA025E64/24CS128 family (see mock_i2c.c). Review regression
 * tests are named for the REVIEW.md finding they verify.
 *
 * The implementation file is #included directly so its static functions
 * (eeprom_write_bytes / eeprom_read_bytes bounds checks) are testable.
 */

#include "mock_i2c.h"
#include "../../src/esp_hardware_discovery.c"

#include <pthread.h>
#include <stdio.h>

static int s_failures = 0;

#define CHECK(cond) do {                                                \
        if (!(cond)) {                                                  \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            s_failures++;                                               \
        }                                                               \
    } while (0)

#define TEST_BEGIN(name) printf("=== %s ===\n", name)

#define TEST_TIMESTAMP 1730000000ULL

// ============================================================================
// FIXTURES
// ============================================================================

/**
 * Standard 5-component test board. components[3] and [4] are deliberately the
 * same part (MCP9808) at different addresses to exercise R-017.
 */
static void make_test_board(eeprom_capabilities_t *caps) {
    memset(caps, 0, sizeof(*caps));
    caps->magic = CAP_MAGIC_PREFERRED;
    caps->project_id = PROJECT_SHEPHERD;
    caps->pcb_id = SHEPHERD_PCB_ROVER;
    caps->revision = 1;
    caps->component_count = 5;
    caps->timestamp = TEST_TIMESTAMP;
    caps->i2c_address = EEPROM_I2C_ADDR_0;

    caps->components[0] = IC_EEPROM_SELF(EEPROM_I2C_ADDR_0);
    caps->components[1] = IC_I2C(CAT_RTC, RTC_MCP79412, 0x6F);
    caps->components[2] = IC_I2C(CAT_IMU, IMU_ICM20948, 0x68);
    caps->components[3] = IC_I2C(CAT_TEMP, TEMP_MCP9808, 0x18);
    caps->components[4] = IC_I2C(CAT_TEMP, TEMP_MCP9808, 0x19);
}

static void use_addressable_manifest_eeprom(eeprom_capabilities_t *caps) {
    caps->components[0] = IC_EEPROM_SELF_24AA025E64(caps->i2c_address);
}

/** Program the standard board onto a present, erased device at 0x50. */
static void program_test_board(void) {
    eeprom_capabilities_t caps;
    make_test_board(&caps);
    mock_reset();
    mock_set_present(EEPROM_I2C_ADDR_0, true);
    CHECK(eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false));
}

// ============================================================================
// R-009: init requirement, and behavior before init
// ============================================================================

static void test_r009_uninitialized_and_init(void) {
    TEST_BEGIN("R-009: functions refuse to run before eeprom_discovery_init");

    mock_reset();
    mock_set_present(EEPROM_I2C_ADDR_0, true);

    eeprom_capabilities_t caps;
    uint8_t uid[EEPROM_UNIQUE_ID_SIZE];
    eeprom_capabilities_t boards[2];

    make_test_board(&caps);

    CHECK(eeprom_is_programmed(EEPROM_I2C_ADDR_0) == false);
    CHECK(eeprom_get_program_state(EEPROM_I2C_ADDR_0) ==
          EEPROM_PROGRAM_STATE_BUS_ERROR);
    CHECK(eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps) == false);
    CHECK(eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false) == false);
    CHECK(eeprom_read_unique_id(EEPROM_I2C_ADDR_0, uid) == false);
    CHECK(eeprom_scan_bus(boards, 2) == 0);
    CHECK(eeprom_update_ic_status(EEPROM_I2C_ADDR_0, CAT_IMU, IMU_ICM20948,
                                  IC_STATUS_FAILED) == false);
    CHECK(mock_write_txn_count() == 0);

    CHECK(eeprom_discovery_init(NULL) == ESP_ERR_INVALID_ARG);
    CHECK(eeprom_set_profile(0x50, EEPROM_PROFILE_24CS128) == ESP_ERR_INVALID_STATE);
    eeprom_factory_id_t identity;
    memset(&identity, 0xAA, sizeof(identity));
    CHECK(!eeprom_read_factory_id(0x50, &identity));
    CHECK(identity.kind == EEPROM_FACTORY_ID_NONE && identity.length == 0);

    i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = 21,
        .scl_io_num = 22,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    CHECK(i2c_new_master_bus(&bus_config, &bus) == ESP_OK);

    CHECK(eeprom_discovery_init(bus) == ESP_OK);
    CHECK(eeprom_discovery_init(bus) == ESP_ERR_INVALID_STATE);
}

// ============================================================================
// R-001 + R-002: page-safe chunking, canonical layout, timestamp round-trip
// ============================================================================

static void test_r001_r002_write_layout(void) {
    TEST_BEGIN("R-001/R-002: page-chunked writes, documented layout, timestamp");

    eeprom_capabilities_t caps;
    make_test_board(&caps);

    mock_reset();
    mock_set_present(EEPROM_I2C_ADDR_0, true);

    CHECK(eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false));

    // R-001: every write transaction fits the 8-byte page buffer and never
    // crosses a page boundary.
    CHECK(mock_write_txn_count() > 0);
    for (int i = 0; i < mock_write_txn_count(); i++) {
        const mock_write_txn_t *txn = mock_write_txn(i);
        CHECK(txn->data_len >= 1 && txn->data_len <= 8);
        CHECK((txn->mem_addr % 8) + txn->data_len <= 8);
    }

    // R-002: byte-exact image per the documented memory map.
    const uint8_t *mem = mock_mem(EEPROM_I2C_ADDR_0);
    CHECK(mem[CAP_OFFSET_MAGIC] == CAP_MAGIC_PREFERRED);
    CHECK(mem[CAP_OFFSET_PROJECT] == PROJECT_SHEPHERD);
    CHECK(mem[CAP_OFFSET_PCB] == SHEPHERD_PCB_ROVER);
    CHECK(mem[CAP_OFFSET_REVISION] == 1);
    CHECK(mem[4] == 0 && mem[5] == 0 && mem[6] == 0);   // reserved
    CHECK(mem[CAP_OFFSET_IC_COUNT] == 5);

    // Timestamp at bytes 8-15, little-endian
    uint64_t stored_ts = 0;
    for (int i = 0; i < 8; i++) {
        stored_ts |= ((uint64_t)mem[CAP_OFFSET_TIMESTAMP + i]) << (8 * i);
    }
    CHECK(stored_ts == TEST_TIMESTAMP);

    // Descriptors start at byte 16
    for (int i = 0; i < caps.component_count; i++) {
        const uint8_t *d = &mem[CAP_OFFSET_COMPONENTS + i * CAP_BYTES_PER_IC];
        CHECK(d[0] == caps.components[i].category);
        CHECK(d[1] == caps.components[i].id);
        CHECK(d[2] == caps.components[i].i2c_address);
        CHECK(d[3] == caps.components[i].status);
    }

    // Round trip: read back and compare
    eeprom_capabilities_t verify;
    CHECK(eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &verify));
    CHECK(verify.is_valid);
    CHECK(verify.magic == caps.magic);
    CHECK(verify.project_id == caps.project_id);
    CHECK(verify.pcb_id == caps.pcb_id);
    CHECK(verify.revision == caps.revision);
    CHECK(verify.component_count == caps.component_count);
    CHECK(verify.timestamp == TEST_TIMESTAMP);
    CHECK(memcmp(verify.components, caps.components,
                 caps.component_count * sizeof(eeprom_ic_descriptor_t)) == 0);
    CHECK(eeprom_validate_self_reference(&verify));
}

// ============================================================================
// R-004: commit ordering and partial-failure behavior
// ============================================================================

static void test_r004_commit_order(void) {
    TEST_BEGIN("R-004: magic committed last; interrupted write leaves no valid magic");

    eeprom_capabilities_t caps;
    make_test_board(&caps);

    // Success path: descriptor pages, then the timestamp page, then the
    // header page carrying the magic byte -- strictly last.
    mock_reset();
    mock_set_present(EEPROM_I2C_ADDR_0, true);
    CHECK(eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false));

    int first_descriptor_txn = -1, timestamp_txn = -1, header_txn = -1;
    for (int i = 0; i < mock_write_txn_count(); i++) {
        const mock_write_txn_t *txn = mock_write_txn(i);
        if (txn->mem_addr >= CAP_OFFSET_COMPONENTS && first_descriptor_txn < 0) {
            first_descriptor_txn = i;
        }
        if (txn->mem_addr == CAP_OFFSET_TIMESTAMP) {
            timestamp_txn = i;
        }
        if (txn->mem_addr == CAP_OFFSET_MAGIC) {
            header_txn = i;
        }
    }
    CHECK(first_descriptor_txn >= 0 && timestamp_txn >= 0 && header_txn >= 0);
    CHECK(first_descriptor_txn < timestamp_txn);
    CHECK(timestamp_txn < header_txn);
    CHECK(header_txn == mock_write_txn_count() - 1);

    // Failure path: descriptor write dies mid-programming -> the magic byte
    // was never committed, so the board still reads as unprogrammed.
    mock_reset();
    mock_set_present(EEPROM_I2C_ADDR_0, true);
    mock_fail_transmit_at_memaddr(CAP_OFFSET_COMPONENTS);
    CHECK(eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false) == false);
    CHECK(mock_mem(EEPROM_I2C_ADDR_0)[CAP_OFFSET_MAGIC] == 0xFF);   // still erased
    CHECK(eeprom_is_programmed(EEPROM_I2C_ADDR_0) == false);
}

// ============================================================================
// R-003: overwrite guard vs bus errors
// ============================================================================

static void test_r003_guard(void) {
    TEST_BEGIN("R-003: bus error during guard check refuses to write");

    program_test_board();

    CHECK(eeprom_get_program_state(EEPROM_I2C_ADDR_0) ==
          EEPROM_PROGRAM_STATE_PROGRAMMED);

    eeprom_capabilities_t caps;
    make_test_board(&caps);

    // Normal guard still works: programmed + force=false -> refused.
    int txns_before = mock_write_txn_count();
    CHECK(eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false) == false);
    CHECK(mock_write_txn_count() == txns_before);

    // Bus error while reading the magic byte must NOT count as "blank".
    mock_fail_receive_at_memaddr(CAP_OFFSET_MAGIC);
    CHECK(eeprom_get_program_state(EEPROM_I2C_ADDR_0) ==
          EEPROM_PROGRAM_STATE_BUS_ERROR);
    mock_fail_receive_at_memaddr(CAP_OFFSET_MAGIC);
    CHECK(eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false) == false);
    CHECK(mock_write_txn_count() == txns_before);

    // Data intact, and force=true still allows a deliberate reprogram.
    CHECK(eeprom_is_programmed(EEPROM_I2C_ADDR_0) == true);
    CHECK(eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, true) == true);
}

static void test_program_state_blank(void) {
    TEST_BEGIN("program state distinguishes blank, partial image, and bus error");

    mock_reset();
    mock_set_present(EEPROM_I2C_ADDR_0, true);
    CHECK(eeprom_get_program_state(EEPROM_I2C_ADDR_0) ==
          EEPROM_PROGRAM_STATE_BLANK);
    CHECK(!eeprom_is_programmed(EEPROM_I2C_ADDR_0));

    // A magic byte that still looks erased is not permission to overwrite
    // descriptor data left by an interrupted or partial operation.
    mock_mem(EEPROM_I2C_ADDR_0)[CAP_OFFSET_COMPONENTS] = CAT_GPS;
    CHECK(eeprom_get_program_state(EEPROM_I2C_ADDR_0) ==
          EEPROM_PROGRAM_STATE_INVALID);
    eeprom_capabilities_t caps;
    make_test_board(&caps);
    int txns_before = mock_write_txn_count();
    CHECK(!eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false));
    CHECK(mock_write_txn_count() == txns_before);

    // 0x00 is the alternate documented blank fill.
    memset(mock_mem(EEPROM_I2C_ADDR_0), 0, EEPROM_24AAXXE64_WRITABLE_SIZE);
    CHECK(eeprom_get_program_state(EEPROM_I2C_ADDR_0) ==
          EEPROM_PROGRAM_STATE_BLANK);
}

static void test_navlistener_catalog_extensions(void) {
    TEST_BEGIN("navlistener catalog extensions have stable names");

    // A programmed EEPROM stores the number, not the symbol, so renumbering any
    // of these silently rewrites what boards in the field claim to be.
    _Static_assert(GPS_MAX_M10S == 11 && GPS_MAX_M10N == 12 &&
                   GPS_MAX_F10S == 13 && GPS_ZED_X20P == 14,
                   "GPS catalog values are written into EEPROMs; append, never renumber");
    _Static_assert(RTC_MAX31328 == 7, "RTC catalog value is written into EEPROMs");
    _Static_assert(IMU_ICM45686 == 7, "IMU catalog value is written into EEPROMs");
    _Static_assert(PRESSURE_MS5607 == 5, "pressure catalog value is written into EEPROMs");
    _Static_assert(SENSOR_MAG_MMC34160PJ == 9, "sensor catalog value is written into EEPROMs");
    _Static_assert(BATTERY_CR2032 == 8 && BATTERY_CR1220 == 9,
                   "battery catalog values are written into EEPROMs");

    eeprom_ic_descriptor_t parts[] = {
        IC_INSTALLED(CAT_GPS, GPS_NEO_M10),
        IC_INSTALLED(CAT_GPS, GPS_NEO_F10N),
        IC_INSTALLED(CAT_GPS, GPS_NEO_F10T),
        IC_INSTALLED(CAT_GPS, GPS_ZED_F9T),
        IC_I2C(CAT_PRESSURE, PRESSURE_BMP390, 0x76),
        IC_I2C(CAT_SENSOR, SENSOR_HDC2080, 0x40),
        IC_GPIO(CAT_POWER, POWER_ADM7150, 38),
        IC_GPIO(CAT_POWER, POWER_RT9193, 21),
        IC_INSTALLED(CAT_BATTERY, BATTERY_CR123A),
        IC_INSTALLED(CAT_GPS, GPS_MAX_M10S),
        IC_INSTALLED(CAT_GPS, GPS_MAX_M10N),
        IC_INSTALLED(CAT_GPS, GPS_MAX_F10S),
        IC_INSTALLED(CAT_GPS, GPS_ZED_X20P),
        IC_I2C(CAT_RTC, RTC_MAX31328, 0x68),
        IC_I2C(CAT_IMU, IMU_ICM45686, 0x69),
        IC_I2C(CAT_SENSOR, SENSOR_MAG_MMC34160PJ, 0x30),
        IC_I2C(CAT_PRESSURE, PRESSURE_MS5607, 0x77),
        IC_INSTALLED(CAT_BATTERY, BATTERY_CR2032),
        IC_INSTALLED(CAT_BATTERY, BATTERY_CR1220),
    };
    const char *names[] = {
        "NEO-M10", "NEO-F10N", "NEO-F10T", "ZED-F9T", "BMP390",
        "HDC2080", "ADM7150", "RT9193", "CR123A",
        "MAX-M10S", "MAX-M10N", "MAX-F10S", "ZED-X20P", "MAX31328",
        "ICM-45686", "MMC34160PJ", "MS5607", "CR2032", "CR1220",
    };

    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        CHECK(strcmp(eeprom_ic_name(&parts[i]), names[i]) == 0);
    }
}

// ============================================================================
// R-006: partial read failure leaves is_valid = false
// ============================================================================

static void test_r006_partial_read(void) {
    TEST_BEGIN("R-006: failed descriptor read -> is_valid stays false");

    program_test_board();

    eeprom_capabilities_t caps;
    mock_fail_receive_at_memaddr(CAP_OFFSET_COMPONENTS);
    CHECK(eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps) == false);
    CHECK(caps.is_valid == false);

    // Struct-gated query helpers must also treat it as invalid.
    CHECK(eeprom_has_ic(&caps, CAT_RTC, RTC_MCP79412) == false);
    CHECK(eeprom_count_category(&caps, CAT_TEMP) == 0);
    CHECK(eeprom_find_category(&caps, CAT_IMU) == NULL);
}

// ============================================================================
// R-012: unique-ID read failure is survivable and leaves zeros
// ============================================================================

static void test_r012_uid_failure(void) {
    TEST_BEGIN("R-012: UID read failure warns but capability read succeeds");

    program_test_board();

    eeprom_capabilities_t caps;
    mock_fail_receive_at_memaddr(EEPROM_UNIQUE_ID_START);
    CHECK(eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps) == true);
    CHECK(caps.is_valid == true);

    uint8_t zeros[EEPROM_UNIQUE_ID_SIZE] = {0};
    CHECK(memcmp(caps.unique_id, zeros, sizeof(zeros)) == 0);
}

// ============================================================================
// R-014: low-level bounds checks (direct static-function tests)
// ============================================================================

static void test_r014_bounds(void) {
    TEST_BEGIN("R-014: out-of-bounds and UID-overlap rejection");

    mock_reset();
    mock_set_present(EEPROM_I2C_ADDR_0, true);

    uint8_t buf[16] = {0};

    int txns_before = mock_write_txn_count();
    CHECK(eeprom_write_bytes(EEPROM_I2C_ADDR_0, 250, buf, 10) == ESP_ERR_INVALID_SIZE);
    CHECK(eeprom_read_bytes(EEPROM_I2C_ADDR_0, 250, buf, 10) == ESP_ERR_INVALID_SIZE);
    CHECK(eeprom_write_bytes(EEPROM_I2C_ADDR_0, 244, buf, 8) == ESP_ERR_INVALID_ARG);
    CHECK(mock_write_txn_count() == txns_before);   // rejected before the bus

    // The reserved footer (240-247) remains writable; the factory UID does not.
    CHECK(eeprom_write_bytes(EEPROM_I2C_ADDR_0, CAP_OFFSET_FOOTER, buf, 8) == ESP_OK);

    // Reading the UID region is legal.
    CHECK(eeprom_read_bytes(EEPROM_I2C_ADDR_0, EEPROM_UNIQUE_ID_START, buf,
                            EEPROM_UNIQUE_ID_SIZE) == ESP_OK);
}

// ============================================================================
// R-017: duplicate (category, id) handling
// ============================================================================

static void test_r017_duplicates(void) {
    TEST_BEGIN("R-017: address-qualified status update targets the right part");

    program_test_board();

    // Two MCP9808s: 0x18 (index 3) and 0x19 (index 4). Target the second.
    CHECK(eeprom_update_ic_status_at(EEPROM_I2C_ADDR_0, CAT_TEMP, TEMP_MCP9808,
                                     0x19, IC_STATUS_FAILED) == true);

    eeprom_capabilities_t caps;
    CHECK(eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps));
    CHECK(caps.components[3].status == IC_STATUS_INSTALLED);
    CHECK(caps.components[4].status == IC_STATUS_FAILED);

    // Legacy call updates the FIRST match (documented behavior).
    CHECK(eeprom_update_ic_status(EEPROM_I2C_ADDR_0, CAT_TEMP, TEMP_MCP9808,
                                  IC_STATUS_DISABLED) == true);
    CHECK(eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps));
    CHECK(caps.components[3].status == IC_STATUS_DISABLED);
    CHECK(caps.components[4].status == IC_STATUS_FAILED);

    // No descriptor at that address -> refused.
    CHECK(eeprom_update_ic_status_at(EEPROM_I2C_ADDR_0, CAT_TEMP, TEMP_MCP9808,
                                     0x20, IC_STATUS_FAILED) == false);
}

// ============================================================================
// R-011: scan reports foreign EEPROMs (kept, with a warning)
// ============================================================================

static void test_r011_scan_foreign(void) {
    TEST_BEGIN("R-011: scan flags devices without a valid self-reference");

    eeprom_capabilities_t programmed;
    make_test_board(&programmed);
    use_addressable_manifest_eeprom(&programmed);
    mock_reset();
    mock_set_present(EEPROM_I2C_ADDR_0, true);
    CHECK(eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &programmed, false));

    // 0x51: foreign EEPROM with a plausible magic but garbage contents.
    mock_set_present(0x51, true);
    mock_mem(0x51)[CAP_OFFSET_MAGIC] = 42;

    // 0x52: present but erased -> "not programmed", not counted.
    mock_set_present(0x52, true);

    eeprom_capabilities_t boards[8];
    int found = eeprom_scan_bus(boards, 8);

    CHECK(found == 2);
    CHECK(boards[0].i2c_address == EEPROM_I2C_ADDR_0);
    CHECK(eeprom_validate_self_reference(&boards[0]) == true);
    CHECK(boards[1].i2c_address == 0x51);
    CHECK(boards[1].magic == 42);
    CHECK(eeprom_validate_self_reference(&boards[1]) == false);
}

// ============================================================================
// Dual EEPROM profile: addressable validation and non-addressable aliases
// ============================================================================

static void test_dual_eeprom_profiles(void) {
    TEST_BEGIN("24AA02E64 and 24AA025E64 profiles validate and scan correctly");

    eeprom_capabilities_t caps;
    make_test_board(&caps);

    // The new addressable part gets its own stable manifest ID and name.
    use_addressable_manifest_eeprom(&caps);
    mock_reset();
    mock_set_present(EEPROM_I2C_ADDR_0, true);
    CHECK(eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false));

    eeprom_capabilities_t verify;
    CHECK(eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &verify));
    CHECK(eeprom_validate_self_reference(&verify));
    CHECK(verify.components[0].id == MEMORY_24AA025E64);
    CHECK(strcmp(eeprom_ic_name(&verify.components[0]), "24AA025E64") == 0);

    // One non-addressable part physically ACKs every address, but a scan must
    // return it once rather than manufacture eight logical boards.
    make_test_board(&caps);
    mock_reset();
    mock_set_nonaddressable_present(true);
    CHECK(eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false));
    for (uint8_t addr = EEPROM_I2C_ADDR_0; addr <= EEPROM_I2C_ADDR_7; addr++) {
        CHECK(eeprom_is_programmed(addr));
    }

    eeprom_capabilities_t boards[8];
    CHECK(eeprom_scan_bus(boards, 8) == 1);
    CHECK(boards[0].i2c_address == EEPROM_I2C_ADDR_0);
    CHECK(boards[0].components[0].id == MEMORY_24AA02E64);
    CHECK(eeprom_validate_self_reference(&boards[0]));
}

// ============================================================================
// R-007: concurrent field updates stay serialized and correct
// ============================================================================

#define R007_ITERATIONS 200

static void *r007_worker_temp(void *arg) {
    (void)arg;
    for (int i = 0; i < R007_ITERATIONS; i++) {
        uint8_t status = (i % 2) ? IC_STATUS_FAILED : IC_STATUS_INSTALLED;
        eeprom_update_ic_status_at(EEPROM_I2C_ADDR_0, CAT_TEMP, TEMP_MCP9808,
                                   0x18, status);
    }
    return NULL;
}

static void *r007_worker_imu(void *arg) {
    (void)arg;
    for (int i = 0; i < R007_ITERATIONS; i++) {
        uint8_t status = (i % 2) ? IC_STATUS_DISABLED : IC_STATUS_INSTALLED;
        eeprom_update_ic_status(EEPROM_I2C_ADDR_0, CAT_IMU, IMU_ICM20948,
                                status);
    }
    return NULL;
}

static void test_r007_concurrency(void) {
    TEST_BEGIN("R-007: two tasks hammering status updates never interleave on the bus");

    program_test_board();

    pthread_t t1, t2;
    pthread_create(&t1, NULL, r007_worker_temp, NULL);
    pthread_create(&t2, NULL, r007_worker_imu, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    CHECK(mock_concurrency_violations() == 0);

    // Both final statuses are exactly what the last iteration (odd) set.
    eeprom_capabilities_t caps;
    CHECK(eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps));
    CHECK(caps.components[3].status == IC_STATUS_FAILED);      // TEMP at 0x18
    CHECK(caps.components[2].status == IC_STATUS_DISABLED);    // IMU

    // Every write issued under contention was still page-safe.
    for (int i = 0; i < mock_write_txn_count(); i++) {
        const mock_write_txn_t *txn = mock_write_txn(i);
        CHECK(txn->data_len >= 1 && txn->data_len <= 8);
        CHECK((txn->mem_addr % 8) + txn->data_len <= 8);
    }
}

// ============================================================================
// R-018: reserved footer is surfaced to callers
// ============================================================================

static void test_r018_footer(void) {
    TEST_BEGIN("R-018: reserved_footer populated from bytes 240-247");

    program_test_board();

    uint8_t expected[8] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18};
    memcpy(&mock_mem(EEPROM_I2C_ADDR_0)[CAP_OFFSET_FOOTER], expected, 8);

    eeprom_capabilities_t caps;
    CHECK(eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps));
    CHECK(memcmp(caps.reserved_footer, expected, 8) == 0);
}

// ============================================================================

static void test_cs128_identity(void) {
    TEST_BEGIN("24CS128 full serial at every strapped security address; 24AA EUI retained");
    _Static_assert(MEMORY_24CS128 == 7 && MEMORY_24AA025E64 == 6 && MEMORY_24AA02E64 == 1,
                   "Memory catalog IDs must stay stable");
    CHECK(eeprom_set_profile(0x58, EEPROM_PROFILE_24CS128) == ESP_ERR_INVALID_ARG);
    CHECK(eeprom_set_profile(0x4F, EEPROM_PROFILE_24CS128) == ESP_ERR_INVALID_ARG);
    CHECK(eeprom_set_profile(0x50, (eeprom_profile_t)99) == ESP_ERR_INVALID_ARG);
    mock_reset();
    for (uint8_t addr = 0x50; addr <= 0x57; addr++) {
        mock_set_cs128_present(addr);
        CHECK(eeprom_set_profile(addr, EEPROM_PROFILE_24CS128) == ESP_OK);
        eeprom_factory_id_t identity;
        CHECK(eeprom_read_factory_id(addr, &identity));
        CHECK(identity.kind == EEPROM_FACTORY_ID_SERIAL128 && identity.length == 16);
        CHECK(memcmp(identity.bytes, mock_serial(addr), 16) == 0);
        const mock_write_txn_t *txn = mock_read_txn(mock_read_txn_count() - 1);
        CHECK(txn && txn->dev_addr == addr + 8 && txn->mem_addr == 0x0800 &&
              txn->address_len == 2 && txn->data_len == 16);
        uint8_t eui[8];
        memset(eui, 0x5A, sizeof(eui));
        CHECK(!eeprom_read_unique_id(addr, eui));
        for (size_t i = 0; i < sizeof(eui); i++) CHECK(eui[i] == 0x5A);
        mock_fail_receive_at_memaddr(0x0800);
        CHECK(!eeprom_read_factory_id(addr, &identity));
        CHECK(identity.kind == EEPROM_FACTORY_ID_NONE && identity.length == 0);
        for (size_t i = 0; i < sizeof(identity.bytes); i++) CHECK(identity.bytes[i] == 0);
        CHECK(eeprom_set_profile(addr, EEPROM_PROFILE_24AAXXE64) == ESP_OK);
    }
    CHECK(mock_write_txn_count() == 0);
    mock_reset();
    mock_set_present(0x50, true);
    const uint8_t eui[8] = { 0, 4, 0xA3, 1, 2, 3, 4, 5 };
    memcpy(mock_mem(0x50) + 248, eui, sizeof(eui));
    eeprom_factory_id_t identity;
    CHECK(eeprom_read_factory_id(0x50, &identity));
    CHECK(identity.kind == EEPROM_FACTORY_ID_EUI64 && identity.length == 8);
    CHECK(memcmp(identity.bytes, eui, 8) == 0);
    for (size_t i = 8; i < sizeof(identity.bytes); i++) CHECK(identity.bytes[i] == 0);
    CHECK(!eeprom_read_factory_id(0x58, &identity));
    CHECK(identity.length == 0);
    CHECK(!eeprom_read_factory_id(0x50, NULL));
}

static void test_cs128_manifest(void) {
    TEST_BEGIN("24CS128 manifest, page boundaries, mixed scan and preserved extra storage");
    mock_reset();
    mock_set_cs128_present(0x50);
    eeprom_capabilities_t caps;
    make_test_board(&caps);
    caps.components[0] = IC_EEPROM_SELF_24CS128(0x50);
    // Never infer the transport from an ACK or the caller's descriptor.
    CHECK(!eeprom_write_capabilities(0x50, &caps, true));
    CHECK(mock_write_txn_count() == 0 && mock_read_txn_count() == 0);
    CHECK(eeprom_set_profile(0x50, EEPROM_PROFILE_24CS128) == ESP_OK);
    caps.component_count = CAP_MAX_COMPONENTS;
    for (int i = 5; i < CAP_MAX_COMPONENTS; i++) {
        caps.components[i] = IC_GPIO(CAT_BUTTON, BUTTON_USER_2, i);
    }
    // Blank checks concern the manifest only; unrelated storage must survive.
    memset(mock_mem(0x50) + 240, 0xC3, MOCK_CS128_SIZE - 240);
    memset(mock_mem(0x50) + 240, 0xFF, 8);
    CHECK(eeprom_get_program_state(0x50) == EEPROM_PROGRAM_STATE_BLANK);
    CHECK(eeprom_write_capabilities(0x50, &caps, false));
    CHECK(mock_write_txn_count() == 6); // 48+64+64+48 descriptors, timestamp, header
    for (int i = 0; i < mock_write_txn_count(); i++) {
        const mock_write_txn_t *txn = mock_write_txn(i);
        CHECK(txn->dev_addr == 0x50 && txn->address_len == 2);
        CHECK(txn->data_len <= 64 && txn->mem_addr / 64 ==
              (txn->mem_addr + txn->data_len - 1) / 64);
    }
    CHECK(mock_write_txn(0)->mem_addr == 16 && mock_write_txn(0)->data_len == 48);
    CHECK(mock_write_txn(5)->mem_addr == 0);
    for (size_t i = 248; i < MOCK_CS128_SIZE; i++) CHECK(mock_mem(0x50)[i] == 0xC3);
    eeprom_capabilities_t verify;
    CHECK(eeprom_read_capabilities(0x50, &verify));
    CHECK(eeprom_validate_self_reference(&verify));
    CHECK(strcmp(eeprom_ic_name(&verify.components[0]), "24CS128") == 0);
    CHECK(verify.timestamp == caps.timestamp && verify.component_count == CAP_MAX_COMPONENTS);
    CHECK(memcmp(verify.components, caps.components, sizeof(caps.components)) == 0);
    for (size_t i = 0; i < sizeof(verify.unique_id); i++) CHECK(verify.unique_id[i] == 0);
    CHECK(eeprom_update_ic_status_at(0x50, CAT_TEMP, TEMP_MCP9808, 0x19, IC_STATUS_FAILED));
    CHECK(eeprom_read_capabilities(0x50, &verify));
    CHECK(verify.components[4].status == IC_STATUS_FAILED);
    // Addressable 24AA at 0x51 and an RTC-like blank image at 0x57 coexist.
    make_test_board(&caps);
    caps.i2c_address = 0x51;
    caps.components[0] = IC_EEPROM_SELF_24AA025E64(0x51);
    mock_set_present(0x51, true);
    mock_set_present(0x57, true);
    CHECK(eeprom_write_capabilities(0x51, &caps, false));
    eeprom_capabilities_t boards[8];
    CHECK(eeprom_scan_bus(boards, 8) == 2);
    CHECK(boards[0].i2c_address == 0x50 && boards[1].i2c_address == 0x51);
    CHECK(boards[0].components[0].id == MEMORY_24CS128);
    CHECK(boards[1].components[0].id == MEMORY_24AA025E64);
    CHECK(eeprom_set_profile(0x50, EEPROM_PROFILE_24AAXXE64) == ESP_OK);
}

static void test_st_manifest(void) {
    TEST_BEGIN("M24128_U manifest, page boundaries, mixed scan and preserved extra storage");
    mock_reset();
    mock_set_st_present(0x50);
    eeprom_capabilities_t caps;
    make_test_board(&caps);
    caps.components[0] = IC_EEPROM_SELF_M24128_U(0x50);
    // Never infer the transport from an ACK or the caller's descriptor.
    CHECK(!eeprom_write_capabilities(0x50, &caps, true));
    CHECK(mock_write_txn_count() == 0 && mock_read_txn_count() == 0);
    CHECK(eeprom_set_profile(0x50, EEPROM_PROFILE_M24128_U) == ESP_OK);
    caps.component_count = CAP_MAX_COMPONENTS;
    for (int i = 5; i < CAP_MAX_COMPONENTS; i++) {
        caps.components[i] = IC_GPIO(CAT_BUTTON, BUTTON_USER_2, i);
    }
    // Blank checks concern the manifest only; unrelated storage must survive.
    memset(mock_mem(0x50) + 240, 0xC3, MOCK_CS128_SIZE - 240);
    memset(mock_mem(0x50) + 240, 0xFF, 8);
    CHECK(eeprom_get_program_state(0x50) == EEPROM_PROGRAM_STATE_BLANK);
    CHECK(eeprom_write_capabilities(0x50, &caps, false));
    CHECK(mock_write_txn_count() == 6); // 48+64+64+48 descriptors, timestamp, header
    for (int i = 0; i < mock_write_txn_count(); i++) {
        const mock_write_txn_t *txn = mock_write_txn(i);
        CHECK(txn->dev_addr == 0x50 && txn->address_len == 2);
        CHECK(txn->data_len <= 64 && txn->mem_addr / 64 ==
              (txn->mem_addr + txn->data_len - 1) / 64);
    }
    CHECK(mock_write_txn(0)->mem_addr == 16 && mock_write_txn(0)->data_len == 48);
    CHECK(mock_write_txn(5)->mem_addr == 0);
    for (size_t i = 248; i < MOCK_CS128_SIZE; i++) CHECK(mock_mem(0x50)[i] == 0xC3);
    eeprom_capabilities_t verify;
    CHECK(eeprom_read_capabilities(0x50, &verify));
    CHECK(eeprom_validate_self_reference(&verify));
    CHECK(strcmp(eeprom_ic_name(&verify.components[0]), "M24128-U") == 0);
    CHECK(verify.timestamp == caps.timestamp && verify.component_count == CAP_MAX_COMPONENTS);
    CHECK(memcmp(verify.components, caps.components, sizeof(caps.components)) == 0);
    for (size_t i = 0; i < sizeof(verify.unique_id); i++) CHECK(verify.unique_id[i] == 0);
    CHECK(eeprom_update_ic_status_at(0x50, CAT_TEMP, TEMP_MCP9808, 0x19, IC_STATUS_FAILED));
    CHECK(eeprom_read_capabilities(0x50, &verify));
    CHECK(verify.components[4].status == IC_STATUS_FAILED);
    // Addressable 24AA at 0x51 and an RTC-like blank image at 0x57 coexist.
    make_test_board(&caps);
    caps.i2c_address = 0x51;
    caps.components[0] = IC_EEPROM_SELF_24AA025E64(0x51);
    mock_set_present(0x51, true);
    mock_set_present(0x57, true);
    CHECK(eeprom_write_capabilities(0x51, &caps, false));
    eeprom_capabilities_t boards[8];
    CHECK(eeprom_scan_bus(boards, 8) == 2);
    CHECK(boards[0].i2c_address == 0x50 && boards[1].i2c_address == 0x51);
    CHECK(boards[0].components[0].id == MEMORY_M24128_U);
    CHECK(boards[1].components[0].id == MEMORY_24AA025E64);
    CHECK(eeprom_set_profile(0x50, EEPROM_PROFILE_24AAXXE64) == ESP_OK);
}

static void test_cs128_guards(void) {
    TEST_BEGIN("24CS128 capacity, protection, dirty images and read/write faults");
    mock_reset();
    mock_set_cs128_present(0x50);
    CHECK(eeprom_set_profile(0x50, EEPROM_PROFILE_24CS128) == ESP_OK);
    uint8_t data[100], verify[100];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
    CHECK(eeprom_write_bytes(0x50, 0x013F, data, sizeof(data)) == ESP_OK);
    CHECK(mock_write_txn_count() == 3); // 1 + 64 + 35
    CHECK(eeprom_read_bytes(0x50, 0x013F, verify, sizeof(verify)) == ESP_OK);
    CHECK(memcmp(data, verify, sizeof(data)) == 0);
    CHECK(eeprom_write_bytes(0x50, 0x3FFF, data, 1) == ESP_OK);
    CHECK(eeprom_read_bytes(0x50, 0x3FFF, verify, 1) == ESP_OK && verify[0] == data[0]);
    int writes = mock_write_txn_count();
    CHECK(eeprom_write_bytes(0x50, 0x3FFF, data, 2) == ESP_ERR_INVALID_SIZE);
    CHECK(eeprom_read_bytes(0x50, 0x4000, verify, 1) == ESP_ERR_INVALID_SIZE);
    CHECK(eeprom_write_bytes(0x50, 0, data, SIZE_MAX) == ESP_ERR_INVALID_SIZE);
    CHECK(eeprom_read_bytes(0x58, 0, verify, 1) == ESP_ERR_INVALID_ARG);
    CHECK(mock_write_txn_count() == writes);
    eeprom_capabilities_t caps;
    make_test_board(&caps);
    CHECK(!eeprom_write_capabilities(0x50, &caps, true));
    caps.components[0] = IC_EEPROM_SELF_24CS128(0x50);
    mock_set_cs128_config(0x50, 0x0201); // Enhanced protection on manifest zone
    CHECK(!eeprom_write_capabilities(0x50, &caps, true));
    CHECK(mock_write_txn_count() == writes);
    mock_set_cs128_config(0x50, 0x0202); // Different zone does not block manifest
    mock_fail_receive_at_memaddr(0x8800);
    CHECK(!eeprom_write_capabilities(0x50, &caps, true));
    CHECK(mock_write_txn_count() == writes);
    CHECK(eeprom_write_capabilities(0x50, &caps, false));
    CHECK(!eeprom_write_capabilities(0x50, &caps, false));
    mock_fail_receive_at_memaddr(CAP_OFFSET_MAGIC);
    CHECK(eeprom_get_program_state(0x50) == EEPROM_PROGRAM_STATE_BUS_ERROR);
    mock_set_cs128_config(0x50, 0);
    mock_set_write_protected(0x50, true);
    CHECK(!eeprom_update_ic_status(0x50, CAT_IMU, IMU_ICM20948, IC_STATUS_FAILED));
    caps.revision++;
    CHECK(!eeprom_write_capabilities(0x50, &caps, true)); // ACK alone isn't success
    mock_reset();
    mock_set_cs128_present(0x50);
    mock_fail_transmit_at_memaddr(64);
    caps.component_count = CAP_MAX_COMPONENTS;
    CHECK(!eeprom_write_capabilities(0x50, &caps, false));
    CHECK(mock_mem(0x50)[0] == 0xFF);
    CHECK(eeprom_get_program_state(0x50) == EEPROM_PROGRAM_STATE_INVALID);
    CHECK(!eeprom_write_capabilities(0x50, &caps, false));
    CHECK(eeprom_set_profile(0x50, EEPROM_PROFILE_24AAXXE64) == ESP_OK);
}

static void test_st_identity_and_protection(void) {
    TEST_BEGIN("M24128-U UID header, address straps, WC, bounds and bus faults");
    mock_reset();
    for (uint8_t addr = 0x50; addr <= 0x57; addr++) {
        mock_set_st_present(addr);
        CHECK(eeprom_set_profile(addr, EEPROM_PROFILE_M24128_U) == ESP_OK);
        eeprom_factory_id_t id;
        CHECK(eeprom_read_factory_id(addr, &id));
        CHECK(id.kind == EEPROM_FACTORY_ID_ST_UID128 && id.length == 16);
        CHECK(!memcmp(id.bytes, mock_serial(addr), 16));
        const mock_write_txn_t *read = mock_read_txn(mock_read_txn_count()-1);
        CHECK(read->dev_addr == addr+8 && read->mem_addr == 0 && read->address_len == 2);
        uint8_t eui[8]; CHECK(!eeprom_read_unique_id(addr, eui));
        mock_serial(addr)[2] ^= 1; // Wrong density must not qualify.
        CHECK(!eeprom_read_factory_id(addr, &id) && id.kind == EEPROM_FACTORY_ID_NONE);
        mock_serial(addr)[2] ^= 1;
        mock_fail_receive_at_memaddr(0);
        CHECK(!eeprom_read_factory_id(addr, &id));
        CHECK(eeprom_set_profile(addr, EEPROM_PROFILE_24AAXXE64) == ESP_OK);
    }
    CHECK(mock_write_txn_count() == 0);
    mock_reset(); mock_set_st_present(0x50);
    CHECK(eeprom_set_profile(0x50, EEPROM_PROFILE_M24128_U) == ESP_OK);
    uint8_t data[100], verify[100];
    memset(data, 0x6a, sizeof data);
    CHECK(eeprom_write_bytes(0x50, 0x13f, data, sizeof data) == ESP_OK);
    CHECK(mock_write_txn_count() == 3);
    CHECK(eeprom_read_bytes(0x50, 0x13f, verify, sizeof verify) == ESP_OK);
    CHECK(!memcmp(data, verify, sizeof data));
    CHECK(eeprom_write_bytes(0x50, 0x3fff, data, 1) == ESP_OK);
    CHECK(eeprom_write_bytes(0x50, 0x3fff, data, 2) == ESP_ERR_INVALID_SIZE);
    CHECK(eeprom_read_bytes(0x50, 0x4000, verify, 1) == ESP_ERR_INVALID_SIZE);
    eeprom_capabilities_t caps; make_test_board(&caps);
    caps.components[0] = IC_EEPROM_SELF_24CS128(0x50);
    CHECK(!eeprom_write_capabilities(0x50, &caps, true));
    caps.components[0] = IC_EEPROM_SELF_M24128_U(0x50);
    CHECK(eeprom_write_capabilities(0x50, &caps, false));
    mock_set_write_protected(0x50, true);
    CHECK(eeprom_write_bytes(0x50, 0, data, 1) == ESP_ERR_INVALID_RESPONSE);
    CHECK(!eeprom_update_ic_status(0x50, CAT_IMU, IMU_ICM20948, IC_STATUS_FAILED));
    caps.revision++; CHECK(!eeprom_write_capabilities(0x50, &caps, true));
    mock_set_write_protected(0x50, false);
    mock_fail_transmit_at_memaddr(16);
    CHECK(!eeprom_write_capabilities(0x50, &caps, true));
    for (int i = 0; i < mock_read_txn_count(); i++) {
        CHECK(mock_read_txn(i)->dev_addr == 0x50); // Never read Microchip config on ST.
    }
    for (int i = 0; i < mock_write_txn_count(); i++) CHECK(mock_write_txn(i)->dev_addr == 0x50);
    CHECK(eeprom_set_profile(0x50, EEPROM_PROFILE_24AAXXE64) == ESP_OK);
}

int main(void) {
    test_r009_uninitialized_and_init();
    test_r001_r002_write_layout();
    test_r004_commit_order();
    test_r003_guard();
    test_program_state_blank();
    test_r006_partial_read();
    test_r012_uid_failure();
    test_r014_bounds();
    test_r017_duplicates();
    test_r011_scan_foreign();
    test_dual_eeprom_profiles();
    test_r007_concurrency();
    test_r018_footer();
    test_navlistener_catalog_extensions();
    test_cs128_identity();
    test_cs128_manifest();
    test_cs128_guards();
    test_st_manifest();
    test_st_identity_and_protection();

    printf("\n%s: %d failure(s)\n", s_failures ? "FAILED" : "ALL TESTS PASSED",
           s_failures);
    return s_failures ? 1 : 0;
}
