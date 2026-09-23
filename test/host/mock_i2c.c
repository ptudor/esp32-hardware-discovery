/**
 * Simulated 24AA02E64/24AA025E64, 24CS128/256/512 and M24128-U behind the
 * ESP-IDF i2c_master API,
 * plus the FreeRTOS primitives the module under test needs. Addressable mock
 * devices respond at one selected address; the non-addressable mock models a
 * single 24AA02E64 aliasing across the complete 0x50-0x57 block.
 *
 * The write path models 8-byte 24AA and 64- or 128-byte two-byte-address page
 * buffers: a write transaction that carries more bytes than fit in the
 * addressed page wraps around WITHIN the page (the address pointer's low 3, 6
 * or 7 bits increment, the high bits stay frozen), exactly like the real silicon. Code that does not
 * chunk its writes correctly therefore corrupts the simulated image the same
 * way it would corrupt a real board.
 *
 * After every successful write the device NACKs the next few probes to model
 * the internal write cycle (Twc), so ACK-polling logic gets exercised.
 */

#include "mock_i2c.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_BUSY_PROBES_AFTER_WRITE 2
#define MOCK_MAX_TXNS 8192

// ============================================================================
// SIMULATED DEVICES
// ============================================================================

struct mock_i2c_bus { int unused; };
struct mock_i2c_dev { uint8_t addr; };

typedef struct {
    bool present;
    bool cs;            // Microchip 24CS128/256/512 register map
    bool st;
    bool write_protected;
    size_t capacity;
    size_t page_size;
    uint8_t mem[MOCK_CS512_SIZE];
    uint8_t serial[16];
    uint8_t config[2];
    int busy_probes;    // probes to NACK before the write cycle "completes"
} mock_eeprom_t;

static struct mock_i2c_bus s_bus_obj;
static struct mock_i2c_dev s_dev_objs[2 * MOCK_EEPROM_COUNT];
static mock_eeprom_t s_eeproms[MOCK_EEPROM_COUNT];
static mock_eeprom_t s_nonaddressable_eeprom;

static int s_fail_receive_memaddr = -1;
static int s_fail_transmit_memaddr = -1;

static mock_write_txn_t s_write_txns[MOCK_MAX_TXNS];
static int s_write_txn_count = 0;
static mock_write_txn_t s_read_txns[MOCK_MAX_TXNS];
static int s_read_txn_count = 0;

// Concurrency canary: counts overlapping entries into the bus functions.
static _Atomic int s_in_use = 0;
static _Atomic int s_concurrency_violations = 0;

static void canary_enter(void) {
    if (atomic_fetch_add(&s_in_use, 1) != 0) {
        atomic_fetch_add(&s_concurrency_violations, 1);
    }
}

static void canary_exit(void) {
    atomic_fetch_sub(&s_in_use, 1);
}

static mock_eeprom_t *device_at(uint16_t addr) {
    if (addr >= 0x58 && addr <= 0x5F) {
        mock_eeprom_t *dev = &s_eeproms[addr - 0x58];
        return (dev->cs || dev->st) ? dev : NULL;
    }
    if (addr < MOCK_EEPROM_BASE || addr >= MOCK_EEPROM_BASE + MOCK_EEPROM_COUNT) {
        return NULL;
    }
    if (s_nonaddressable_eeprom.present) {
        return &s_nonaddressable_eeprom;
    }
    return &s_eeproms[addr - MOCK_EEPROM_BASE];
}

// ============================================================================
// CONTROL API
// ============================================================================

void mock_reset(void) {
    for (int i = 0; i < MOCK_EEPROM_COUNT; i++) {
        memset(&s_eeproms[i], 0, sizeof(s_eeproms[i]));
        memset(s_eeproms[i].mem, 0xFF, sizeof(s_eeproms[i].mem));
        s_eeproms[i].capacity = MOCK_EEPROM_SIZE;
        s_eeproms[i].page_size = 8;
    }
    s_nonaddressable_eeprom.present = false;
    s_nonaddressable_eeprom.capacity = MOCK_EEPROM_SIZE;
    s_nonaddressable_eeprom.page_size = 8;
    memset(s_nonaddressable_eeprom.mem, 0xFF, MOCK_EEPROM_SIZE);
    s_nonaddressable_eeprom.busy_probes = 0;
    s_fail_receive_memaddr = -1;
    s_fail_transmit_memaddr = -1;
    s_write_txn_count = 0;
    s_read_txn_count = 0;
    atomic_store(&s_concurrency_violations, 0);
}

void mock_set_present(uint8_t dev_addr, bool present) {
    mock_eeprom_t *dev = device_at(dev_addr);
    if (dev != NULL) {
        dev->present = present;
    }
}

void mock_set_nonaddressable_present(bool present) {
    s_nonaddressable_eeprom.present = present;
}

void mock_set_cs_present(uint8_t dev_addr, size_t capacity, size_t page_size) {
    mock_eeprom_t *dev = device_at(dev_addr);
    dev->present = true;
    dev->cs = true;
    dev->capacity = capacity;
    dev->page_size = page_size;
    for (size_t i = 0; i < sizeof(dev->serial); i++) {
        dev->serial[i] = (uint8_t)(0xA0 + i + dev_addr - MOCK_EEPROM_BASE);
    }
}

void mock_set_cs128_present(uint8_t dev_addr) {
    mock_set_cs_present(dev_addr, MOCK_CS128_SIZE, 64);
}

void mock_set_st_present(uint8_t addr) {
    mock_set_cs128_present(addr);
    device_at(addr)->cs = false;
    device_at(addr)->st = true;
    memcpy(device_at(addr)->serial, "\x20\xe0\x0e\xff", 4);
}

void mock_set_cs128_config(uint8_t dev_addr, uint16_t config) {
    mock_eeprom_t *dev = device_at(dev_addr);
    dev->config[0] = (uint8_t)(config >> 8);
    dev->config[1] = (uint8_t)config;
}

void mock_set_write_protected(uint8_t dev_addr, bool protected) {
    device_at(dev_addr)->write_protected = protected;
}

uint8_t *mock_serial(uint8_t dev_addr) {
    return device_at(dev_addr)->serial;
}

uint8_t *mock_mem(uint8_t dev_addr) {
    mock_eeprom_t *dev = device_at(dev_addr);
    return (dev != NULL) ? dev->mem : NULL;
}

void mock_fail_receive_at_memaddr(int mem_addr) {
    s_fail_receive_memaddr = mem_addr;
}

void mock_fail_transmit_at_memaddr(int mem_addr) {
    s_fail_transmit_memaddr = mem_addr;
}

int mock_write_txn_count(void) {
    return s_write_txn_count;
}

const mock_write_txn_t *mock_write_txn(int i) {
    if (i < 0 || i >= s_write_txn_count) {
        return NULL;
    }
    return &s_write_txns[i];
}

int mock_concurrency_violations(void) {
    return atomic_load(&s_concurrency_violations);
}

int mock_read_txn_count(void) {
    return s_read_txn_count;
}

const mock_write_txn_t *mock_read_txn(int i) {
    return (i >= 0 && i < s_read_txn_count) ? &s_read_txns[i] : NULL;
}

// ============================================================================
// I2C MASTER API
// ============================================================================

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                             i2c_master_bus_handle_t *ret_bus) {
    if (config == NULL || ret_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_bus = &s_bus_obj;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *ret_dev) {
    if (bus == NULL || config == NULL || ret_dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->device_address < MOCK_EEPROM_BASE ||
        config->device_address >= MOCK_EEPROM_BASE + 2 * MOCK_EEPROM_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    int slot = config->device_address - MOCK_EEPROM_BASE;
    s_dev_objs[slot].addr = (uint8_t)config->device_address;
    *ret_dev = &s_dev_objs[slot];
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
                              const uint8_t *data, size_t len,
                              int xfer_timeout_ms) {
    (void)xfer_timeout_ms;
    canary_enter();

    esp_err_t result = ESP_OK;
    mock_eeprom_t *eeprom = device_at(dev->addr);
    size_t address_len = eeprom != NULL && (eeprom->cs || eeprom->st) ? 2 : 1;
    uint16_t mem_addr = data != NULL && len >= address_len ?
        (address_len == 2 ? ((uint16_t)data[0] << 8) | data[1] : data[0]) : 0;

    if (eeprom == NULL || !eeprom->present) {
        result = ESP_ERR_NOT_FOUND;
    } else if (data == NULL || len < address_len || dev->addr >= 0x58) {
        result = ESP_ERR_INVALID_ARG;
    } else if (eeprom->st && eeprom->write_protected && len > address_len) {
        result = ESP_ERR_INVALID_RESPONSE;
    } else if (s_fail_transmit_memaddr >= 0 &&
               mem_addr == s_fail_transmit_memaddr) {
        s_fail_transmit_memaddr = -1;
        result = ESP_FAIL;
    } else {
        size_t data_len = len - address_len;

        if (s_write_txn_count < MOCK_MAX_TXNS) {
            s_write_txns[s_write_txn_count].dev_addr = dev->addr;
            s_write_txns[s_write_txn_count].mem_addr = mem_addr;
            s_write_txns[s_write_txn_count].data_len = data_len;
            s_write_txns[s_write_txn_count].address_len = address_len;
            s_write_txn_count++;
        }

        // Real page wraparound within the part's 8-, 64- or 128-byte page.
        size_t page_size = eeprom->page_size;
        size_t capacity = eeprom->capacity;
        for (size_t i = 0; i < data_len; i++) {
            size_t dest = ((mem_addr / page_size) * page_size + (mem_addr + i) % page_size) % capacity;
            // Enhanced protection: eight equal zones, SWPn in configuration byte 1.
            bool swp = eeprom->cs && (eeprom->config[0] & 2) &&
                       (eeprom->config[1] & (1U << (dest / (capacity / 8))));
            if (!eeprom->write_protected && !swp) {
                eeprom->mem[dest] = data[address_len + i];
            }
        }

        if (data_len > 0) {
            eeprom->busy_probes = MOCK_BUSY_PROBES_AFTER_WRITE;
        }
    }

    canary_exit();
    return result;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                      const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len,
                                      int xfer_timeout_ms) {
    (void)xfer_timeout_ms;
    canary_enter();

    esp_err_t result = ESP_OK;
    mock_eeprom_t *eeprom = device_at(dev->addr);
    size_t address_len = eeprom != NULL && (eeprom->cs || eeprom->st) ? 2 : 1;
    uint16_t mem_addr = tx != NULL && tx_len == address_len ?
        (address_len == 2 ? ((uint16_t)tx[0] << 8) | tx[1] : tx[0]) : 0;

    if (eeprom == NULL || !eeprom->present) {
        result = ESP_ERR_NOT_FOUND;
    } else if (tx == NULL || tx_len != address_len || rx == NULL || rx_len == 0) {
        result = ESP_ERR_INVALID_ARG;
    } else if (s_fail_receive_memaddr >= 0 &&
               mem_addr == s_fail_receive_memaddr) {
        s_fail_receive_memaddr = -1;
        result = ESP_FAIL;
    } else {
        if (s_read_txn_count < MOCK_MAX_TXNS) {
            s_read_txns[s_read_txn_count++] = (mock_write_txn_t){
                dev->addr, mem_addr, rx_len, tx_len
            };
        }
        if (dev->addr >= 0x58) {
            if (((eeprom->cs && mem_addr == 0x0800) ||
                 (eeprom->st && (mem_addr & 0x3f) == 0)) && rx_len == 16) {
                memcpy(rx, eeprom->serial, rx_len);
            } else if (eeprom->cs && mem_addr == 0x8800 && rx_len == 2) {
                memcpy(rx, eeprom->config, rx_len);
            } else {
                result = ESP_ERR_INVALID_ARG;
            }
        } else {
            // Sequential reads roll over the entire array, not the page.
            for (size_t i = 0; i < rx_len; i++) {
                rx[i] = eeprom->mem[(mem_addr + i) % eeprom->capacity];
            }
        }
    }

    canary_exit();
    return result;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
                           int xfer_timeout_ms) {
    (void)xfer_timeout_ms;
    canary_enter();

    esp_err_t result = ESP_OK;
    mock_eeprom_t *eeprom = device_at(address);

    if (bus == NULL || eeprom == NULL || !eeprom->present) {
        result = ESP_ERR_NOT_FOUND;
    } else if (eeprom->busy_probes > 0) {
        eeprom->busy_probes--;                  // still in its write cycle
        result = ESP_ERR_NOT_FOUND;
    }

    canary_exit();
    return result;
}

// ============================================================================
// FREERTOS PRIMITIVES
// ============================================================================

static _Atomic uint32_t s_ticks = 0;

TickType_t xTaskGetTickCount(void) {
    return (TickType_t)atomic_fetch_add(&s_ticks, 1);
}

void vTaskDelay(TickType_t ticks) {
    atomic_fetch_add(&s_ticks, ticks);
}

struct mock_semaphore {
    pthread_mutex_t mutex;
};

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    struct mock_semaphore *sem = malloc(sizeof(*sem));
    if (sem == NULL) {
        return NULL;
    }
    pthread_mutex_init(&sem->mutex, NULL);
    return sem;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout) {
    (void)timeout;
    return (pthread_mutex_lock(&sem->mutex) == 0) ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    return (pthread_mutex_unlock(&sem->mutex) == 0) ? pdTRUE : pdFALSE;
}
