/* Control API for the simulated 24AA02E64/24AA025E64 host-test devices */
#ifndef MOCK_I2C_H
#define MOCK_I2C_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MOCK_EEPROM_SIZE    256
#define MOCK_EEPROM_BASE    0x50
#define MOCK_EEPROM_COUNT   8

typedef struct {
    uint8_t dev_addr;   // I2C device address
    uint8_t mem_addr;   // first memory address targeted by the write
    size_t data_len;    // number of data bytes (excluding the address byte)
} mock_write_txn_t;

// Reset device images (erased 0xFF, not present), faults, and the txn log.
void mock_reset(void);

// Make a device at dev_addr respond (or stop responding) on the bus.
void mock_set_present(uint8_t dev_addr, bool present);

// Make one non-addressable 24AA02E64 respond at every address 0x50-0x57.
void mock_set_nonaddressable_present(bool present);

// Direct access to a device's 256-byte memory image.
uint8_t *mock_mem(uint8_t dev_addr);

// Fail the next receive/transmit whose target memory address matches, once.
// Pass -1 to disable.
void mock_fail_receive_at_memaddr(int mem_addr);
void mock_fail_transmit_at_memaddr(int mem_addr);

// Write-transaction log (records every i2c_master_transmit that succeeded
// or failed after the fault check).
int mock_write_txn_count(void);
const mock_write_txn_t *mock_write_txn(int i);

// Number of times two threads were inside the (mock) bus at once. Must stay
// zero if the module under test serializes correctly.
int mock_concurrency_violations(void);

#endif
