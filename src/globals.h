#ifndef GLOBALS_H 
#define GLOBALS_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

struct SystemData {
    uint32_t gpio_mask;
    uint32_t free_sram;
    uint32_t free_flash;
    float voltage;
    float temperature;
};

struct LogMessage {
    char message[256];
};

struct Notification {
    char process[32];
    char title[64];
    char message[256];
    char color[16];
};

extern QueueHandle_t queue_to_synapse;
extern QueueHandle_t queue_to_logs;
extern QueueHandle_t queue_to_notifications;
extern SemaphoreHandle_t serial_mutex;

void serial_printf(const char* fmt, ...);
bool sendCortexNotification(const char* process, const char* title, const char* message, const char* color);

#endif