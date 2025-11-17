#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <LittleFS.h>
#include <time.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "secrets.h"

// ========== PIN CONFIGURATION ==========
#define RELAY_PIN 23          // GPIO23 for relay control (Active-Low)
#define ONE_WIRE_BUS 4        // GPIO4 for DS18B20 sensors (both on same bus)
#define TRIG_PIN 5            // GPIO5 for JSN-SR04T TRIG
#define ECHO_PIN 18           // GPIO18 for JSN-SR04T ECHO

// ========== CONFIGURATION ==========
#define FIRMWARE_VERSION "v2.3.0"     // Version anzeigen im Dashboard
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
#define WEATHER_UPDATE_INTERVAL 600000  // Update weather every 10 minutes

// ========== GLOBAL OBJECTS ==========
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");  // WebSocket for Serial Monitor
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
    
    // Weather & Location
    float latitude = 50.952149;         // Default: Cologne
    float longitude = 7.1229;
} state;

// ========== WEATHER CACHE ==========
struct WeatherData {
    bool valid = false;
    unsigned long lastUpdate = 0;
    
    // Current weather
    float temperature = 0.0;
    int weatherCode = 0;
    int humidity = 0;
    float windSpeed = 0.0;
    
    // Tomorrow forecast
    float tempMin = 0.0;
    float tempMax = 0.0;
    int forecastWeatherCode = 0;
    float precipitation = 0.0;
    
    // Location name
    String locationName = "";
} weather;

unsigned long lastToggleTime = 0;
unsigned long lastTempRead = 0;
unsigned long lastTankRead = 0;
unsigned long bootTime = 0;
unsigned long lastStateChangeTime = 0;

// ========== SERIAL MONITOR (WebSocket) ==========
#define LOG_BUFFER_SIZE 50
String logBuffer[LOG_BUFFER_SIZE];
int logBufferIndex = 0;
int logBufferCount = 0;

// Custom print function that sends to both Serial and WebSocket
void serialLog(const char* message) {
    Serial.print(message);
    
    // Add to buffer
    logBuffer[logBufferIndex] = String(message);
    logBufferIndex = (logBufferIndex + 1) % LOG_BUFFER_SIZE;
    if (logBufferCount < LOG_BUFFER_SIZE) logBufferCount++;
    
    // Send to all WebSocket clients
    if (ws.count() > 0) {
        ws.textAll(message);
    }
}

void serialLogLn(const char* message) {
    serialLog(message);
    serialLog("\n");
}

// WebSocket event handler
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
                     AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        Serial.printf("WebSocket client #%u connected\n", client->id());
        
        // Send log history to new client
        for (int i = 0; i < logBufferCount; i++) {
            int idx = (logBufferIndex - logBufferCount + i + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
            client->text(logBuffer[idx]);
        }
    } else if (type == WS_EVT_DISCONNECT) {
        Serial.printf("WebSocket client #%u disconnected\n", client->id());
    }
}

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

// ========== REVERSE GEOCODING ==========
String fetchLocationName(float lat, float lon) {
    HTTPClient http;
    
    // OpenStreetMap Nominatim API (free, no API key needed)
    String url = "http://nominatim.openstreetmap.org/reverse?";
    url += "lat=" + String(lat, 6);
    url += "&lon=" + String(lon, 6);
    url += "&format=json";
    url += "&zoom=10";  // City level
    
    http.begin(url);
    http.addHeader("User-Agent", "ESP32-HeaterControl/2.3.0");
    http.setTimeout(5000);
    
    int httpCode = http.GET();
    String locationName = "Unbekannter Ort";
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);
        
        if (!error) {
            // Try to get city, town, or village
            if (doc["address"]["city"]) {
                locationName = doc["address"]["city"].as<String>();
            } else if (doc["address"]["town"]) {
                locationName = doc["address"]["town"].as<String>();
            } else if (doc["address"]["village"]) {
                locationName = doc["address"]["village"].as<String>();
            } else if (doc["address"]["municipality"]) {
                locationName = doc["address"]["municipality"].as<String>();
            }
        }
    }
    
    http.end();
    return locationName;
}

// ========== FETCH WEATHER DATA ==========
void fetchWeatherData() {
    // Check if WiFi is connected
    if (WiFi.status() != WL_CONNECTED) {
        serialLogLn("[Weather] WiFi not connected, skipping update");
        return;
    }
    
    // Check cache validity (10 min)
    if (weather.valid && (millis() - weather.lastUpdate < WEATHER_UPDATE_INTERVAL)) {
        serialLogLn("[Weather] Using cached data");
        return;
    }
    
    serialLogLn("[Weather] Fetching new weather data...");
    
    HTTPClient http;
    
    // Build API URL
    String url = "http://api.open-meteo.com/v1/forecast?";
    url += "latitude=" + String(state.latitude, 6);
    url += "&longitude=" + String(state.longitude, 6);
    url += "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m";
    url += "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum";
    url += "&timezone=Europe/Berlin&forecast_days=2";
    
    http.begin(url);
    http.setTimeout(10000);  // 10 sec timeout
    
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        // Parse JSON
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);
        
        if (!error) {
            // Current weather
            weather.temperature = doc["current"]["temperature_2m"];
            weather.weatherCode = doc["current"]["weather_code"];
            weather.humidity = doc["current"]["relative_humidity_2m"];
            weather.windSpeed = doc["current"]["wind_speed_10m"];
            
            // Tomorrow forecast (index 1)
            weather.tempMin = doc["daily"]["temperature_2m_min"][1];
            weather.tempMax = doc["daily"]["temperature_2m_max"][1];
            weather.forecastWeatherCode = doc["daily"]["weather_code"][1];
            weather.precipitation = doc["daily"]["precipitation_sum"][1];
            
            weather.valid = true;
            weather.lastUpdate = millis();
            
            // Fetch location name (only if not already set or coordinates changed)
            if (weather.locationName == "" || weather.locationName == "Unbekannter Ort") {
                serialLogLn("[Weather] Fetching location name...");
                weather.locationName = fetchLocationName(state.latitude, state.longitude);
                serialLog("[Weather] Location: ");
                serialLogLn(weather.locationName);
            }
            
            serialLogLn("[Weather] ✅ Data updated successfully");
            serialLog("[Weather] Current: ");
            serialLog(String(weather.temperature, 1));
            serialLog("°C, ");
            serialLog(String(weather.humidity));
            serialLog("% humidity, ");
            serialLog(String(weather.windSpeed, 1));
            serialLogLn(" km/h wind");
        } else {
            serialLog("[Weather] ❌ JSON parse error: ");
            serialLogLn(error.c_str());
            weather.valid = false;
        }
    } else {
        serialLog("[Weather] ❌ HTTP error: ");
        serialLogLn(String(httpCode));
        weather.valid = false;
    }
    
    http.end();
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
    
    // Load location (default: Cologne, Germany)
    state.latitude = prefs.getFloat("latitude", 50.952149);
    state.longitude = prefs.getFloat("longitude", 7.1229);
    
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
    
    // Save location
    prefs.putFloat("latitude", state.latitude);
    prefs.putFloat("longitude", state.longitude);
    
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
        doc["version"] = FIRMWARE_VERSION;
        
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
        
        // Location
        doc["latitude"] = state.latitude;
        doc["longitude"] = state.longitude;
        
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
    
    // API: Geocode location (city/PLZ to coordinates)
    server.on("/api/geocode", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!request->hasParam("query")) {
            request->send(400, "application/json", "{\"error\":\"Missing query parameter\"}");
            return;
        }
        
        String query = request->getParam("query")->value();
        
        HTTPClient http;
        
        // OpenStreetMap Nominatim API (forward geocoding)
        String url = "http://nominatim.openstreetmap.org/search?";
        url += "q=" + query;
        url += "&format=json&limit=1&countrycodes=de"; // Limit to Germany
        
        http.begin(url);
        http.addHeader("User-Agent", "ESP32-HeaterControl/2.3.0");
        http.setTimeout(5000);
        
        int httpCode = http.GET();
        
        StaticJsonDocument<512> doc;
        
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            
            // Parse array response
            JsonDocument responseDoc;
            DeserializationError error = deserializeJson(responseDoc, payload);
            
            if (!error && responseDoc.size() > 0) {
                JsonObject firstResult = responseDoc[0];
                
                doc["found"] = true;
                doc["latitude"] = firstResult["lat"].as<float>();
                doc["longitude"] = firstResult["lon"].as<float>();
                doc["displayName"] = firstResult["display_name"].as<String>();
            } else {
                doc["found"] = false;
                doc["error"] = "Location not found";
            }
        } else {
            doc["found"] = false;
            doc["error"] = "Geocoding service unavailable";
        }
        
        http.end();
        
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });
    
    // API: Get weather data
    server.on("/api/weather", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<512> doc;
        
        if (weather.valid) {
            doc["valid"] = true;
            doc["temperature"] = round(weather.temperature * 10) / 10.0;
            doc["weatherCode"] = weather.weatherCode;
            doc["humidity"] = weather.humidity;
            doc["windSpeed"] = round(weather.windSpeed * 10) / 10.0;
            doc["locationName"] = weather.locationName;
            
            // Tomorrow forecast
            JsonObject forecast = doc.createNestedObject("tomorrow");
            forecast["tempMin"] = round(weather.tempMin * 10) / 10.0;
            forecast["tempMax"] = round(weather.tempMax * 10) / 10.0;
            forecast["weatherCode"] = weather.forecastWeatherCode;
            forecast["precipitation"] = round(weather.precipitation * 10) / 10.0;
        } else {
            doc["valid"] = false;
            doc["error"] = "No weather data available";
        }
        
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });
    
    // API: Update location
    server.on("/api/location", HTTP_POST, 
        [](AsyncWebServerRequest *request) {},
        NULL,
        [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
            if (!request->authenticate(AUTH_USER, AUTH_PASS)) {
                return request->requestAuthentication();
            }
            
            StaticJsonDocument<256> doc;
            DeserializationError error = deserializeJson(doc, data, len);
            
            if (error) {
                request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                return;
            }
            
            bool changed = false;
            
            if (doc.containsKey("latitude")) {
                float lat = doc["latitude"];
                if (lat >= -90 && lat <= 90) {
                    state.latitude = lat;
                    changed = true;
                }
            }
            
            if (doc.containsKey("longitude")) {
                float lon = doc["longitude"];
                if (lon >= -180 && lon <= 180) {
                    state.longitude = lon;
                    changed = true;
                }
            }
            
            if (changed) {
                saveSettings();
                // Force weather update on next loop (and refetch location name)
                weather.valid = false;
                weather.lastUpdate = 0;
                weather.locationName = "";
                request->send(200, "application/json", "{\"success\":true,\"message\":\"Location updated\"}");
            } else {
                request->send(400, "application/json", "{\"error\":\"Invalid location data\"}");
            }
        }
    );
    
    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not found");
    });
    
    // Initialize WebSocket for Serial Monitor
    ws.onEvent(onWebSocketEvent);
    server.addHandler(&ws);
    Serial.println("WebSocket initialized at /ws");
    
    // Initialize OTA Updates (custom handler)
    server.on("/update", HTTP_POST, 
        [](AsyncWebServerRequest *request) {
            bool shouldReboot = !Update.hasError();
            AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", 
                shouldReboot ? "OK" : "FAIL");
            response->addHeader("Connection", "close");
            request->send(response);
            
            if (shouldReboot) {
                Serial.println("OTA Update successful, rebooting...");
                delay(1000);
                ESP.restart();
            }
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                Serial.printf("OTA Update Start: %s\n", filename.c_str());
                if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
                    Update.printError(Serial);
                }
            }
            if (!Update.hasError()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                }
            }
            if (final) {
                if (Update.end(true)) {
                    Serial.printf("OTA Update Success: %u bytes\n", index + len);
                } else {
                    Update.printError(Serial);
                }
            }
        }
    );
    Serial.println("OTA initialized at /update");
    
    // Initialize LittleFS OTA Updates (for HTML/CSS/JS)
    server.on("/update-fs", HTTP_POST, 
        [](AsyncWebServerRequest *request) {
            bool shouldReboot = !Update.hasError();
            AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", 
                shouldReboot ? "OK" : "FAIL");
            response->addHeader("Connection", "close");
            request->send(response);
            
            if (shouldReboot) {
                Serial.println("LittleFS OTA Update successful, rebooting...");
                delay(1000);
                ESP.restart();
            }
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                Serial.printf("LittleFS OTA Start: %s\n", filename.c_str());
                // UPDATE_TYPE_FILESYSTEM = U_SPIFFS
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
                    Update.printError(Serial);
                }
            }
            if (!Update.hasError()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                }
            }
            if (final) {
                if (Update.end(true)) {
                    Serial.printf("LittleFS OTA Success: %u bytes\n", index + len);
                } else {
                    Update.printError(Serial);
                }
            }
        }
    );
    Serial.println("LittleFS OTA initialized at /update-fs");
    
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
    
    // Update weather data (cached, updates every 10 min)
    fetchWeatherData();
    
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
    
    // Cleanup disconnected WebSocket clients
    ws.cleanupClients();
    
    delay(10);
}
