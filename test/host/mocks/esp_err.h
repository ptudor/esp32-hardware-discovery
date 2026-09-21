/* Host-test mock of esp_err.h */
#ifndef MOCK_ESP_ERR_H
#define MOCK_ESP_ERR_H

#include <stdio.h>
#include <stdlib.h>

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG    0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_TIMEOUT         0x107

#define ESP_ERROR_CHECK(x) do {                                     \
        esp_err_t err_rc_ = (x);                                    \
        if (err_rc_ != ESP_OK) {                                    \
            fprintf(stderr, "ESP_ERROR_CHECK failed: 0x%x at %s:%d\n", \
                    err_rc_, __FILE__, __LINE__);                   \
            abort();                                                \
        }                                                           \
    } while (0)

#endif
