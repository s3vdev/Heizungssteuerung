#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <time.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include "secrets.h"

// ========== PIN CONFIGURATION ==========
#define RELAY_PIN 23          // GPIO23 for relay control (Active-Low)
#define ONE_WIRE_BUS 4        // GPIO4 for DS18B20 sensors (both on same bus)
#define TRIG_PIN 5            // GPIO5 for JSN-SR04T TRIG
#define ECHO_PIN 18           // GPIO18 for JSN-SR04T ECHO

// ========== CONFIGURATION ==========
#define HOSTNAME "heater"
#define AP_SSID "HeaterSetup"
#define AP_PASSWORD "12345678"
#define WIFI_TIMEOUT_MS 20000
#define NTP_SERVER "pool.ntp.org"
#define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"  // Europe/Berlin
#define DEBOUNCE_MS 300
#define TEMP_READ_INTERVAL 1000
#define MAX_SCHEDULES 4
#define TANK_READ_INTERVAL 5000       // Read tank level every 5 seconds
#define ULTRASONIC_TIMEOUT 30000      // 30ms timeout for echo (max ~5m range)

// ========== GLOBAL OBJECTS ==========
AsyncWebServer server(80);
Preferences prefs;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ========== TEMPERATURE SENSOR ADDRESSES ==========
DeviceAddress sensor1Address, sensor2Address;  // Will store unique addresses
bool sensor1Found = false;
bool sensor2Found = false;

// ========== SCHEDULE STRUCTURE ==========
struct Schedule {
    bool enabled = false;
    uint8_t startHour = 0;
    uint8_t startMinute = 0;
    uint8_t endHour = 0;
    uint8_t endMinute = 0;
};

// ========== STATISTICS ==========
struct Statistics {
    unsigned long switchCount = 0;         // Total switches since boot
    unsigned long onTimeSeconds = 0;       // Total ON time in seconds
    unsigned long offTimeSeconds = 0;      // Total OFF time in seconds
    unsigned long todaySwitches = 0;       // Switches today
    unsigned long lastResetDay = 0;        // Day of last reset (for daily stats)
} stats;

// ========== GLOBAL STATE ==========
struct SystemState {
    bool heatingOn = false;
    float tempVorlauf = NAN;    // Forward flow temperature
    float tempRuecklauf = NAN;  // Return flow temperature
    String mode = "manual";     // "manual", "auto", "schedule", or "frost"
    float tempOn = 30.0;        // Turn ON temperature (hysteresis min)
    float tempOff = 40.0;       // Turn OFF temperature (hysteresis max)
    Schedule schedules[MAX_SCHEDULES];
    unsigned long uptime = 0;
    bool apModeActive = false;
    bool ntpSynced = false;
    
    // Frost protection
    bool frostProtectionEnabled = false;
    float frostProtectionTemp = 8.0;  // Minimum temperature
    
    // Tank level monitoring (JSN-SR04T)
    bool tankSensorAvailable = false;   // Sensor detected and working
    float tankHeight = 100.0;           // Tank height in cm (from sensor to bottom)
    float tankCapacity = 1000.0;        // Tank capacity in liters
    float tankDistance = -1.0;          // Current distance to liquid surface in cm
    float tankLiters = 0.0;             // Calculated liters
    int tankPercent = 0;                // Calculated fill percentage
} state;

unsigned long lastToggleTime = 0;
unsigned long lastTempRead = 0;
unsigned long lastTankRead = 0;
unsigned long bootTime = 0;
unsigned long lastStateChangeTime = 0;

// ========== RELAY CONTROL (Active-Low) ==========
void setHeater(bool on, bool saveToNVS = true) {
    // Only count as switch if state actually changes
    if (on != state.heatingOn) {
        stats.switchCount++;
        stats.todaySwitches++;
        lastStateChangeTime = millis();
        Serial.printf("Switch #%lu: Heater %s\n", stats.switchCount, on ? "ON" : "OFF");
    }
    
    state.heatingOn = on;
    // Active-Low: LOW = ON, HIGH = OFF
    digitalWrite(RELAY_PIN, on ? LOW : HIGH);
    
    if (saveToNVS && state.mode == "manual") {
        prefs.begin("heater", false);
        prefs.putBool("heatingOn", on);
        prefs.end();
    }
    
    Serial.printf("Heater %s (Relay: %s)\n", on ? "ON" : "OFF", on ? "LOW" : "HIGH");
}

// ========== UPDATE STATISTICS ==========
void updateStatistics() {
    unsigned long now = millis();
    unsigned long elapsed = (now - lastStateChangeTime) / 1000;  // seconds since last change
    
    if (state.heatingOn) {
        stats.onTimeSeconds += 1;
    } else {
        stats.offTimeSeconds += 1;
    }
    
    // Reset daily counter at midnight
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 100)) {
        unsigned long currentDay = timeinfo.tm_yday;  // Day of year
        if (stats.lastResetDay != currentDay) {
            stats.todaySwitches = 0;
            stats.lastResetDay = currentDay;
            Serial.println("Daily statistics reset");
        }
    }
}

// ========== READ TANK LEVEL (JSN-SR04T) ==========
float readTankDistance() {
    // Send 10us pulse to TRIG
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    
    // Read ECHO pulse duration (timeout after 30ms)
    long duration = pulseIn(ECHO_PIN, HIGH, ULTRASONIC_TIMEOUT);
    
    // No echo received = sensor error
    if (duration == 0) {
        return -1.0;
    }
    
    // Calculate distance in cm (speed of sound: 343 m/s = 0.0343 cm/us)
    // Distance = (duration / 2) * 0.0343
    float distance = (duration * 0.0343) / 2.0;
    
    // Sanity check: JSN-SR04T range 25cm-450cm
    if (distance < 2.0 || distance > 500.0) {
        return -1.0;
    }
    
    return distance;
}

void updateTankLevel() {
    float distance = readTankDistance();
    
    if (distance < 0) {
        // Sensor error or not available
        state.tankSensorAvailable = false;
        state.tankDistance = -1.0;
        state.tankLiters = 0.0;
        state.tankPercent = 0;
        return;
    }
    
    // Sensor working
    state.tankSensorAvailable = true;
    state.tankDistance = distance;
    
    // Calculate fill level
    // fillHeight = tankHeight - distance (distance from sensor to surface)
    float fillHeight = state.tankHeight - distance;
    
    // Clamp to valid range
    if (fillHeight < 0) fillHeight = 0;
    if (fillHeight > state.tankHeight) fillHeight = state.tankHeight;
    
    // Calculate percentage
    state.tankPercent = (int)((fillHeight / state.tankHeight) * 100.0);
    
    // Calculate liters (assuming cylindrical/rectangular tank)
    state.tankLiters = (fillHeight / state.tankHeight) * state.tankCapacity;
    
    // Round to 1 decimal
    state.tankLiters = round(state.tankLiters * 10) / 10.0;
}

// ========== INITIALIZE TEMPERATURE SENSORS ==========
void initSensors() {
    sensors.begin();
    int deviceCount = sensors.getDeviceCount();
    
    Serial.printf("Found %d DS18B20 sensor(s) on OneWire bus\n", deviceCount);
    
    if (deviceCount >= 1) {
        sensors.getAddress(sensor1Address, 0);
        sensor1Found = true;
        Serial.print("Sensor 1 (Vorlauf) address: ");
        for (uint8_t i = 0; i < 8; i++) {
            Serial.printf("%02X", sensor1Address[i]);
        }
        Serial.println();
    }
    
    if (deviceCount >= 2) {
        sensors.getAddress(sensor2Address, 1);
        sensor2Found = true;
        Serial.print("Sensor 2 (Rücklauf) address: ");
        for (uint8_t i = 0; i < 8; i++) {
            Serial.printf("%02X", sensor2Address[i]);
        }
        Serial.println();
    }
    
    if (deviceCount < 2) {
        Serial.println("WARNING: Less than 2 sensors found. Using single sensor for both values.");
    }
}

// ========== TEMPERATURE READING ==========
void readTemperatures() {
    sensors.requestTemperatures();
    
    // Read sensor 1 (Vorlauf)
    if (sensor1Found) {
        float temp = sensors.getTempC(sensor1Address);
        if (temp != DEVICE_DISCONNECTED_C && temp >= -55.0 && temp <= 125.0) {
            state.tempVorlauf = temp;
        } else {
            state.tempVorlauf = NAN;
        }
    }
    
    // Read sensor 2 (Rücklauf)
    if (sensor2Found) {
        float temp = sensors.getTempC(sensor2Address);
        if (temp != DEVICE_DISCONNECTED_C && temp >= -55.0 && temp <= 125.0) {
            state.tempRuecklauf = temp;
        } else {
            state.tempRuecklauf = NAN;
        }
    } else if (sensor1Found) {
        // Fallback: If only one sensor, use it for both
        state.tempRuecklauf = state.tempVorlauf;
    }
}

// ========== GET CURRENT TIME ==========
bool getCurrentTime(int &hour, int &minute) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 100)) {
        return false;
    }
    hour = timeinfo.tm_hour;
    minute = timeinfo.tm_min;
    return true;
}

// ========== CHECK IF TIME IS IN SCHEDULE ==========
bool isInSchedule() {
    int currentHour, currentMinute;
    if (!getCurrentTime(currentHour, currentMinute)) {
        return false;  // Can't get time, assume OFF
    }
    
    int currentMinutes = currentHour * 60 + currentMinute;
    
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        if (!state.schedules[i].enabled) continue;
        
        int startMinutes = state.schedules[i].startHour * 60 + state.schedules[i].startMinute;
        int endMinutes = state.schedules[i].endHour * 60 + state.schedules[i].endMinute;
        
        // Handle overnight schedules (e.g., 23:00 - 06:00)
        if (startMinutes > endMinutes) {
            if (currentMinutes >= startMinutes || currentMinutes < endMinutes) {
                return true;
            }
        } else {
            if (currentMinutes >= startMinutes && currentMinutes < endMinutes) {
                return true;
            }
        }
    }
    
    return false;
}

// ========== SCHEDULE CONTROL ==========
void scheduleControl() {
    if (state.mode != "schedule") {
        return;
    }
    
    bool shouldBeOn = isInSchedule();
    
    if (shouldBeOn != state.heatingOn) {
        Serial.printf("SCHEDULE: Should be %s, turning heater %s\n", 
                     shouldBeOn ? "ON" : "OFF", shouldBeOn ? "ON" : "OFF");
        setHeater(shouldBeOn, false);
    }
}

// ========== FROST PROTECTION ==========
void frostProtection() {
    if (!state.frostProtectionEnabled) {
        return;
    }
    
    // Use the lower temperature (usually Rücklauf)
    float checkTemp = state.tempRuecklauf;
    if (isnan(checkTemp) && !isnan(state.tempVorlauf)) {
        checkTemp = state.tempVorlauf;
    }
    
    if (isnan(checkTemp)) {
        return;  // No valid temperature
    }
    
    // Turn ON if below frost protection temperature
    if (checkTemp < state.frostProtectionTemp && !state.heatingOn) {
        Serial.printf("FROST: Temperature %.1f°C < %.1f°C, turning heater ON\n", 
                     checkTemp, state.frostProtectionTemp);
        setHeater(true, false);
    }
    // Turn OFF if 2°C above frost protection (hysteresis)
    else if (checkTemp > (state.frostProtectionTemp + 2.0) && state.heatingOn) {
        Serial.printf("FROST: Temperature %.1f°C safe, turning heater OFF\n", checkTemp);
        setHeater(false, false);
    }
}

// ========== AUTOMATIC CONTROL WITH HYSTERESIS ==========
void automaticControl() {
    if (state.mode != "auto") {
        return;
    }
    
    // Use Rücklauf temperature for control
    if (isnan(state.tempRuecklauf)) {
        return;
    }
    
    // Hysteresis logic
    if (state.tempRuecklauf <= state.tempOn && !state.heatingOn) {
        Serial.printf("AUTO: Rücklauf %.1f°C <= %.1f°C, turning heater ON\n", 
                     state.tempRuecklauf, state.tempOn);
        setHeater(true, false);
    } 
    else if (state.tempRuecklauf >= state.tempOff && state.heatingOn) {
        Serial.printf("AUTO: Rücklauf %.1f°C >= %.1f°C, turning heater OFF\n", 
                     state.tempRuecklauf, state.tempOff);
        setHeater(false, false);
    }
}

// ========== FAILSAFE CHECK ==========
void checkFailsafe() {
    // If both sensors fail, turn off
    if (isnan(state.tempVorlauf) && isnan(state.tempRuecklauf) && state.heatingOn) {
        Serial.println("FAILSAFE: All sensors failed, turning heater OFF");
        setHeater(false);
    }
}

// ========== LOAD SETTINGS FROM NVS ==========
void loadSettings() {
    prefs.begin("heater", true);
    
    state.heatingOn = prefs.getBool("heatingOn", false);
    state.mode = prefs.getString("mode", "manual");
    state.tempOn = prefs.getFloat("tempOn", 30.0);
    state.tempOff = prefs.getFloat("tempOff", 40.0);
    state.frostProtectionEnabled = prefs.getBool("frostEnabled", false);
    state.frostProtectionTemp = prefs.getFloat("frostTemp", 8.0);
    
    // Load tank configuration
    state.tankHeight = prefs.getFloat("tankHeight", 100.0);
    state.tankCapacity = prefs.getFloat("tankCapacity", 1000.0);
    
    // Load schedules
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        String key = "sched" + String(i);
        if (prefs.isKey(key.c_str())) {
            size_t len = prefs.getBytesLength(key.c_str());
            if (len == sizeof(Schedule)) {
                prefs.getBytes(key.c_str(), &state.schedules[i], sizeof(Schedule));
            }
        }
    }
    
    prefs.end();
    
    Serial.println("=== Settings loaded from NVS ===");
    Serial.printf("Mode: %s\n", state.mode.c_str());
    Serial.printf("Heating: %s\n", state.heatingOn ? "ON" : "OFF");
    Serial.printf("Temp ON: %.1f°C\n", state.tempOn);
    Serial.printf("Temp OFF: %.1f°C\n", state.tempOff);
    Serial.printf("Frost Protection: %s (%.1f°C)\n", 
                 state.frostProtectionEnabled ? "ON" : "OFF", 
                 state.frostProtectionTemp);
    Serial.printf("Schedules loaded: %d\n", MAX_SCHEDULES);
}

// ========== SAVE SETTINGS TO NVS ==========
void saveSettings() {
    prefs.begin("heater", false);
    
    prefs.putString("mode", state.mode);
    prefs.putFloat("tempOn", state.tempOn);
    prefs.putFloat("tempOff", state.tempOff);
    prefs.putBool("frostEnabled", state.frostProtectionEnabled);
    prefs.putFloat("frostTemp", state.frostProtectionTemp);
    
    // Save tank configuration
    prefs.putFloat("tankHeight", state.tankHeight);
    prefs.putFloat("tankCapacity", state.tankCapacity);
    
    if (state.mode == "manual") {
        prefs.putBool("heatingOn", state.heatingOn);
    }
    
    // Save schedules
    for (int i = 0; i < MAX_SCHEDULES; i++) {
        String key = "sched" + String(i);
        prefs.putBytes(key.c_str(), &state.schedules[i], sizeof(Schedule));
    }
    
    prefs.end();
    
    Serial.println("Settings saved to NVS");
}

// ========== WIFI SETUP ==========
bool setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
    
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < WIFI_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi connected! IP: ");
        Serial.println(WiFi.localIP());
        Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
        return true;
    }
    
    Serial.println("WiFi connection failed!");
    return false;
}

// ========== ACCESS POINT MODE ==========
void setupAccessPoint() {
    state.apModeActive = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    
    Serial.println("Access Point Mode activated");
    Serial.printf("AP SSID: %s\n", AP_SSID);
    Serial.printf("AP Password: %s\n", AP_PASSWORD);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
}

// ========== mDNS SETUP ==========
void setupMDNS() {
    if (MDNS.begin(HOSTNAME)) {
        Serial.printf("mDNS responder started: http://%s.local/\n", HOSTNAME);
        MDNS.addService("http", "tcp", 80);
    } else {
        Serial.println("Error setting up mDNS responder!");
    }
}

// ========== NTP SETUP ==========
void setupNTP() {
    configTzTime(TIMEZONE, NTP_SERVER);
    Serial.println("NTP time sync initiated");
    
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
        state.ntpSynced = true;
        Serial.printf("NTP synced! Current time: %02d:%02d:%02d\n", 
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        Serial.println("NTP time sync pending...");
        state.ntpSynced = false;
    }
}

// ========== WEB SERVER ROUTES ==========
void setupWebServer() {
    // Serve index.html from LittleFS
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LittleFS, "/index.html", "text/html");
    });
    
    // API: Get current status
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<768> doc;
        
        // Temperatures
        if (isnan(state.tempVorlauf)) {
            doc["tempVorlauf"] = nullptr;
        } else {
            doc["tempVorlauf"] = round(state.tempVorlauf * 10) / 10.0;
        }
        
        if (isnan(state.tempRuecklauf)) {
            doc["tempRuecklauf"] = nullptr;
        } else {
            doc["tempRuecklauf"] = round(state.tempRuecklauf * 10) / 10.0;
        }
        
        doc["heating"] = state.heatingOn;
        doc["mode"] = state.mode;
        doc["tempOn"] = state.tempOn;
        doc["tempOff"] = state.tempOff;
        doc["relayActiveLow"] = true;
        doc["rssi"] = WiFi.RSSI();
        doc["apMode"] = state.apModeActive;
        doc["uptime"] = state.uptime;
        doc["ntpSynced"] = state.ntpSynced;
        
        // Current time
        int hour, minute;
        if (getCurrentTime(hour, minute)) {
            char timeStr[6];
            sprintf(timeStr, "%02d:%02d", hour, minute);
            doc["currentTime"] = timeStr;
        }
        
        // Temperature difference & efficiency
        if (!isnan(state.tempVorlauf) && !isnan(state.tempRuecklauf)) {
            float diff = state.tempVorlauf - state.tempRuecklauf;
            doc["tempDiff"] = round(diff * 10) / 10.0;
            
            // Efficiency: optimal is 10-15°C difference
            float efficiency = 0;
            if (diff >= 10 && diff <= 15) {
                efficiency = 100;
            } else if (diff > 15) {
                efficiency = 100 - ((diff - 15) * 5);  // Decrease above 15
            } else if (diff > 0) {
                efficiency = (diff / 10.0) * 100;  // Scale 0-10 to 0-100
            }
            efficiency = max(0.0f, min(100.0f, efficiency));
            doc["efficiency"] = (int)efficiency;
        }
        
        // Statistics
        doc["switchCount"] = stats.switchCount;
        doc["todaySwitches"] = stats.todaySwitches;
        doc["onTimeSeconds"] = stats.onTimeSeconds;
        doc["offTimeSeconds"] = stats.offTimeSeconds;
        
        // Frost protection
        doc["frostEnabled"] = state.frostProtectionEnabled;
        doc["frostTemp"] = state.frostProtectionTemp;
        
        // Tank level
        doc["tankAvailable"] = state.tankSensorAvailable;
        if (state.tankSensorAvailable) {
            doc["tankDistance"] = round(state.tankDistance * 10) / 10.0;
            doc["tankLiters"] = round(state.tankLiters * 10) / 10.0;
            doc["tankPercent"] = state.tankPercent;
        } else {
            doc["tankDistance"] = nullptr;
            doc["tankLiters"] = nullptr;
            doc["tankPercent"] = nullptr;
        }
        doc["tankHeight"] = state.tankHeight;
        doc["tankCapacity"] = state.tankCapacity;
        
        // Schedules
        JsonArray schedArray = doc.createNestedArray("schedules");
        for (int i = 0; i < MAX_SCHEDULES; i++) {
            JsonObject sched = schedArray.createNestedObject();
            sched["enabled"] = state.schedules[i].enabled;
            
            char startTime[6], endTime[6];
            sprintf(startTime, "%02d:%02d", state.schedules[i].startHour, state.schedules[i].startMinute);
            sprintf(endTime, "%02d:%02d", state.schedules[i].endHour, state.schedules[i].endMinute);
            
            sched["start"] = startTime;
            sched["end"] = endTime;
        }
        
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });
    
    // API: Toggle heater (manual mode only)
    server.on("/api/toggle", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!request->authenticate(AUTH_USER, AUTH_PASS)) {
            return request->requestAuthentication();
        }
        
        if (millis() - lastToggleTime < DEBOUNCE_MS) {
            request->send(429, "application/json", "{\"error\":\"Too many requests\"}");
            return;
        }
        
        if (state.mode != "manual") {
            request->send(400, "application/json", "{\"error\":\"Not in manual mode\"}");
            return;
        }
        
        setHeater(!state.heatingOn);
        lastToggleTime = millis();
        
        StaticJsonDocument<64> doc;
        doc["success"] = true;
        doc["heating"] = state.heatingOn;
        
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });
    
    // API: Save settings
    server.on("/api/settings", HTTP_POST, 
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (!request->authenticate(AUTH_USER, AUTH_PASS)) {
                return request->requestAuthentication();
            }
            
            StaticJsonDocument<1024> doc;
            DeserializationError error = deserializeJson(doc, data, len);
            
            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            
            bool changed = false;
            
            // Update mode
            if (doc.containsKey("mode")) {
                String newMode = doc["mode"].as<String>();
                if (newMode == "manual" || newMode == "auto" || newMode == "schedule") {
                    state.mode = newMode;
                    changed = true;
                    Serial.printf("Mode changed to: %s\n", newMode.c_str());
                    
                    if (newMode != "manual") {
                        setHeater(false, false);
                    }
                }
            }
            
            // Update frost protection
            if (doc.containsKey("frostEnabled")) {
                state.frostProtectionEnabled = doc["frostEnabled"];
                changed = true;
            }
            
            if (doc.containsKey("frostTemp")) {
                float temp = doc["frostTemp"];
                if (temp >= 5 && temp <= 15) {
                    state.frostProtectionTemp = temp;
                    changed = true;
                }
            }
            
            // Update tank configuration
            if (doc.containsKey("tankHeight")) {
                float height = doc["tankHeight"];
                if (height > 0 && height <= 500) {  // Max 5 meters
                    state.tankHeight = height;
                    changed = true;
                }
            }
            
            if (doc.containsKey("tankCapacity")) {
                float capacity = doc["tankCapacity"];
                if (capacity > 0 && capacity <= 10000) {  // Max 10000 liters
                    state.tankCapacity = capacity;
                    changed = true;
                }
            }
            
            // Update temperatures
            if (doc.containsKey("tempOn")) {
                state.tempOn = doc["tempOn"];
                changed = true;
            }
            
            if (doc.containsKey("tempOff")) {
                state.tempOff = doc["tempOff"];
                changed = true;
            }
            
            // Update schedules
            if (doc.containsKey("schedules")) {
                JsonArray schedArray = doc["schedules"];
                for (int i = 0; i < MAX_SCHEDULES && i < (int)schedArray.size(); i++) {
                    JsonObject sched = schedArray[i];
                    
                    state.schedules[i].enabled = sched["enabled"] | false;
                    
                    String start = sched["start"] | "00:00";
                    String end = sched["end"] | "00:00";
                    
                    sscanf(start.c_str(), "%hhu:%hhu", 
                          &state.schedules[i].startHour, 
                          &state.schedules[i].startMinute);
                    sscanf(end.c_str(), "%hhu:%hhu", 
                          &state.schedules[i].endHour, 
                          &state.schedules[i].endMinute);
                }
                changed = true;
            }
            
            if (state.tempOff <= state.tempOn) {
                request->send(400, "application/json", 
                    "{\"error\":\"tempOff must be greater than tempOn\"}");
                return;
            }
            
            if (changed) {
                saveSettings();
                
                if (state.mode == "auto") {
                    automaticControl();
                } else if (state.mode == "schedule") {
                    scheduleControl();
                }
            }
            
            request->send(200, "application/json", "{\"success\":true}");
        }
    );
    
    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not found");
    });
    
    server.begin();
    Serial.println("Web server started");
}

// ========== SETUP ==========
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== ESP32 Heater Control v2.0 ===");
    
    bootTime = millis();
    
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, HIGH);  // Relay OFF
    
    // Initialize ultrasonic sensor pins
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    digitalWrite(TRIG_PIN, LOW);
    
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed!");
        return;
    }
    Serial.println("LittleFS mounted");
    
    // Initialize sensors
    initSensors();
    
    // Load settings
    loadSettings();
    
    if (state.mode == "manual") {
        setHeater(state.heatingOn, false);
    } else {
        setHeater(false, false);
    }
    
    bool wifiConnected = setupWiFi();
    
    if (!wifiConnected) {
        setupAccessPoint();
    } else {
        setupMDNS();
        setupNTP();
    }
    
    setupWebServer();
    
    readTemperatures();
    Serial.printf("Vorlauf: %.1f°C, Rücklauf: %.1f°C\n", 
                 state.tempVorlauf, state.tempRuecklauf);
    
    // Test tank sensor
    updateTankLevel();
    if (state.tankSensorAvailable) {
        Serial.printf("Tank sensor detected: %.1f L (%.0f%%)\n", 
                     state.tankLiters, (float)state.tankPercent);
    } else {
        Serial.println("Tank sensor not available");
    }
    
    checkFailsafe();
    
    if (state.mode == "auto") {
        automaticControl();
    } else if (state.mode == "schedule") {
        scheduleControl();
    }
    
    Serial.println("=== Setup complete ===\n");
}

// ========== LOOP ==========
void loop() {
    unsigned long now = millis();
    
    state.uptime = (now - bootTime) / 1000;
    
    // Sync NTP if not yet synced
    if (!state.ntpSynced && !state.apModeActive) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 100)) {
            state.ntpSynced = true;
            Serial.println("NTP time synced!");
        }
    }
    
    if (now - lastTempRead >= TEMP_READ_INTERVAL) {
        lastTempRead = now;
        readTemperatures();
        
        // Update statistics
        updateStatistics();
        
        checkFailsafe();
        
        // Frost protection has highest priority
        if (state.frostProtectionEnabled) {
            frostProtection();
        }
        // Then normal modes
        else if (state.mode == "auto") {
            automaticControl();
        } else if (state.mode == "schedule") {
            scheduleControl();
        }
    }
    
    // Read tank level every 5 seconds
    if (now - lastTankRead >= TANK_READ_INTERVAL) {
        lastTankRead = now;
        updateTankLevel();
    }
    
    if (!state.apModeActive && WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi lost, reconnecting...");
        setupWiFi();
    }
    
    delay(10);
}
