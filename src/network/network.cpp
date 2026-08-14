#include <Arduino.h>            
#include <freertos/FreeRTOS.h> 
#include <freertos/task.h>    
#include <freertos/queue.h>  
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Update.h>

#include "network.h"
#include "../globals.h"

extern Preferences preferences;

#define WS_SERVER_PORT 3000

#ifndef LED_PIN
#define LED_PIN 8
#endif

#ifndef OS_NAME
#define OS_NAME "Mimi OS"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.1.0"
#endif

WebServer server(80);

static WebSocketsClient webSocket;
static bool wsConnected = false;
static String deviceId = "";

typedef void (*CmdHandler)(JsonDocument &doc);

struct Command {
    const char *name;
    CmdHandler handler;
};


static void cmdGpio(JsonDocument &doc) {
    uint32_t pin = doc["pin"] | 0;
    uint8_t val = doc["value"] | 0;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, val);
    serial_printf("[WS] GPIO %u -> %u\n", pin, val);
}

static void cmdReboot(JsonDocument &doc) {
    bool configMode = false;

    if (doc["mode"].is<const char*>()) {
        const char* mode = doc["mode"].as<const char*>();
        configMode = String(mode).equalsIgnoreCase("config");
    }

    if (configMode) {
        preferences.begin("system-config", false);
        preferences.putBool("config_mode", true);
        preferences.end();
        serial_printf("[WS] Rebooting into configuration mode...\n");
    } else {
        serial_printf("[WS] Reboot...\n");
    }

    ESP.restart();
}


static void loadUserConfig(String &device_name, String &username, String &password);

static void sendRegister() {
    String device_name;
    String username;
    String password;
    loadUserConfig(device_name, username, password);

    JsonDocument reg;
    reg["type"] = "register";
    reg["device_id"] = deviceId;
    reg["device_name"] = device_name;

    String body;
    serializeJson(reg, body);
    webSocket.sendTXT(body);
    serial_printf("[WS] Registered as '%s' (%s)\n", device_name.c_str(), deviceId.c_str());
}

static void cmdPing(JsonDocument &doc) {
    serial_printf("[WS] Pong...\n");

    JsonDocument response;
    response["type"] = "pong";
    response["device_id"] = deviceId;
    String body;
    serializeJson(response, body);
    webSocket.sendTXT(body);

    for (int i = 0; i < 3; ++i) {
        digitalWrite(LED_PIN, LOW);
        vTaskDelay(pdMS_TO_TICKS(50));
        digitalWrite(LED_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}


static bool validateStoredCredentials(const String &username, const String &password) {
    preferences.begin("user-config", true);
    String storedUsername = preferences.getString("username", "admin");
    String storedPassword = preferences.getString("userpass", "12345678");
    preferences.end();

    return storedUsername.equals(username) && storedPassword.equals(password);
}

static void cmdValidateAuth(JsonDocument &doc) {
    const char* username = doc["username"] | "";
    const char* password = doc["password"] | "";

    JsonDocument response;
    response["cmd"] = "validate_auth_result";
    response["device_id"] = deviceId;
    response["valid"] = validateStoredCredentials(String(username), String(password));

    String body;
    serializeJson(response, body);
    webSocket.sendTXT(body);
    serial_printf("[WS] Auth %s\n", response["valid"] ? "[SUCCESS]" : "[FAILED]");
}


static const char* PREFS_PATH = "/config/prefs.json";

static bool ensureFS() {
    if (!LittleFS.begin(true)) {
        serial_printf("[WS] LittleFS mount failed\n");
        return false;
    }
    if (!LittleFS.exists("/config")) {
        LittleFS.mkdir("/config");
    }
    return true;
}

static String loadPreferencesJson() {
    if (!ensureFS()) return "{}";

    File file = LittleFS.open(PREFS_PATH, "r");
    if (!file) {
        serial_printf("[WS] Missing %s, returning empty config\n", PREFS_PATH);
        return "{}";
    }

    String json = file.readString();
    file.close();

    if (json.isEmpty()) return "{}";
    return json;
}

static String loadCartridge();
static String loadCartridgeVersion();

static void sendPreferences() {
    if (!wsConnected) return;

    JsonDocument stored;
    if (deserializeJson(stored, loadPreferencesJson())) {
        serial_printf("[WS] Invalid preferences json\n");
        return;
    }

    serial_printf("[WS] Sending preferences: \n%s\n", stored.as<String>().c_str());

    JsonDocument out;
    out["type"] = "preferences";
    out["device_id"] = deviceId;
    out["config"] = stored;
    out["os"] = OS_NAME;
    out["cartridge"] = loadCartridge();
    out["cartridge_version"] = loadCartridgeVersion();
    out["model"] = ESP.getChipModel();
    out["cores"] = ESP.getChipCores();
    out["cpu_freq"] = ESP.getCpuFreqMHz();
    out["revision"] = ESP.getChipRevision();
    out["flash_size"] = ESP.getFlashChipSize();
    out["flash_speed"] = ESP.getFlashChipSpeed();
    out["heap"] = ESP.getHeapSize();
    out["mac"] = WiFi.macAddress();


    String body;
    serializeJson(out, body);
    webSocket.sendTXT(body);
}

static void cmdSetPreference(JsonDocument &doc) {
    const char* key = doc["key"] | "";
    if (!key[0]) {
        serial_printf("[WS] set_preference: missing key\n");
        return;
    }

    JsonDocument stored;
    deserializeJson(stored, loadPreferencesJson());

    stored[key] = doc["value"];

    String body;
    serializeJson(stored, body);

    if (ensureFS()) {
        File file = LittleFS.open(PREFS_PATH, "w");
        if (file) {
            file.print(body);
            file.close();
        }
    }

    serial_printf("[WS] Preference '%s' saved\n", key);
    sendPreferences();
}

static String loadCartridge() {
    preferences.begin("system-config", true);
    String name = preferences.getString("cartridge", "");
    preferences.end();
    return name;
}

static String loadCartridgeVersion() {
    preferences.begin("system-config", true);
    String version = preferences.getString("cartridge_version", "");
    preferences.end();
    return version;
}

static void cmdSetCartridge(JsonDocument &doc) {
    const char* name = doc["name"] | "";
    const char* version = doc["version"] | "";
    preferences.begin("system-config", false);
    preferences.putString("cartridge", name);
    preferences.putString("cartridge_version", version);
    preferences.end();
    serial_printf("[WS] Cartridge '%s' (v%s) saved to NVS\n", name, version);
    sendPreferences();
}

// =============================================================
// OTA over WebSocket. El firmware llega en chunks binarios por
// WStype_BIN. Si algo falla a mitad, abortamos y reiniciamos:
// el bootloader vuelve a la particion anterior (rollback).
// =============================================================

static bool otaActive = false;
static size_t otaSize = 0;
static size_t otaReceived = 0;

static void sendOtaResult(bool ok, const char* msg) {
    JsonDocument response;
    response["cmd"] = "ota_result";
    response["device_id"] = deviceId;
    response["ok"] = ok;
    response["msg"] = msg;

    String body;
    serializeJson(response, body);
    webSocket.sendTXT(body);
}

static void cmdOtaStart(JsonDocument &doc) {
    size_t size = doc["file_size"] | 0;
    if (size == 0) {
        sendOtaResult(false, "missing file_size");
        return;
    }

    otaSize = size;
    otaReceived = 0;

    if (!Update.begin(size)) {
        Update.printError(Serial);
        otaActive = false;
        sendOtaResult(false, "could not begin update");
        return;
    }

    otaActive = true;
    serial_printf("[OTA] Started, %u bytes\n", (unsigned int)size);
    sendOtaResult(true, "started");
}

static void cmdOtaEnd(JsonDocument &doc) {
    if (!otaActive) {
        sendOtaResult(false, "no update in progress");
        return;
    }

    if (!Update.end()) {
        Update.printError(Serial);
        Update.abort();
        otaActive = false;
        sendOtaResult(false, "flash failed");
        ESP.restart();
        return;
    }

    otaActive = false;
    serial_printf("[OTA] Complete, rebooting...\n");
    sendOtaResult(true, "flash complete");
    delay(500);
    ESP.restart();
}

static void cmdOtaCancel(JsonDocument &doc) {
    if (!otaActive) {
        sendOtaResult(false, "no update in progress");
        return;
    }

    Update.abort();
    otaActive = false;
    serial_printf("[OTA] Cancelled\n");
    sendOtaResult(true, "cancelled");
}

static void cmdOtaRollback(JsonDocument &doc) {
    serial_printf("[OTA] Rollback...\n");
    bool ok = Update.rollBack();
    sendOtaResult(ok, ok ? "rollback scheduled" : "rollback failed");
    if (ok) {
        delay(500);
        ESP.restart();
    }
}

static const Command commands[] = {
    { "gpio",   cmdGpio },
    { "reboot", cmdReboot },
    { "ping", cmdPing },
    { "validate_auth", cmdValidateAuth },
    { "set_preference", cmdSetPreference },
    { "set_cartridge", cmdSetCartridge },
    { "ota_start", cmdOtaStart },
    { "ota_end", cmdOtaEnd },
    { "ota_cancel", cmdOtaCancel },
    { "ota_rollback", cmdOtaRollback },
};


static void onWSEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            serial_printf("[WS] Disconnected\n");
            wsConnected = false;
            pinMode(LED_PIN, OUTPUT);
            digitalWrite(LED_PIN, HIGH);
            break;
        case WStype_CONNECTED:
            serial_printf("[WS] Connected to webserver\n");
            wsConnected = true;
            if (deviceId.isEmpty()) deviceId = getDeviceID();
            sendRegister();
            sendPreferences();
            break;
        case WStype_TEXT: {
            JsonDocument rxDoc;
            DeserializationError err = deserializeJson(rxDoc, payload, length);
            if (err) {
                serial_printf("[WS] Invalid Json: %s\n", err.c_str());
                break;
            }
            const char* cmd = rxDoc["cmd"];
            if (!cmd) {
                serial_printf("[WS] Message: %s\n", payload);
                break;
            }

            for (const auto &c : commands) {
                if (strcmp(cmd, c.name) == 0) {
                    serial_printf("[CMD] %s\n", payload);
                    c.handler(rxDoc);
                    break;
                }
            }
            break;
        }
        case WStype_BIN: {
            if (!otaActive) {
                serial_printf("[OTA] Unexpected binary data\n");
                break;
            }

            size_t written = Update.write(payload, length);
            otaReceived += written;

            if (written != length) {
                serial_printf("[OTA] Write error at %u bytes\n", (unsigned int)otaReceived);
                Update.printError(Serial);
                Update.abort();
                otaActive = false;
                sendOtaResult(false, "write failed");
                ESP.restart();
                break;
            }

            if (otaReceived >= otaSize) {
                serial_printf("[OTA] All %u bytes received\n", (unsigned int)otaReceived);
                if (!Update.end()) {
                    Update.printError(Serial);
                    Update.abort();
                    otaActive = false;
                    sendOtaResult(false, "flash failed");
                    ESP.restart();
                    break;
                }
                otaActive = false;
                sendOtaResult(true, "flash complete");
                delay(500);
                ESP.restart();
            }
            break;
        }
        default:
            break;
    }
}

String getDeviceID() {
    return WiFi.macAddress();
}

static void loadNetworkConfig(String &ssid, String &password, String &ip) {
    preferences.begin("net-config", true);
    ssid = preferences.getString("ssid", "admin");
    password = preferences.getString("pass", "12345678");
    ip = preferences.getString("ip", "192.168.1.1");
    preferences.end();
}


static void loadUserConfig(String &device_name, String &username, String &password) {
    preferences.begin("user-config", true);
    username = preferences.getString("username", "admin");
    password = preferences.getString("userpass", "12345678");
    device_name = preferences.getString("device_name", "Mimi OS");
    preferences.end();
}

static bool sendPortalPage(const char *path) {
    File page = LittleFS.open(path, "r");
    if (!page) {
        serial_printf("[AP] Missing portal file: %s\n", path);
        server.send(500, "text/plain", "Portal page unavailable");
        return false;
    }

    server.streamFile(page, "text/html");
    page.close();
    return true;
}


void ConfigurationPortal() {
    String device_name;
    String username;
    String password;


  loadUserConfig(device_name, username, password);



  WiFi.mode(WIFI_AP);
  WiFi.softAP(device_name.c_str(), password.c_str());

    if (!LittleFS.begin()) {
        serial_printf("[AP] LittleFS mount failed; upload the filesystem image\n");
    }
  

  serial_printf("\n======[AP]=======\n");
  serial_printf("[AP] Configuration portal started\n");
  serial_printf("[AP] Device Name: %s\n", device_name.c_str());
  serial_printf("[AP] Username: %s\n", username.c_str());
  serial_printf("[AP] Password: %s\n", password.c_str());
  serial_printf("[AP] IP address: %s\n", WiFi.softAPIP().toString().c_str());
  serial_printf("======[AP]=======\n");

  
  server.on("/", HTTP_GET, []() {
    sendPortalPage("/portal/index.html");
  });

    server.on("/save", HTTP_POST, []() {
      String ssid = server.arg("ssid");
      String pass = server.arg("pass");
      String ip = server.arg("ip");
      String device_name = server.arg("device_name");
      String username = server.arg("username");
      String password = server.arg("userpass");

    // Save the configuration to preferences 
    // Net Config
    preferences.begin("net-config", false);
    preferences.putString("ssid", ssid);
    preferences.putString("pass", pass);
    preferences.putString("ip", ip);
    preferences.end();


    // User Config
    preferences.begin("user-config", false);
    preferences.putString("device_name", device_name);
    preferences.putString("username", username);
    preferences.putString("userpass", password);

    preferences.end();

    sendPortalPage("/portal/reboot.html");
    delay(2000);
    ESP.restart();
  });

  server.begin();

  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
  }
}


void TaskCortex(void *pvParameters) {
    String ssid;
    String password;
    String ip;

    String device_name;
    String username;
    String userpass;

    loadNetworkConfig(ssid, password, ip);
    loadUserConfig(device_name, username, userpass); 


    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    serial_printf("\n=======[WiFi]=======\n");
    serial_printf("[WiFi] Connecting...");
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
        serial_printf(" (status=%d)", WiFi.status());
    }
    
    serial_printf("\n[WiFi] Connected to '%s'\n", ssid.c_str());
    serial_printf("[WiFi] SSID: '%s'\n", ssid.c_str());
    serial_printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
    serial_printf("=======[WiFi]=======\n\n");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    unsigned long lastBlink = millis();
    bool blinkState = true;
    const unsigned long blinkInterval = 1000;
    serial_printf("========[WS]========\n");
    serial_printf("[WS] Starting WebSocket server...\n");
    webSocket.begin(ip, WS_SERVER_PORT, "/ws/esp32");
    serial_printf("[WS] WS at %s:%d\n", ip.c_str(), WS_SERVER_PORT);
    serial_printf("[WS] IP: %s\n", ip.c_str());
    webSocket.onEvent(onWSEvent);
    webSocket.setReconnectInterval(5000);
    serial_printf("========[WS]========\n\n");

    SystemData data;
    Notification notif;

    for (;;) {
        webSocket.loop();

        if (wsConnected) {
            if (millis() - lastBlink >= blinkInterval) {
                lastBlink = millis();
                blinkState = !blinkState;
                digitalWrite(LED_PIN, blinkState ? LOW : HIGH);
            }

            if (xQueueReceive(queue_to_synapse, &data, 0) == pdTRUE) {
                JsonDocument doc;
                doc["type"] = "sensor";
                doc["device_id"] = deviceId;
                doc["gpio"] = data.gpio_mask;
                doc["voltage"] = data.voltage;
                doc["temperature"] = data.temperature;
                doc["free_sram"] = data.free_sram;
                doc["free_flash"] = data.free_flash;

                String body;
                serializeJson(doc, body);
                webSocket.sendTXT(body);
            }

            if (xQueueReceive(queue_to_notifications, &notif, 0) == pdTRUE) {
                JsonDocument doc;
                doc["type"] = "notification";
                doc["device_id"] = deviceId;
                doc["process"] = notif.process;
                doc["title"] = notif.title;
                doc["message"] = notif.message;
                doc["color"] = notif.color;

                String body;
                serializeJson(doc, body);
                webSocket.sendTXT(body);
            }

            LogMessage logMessage;
            if (xQueueReceive(queue_to_logs, &logMessage, 0) == pdTRUE) {
                JsonDocument doc;
                doc["type"] = "log";
                doc["device_id"] = deviceId;
                doc["message"] = logMessage.message;

                String body;
                serializeJson(doc, body);
                webSocket.sendTXT(body);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

