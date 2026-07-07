/**
 * Simulated 24AA02E64 EEPROMs behind the ESP-IDF i2c_master API, plus the
 * FreeRTOS primitives the module under test needs.
 *
 * The write path faithfully models the device's 8-byte page buffer: a write
 * transaction that carries more bytes than fit in the addressed page wraps
 * around WITHIN the page (the address pointer's low 3 bits increment, the
 * high bits stay frozen), exactly like the real silicon. Code that does not
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
    uint8_t mem[MOCK_EEPROM_SIZE];
    int busy_probes;    // probes to NACK before the write cycle "completes"
} mock_eeprom_t;

static struct mock_i2c_bus s_bus_obj;
static struct mock_i2c_dev s_dev_objs[MOCK_EEPROM_COUNT];
static mock_eeprom_t s_eeproms[MOCK_EEPROM_COUNT];

static int s_fail_receive_memaddr = -1;
static int s_fail_transmit_memaddr = -1;

static mock_write_txn_t s_write_txns[MOCK_MAX_TXNS];
static int s_write_txn_count = 0;

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
    if (addr < MOCK_EEPROM_BASE || addr >= MOCK_EEPROM_BASE + MOCK_EEPROM_COUNT) {
        return NULL;
    }
    return &s_eeproms[addr - MOCK_EEPROM_BASE];
}

// ============================================================================
// CONTROL API
// ============================================================================

void mock_reset(void) {
    for (int i = 0; i < MOCK_EEPROM_COUNT; i++) {
        s_eeproms[i].present = false;
        memset(s_eeproms[i].mem, 0xFF, MOCK_EEPROM_SIZE);   // erased state
        s_eeproms[i].busy_probes = 0;
    }
    s_fail_receive_memaddr = -1;
    s_fail_transmit_memaddr = -1;
    s_write_txn_count = 0;
    atomic_store(&s_concurrency_violations, 0);
}

void mock_set_present(uint8_t dev_addr, bool present) {
    mock_eeprom_t *dev = device_at(dev_addr);
    if (dev != NULL) {
        dev->present = present;
    }
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
        config->device_address >= MOCK_EEPROM_BASE + MOCK_EEPROM_COUNT) {
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

    if (eeprom == NULL || !eeprom->present) {
        result = ESP_ERR_NOT_FOUND;
    } else if (len < 1) {
        result = ESP_ERR_INVALID_ARG;
    } else if (s_fail_transmit_memaddr >= 0 &&
               data[0] == (uint8_t)s_fail_transmit_memaddr) {
        s_fail_transmit_memaddr = -1;
        result = ESP_FAIL;
    } else {
        uint8_t mem_addr = data[0];
        size_t data_len = len - 1;

        if (s_write_txn_count < MOCK_MAX_TXNS) {
            s_write_txns[s_write_txn_count].dev_addr = dev->addr;
            s_write_txns[s_write_txn_count].mem_addr = mem_addr;
            s_write_txns[s_write_txn_count].data_len = data_len;
            s_write_txn_count++;
        }

        // Page-buffer semantics: low 3 address bits increment and wrap, the
        // page (high bits) is frozen for the whole transaction.
        for (size_t i = 0; i < data_len; i++) {
            uint8_t dest = (uint8_t)((mem_addr & 0xF8) | ((mem_addr + i) & 0x07));
            eeprom->mem[dest] = data[1 + i];
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

    if (eeprom == NULL || !eeprom->present) {
        result = ESP_ERR_NOT_FOUND;
    } else if (tx == NULL || tx_len != 1 || rx == NULL || rx_len == 0) {
        result = ESP_ERR_INVALID_ARG;
    } else if (s_fail_receive_memaddr >= 0 &&
               tx[0] == (uint8_t)s_fail_receive_memaddr) {
        s_fail_receive_memaddr = -1;
        result = ESP_FAIL;
    } else {
        // Sequential read: the address counter rolls over the whole array.
        for (size_t i = 0; i < rx_len; i++) {
            rx[i] = eeprom->mem[(tx[0] + i) % MOCK_EEPROM_SIZE];
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
