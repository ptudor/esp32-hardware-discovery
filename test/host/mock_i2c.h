/* Control API for the simulated 24AA, 24CS128/256/512 and M24128-U devices */
#ifndef MOCK_I2C_H
#define MOCK_I2C_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MOCK_EEPROM_SIZE    256
#define MOCK_EEPROM_BASE    0x50
#define MOCK_EEPROM_COUNT   8
#define MOCK_CS128_SIZE     16384
#define MOCK_CS256_SIZE     32768
#define MOCK_CS512_SIZE     65536
#define MOCK_MANUFACTURER_ADDR 0x7C

typedef struct {
    uint8_t dev_addr;   // I2C device address
    uint16_t mem_addr;  // first memory address targeted by the transaction
    size_t data_len;    // number of data bytes (excluding the word address)
    size_t address_len;
} mock_write_txn_t;

// Reset device images (erased 0xFF, not present), faults, and the txn log.
void mock_reset(void);

// Make a device at dev_addr respond (or stop responding) on the bus.
void mock_set_present(uint8_t dev_addr, bool present);
void mock_set_st_present(uint8_t dev_addr);
void mock_set_cs128_present(uint8_t dev_addr);
// A Microchip 24CS part: CS128 (16384/64), CS256 (32768/64) or CS512 (65536/128).
void mock_set_cs_present(uint8_t dev_addr, size_t capacity, size_t page_size);
void mock_set_cs128_config(uint8_t dev_addr, uint16_t config);
// Replace a 24CS part's Manufacturer ID (default by capacity: 00D0B8/C0/C8).
void mock_set_manufacturer_id(uint8_t dev_addr, const uint8_t id[3]);
// Time out every probe of the Manufacturer ID address, as a stuck bus would.
void mock_set_manufacturer_bus_fault(bool fault);
void mock_set_write_protected(uint8_t dev_addr, bool protected);
uint8_t *mock_serial(uint8_t dev_addr);

// Make one non-addressable 24AA02E64 respond at every address 0x50-0x57.
void mock_set_nonaddressable_present(bool present);

// Direct access to a device's memory image (256 bytes on 24AA, else its capacity).
uint8_t *mock_mem(uint8_t dev_addr);

// Fail the next receive/transmit whose target memory address matches, once.
// Pass -1 to disable.
void mock_fail_receive_at_memaddr(int mem_addr);
void mock_fail_transmit_at_memaddr(int mem_addr);

// Write-transaction log (records every i2c_master_transmit that succeeded
// or failed after the fault check).
int mock_write_txn_count(void);
const mock_write_txn_t *mock_write_txn(int i);
int mock_read_txn_count(void);
const mock_write_txn_t *mock_read_txn(int i);

// Number of times two threads were inside the (mock) bus at once. Must stay
// zero if the module under test serializes correctly.
int mock_concurrency_violations(void);

#endif
