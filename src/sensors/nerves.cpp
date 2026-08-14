#include <Arduino.h>            
#include <freertos/FreeRTOS.h> 
#include <freertos/task.h>    
#include <freertos/queue.h>  
#include <soc/gpio_reg.h>
#include <soc/soc.h>
#include <esp_ota_ops.h>
#include <LittleFS.h>

#include "sensors.h"
#include "../globals.h"

#define VOLTAGE_PIN 4
#define ADC_RESOLUTION 4095
#define ADC_VOLTAGE 3.3

void TaskNerves(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const esp_partition_t* running_part = esp_ota_get_running_partition();
    uint32_t max_app_size = running_part->size;
    float lastTemp = NAN;
    SystemData data;
    
    for (;;) {
        data.free_sram = esp_get_free_heap_size();
        data.free_flash = max_app_size - ESP.getSketchSize();
        data.gpio_mask = REG_READ(GPIO_IN_REG);
        data.voltage = (analogRead(VOLTAGE_PIN) * ADC_VOLTAGE) / ADC_RESOLUTION;

        float temp = temperatureRead();
        if (!isnan(temp)) lastTemp = temp;
        data.temperature = isnan(lastTemp) ? 0.0f : lastTemp;

        xQueueSend(queue_to_synapse, &data, 0);
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(50));
    }
}
