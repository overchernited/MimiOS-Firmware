#ifndef NETWORK_H
#define NETWORK_H

#include <Arduino.h>

String getDeviceID();

void TaskCortex(void *pvParameters);
void ConfigurationPortal();

#endif