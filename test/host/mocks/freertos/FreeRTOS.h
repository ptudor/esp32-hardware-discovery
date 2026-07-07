/* Host-test mock of freertos/FreeRTOS.h */
#ifndef MOCK_FREERTOS_H
#define MOCK_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define pdTRUE  1
#define pdFALSE 0

#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))
#define portMAX_DELAY       ((TickType_t)0xFFFFFFFFu)

#endif
