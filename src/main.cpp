//#=#=#=#=#=#=#=#=#=#=#=#===
//$ Mimicro OS             $ 
// % OS ESP32 and variants %
//#                       #
//& Using FreeRTOS, Preact. &
//=#=#=#=#=#=#=#=#=#=#=#=#===


#include <Arduino.h>
#include <cstdarg>
#include <cstdio>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"


//SERVICES
#include "globals.h"
#include "sensors/sensors.h"
#include "network/network.h"

#ifndef LED_PIN
#define LED_PIN 8
#endif

#ifndef CONFIG_BUTTON_PIN
#define CONFIG_BUTTON_PIN 9
#endif

QueueHandle_t queue_to_synapse;
QueueHandle_t queue_to_logs;
QueueHandle_t queue_to_notifications;
SemaphoreHandle_t serial_mutex;
Preferences preferences;

void serial_printf(const char* fmt, ...) {
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    xSemaphoreTake(serial_mutex, portMAX_DELAY);
    Serial.print(buffer);
    Serial.flush();
    xSemaphoreGive(serial_mutex);

    LogMessage logMessage{};
    strlcpy(logMessage.message, buffer, sizeof(logMessage.message));
    xQueueSend(queue_to_logs, &logMessage, 0);
}

void welcome() {
    serial_printf("\nWelcome to Mimicro OS\n");
    serial_printf("Version: 1.0\n");
    serial_printf("Author: overchernited.github.io\n");
    serial_printf("Description: A lightweight operating system for ESP32\n");
    serial_printf("License: MIT\n\n");
}


bool checkConfigModeRequested() {
    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);

    if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
        delay(2000);
        if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
            return true;
        }
    }
    return false;
}

bool shouldEnterConfigMode() {
    preferences.begin("system-config", false);
    bool requested = preferences.getBool("config_mode", false);
    preferences.putBool("config_mode", false);
    preferences.end();
    return requested;
}

bool sendCortexNotification(const char* process, const char* title, const char* message, const char* color) {
    Notification notif;
    strlcpy(notif.process, process, sizeof(notif.process));
    strlcpy(notif.title, title, sizeof(notif.title));
    strlcpy(notif.message, message, sizeof(notif.message));
    strlcpy(notif.color, color, sizeof(notif.color));

    return xQueueSend(queue_to_notifications, &notif, 0) == pdTRUE;
}

void setup() {
    serial_mutex = xSemaphoreCreateMutex();
    queue_to_synapse = xQueueCreate(10, sizeof(SystemData));
    queue_to_logs = xQueueCreate(20, sizeof(LogMessage));
    queue_to_notifications = xQueueCreate(10, sizeof(Notification));

    Serial.begin(115200);

    unsigned long start = millis();
    while (!Serial && (millis() - start < 3000)) {
        delay(10);
    }

    welcome();
    
    
    if (checkConfigModeRequested() || shouldEnterConfigMode()) {
        serial_printf("*/CONFIGURATION MODE*/\n");
        ConfigurationPortal();
    } else {
        serial_printf("*/STARTING SYSTEM*/\n");
    }
    
    xTaskCreate(TaskNerves, "Nerves", 2048, NULL, 1, NULL);
    xTaskCreate(TaskCortex, "Cortex", 6000, NULL, 3, NULL);
}


void loop() {
    vTaskDelete(NULL);
}