/* Host-test mock of freertos/semphr.h (backed by pthread mutexes) */
#ifndef MOCK_SEMPHR_H
#define MOCK_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef struct mock_semaphore *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);

#endif
