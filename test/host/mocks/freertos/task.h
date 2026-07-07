/* Host-test mock of freertos/task.h */
#ifndef MOCK_TASK_H
#define MOCK_TASK_H

#include "freertos/FreeRTOS.h"

void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);

#endif
