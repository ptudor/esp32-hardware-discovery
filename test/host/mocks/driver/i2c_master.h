/* Host-test mock of driver/i2c_master.h (ESP-IDF >= 5.2 I2C master API) */
#ifndef MOCK_I2C_MASTER_H
#define MOCK_I2C_MASTER_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct mock_i2c_bus *i2c_master_bus_handle_t;
typedef struct mock_i2c_dev *i2c_master_dev_handle_t;

typedef enum {
    I2C_ADDR_BIT_LEN_7 = 0,
    I2C_ADDR_BIT_LEN_10 = 1,
} i2c_addr_bit_len_t;

#define I2C_CLK_SRC_DEFAULT 0

typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
} i2c_device_config_t;

typedef struct {
    int i2c_port;
    int sda_io_num;
    int scl_io_num;
    int clk_source;
    uint8_t glitch_ignore_cnt;
    struct {
        uint32_t enable_internal_pullup: 1;
    } flags;
} i2c_master_bus_config_t;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
                             i2c_master_bus_handle_t *ret_bus);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *ret_dev);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,
                              const uint8_t *data, size_t len,
                              int xfer_timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                      const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len,
                                      int xfer_timeout_ms);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
                           int xfer_timeout_ms);

#endif
