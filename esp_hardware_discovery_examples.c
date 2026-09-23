/**
 * @file esp_hardware_discovery_examples.c
 * @brief Usage examples for 24AA and 24CS-family capability discovery
 *
 * Reference only - this file is not compiled as part of the component.
 * Copy the pieces you need into your application (it defines app_main,
 * so it cannot be added to the component's SRCS as-is).
 */

#include "esp_hardware_discovery.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "EEPROM_EXAMPLES";

// Call after eeprom_discovery_init(), before any bus scan or provisioning.
// Assembly information selects the fitted EEPROM; an ACK cannot identify it.
bool example_select_eeprom_and_read_board_id(bool fitted_cs128,
                                              eeprom_factory_id_t *board_id) {
    eeprom_profile_t profile = fitted_cs128 ? EEPROM_PROFILE_24CS128 :
                                            EEPROM_PROFILE_24AAXXE64;
    if (eeprom_set_profile(0x50, profile) != ESP_OK) {
        return false;
    }
    // For manufacturing, pair the profile with IC_EEPROM_SELF_24CS128(0x50)
    // or IC_EEPROM_SELF_24AA025E64(0x50) in components[0].
    // CS128's full 16 bytes are the board ID; 24AA returns an 8-byte EUI.
    // Persist/compare kind, length, and every returned byte; handle failure.
    return eeprom_read_factory_id(0x50, board_id);
}

// ============================================================================
// EXAMPLE 1: Manufacturing - Program a Shepherd Rover Board
// ============================================================================

void example_program_shepherd_rover(void) {
    ESP_LOGI(TAG, "=== EXAMPLE: Program Shepherd Rover Board ===");
    
    eeprom_capabilities_t caps = {
        .magic = CAP_MAGIC_PREFERRED,       // Production magic = 79
        .project_id = PROJECT_SHEPHERD,
        .pcb_id = SHEPHERD_PCB_ROVER,
        .revision = 1,
        .reserved = {0, 0, 0},
        .component_count = 7,
        .i2c_address = EEPROM_I2C_ADDR_0
    };

    // Define components on this board
    // CRITICAL CONVENTION: components[0] is ALWAYS the EEPROM itself
    // MCP79412 also exposes EEPROM/EUI at fixed address 0x57, so this board
    // requires the addressable manifest EEPROM strapped to 0x50.
    caps.components[0] = IC_EEPROM_SELF_24AA025E64(EEPROM_I2C_ADDR_0);
    caps.components[1] = IC_I2C(CAT_RTC, RTC_MCP79412, 0x6F);
    caps.components[2] = IC_INSTALLED(CAT_GPS, GPS_ZED_F9P);           // UART
    caps.components[3] = IC_I2C(CAT_IMU, IMU_ICM20948, 0x68);
    caps.components[4] = IC_I2C(CAT_CRYPTO, CRYPTO_ATECC608C, 0x60);
    caps.components[5] = IC_I2C(CAT_TEMP, TEMP_MCP9808, 0x18);
    caps.components[6] = IC_INSTALLED(CAT_LED, LED_WS2812B);           // GPIO

    // Write to EEPROM
    if (eeprom_write_capabilities(EEPROM_I2C_ADDR_0, &caps, false)) {
        ESP_LOGI(TAG, "✓ Board programmed successfully");

        // Verify
        eeprom_capabilities_t verify;
        if (eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &verify)) {
            eeprom_print_capabilities(&verify);

            if (!eeprom_validate_self_reference(&verify)) {
                ESP_LOGW(TAG, "Self-reference validation failed");
            }
        }
    } else {
        ESP_LOGE(TAG, "✗ Failed to program board");
    }
}

// ============================================================================
// EXAMPLE 2: Runtime Discovery - Initialize Hardware from EEPROM
// ============================================================================

void example_runtime_discovery(void) {
    ESP_LOGI(TAG, "=== EXAMPLE: Runtime Hardware Discovery ===");
    
    eeprom_capabilities_t caps;
    
    if (!eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps)) {
        ESP_LOGE(TAG, "No EEPROM found or not programmed");
        return;
    }
    
    ESP_LOGI(TAG, "Discovered: %s %s PCB v%d.%d",
             eeprom_project_name(caps.project_id),
             caps.pcb_id == SHEPHERD_PCB_ROVER ? "Rover" : "Unknown",
             caps.pcb_id, caps.revision);
    
    // Initialize components based on discovery
    // Start at 1: components[0] is the EEPROM itself
    for (int i = 1; i < caps.component_count; i++) {
        eeprom_ic_descriptor_t *ic = &caps.components[i];
        
        // Skip if not installed
        if (ic->status != IC_STATUS_INSTALLED) {
            ESP_LOGW(TAG, "Skipping %s: %s", 
                     eeprom_ic_name(ic), eeprom_status_name(ic->status));
            continue;
        }
        
        ESP_LOGI(TAG, "Initializing: %s at 0x%02X", 
                 eeprom_ic_name(ic), ic->i2c_address);
        
        switch (ic->category) {
            case CAT_RTC:
                if (ic->id == RTC_MCP79412) {
                    // init_mcp79412(ic->i2c_address);
                    ESP_LOGI(TAG, "  → RTC initialized");
                }
                break;
                
            case CAT_GPS:
                if (ic->id == GPS_ZED_F9P) {
                    // init_zed_f9p();
                    ESP_LOGI(TAG, "  → GPS initialized");
                }
                break;
                
            case CAT_IMU:
                if (ic->id == IMU_ICM20948) {
                    // init_icm20948(ic->i2c_address);
                    ESP_LOGI(TAG, "  → IMU initialized");
                }
                break;
                
            case CAT_CRYPTO:
                if (ic->id == CRYPTO_ATECC608C) {
                    // init_atecc608c(ic->i2c_address);
                    ESP_LOGI(TAG, "  → Crypto initialized");
                } else if (ic->id == CRYPTO_ATSHA204A) {
                    // Old stock! Different API
                    // init_atsha204a(ic->i2c_address);
                    ESP_LOGI(TAG, "  → Legacy crypto initialized");
                }
                break;
                
            case CAT_TEMP:
                // init_temp_sensor(ic->i2c_address);
                ESP_LOGI(TAG, "  → Temperature sensor initialized");
                break;
                
            case CAT_LED:
                // init_led_strip();
                ESP_LOGI(TAG, "  → LED strip initialized");
                break;
        }
    }
}

// ============================================================================
// EXAMPLE 3: Field Update - Mark Component Failed
// ============================================================================

void example_field_update_failed_imu(void) {
    ESP_LOGI(TAG, "=== EXAMPLE: Field Update (IMU Failed) ===");
    
    // In the field, IMU stopped responding
    // Mark it as failed without reprogramming entire EEPROM
    
    if (eeprom_update_ic_status(EEPROM_I2C_ADDR_0, 
                                 CAT_IMU, IMU_ICM20948, 
                                 IC_STATUS_FAILED)) {
        ESP_LOGI(TAG, "✓ IMU marked as failed");
        
        // System will now skip IMU initialization on next boot
        ESP_LOGI(TAG, "Next boot will skip IMU initialization");
    } else {
        ESP_LOGE(TAG, "✗ Failed to update status");
    }
}

// ============================================================================
// EXAMPLE 4: Check for Specific Hardware
// ============================================================================

void example_check_hardware(void) {
    ESP_LOGI(TAG, "=== EXAMPLE: Check for Specific Hardware ===");
    
    eeprom_capabilities_t caps;
    if (!eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps)) {
        return;
    }
    
    // Check for RTK GPS
    if (eeprom_has_ic(&caps, CAT_GPS, GPS_ZED_F9P)) {
        ESP_LOGI(TAG, "✓ Board has RTK GPS (ZED-F9P)");
        // Enable RTK features
    } else {
        ESP_LOGW(TAG, "✗ No RTK GPS found");
        // Disable RTK features
    }
    
    // Check for crypto
    if (eeprom_has_ic(&caps, CAT_CRYPTO, CRYPTO_ATECC608C)) {
        ESP_LOGI(TAG, "✓ Board has ATECC608C - secure boot available");
    } else if (eeprom_has_ic(&caps, CAT_CRYPTO, CRYPTO_ATSHA204A)) {
        ESP_LOGI(TAG, "⚠ Board has legacy ATSHA204A - limited features");
    } else {
        ESP_LOGW(TAG, "✗ No crypto chip found");
    }
    
    // Count temperature sensors
    int temp_count = eeprom_count_category(&caps, CAT_TEMP);
    ESP_LOGI(TAG, "Found %d temperature sensor(s)", temp_count);
}

// ============================================================================
// EXAMPLE 5: Scan Bus for Multiple Boards
// ============================================================================

void example_scan_multiple_boards(void) {
    ESP_LOGI(TAG, "=== EXAMPLE: Scan for Multiple Boards ===");
    
    eeprom_capabilities_t boards[8];
    int found = eeprom_scan_bus(boards, 8);
    
    ESP_LOGI(TAG, "Found %d programmed board(s):", found);
    
    for (int i = 0; i < found; i++) {
        ESP_LOGI(TAG, "\nBoard %d at 0x%02X:", i + 1, boards[i].i2c_address);
        ESP_LOGI(TAG, "  Project: %s", eeprom_project_name(boards[i].project_id));
        ESP_LOGI(TAG, "  PCB: %d", boards[i].pcb_id);
        ESP_LOGI(TAG, "  Revision: %d", boards[i].revision);
        ESP_LOGI(TAG, "  Components: %d", boards[i].component_count);
        
        // Print unique ID
        ESP_LOGI(TAG, "  UID: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                 boards[i].unique_id[0], boards[i].unique_id[1],
                 boards[i].unique_id[2], boards[i].unique_id[3],
                 boards[i].unique_id[4], boards[i].unique_id[5],
                 boards[i].unique_id[6], boards[i].unique_id[7]);
    }
}

// ============================================================================
// EXAMPLE 6: Dual Device Support (Two RTCs)
// ============================================================================

void example_dual_rtc_board(void) {
    ESP_LOGI(TAG, "=== EXAMPLE: Board with Two RTCs ===");
    
    eeprom_capabilities_t caps = {
        .magic = CAP_MAGIC_PREFERRED,
        .project_id = PROJECT_GNSS,
        .pcb_id = GNSS_PCB_MAIN,
        .revision = 2,
        .component_count = 4
    };

    // CRITICAL CONVENTION: components[0] is ALWAYS the EEPROM itself
    // MCP79412's EEPROM/EUI occupies 0x57; use the addressable variant.
    caps.components[0] = IC_EEPROM_SELF_24AA025E64(EEPROM_I2C_ADDR_0);
    // Two different RTCs at different addresses
    caps.components[1] = IC_I2C(CAT_RTC, RTC_MCP79412, 0x6F);  // Primary
    caps.components[2] = IC_I2C(CAT_RTC, RTC_DS3231, 0x68);    // Backup
    caps.components[3] = IC_I2C(CAT_GPS, GPS_ZED_F9P, 0x42);
    
    ESP_LOGI(TAG, "Board has dual RTC configuration:");
    ESP_LOGI(TAG, "  Primary: MCP79412 at 0x6F");
    ESP_LOGI(TAG, "  Backup:  DS3231 at 0x68");
}

// ============================================================================
// EXAMPLE 7: Handle Old Stock Parts
// ============================================================================

void example_old_stock_crypto(void) {
    ESP_LOGI(TAG, "=== EXAMPLE: Handle Old Stock Parts ===");
    
    eeprom_capabilities_t caps;
    if (!eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps)) {
        return;
    }
    
    // Find crypto chip
    const eeprom_ic_descriptor_t *crypto = eeprom_find_category(&caps, CAT_CRYPTO);
    
    if (crypto == NULL) {
        ESP_LOGW(TAG, "No crypto chip found");
        return;
    }
    
    // Handle different crypto chips (old stock)
    if (crypto->id == CRYPTO_ATECC608C) {
        ESP_LOGI(TAG, "Using ATECC608C (current generation)");
        // init_atecc608c(crypto->i2c_address);
    } else if (crypto->id == CRYPTO_ATSHA204A) {
        ESP_LOGI(TAG, "Using ATSHA204A (legacy - old stock)");
        // Different initialization for old stock
        // init_atsha204a(crypto->i2c_address);
        ESP_LOGW(TAG, "Some features not available with legacy crypto");
    }
}

// ============================================================================
// EXAMPLE 8: Board Variant Detection
// ============================================================================

void example_board_variant_detection(void) {
    ESP_LOGI(TAG, "=== EXAMPLE: Detect Board Variant ===");
    
    eeprom_capabilities_t caps;
    if (!eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps)) {
        return;
    }
    
    if (caps.project_id == PROJECT_SHEPHERD) {
        switch (caps.pcb_id) {
            case SHEPHERD_PCB_ROVER:
                ESP_LOGI(TAG, "Detected: Shepherd Rover");
                ESP_LOGI(TAG, "  → Mobile unit with GPS and motors");
                break;
                
            case SHEPHERD_PCB_BASE:
                ESP_LOGI(TAG, "Detected: Shepherd Base Station");
                ESP_LOGI(TAG, "  → Fixed RTK base station");
                break;
                
            case SHEPHERD_PCB_CORNER:
                ESP_LOGI(TAG, "Detected: Shepherd Corner Unit");
                ESP_LOGI(TAG, "  → UWB anchor point");
                break;
                
            case SHEPHERD_PCB_LED_BUTTON:
                ESP_LOGI(TAG, "Detected: Shepherd LED/Button Controller");
                ESP_LOGI(TAG, "  → Night ops interface");
                break;
                
            default:
                ESP_LOGW(TAG, "Unknown Shepherd PCB ID: %d", caps.pcb_id);
        }
    }
}

// ============================================================================
// EXAMPLE 9: Complete Initialization Function
// ============================================================================

esp_err_t initialize_hardware_from_eeprom(void) {
    ESP_LOGI(TAG, "=== Hardware Initialization from EEPROM ===");
    
    // Initialize I2C first (assumed already done)
    
    // Scan for EEPROM
    if (!eeprom_is_programmed(EEPROM_I2C_ADDR_0)) {
        ESP_LOGE(TAG, "EEPROM not programmed - cannot initialize hardware");
        return ESP_ERR_NOT_FOUND;
    }
    
    // Read capabilities
    eeprom_capabilities_t caps;
    if (!eeprom_read_capabilities(EEPROM_I2C_ADDR_0, &caps)) {
        ESP_LOGE(TAG, "Failed to read capabilities");
        return ESP_FAIL;
    }
    
    // Print discovered hardware
    eeprom_print_capabilities(&caps);
    
    // Initialize each component
    int initialized = 0;
    int failed = 0;
    
    for (int i = 0; i < caps.component_count; i++) {
        eeprom_ic_descriptor_t *ic = &caps.components[i];
        
        if (ic->status != IC_STATUS_INSTALLED) {
            continue;  // Skip non-installed components
        }
        
        bool init_ok = false;
        
        // Add your actual initialization calls here
        switch (ic->category) {
            case CAT_RTC:
                // init_ok = init_rtc(ic->id, ic->i2c_address);
                init_ok = true;  // Placeholder
                break;
            case CAT_GPS:
                // init_ok = init_gps(ic->id);
                init_ok = true;  // Placeholder
                break;
            case CAT_IMU:
                // init_ok = init_imu(ic->id, ic->i2c_address);
                init_ok = true;  // Placeholder
                break;
            // ... etc
        }
        
        if (init_ok) {
            initialized++;
            ESP_LOGI(TAG, "✓ %s initialized", eeprom_ic_name(ic));
        } else {
            failed++;
            ESP_LOGE(TAG, "✗ %s failed to initialize", eeprom_ic_name(ic));
            
            // Mark as failed in EEPROM
            eeprom_update_ic_status(caps.i2c_address, 
                                    ic->category, ic->id, 
                                    IC_STATUS_FAILED);
        }
    }
    
    ESP_LOGI(TAG, "Initialization complete: %d OK, %d failed", 
             initialized, failed);
    
    return (failed == 0) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

// ============================================================================
// Main Function (for testing)
// ============================================================================

#define EXAMPLE_I2C_PORT    (-1)    // Auto-select
#define EXAMPLE_SDA_PIN     21
#define EXAMPLE_SCL_PIN     22

void app_main(void) {
    ESP_LOGI(TAG, "EEPROM Capability Discovery Examples");
    ESP_LOGI(TAG, "======================================");

    // Create the I2C master bus and initialize the discovery module
    i2c_master_bus_config_t bus_config = {
        .i2c_port = EXAMPLE_I2C_PORT,
        .sda_io_num = EXAMPLE_SDA_PIN,
        .scl_io_num = EXAMPLE_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));
    ESP_ERROR_CHECK(eeprom_discovery_init(bus));

    // Run examples
    example_program_shepherd_rover();
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    example_runtime_discovery();
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    example_check_hardware();
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    example_scan_multiple_boards();
}
