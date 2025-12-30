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
#include <stdarg.h>
#include "secrets.h"

// ========== PIN CONFIGURATION ==========
#define HEATING_RELAY_PIN 21  // GPIO21 for heating relay control (Active-Low) (GPIO23 seems unreliable on some boards)
#define PUMP_RELAY_PIN 22     // GPIO22 for pump relay control (Active-Low)
#define DEFAULT_HEATING_RELAY_PIN HEATING_RELAY_PIN
#define DEFAULT_PUMP_RELAY_PIN PUMP_RELAY_PIN
#define RELAY_PIN HEATING_RELAY_PIN  // Legacy alias for backward compatibility
#define ONE_WIRE_BUS 27       // GPIO27 for DS18B20 sensors (chosen for convenient wiring; avoid strapping pin GPIO4 which can break boot on some boards)
#define TRIG_PIN 16           // GPIO16 for JSN-SR04T TRIG
#define ECHO_PIN 18           // GPIO18 for JSN-SR04T ECHO

// ========== CONFIGURATION ==========
#define FIRMWARE_VERSION "v2.2.7"     // Version shown in dashboard
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
#define PUMP_COOLDOWN_MS 180000       // Pump stays ON for 180 seconds after heating turns OFF (3 minutes)

// ========== GLOBAL OBJECTS ==========
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");  // WebSocket for Serial Monitor
Preferences prefs;
// NOTE: Do NOT construct OneWire/DallasTemperature as global objects.
// On some ESP32 boards this can crash during static initialization (before Arduino runtime is ready).
// We initialize them in setup() instead.
OneWire* oneWire = nullptr;
DallasTemperature* sensors = nullptr;

// ========== TANK SENSOR DEBUG ==========
static volatile unsigned long lastUltrasonicDurationUs = 0;
static volatile int lastEchoBefore = -1;
static volatile int lastEchoAfter = -1;
static volatile float lastUltrasonicDistanceCm = -1.0f;
static volatile uint8_t lastTankErrorCode = 0; // 0=OK, 1=TIMEOUT, 2=OUT_OF_RANGE

// Smooth tank availability to avoid UI flapping on occasional missed echoes
static unsigned long lastTankGoodMs = 0;
static float lastTankGoodDistanceCm = -1.0f;

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

// ========== SWITCH EVENT HISTORY ==========
#define MAX_SWITCH_EVENTS 50  // Store last 50 switch events
struct SwitchEvent {
    unsigned long timestamp = 0;  // Unix timestamp (or 0 if NTP not synced)
    bool isOn = false;            // true = ON, false = OFF
    float tempVorlauf = NAN;      // Temperature when switched
    float tempRuecklauf = NAN;
    unsigned long uptimeMs = 0;   // Uptime in milliseconds (fallback if no NTP)
    float tankLiters = NAN;       // Tank level in liters when switched (for consumption comparison)
} switchEvents[MAX_SWITCH_EVENTS];
int switchEventIndex = 0;  // Ring buffer index

// ========== BEHAVIOR WARNING TRACKING ==========
#define MAX_SWITCH_HISTORY 20
#define WARNING_THRESHOLD_SWITCHES 10  // Warn if more than 10 switches
#define WARNING_TIME_WINDOW_MS (15 * 60 * 1000)  // in last 15 minutes
unsigned long switchTimestamps[MAX_SWITCH_HISTORY];
int switchHistoryIndex = 0;
bool behaviorWarningActive = false;
unsigned long lastBehaviorWarningTime = 0;

// ========== GLOBAL STATE ==========
struct SystemState {
    bool heatingOn = false;
    bool pumpOn = false;          // Pump state (circulation pump)
    bool pumpManualMode = false;  // Manual pump override (only in manual mode)
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
    
    // Diesel consumption calculation
    float dieselConsumptionPerHour = 2.0;  // Default: 2.0 liters per hour when heating is ON
    
    // Weather & Location
    float latitude = 50.952149;         // Default: Cologne
    float longitude = 7.1229;
    String locationName = "";           // Saved location name (city)

    // Relay configuration (per output)
    // - activeLow: true => ON=LOW, OFF=HIGH
    // - openDrainOff: true => when OFF requires HIGH, use OUTPUT_OPEN_DRAIN + HIGH (floating)
    bool heaterRelayActiveLow = true;
    bool pumpRelayActiveLow = true;
    // OFF mode when the relay should be electrically HIGH:
    // 0 = OUTPUT HIGH (push-pull 3.3V)
    // 1 = OPEN-DRAIN HIGH (floating output driver)
    // 2 = INPUT (hi-Z)
    uint8_t heaterRelayOffMode = 0;
    uint8_t pumpRelayOffMode = 0;

    // Relay GPIO pins (configurable; useful if a pin is damaged or miswired)
    uint8_t heaterRelayPin = DEFAULT_HEATING_RELAY_PIN;
    uint8_t pumpRelayPin = DEFAULT_PUMP_RELAY_PIN;
} state;

// ========== RELAY DRIVER HELPERS ==========
static void applyRelayOutput(uint8_t pin, bool on, bool activeLow, uint8_t offMode, const char* nameForLog) {
    // Determine the electrical level we want on the GPIO pin
    // activeLow: ON => LOW, OFF => HIGH
    bool wantHigh = activeLow ? !on : on;

    // LOW is always actively driven (push-pull), because open-drain LOW is equivalent for our use case.
    if (!wantHigh) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        return;
    }

    // wantHigh == true
    if (!on) {
        // OFF state: choose how HIGH is represented electrically
        switch (offMode) {
            case 2:
                // INPUT (hi-Z)
                pinMode(pin, INPUT);
                break;
            case 1:
                // OPEN-DRAIN HIGH
                pinMode(pin, OUTPUT_OPEN_DRAIN);
                digitalWrite(pin, HIGH);
                break;
            case 0:
            default:
                // OUTPUT HIGH (push-pull)
                pinMode(pin, OUTPUT);
                digitalWrite(pin, HIGH);
                break;
        }
    } else {
        // ON state for active-high relays (wantHigh && on): drive HIGH
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH);
    }

    (void)nameForLog; // reserved for future detailed logging
}

static bool isReservedPinForThisProject(int pin) {
    // Avoid pins used by sensors in this project
    if (pin == ONE_WIRE_BUS || pin == TRIG_PIN || pin == ECHO_PIN) return true;
    // Avoid flash pins (GPIO6-11)
    if (pin >= 6 && pin <= 11) return true;
    // Avoid input-only pins (GPIO34-39)
    if (pin >= 34 && pin <= 39) return true;
    return false;
}

static bool isAllowedRelayPin(int pin) {
    // Conservative set of output-capable pins that are typically safe on ESP32 DevKit boards
    const int allowed[] = {12, 13, 14, 15, 16, 17, 19, 21, 22, 23, 25, 26, 27, 32, 33};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (pin == allowed[i] && !isReservedPinForThisProject(pin)) return true;
    }
    return false;
}

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
unsigned long lastWeatherFetch = 0;
unsigned long bootTime = 0;
unsigned long lastStateChangeTime = 0;
unsigned long lastHeatingOffTime = 0;  // Timestamp when heating was turned OFF (for pump cooldown)
unsigned long scheduledRebootTime = 0;  // Timestamp for scheduled reboot after OTA update
bool rebootScheduled = false;  // Flag to indicate reboot is scheduled
bool otaUpdateInProgress = false;  // Flag to prevent WiFi reconnect during OTA update
unsigned long lastWiFiReconnectAttempt = 0;  // Rate limiting for WiFi reconnect
const unsigned long WIFI_RECONNECT_INTERVAL = 60000;  // Only try reconnect every 60 seconds

// Telegram notification flags
bool sensorErrorNotified = false;
bool tankLowNotified = false;
unsigned long lastTankLowTelegramMs = 0;

// ========== SERIAL MONITOR (WebSocket) ==========
#define LOG_BUFFER_SIZE 200  // Increased to store more boot messages
String logBuffer[LOG_BUFFER_SIZE];
int logBufferIndex = 0;
int logBufferCount = 0;

// Rate limiting for WebSocket - reduced to send more messages
unsigned long lastWebSocketSend = 0;
#define WEBSOCKET_MIN_INTERVAL 10  // Minimum 10ms between sends (was 100ms)

// Queue for pending messages (to avoid losing messages due to rate limiting)
String pendingMessage = "";
bool hasPendingMessage = false;

// Custom print function that sends to both Serial and WebSocket
// IMPORTANT: ALWAYS adds to buffer, even if no WebSocket clients connected
void serialLog(const char* message) {
    // Always print to Serial
    Serial.print(message);
    
    // ALWAYS add to buffer (even if no WebSocket clients yet) - this ensures boot messages are stored
    logBuffer[logBufferIndex] = String(message);
    logBufferIndex = (logBufferIndex + 1) % LOG_BUFFER_SIZE;
    if (logBufferCount < LOG_BUFFER_SIZE) logBufferCount++;
    
    // Send to all WebSocket clients with improved rate limiting (only if clients connected)
    if (ws.count() > 0) {
        unsigned long now = millis();
        if (now - lastWebSocketSend >= WEBSOCKET_MIN_INTERVAL) {
            // Send immediately if enough time has passed
            ws.textAll(message);
            lastWebSocketSend = now;
            hasPendingMessage = false;
        } else {
            // Queue message if rate limit not met yet
            pendingMessage += String(message);
            hasPendingMessage = true;
        }
    }
}

// Call this periodically to flush pending messages
void flushWebSocketMessages() {
    if (hasPendingMessage && ws.count() > 0) {
        unsigned long now = millis();
        if (now - lastWebSocketSend >= WEBSOCKET_MIN_INTERVAL) {
            ws.textAll(pendingMessage.c_str());
            lastWebSocketSend = now;
            pendingMessage = "";
            hasPendingMessage = false;
        }
    }
}

void serialLogLn(const char* message) {
    serialLog(message);
    serialLog("\n");
}

// Helper function for formatted strings (like Serial.printf but also sends to WebSocket)
void serialLogF(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    serialLog(buffer);
}

// ========== TELEGRAM NOTIFICATIONS (FORWARD DECLARATIONS) ==========
bool isTelegramConfigured();
void sendTelegramMessage(String message);

// ========== BEHAVIOR WARNING (FORWARD DECLARATION) ==========
void checkUnusualBehavior();

// WebSocket event handler
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, 
                     AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        serialLogF("WebSocket client #%u connected\n", client->id());
        
        // Send complete log history to new client
        // Send as one properly formatted message to avoid overwhelming the connection
        if (logBufferCount > 0) {
            String fullHistory = "";
            for (int i = 0; i < logBufferCount; i++) {
                int idx = (logBufferIndex - logBufferCount + i + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
                String msg = logBuffer[idx];
                if (msg.length() > 0) {
                    // Ensure each message ends with newline
                    if (msg.charAt(msg.length() - 1) != '\n') {
                        fullHistory += msg + "\n";
                    } else {
                        fullHistory += msg;
                    }
                }
            }
            // Send entire history as one message (WebSocket handles this fine)
            if (fullHistory.length() > 0) {
                client->text(fullHistory);
            }
        }
    } else if (type == WS_EVT_DISCONNECT) {
        serialLogF("WebSocket client #%u disconnected\n", client->id());
    }
}

// ========== PUMP CONTROL (Active-Low) ==========
void setPump(bool on, bool manualOverride = false) {
    bool stateChanged = (on != state.pumpOn);
    
    if (stateChanged) {
        // Build complete message first, then log as one message
        String msg = "[Pump] Setting pump to ";
        msg += (on ? "ON" : "OFF");
        msg += " - GPIO";
        msg += String(state.pumpRelayPin);
        msg += ": ";
        msg += (on ? "LOW (OUTPUT)" : "HIGH (OPEN-DRAIN)");
        serialLogLn(msg.c_str());
        Serial.flush();
    }
    
    state.pumpOn = on;

    // Apply relay output based on configured polarity/off-mode
    applyRelayOutput(state.pumpRelayPin, on, state.pumpRelayActiveLow, state.pumpRelayOffMode, "Pump");
    
    // Verify pin state was set correctly (only log if verification fails)
    delay(50);
    int actualState = digitalRead(state.pumpRelayPin);
    // Read-back check is best-effort (open-drain HIGH may read as HIGH or floating)
    bool expectedLow = state.pumpRelayActiveLow ? on : !on; // activeLow: ON->LOW, activeHigh: OFF->LOW
    bool stateCorrect = expectedLow ? (actualState == LOW) : (actualState != LOW);
    
    if (!stateCorrect && stateChanged) {
        serialLog("[Pump] ⚠️ GPIO22 read back mismatch! Expected: ");
        serialLog(on ? "LOW" : "HIGH");
        serialLog(", Got: ");
        serialLogLn(actualState == LOW ? "LOW" : "HIGH");
    }
    
    if (stateChanged) {
        serialLog("[Pump] Pump ");
        serialLog(on ? "ON" : "OFF");
        serialLog(" - GPIO");
        serialLog(String(state.pumpRelayPin).c_str());
        serialLog(" actual: ");
        serialLogLn(actualState == LOW ? "LOW" : "HIGH");
        Serial.flush();
    }
}

// Forward declarations
void saveSwitchEvents();
void loadSwitchEvents();

// ========== RELAY CONTROL (Active-Low) ==========
void setHeater(bool on, bool saveToNVS = true) {
    bool stateChanged = (on != state.heatingOn);
    
    // Only count as switch if state actually changes
    if (stateChanged) {
        stats.switchCount++;
        stats.todaySwitches++;
        lastStateChangeTime = millis();
        serialLogF("Switch #%lu: Heater %s\n", stats.switchCount, on ? "ON" : "OFF");
        
        // Track switch timestamp for behavior analysis
        switchTimestamps[switchHistoryIndex] = millis();
        switchHistoryIndex = (switchHistoryIndex + 1) % MAX_SWITCH_HISTORY;
        
        // Store switch event with temperatures and tank level
        struct tm timeinfo;
        bool hasTime = getLocalTime(&timeinfo, 100);
        switchEvents[switchEventIndex].isOn = on;
        switchEvents[switchEventIndex].tempVorlauf = state.tempVorlauf;
        switchEvents[switchEventIndex].tempRuecklauf = state.tempRuecklauf;
        switchEvents[switchEventIndex].uptimeMs = millis();
        switchEvents[switchEventIndex].tankLiters = state.tankSensorAvailable ? state.tankLiters : NAN;
        if (hasTime) {
            switchEvents[switchEventIndex].timestamp = mktime(&timeinfo);
        } else {
            switchEvents[switchEventIndex].timestamp = 0;  // No NTP sync
        }
        switchEventIndex = (switchEventIndex + 1) % MAX_SWITCH_EVENTS;
        
        // Save switch events to NVS (persist across reboots)
        saveSwitchEvents();
        
        // Check for unusual behavior
        checkUnusualBehavior();
    }
    
    state.heatingOn = on;
    
    // CRITICAL SAFETY RULE: If heating is turned ON, pump MUST be ON
    // If heating is turned OFF, pump will follow after cooldown period (unless manual override)
    if (on && !state.pumpOn) {
        serialLogLn("[Relay] ⚠️ SAFETY: Heating ON but pump OFF - forcing pump ON!");
        setPump(true, false);
    }
    
    // Build complete message first, then log as one message
    String msg = "[Relay] Setting heater to ";
    msg += (on ? "ON" : "OFF");
    msg += " - GPIO";
    msg += String(state.heaterRelayPin);
    msg += ": ";
    // Log based on expected electrical behavior
    if (state.heaterRelayActiveLow) {
        if (on) {
            msg += "LOW (OUTPUT)";
        } else {
            msg += "HIGH (OFF-MODE)";
        }
    } else {
        msg += (on ? "HIGH (OUTPUT)" : "LOW (OUTPUT)");
    }
    serialLogLn(msg.c_str());
    Serial.flush();
    
    if (on) {
        // Apply configured relay output
        applyRelayOutput(state.heaterRelayPin, true, state.heaterRelayActiveLow, state.heaterRelayOffMode, "Heater");
        // Ensure pump is ON when heating is ON
        if (!state.pumpOn) {
            setPump(true, false);
        }
        lastHeatingOffTime = 0;  // Reset cooldown timer
    } else {
        // Apply configured relay output
        applyRelayOutput(state.heaterRelayPin, false, state.heaterRelayActiveLow, state.heaterRelayOffMode, "Heater");
        // Start cooldown timer for pump (pump will turn OFF after cooldown unless manual override)
        lastHeatingOffTime = millis();
    }
    
    // Verify pin state was set correctly (only log if verification fails)
    delay(50); // Longer delay for relay to respond
    int actualState = digitalRead(state.heaterRelayPin);
    // Read-back check is best-effort (open-drain HIGH may read as HIGH or floating)
    bool expectedLow = state.heaterRelayActiveLow ? on : !on; // activeLow: ON->LOW, activeHigh: OFF->LOW
    bool stateCorrect = expectedLow ? (actualState == LOW) : (actualState != LOW);
    
    if (!stateCorrect) {
        serialLog("[Relay] ⚠️ GPIO23 read back mismatch! Expected: ");
        serialLog(on ? "LOW" : "HIGH");
        serialLog(", Got: ");
        serialLogLn(actualState == LOW ? "LOW" : "HIGH");
    }
    Serial.flush();
    
    if (saveToNVS && state.mode == "manual") {
        prefs.begin("heater", false);
        prefs.putBool("heatingOn", on);
        prefs.end();
    }
    
    // Build complete message first, then log as one message
    String msg2 = "[Relay] Heater ";
    msg2 += (on ? "ON" : "OFF");
    msg2 += " - GPIO";
    msg2 += String(state.heaterRelayPin);
    msg2 += " actual: ";
    msg2 += (actualState == LOW ? "LOW" : "HIGH");
    serialLogLn(msg2.c_str());
    Serial.flush();
    
    // Send Telegram notification on state change
    if (stateChanged && isTelegramConfigured()) {
        String mode = state.mode;
        mode.toUpperCase();
        String emoji = on ? "🔥" : "❄️";
        String status = on ? "EIN" : "AUS";
        String msg = emoji + " Heizung " + status + "\n";
        msg += "Modus: " + mode + "\n";
        if (state.tempVorlauf != -127.0) {
            msg += "🌡️ Vorlauf: " + String(state.tempVorlauf, 1) + "°C";
        }
        sendTelegramMessage(msg);
    }
}

// ========== CHECK FOR UNUSUAL BEHAVIOR ==========
void checkUnusualBehavior() {
    unsigned long now = millis();
    int switchCountInWindow = 0;
    
    // Count switches in the last WARNING_TIME_WINDOW_MS
    for (int i = 0; i < MAX_SWITCH_HISTORY; i++) {
        if (switchTimestamps[i] > 0 && (now - switchTimestamps[i]) < WARNING_TIME_WINDOW_MS) {
            switchCountInWindow++;
        }
    }
    
    // Check if we exceed the threshold
    bool shouldWarn = (switchCountInWindow >= WARNING_THRESHOLD_SWITCHES);
    
    if (shouldWarn && !behaviorWarningActive) {
        behaviorWarningActive = true;
        lastBehaviorWarningTime = now;
        
        Serial.printf("⚠️ WARNUNG: Ungewöhnliches Verhalten erkannt! %d Schaltungen in den letzten 15 Minuten.\n", switchCountInWindow);
        
        // Send Telegram warning if configured
        if (isTelegramConfigured()) {
            String msg = "⚠️ WARNUNG: Ungewöhnliches Verhalten!\n";
            msg += String(switchCountInWindow) + " Schaltungen in den letzten 15 Minuten.\n";
            msg += "Bitte Heizungsanlage prüfen!";
            sendTelegramMessage(msg);
        }
    } else if (!shouldWarn && behaviorWarningActive) {
        // Clear warning if behavior normalized
        behaviorWarningActive = false;
        Serial.println("✅ Verhalten normalisiert - Warnung aufgehoben");
    }
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
    // Capture idle state before triggering (helps diagnose wiring/floating pins)
    lastEchoBefore = digitalRead(ECHO_PIN);

    // Send 10us pulse to TRIG
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    
    // Read ECHO pulse duration (timeout after 30ms)
    long duration = pulseIn(ECHO_PIN, HIGH, ULTRASONIC_TIMEOUT);
    lastUltrasonicDurationUs = (unsigned long)duration;
    lastEchoAfter = digitalRead(ECHO_PIN);
    
    // No echo received = sensor error
    if (duration == 0) {
        lastTankErrorCode = 1;
        lastUltrasonicDistanceCm = -1.0f;
        return -1.0;
    }
    
    // Calculate distance in cm (speed of sound: 343 m/s = 0.0343 cm/us)
    // Distance = (duration / 2) * 0.0343
    float distance = (duration * 0.0343) / 2.0;
    
    // Sanity check: JSN-SR04T range 25cm-450cm
    if (distance < 2.0 || distance > 500.0) {
        lastTankErrorCode = 2;
        lastUltrasonicDistanceCm = distance;
        return -1.0;
    }

    lastTankErrorCode = 0;
    lastUltrasonicDistanceCm = distance;
    return distance;
}

void updateTankLevel() {
    float distance = readTankDistance();
    
    if (distance < 0) {
        // Sensor error / missed echo: don't immediately flip to "unavailable" because JSN-SR04T can miss pulses.
        // Keep last known good value for a short grace period to avoid UI flapping.
        const unsigned long GRACE_MS = 15000; // 15s
        if (lastTankGoodMs > 0 && (millis() - lastTankGoodMs) <= GRACE_MS) {
            state.tankSensorAvailable = true;
            state.tankDistance = lastTankGoodDistanceCm;
            // Keep liters/percent as-is (based on last good distance)
        } else {
            state.tankSensorAvailable = false;
            state.tankDistance = -1.0;
            state.tankLiters = 0.0;
            state.tankPercent = 0;
        }
        return;
    }
    
    // Sensor working
    state.tankSensorAvailable = true;
    state.tankDistance = distance;
    lastTankGoodMs = millis();
    lastTankGoodDistanceCm = distance;
    
    // Calculate fill level
    // fillHeight = tankHeight - distance (distance from sensor to surface)
    float fillHeight = state.tankHeight - distance;
    
    // Clamp to valid range
    if (fillHeight < 0) fillHeight = 0;
    if (fillHeight > state.tankHeight) fillHeight = state.tankHeight;
    
    // If tank geometry isn't configured, don't compute percent/liters (avoid div-by-zero / spam).
    if (state.tankHeight <= 0.1f || state.tankCapacity <= 0.1f) {
        state.tankPercent = 0;
        state.tankLiters = 0.0f;
        return;
    }

    // Calculate percentage
    state.tankPercent = (int)((fillHeight / state.tankHeight) * 100.0);
    
    // Calculate liters (assuming cylindrical/rectangular tank)
    state.tankLiters = (fillHeight / state.tankHeight) * state.tankCapacity;
    
    // Round to 1 decimal
    state.tankLiters = round(state.tankLiters * 10) / 10.0;
    
    // Check for low tank level (< 20%)
    // Only notify when heating is active (avoid spam when heating is OFF).
    // Also apply a minimum interval to avoid repeated notifications due to sensor flapping/reboots.
    const unsigned long MIN_TANK_LOW_TELEGRAM_MS = 6UL * 60UL * 60UL * 1000UL; // 6 hours
    const bool intervalOk = (lastTankLowTelegramMs == 0) || (millis() - lastTankLowTelegramMs >= MIN_TANK_LOW_TELEGRAM_MS);
    if (state.heatingOn && state.tankPercent < 20 && !tankLowNotified && intervalOk && isTelegramConfigured()) {
        String msg = "🪫 TANK NIEDRIG!\n\n";
        msg += "Füllstand: " + String(state.tankPercent) + "% (" + String(state.tankLiters, 1) + "L)\n";
        msg += "Bitte nachfüllen!";
        sendTelegramMessage(msg);
        tankLowNotified = true;
        lastTankLowTelegramMs = millis();
    } else if (state.tankPercent >= 25 && tankLowNotified) {
        // Reset flag when tank is refilled (25% to have some hysteresis)
        tankLowNotified = false;
    }
}

// ========== REVERSE GEOCODING ==========
String fetchLocationName(float lat, float lon) {
    HTTPClient http;
    
    // OpenStreetMap Nominatim API (free, no API key needed)
    // Use HTTPS to avoid HTTP 301 redirect
    String url = "https://nominatim.openstreetmap.org/reverse?";
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
        StaticJsonDocument<512> doc;
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
// Internal function to actually fetch weather data (called by fetchWeatherData)
void doFetchWeatherData(bool forceRefresh = false) {
    // Check if WiFi is connected
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    
    // Silent weather fetch (no logging to reduce WebSocket spam)
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
        
        // Parse JSON (Open-Meteo responses can be large)
        StaticJsonDocument<4096> doc;
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
            lastWeatherFetch = millis();
            
            // Fetch location name (always fetch if forceRefresh is true or not already set)
            if (forceRefresh || weather.locationName == "" || weather.locationName == "Unbekannter Ort") {
                weather.locationName = fetchLocationName(state.latitude, state.longitude);
            }
            
            // Silent success (no logging to reduce WebSocket spam)
        } else {
            serialLog("[Weather] ❌ JSON parse error: ");
            serialLogLn(error.c_str());
            weather.valid = false;
        }
    } else {
        serialLog("[Weather] ❌ HTTP error: ");
        serialLogLn(String(httpCode).c_str());
        weather.valid = false;
    }
    
    http.end();
}

// Public function with time-based throttling
void fetchWeatherData() {
    // Check if WiFi is connected
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    
    unsigned long now = millis();
    
    // If weather data is invalid, fetch immediately (don't wait for interval)
    if (!weather.valid) {
        // Only fetch if we haven't tried in the last 30 seconds (avoid spam)
        if (now - lastWeatherFetch < 30000) {
            return;
        }
        doFetchWeatherData(false);
        return;
    }
    
    // Only check/fetch weather every 10 minutes (avoid spamming serial output)
    if (now - lastWeatherFetch < WEATHER_UPDATE_INTERVAL) {
        return;
    }
    
    // Check cache validity (10 min)
    if (weather.valid && (now - weather.lastUpdate < WEATHER_UPDATE_INTERVAL)) {
        // Silent cache use (no logging)
        return;
    }
    
    doFetchWeatherData(false);
}

// ========== TELEGRAM NOTIFICATIONS ==========
bool isTelegramConfigured() {
    // Check if Telegram is configured (bot token is not the placeholder)
    return (String(TELEGRAM_BOT_TOKEN) != "YOUR_BOT_TOKEN_HERE" && 
            String(TELEGRAM_BOT_TOKEN).length() > 10);
}

void sendTelegramMessage(String message) {
    if (!isTelegramConfigured()) {
        serialLogLn("[Telegram] Not configured, skipping notification");
        return;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        serialLogLn("[Telegram] WiFi not connected, skipping notification");
        return;
    }
    
    serialLog("[Telegram] Sending: ");
    serialLogLn(message.c_str());
    
    HTTPClient http;
    
    String url = "https://api.telegram.org/bot";
    url += TELEGRAM_BOT_TOKEN;
    url += "/sendMessage";
    
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    // Escape special characters in message
    message.replace("\"", "\\\"");
    message.replace("\n", "\\n");
    
    String payload = "{\"chat_id\":\"";
    payload += TELEGRAM_CHAT_ID;
    payload += "\",\"text\":\"";
    payload += message;
    payload += "\"}";
    
    int httpCode = http.POST(payload);
    
    if (httpCode == HTTP_CODE_OK) {
        serialLogLn("[Telegram] ✅ Message sent successfully");
    } else {
        serialLog("[Telegram] ❌ Error: ");
        serialLogLn(String(httpCode).c_str());
    }
    
    http.end();
}

// ========== INITIALIZE TEMPERATURE SENSORS ==========
void initSensors() {
    serialLogF("[Sensor] Initializing DS18B20 sensors on GPIO%d...\n", ONE_WIRE_BUS);
    if (!oneWire) {
        oneWire = new OneWire(ONE_WIRE_BUS);
    }
    if (!sensors) {
        sensors = new DallasTemperature(oneWire);
    }
    sensors->begin();
    
    // Wait a bit for sensors to stabilize
    delay(100);
    
    int deviceCount = sensors->getDeviceCount();
    
    serialLogF("[Sensor] Found %d DS18B20 sensor(s) on OneWire bus\n", deviceCount);
    
    if (deviceCount == 0) {
        serialLogLn("[Sensor] ERROR: No sensors found! Check wiring:");
        serialLogLn("  - Red wire (VDD) -> 3.3V");
        serialLogLn("  - Black wire (GND) -> GND");
        serialLogF("  - Yellow wire (DQ) -> GPIO%d\n", ONE_WIRE_BUS);
        serialLogF("  - 4.7kΩ resistor between GPIO%d and 3.3V\n", ONE_WIRE_BUS);
    }
    
    if (deviceCount >= 1) {
        sensors->getAddress(sensor1Address, 0);
        sensor1Found = true;
        // Build complete address string first, then log as one message
        String addrStr = "[Sensor] Sensor 1 (Vorlauf) address: ";
        for (uint8_t i = 0; i < 8; i++) {
            char hexStr[3];
            sprintf(hexStr, "%02X", sensor1Address[i]);
            addrStr += hexStr;
        }
        serialLogLn(addrStr.c_str());
    } else {
        sensor1Found = false;
    }
    
    if (deviceCount >= 2) {
        sensors->getAddress(sensor2Address, 1);
        sensor2Found = true;
        // Build complete address string first, then log as one message
        String addrStr = "[Sensor] Sensor 2 (Rücklauf) address: ";
        for (uint8_t i = 0; i < 8; i++) {
            char hexStr[3];
            sprintf(hexStr, "%02X", sensor2Address[i]);
            addrStr += hexStr;
        }
        serialLogLn(addrStr.c_str());
    } else {
        sensor2Found = false;
        if (deviceCount < 2) {
            serialLogLn("[Sensor] WARNING: Less than 2 sensors found. Using single sensor for both values.");
        }
    }
}

// ========== TEMPERATURE READING ==========
void readTemperatures() {
    if (!sensors) {
        return;
    }
    sensors->requestTemperatures();
    
    // Read sensor 1 (Vorlauf)
    if (sensor1Found) {
        float temp = sensors->getTempC(sensor1Address);
        if (temp != DEVICE_DISCONNECTED_C && temp >= -55.0 && temp <= 125.0) {
            state.tempVorlauf = temp;
        } else {
            Serial.printf("[Sensor] Sensor 1 read error: %.2f°C (disconnected: %d)\n", temp, (temp == DEVICE_DISCONNECTED_C));
            state.tempVorlauf = NAN;
        }
    } else {
        Serial.println("[Sensor] Sensor 1 not found!");
        state.tempVorlauf = NAN;
    }
    
    // Read sensor 2 (Rücklauf)
    if (sensor2Found) {
        float temp = sensors->getTempC(sensor2Address);
        if (temp != DEVICE_DISCONNECTED_C && temp >= -55.0 && temp <= 125.0) {
            state.tempRuecklauf = temp;
        } else {
            Serial.printf("[Sensor] Sensor 2 read error: %.2f°C (disconnected: %d)\n", temp, (temp == DEVICE_DISCONNECTED_C));
            state.tempRuecklauf = NAN;
        }
    } else if (sensor1Found) {
        // Fallback: If only one sensor, use it for both
        state.tempRuecklauf = state.tempVorlauf;
    } else {
        Serial.println("[Sensor] Sensor 2 not found!");
        state.tempRuecklauf = NAN;
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
    
    // Hysteresis logic (like W1209)
    // Turn ON if below EIN temperature
    if (state.tempRuecklauf <= state.tempOn && !state.heatingOn) {
        Serial.printf("AUTO: Rücklauf %.1f°C <= %.1f°C, turning heater ON\n", 
                     state.tempRuecklauf, state.tempOn);
        setHeater(true, false);
    } 
    // Turn OFF if above AUS temperature
    else if (state.tempRuecklauf >= state.tempOff && state.heatingOn) {
        Serial.printf("AUTO: Rücklauf %.1f°C >= %.1f°C, turning heater OFF\n", 
                     state.tempRuecklauf, state.tempOff);
        setHeater(false, false);
    }
    // Between EIN and AUS: maintain current state (hysteresis zone)
}

// ========== PUMP COOLDOWN LOGIC ==========
void handlePumpCooldown() {
    // Only handle cooldown if heating is OFF and we're not in manual mode with manual pump override
    if (state.heatingOn) {
        return;  // Heating is ON, pump should be ON (handled by setHeater)
    }
    
    // In manual mode: if pumpManualMode is true, keep pump ON regardless of heating state
    if (state.mode == "manual" && state.pumpManualMode) {
        if (!state.pumpOn) {
            setPump(true, true);  // Turn pump ON due to manual override
        }
        return;
    }
    
    // Check if cooldown period has elapsed
    if (lastHeatingOffTime > 0 && state.pumpOn) {
        unsigned long elapsed = millis() - lastHeatingOffTime;
        
        // After cooldown period, turn pump OFF (unless manual override in manual mode)
        if (elapsed >= PUMP_COOLDOWN_MS) {
            if (!(state.mode == "manual" && state.pumpManualMode)) {
                serialLogF("[Pump] Cooldown period (%lu seconds) elapsed, turning pump OFF\n", PUMP_COOLDOWN_MS / 1000);
                setPump(false, false);
                lastHeatingOffTime = 0;  // Reset timer
            }
        } else {
            // Still in cooldown period
            unsigned long remaining = (PUMP_COOLDOWN_MS - elapsed) / 1000;
            // Only log every 30 seconds to avoid spam
            static unsigned long lastCooldownLog = 0;
            if (millis() - lastCooldownLog > 30000) {
                serialLogF("[Pump] Cooldown: %lu seconds remaining\n", remaining);
                lastCooldownLog = millis();
            }
        }
    }
}

// ========== FAILSAFE CHECK ==========
void checkFailsafe() {
    // If both sensors fail, turn off both heating and pump
    if (isnan(state.tempVorlauf) && isnan(state.tempRuecklauf)) {
        if (state.heatingOn) {
            Serial.println("FAILSAFE: All sensors failed, turning heater OFF");
            setHeater(false);
        }
        // If user explicitly enabled manual pump override in manual mode, don't fight it.
        // Otherwise, turn pump OFF for safety to avoid oscillation with cooldown/manual logic.
        if (state.pumpOn && !(state.mode == "manual" && state.pumpManualMode)) {
            Serial.println("FAILSAFE: All sensors failed, turning pump OFF");
            setPump(false, false);
        }
        
        // Send Telegram notification once
        if (!sensorErrorNotified && isTelegramConfigured()) {
            sendTelegramMessage("⚠️ SENSOR-FEHLER!\n\nBeide Temperatursensoren ausgefallen.\nHeizung und Pumpe wurden automatisch deaktiviert.");
            sensorErrorNotified = true;
        }
    } else {
        // Reset flag when sensors are working again
        if (sensorErrorNotified) {
            if (isTelegramConfigured()) {
                sendTelegramMessage("✅ Sensoren wieder OK\n\n🌡️ Vorlauf: " + String(state.tempVorlauf, 1) + "°C");
            }
            sensorErrorNotified = false;
        }
    }
    
    // CRITICAL SAFETY CHECK: Ensure heating is never ON without pump
    if (state.heatingOn && !state.pumpOn) {
        serialLogLn("[FAILSAFE] ⚠️ CRITICAL: Heating ON but pump OFF - forcing pump ON!");
        setPump(true, false);
    }
}

// ========== LOAD SETTINGS FROM NVS ==========
void loadSettings() {
    prefs.begin("heater", true);
    
    state.heatingOn = prefs.getBool("heatingOn", false);
    state.pumpOn = prefs.getBool("pumpOn", false);
    state.pumpManualMode = prefs.getBool("pumpManualMode", false);
    state.mode = prefs.getString("mode", "manual");
    state.tempOn = prefs.getFloat("tempOn", 30.0);
    state.tempOff = prefs.getFloat("tempOff", 40.0);
    state.frostProtectionEnabled = prefs.getBool("frostEnabled", false);
    state.frostProtectionTemp = prefs.getFloat("frostTemp", 8.0);
    
    // Load tank configuration
    state.tankHeight = prefs.getFloat("tankHeight", 100.0);
    state.tankCapacity = prefs.getFloat("tankCapacity", 1000.0);
    
    // Load diesel consumption setting
    state.dieselConsumptionPerHour = prefs.getFloat("dieselPerHour", 2.0);
    
    // Load location (default: Cologne, Germany)
    state.latitude = prefs.getFloat("latitude", 50.952149);
    state.longitude = prefs.getFloat("longitude", 7.1229);
    state.locationName = prefs.getString("locationName", "");

    // Load relay configuration (per output)
    // Only fall back to defaults if the key does not exist (so we can change defaults safely in new versions).
    state.heaterRelayActiveLow = prefs.isKey("hActLow") ? prefs.getBool("hActLow", true) : true;
    state.pumpRelayActiveLow = prefs.isKey("pActLow") ? prefs.getBool("pActLow", true) : true;

    // OFF mode migration:
    // - New keys: hOffMode/pOffMode (0..2)
    // - Old keys: hODOff/pODOff (bool) where true means "floating"; map to INPUT (2), false to OUTPUT HIGH (0)
    if (prefs.isKey("hOffMode")) {
        state.heaterRelayOffMode = (uint8_t)prefs.getUChar("hOffMode", 0);
    } else if (prefs.isKey("hODOff")) {
        state.heaterRelayOffMode = prefs.getBool("hODOff", false) ? 2 : 0;
    } else {
        state.heaterRelayOffMode = 0;
    }

    if (prefs.isKey("pOffMode")) {
        state.pumpRelayOffMode = (uint8_t)prefs.getUChar("pOffMode", 0);
    } else if (prefs.isKey("pODOff")) {
        state.pumpRelayOffMode = prefs.getBool("pODOff", false) ? 2 : 0;
    } else {
        state.pumpRelayOffMode = 0;
    }

    // Load relay pins (fallback to defaults if key missing/invalid)
    int hPin = prefs.isKey("hPin") ? (int)prefs.getUChar("hPin", DEFAULT_HEATING_RELAY_PIN) : DEFAULT_HEATING_RELAY_PIN;
    int pPin = prefs.isKey("pPin") ? (int)prefs.getUChar("pPin", DEFAULT_PUMP_RELAY_PIN) : DEFAULT_PUMP_RELAY_PIN;
    if (isAllowedRelayPin(hPin)) state.heaterRelayPin = (uint8_t)hPin;
    if (isAllowedRelayPin(pPin)) state.pumpRelayPin = (uint8_t)pPin;
    
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
    
    serialLogLn("=== Settings loaded from NVS ===");
    serialLogF("Mode: %s\n", state.mode.c_str());
    serialLogF("Heating: %s\n", state.heatingOn ? "ON" : "OFF");
    serialLogF("Pump: %s\n", state.pumpOn ? "ON" : "OFF");
    serialLogF("Pump Manual Mode: %s\n", state.pumpManualMode ? "ON" : "OFF");
    serialLogF("Temp ON: %.1f°C\n", state.tempOn);
    serialLogF("Temp OFF: %.1f°C\n", state.tempOff);
    serialLogF("Frost Protection: %s (%.1f°C)\n", 
                 state.frostProtectionEnabled ? "ON" : "OFF", 
                 state.frostProtectionTemp);
    serialLogF("Schedules loaded: %d\n", MAX_SCHEDULES);
    
    // Load switch events history
    loadSwitchEvents();
    
    // Safety note:
    // Do NOT silently flip pumpOn here. We will enforce "heating ON => pump ON" during setup()
    // and actually apply the GPIO state there. This avoids state/relay mismatches after reboot.
    if (state.heatingOn && !state.pumpOn) {
        serialLogLn("⚠️ SAFETY: Heating was ON but pump OFF in NVS (will force pump ON during setup)");
    }
}

// ========== SWITCH EVENTS PERSISTENCE ==========
void saveSwitchEvents() {
    prefs.begin("switchevts", false);
    // Save current index
    prefs.putUChar("idx", switchEventIndex);
    // Save all events (as binary blob)
    size_t dataSize = MAX_SWITCH_EVENTS * sizeof(SwitchEvent);
    prefs.putBytes("events", switchEvents, dataSize);
    prefs.end();
}

void loadSwitchEvents() {
    prefs.begin("switchevts", true);
    if (prefs.isKey("idx") && prefs.isKey("events")) {
        switchEventIndex = prefs.getUChar("idx", 0);
        size_t dataSize = MAX_SWITCH_EVENTS * sizeof(SwitchEvent);
        size_t storedSize = prefs.getBytesLength("events");
        if (storedSize == dataSize) {
            prefs.getBytes("events", switchEvents, dataSize);
            serialLogF("[SwitchEvents] Loaded %d events from NVS\n", MAX_SWITCH_EVENTS);
        } else {
            serialLogF("[SwitchEvents] Size mismatch: expected %d, got %d\n", dataSize, storedSize);
        }
    } else {
        // First run: initialize with zeros
        memset(switchEvents, 0, sizeof(switchEvents));
        switchEventIndex = 0;
        serialLogLn("[SwitchEvents] No saved events, initialized empty");
    }
    prefs.end();
}

// Load relay configuration early in setup, before configuring GPIO directions.
// This reduces the time the relay input might be left in a wrong state after a reset.
void loadRelayConfigEarly() {
    prefs.begin("heater", true);
    state.heaterRelayActiveLow = prefs.isKey("hActLow") ? prefs.getBool("hActLow", true) : true;
    state.pumpRelayActiveLow = prefs.isKey("pActLow") ? prefs.getBool("pActLow", true) : true;

    if (prefs.isKey("hOffMode")) {
        state.heaterRelayOffMode = (uint8_t)prefs.getUChar("hOffMode", 0);
    } else if (prefs.isKey("hODOff")) {
        state.heaterRelayOffMode = prefs.getBool("hODOff", false) ? 2 : 0;
    } else {
        state.heaterRelayOffMode = 0;
    }

    if (prefs.isKey("pOffMode")) {
        state.pumpRelayOffMode = (uint8_t)prefs.getUChar("pOffMode", 0);
    } else if (prefs.isKey("pODOff")) {
        state.pumpRelayOffMode = prefs.getBool("pODOff", false) ? 2 : 0;
    } else {
        state.pumpRelayOffMode = 0;
    }

    // Relay pins early (so setup() can initialize correct GPIOs)
    int hPin = prefs.isKey("hPin") ? (int)prefs.getUChar("hPin", DEFAULT_HEATING_RELAY_PIN) : DEFAULT_HEATING_RELAY_PIN;
    int pPin = prefs.isKey("pPin") ? (int)prefs.getUChar("pPin", DEFAULT_PUMP_RELAY_PIN) : DEFAULT_PUMP_RELAY_PIN;
    if (isAllowedRelayPin(hPin)) state.heaterRelayPin = (uint8_t)hPin;
    if (isAllowedRelayPin(pPin)) state.pumpRelayPin = (uint8_t)pPin;
    prefs.end();
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
    
    // Save diesel consumption setting
    prefs.putFloat("dieselPerHour", state.dieselConsumptionPerHour);
    
    // Save location
    prefs.putFloat("latitude", state.latitude);
    prefs.putFloat("longitude", state.longitude);
    prefs.putString("locationName", state.locationName);

    // Save relay configuration (per output)
    prefs.putBool("hActLow", state.heaterRelayActiveLow);
    prefs.putBool("pActLow", state.pumpRelayActiveLow);
    prefs.putUChar("hOffMode", state.heaterRelayOffMode);
    prefs.putUChar("pOffMode", state.pumpRelayOffMode);
    prefs.putUChar("hPin", state.heaterRelayPin);
    prefs.putUChar("pPin", state.pumpRelayPin);
    
    // Save heating and pump state for all modes (needed to restore after reboot)
    prefs.putBool("heatingOn", state.heatingOn);
    prefs.putBool("pumpOn", state.pumpOn);
    prefs.putBool("pumpManualMode", state.pumpManualMode);
    
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
    serialLogLn("=== WiFi Initialization (Robust Mode) ===");
    
    // STEP 1: Complete WiFi stack reset - start from scratch
    WiFi.persistent(false);  // Don't save credentials to NVS - always use secrets.h
    WiFi.disconnect(true);   // true = erase ALL stored credentials
    delay(500);
    
    WiFi.mode(WIFI_OFF);     // Turn WiFi completely OFF
    delay(500);
    
    // STEP 2: Fresh start - initialize WiFi in Station mode
    WiFi.mode(WIFI_STA);
    delay(300);
    
    // STEP 3: Configure WiFi settings
    WiFi.setAutoConnect(false);    // NEVER auto-connect on boot
    WiFi.setAutoReconnect(true);   // Auto-reconnect if connection lost during runtime
    WiFi.setSleep(false);          // Disable WiFi sleep mode for stability
    
    // STEP 4: Print diagnostics
    serialLogF("ESP32 MAC Address: %s\n", WiFi.macAddress().c_str());
    serialLogF("Connecting to: '%s'\n", WIFI_SSID);
    serialLogF("Password length: %d characters\n", strlen(WIFI_PASSWORD));
    
    // STEP 5: Try connection with multiple strategies
    const int MAX_RETRIES = 3;
    bool connected = false;
    
    for (int retry = 0; retry < MAX_RETRIES && !connected; retry++) {
        if (retry > 0) {
            serialLogF("\n--- Retry %d/%d ---\n", retry + 1, MAX_RETRIES);
            
            // Complete reset before retry
            WiFi.disconnect(true);
            delay(1000);
            WiFi.mode(WIFI_OFF);
            delay(500);
            WiFi.mode(WIFI_STA);
            delay(500);
            WiFi.setAutoConnect(false);
            WiFi.setAutoReconnect(true);
        }
        
        // Start connection attempt
        serialLog("Connecting");
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        
        // Wait for connection with progress indication
        unsigned long startTime = millis();
        int dotCount = 0;
        
        while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < WIFI_TIMEOUT_MS) {
            delay(500);
            serialLog(".");
            dotCount++;
            
            if (dotCount % 10 == 0) {
                int elapsed = (millis() - startTime) / 1000;
                int remaining = (WIFI_TIMEOUT_MS - (millis() - startTime)) / 1000;
                serialLogF(" (%ds / %ds remaining)", elapsed, remaining);
                serialLogLn("");
                serialLog("Still connecting");
            }
        }
        serialLogLn("");
        
        // Check connection status
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            
            // Wait a bit to ensure connection is stable
            delay(1000);
            
            // Verify connection is still active
            if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0,0,0,0)) {
                serialLogLn("✅ WiFi connected successfully!");
                serialLogF("   IP Address: %s\n", WiFi.localIP().toString().c_str());
                serialLogF("   Gateway:    %s\n", WiFi.gatewayIP().toString().c_str());
                serialLogF("   Subnet:     %s\n", WiFi.subnetMask().toString().c_str());
                serialLogF("   RSSI:       %d dBm\n", WiFi.RSSI());
                
                // Additional stability delay
                delay(1000);
                return true;
            } else {
                serialLogLn("⚠️ Connection lost immediately after connect");
                connected = false;
            }
        } else {
            serialLogF("❌ Connection attempt %d failed\n", retry + 1);
            serialLogF("   WiFi status: %d\n", WiFi.status());
        }
    }
    
    // All attempts failed
    serialLogLn("❌ WiFi connection FAILED after all attempts!");
    serialLogLn("WiFi Diagnostics:");
    // WiFi.printDiag sends directly to Serial - capture it manually
    Serial.print("WiFi Diagnostics: ");
    WiFi.printDiag(Serial);
    Serial.println();
    serialLogF("SSID tried: '%s'\n", WIFI_SSID);
    serialLogF("Password length: %d\n", strlen(WIFI_PASSWORD));
    serialLogF("MAC Address: %s\n", WiFi.macAddress().c_str());
    
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
    
    // Serve manifest.json for PWA
    server.on("/manifest.json", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LittleFS, "/manifest.json", "application/json");
    });
    
    // Serve service worker
    server.on("/sw.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(LittleFS, "/sw.js", "application/javascript");
    });
    
    // API: Get current status
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
        // NOTE: This payload includes nested arrays/objects (schedules) and optional data.
        // Increase capacity to avoid truncated/missing fields which can break the frontend.
        StaticJsonDocument<2048> doc;
        
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
        doc["pump"] = state.pumpOn;
        doc["pumpManualMode"] = state.pumpManualMode;
        doc["mode"] = state.mode;
        doc["tempOn"] = state.tempOn;
        doc["tempOff"] = state.tempOff;
        doc["relayActiveLow"] = true;
        doc["heaterRelayActiveLow"] = state.heaterRelayActiveLow;
        doc["pumpRelayActiveLow"] = state.pumpRelayActiveLow;
        doc["heaterRelayOffMode"] = state.heaterRelayOffMode;
        doc["pumpRelayOffMode"] = state.pumpRelayOffMode;
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
        doc["behaviorWarning"] = behaviorWarningActive;
        
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
        doc["dieselConsumptionPerHour"] = state.dieselConsumptionPerHour;
        
        // Location
        doc["latitude"] = state.latitude;
        doc["longitude"] = state.longitude;
        
        // Location name (prefer saved name, then weather location name)
        if (state.locationName.length() > 0 && state.locationName != "Unbekannter Ort") {
            doc["locationName"] = state.locationName;
        } else if (weather.locationName.length() > 0 && weather.locationName != "Unbekannter Ort") {
            doc["locationName"] = weather.locationName;
        }
        
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
        serialLogLn("[API] /api/toggle called");
        Serial.flush();
        
        if (!request->authenticate(AUTH_USER, AUTH_PASS)) {
            serialLogLn("[API] Authentication failed");
            Serial.flush();
            return request->requestAuthentication();
        }
        
        if (millis() - lastToggleTime < DEBOUNCE_MS) {
            serialLogLn("[API] Too many requests (debounce)");
            Serial.flush();
            request->send(429, "application/json", "{\"error\":\"Too many requests\"}");
            return;
        }
        
        if (state.mode != "manual") {
            serialLog("[API] Not in manual mode (current: ");
            serialLog(state.mode.c_str());
            serialLogLn(")");
            Serial.flush();
            request->send(400, "application/json", "{\"error\":\"Not in manual mode\"}");
            return;
        }
        
        serialLog("[API] Toggling heater from ");
        serialLog(state.heatingOn ? "ON" : "OFF");
        serialLog(" to ");
        serialLogLn(state.heatingOn ? "OFF" : "ON");
        
        // Read current pin state BEFORE toggle
        int pinBefore = digitalRead(state.heaterRelayPin);
        serialLogF("[API] GPIO%d BEFORE toggle: %s\n", (int)state.heaterRelayPin, pinBefore == LOW ? "LOW" : "HIGH");
        Serial.flush();
        
        setHeater(!state.heatingOn);
        
        // Read pin state AFTER toggle
        delay(100);
        int pinAfter = digitalRead(state.heaterRelayPin);
        serialLogF("[API] GPIO%d AFTER toggle: %s\n", (int)state.heaterRelayPin, pinAfter == LOW ? "LOW" : "HIGH");
        Serial.flush();
        lastToggleTime = millis();
        
        StaticJsonDocument<64> doc;
        doc["success"] = true;
        doc["heating"] = state.heatingOn;
        doc["pump"] = state.pumpOn;  // Include pump state in response
        
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });
    
    // API: Toggle pump (manual mode only)
    server.on("/api/toggle-pump", HTTP_GET, [](AsyncWebServerRequest *request) {
        serialLogLn("[API] /api/toggle-pump called");
        Serial.flush();
        
        if (!request->authenticate(AUTH_USER, AUTH_PASS)) {
            serialLogLn("[API] Authentication failed");
            Serial.flush();
            return request->requestAuthentication();
        }
        
        if (millis() - lastToggleTime < DEBOUNCE_MS) {
            serialLogLn("[API] Too many requests (debounce)");
            Serial.flush();
            request->send(429, "application/json", "{\"error\":\"Too many requests\"}");
            return;
        }
        
        if (state.mode != "manual") {
            serialLog("[API] Not in manual mode (current: ");
            serialLog(state.mode.c_str());
            serialLogLn(")");
            Serial.flush();
            request->send(400, "application/json", "{\"error\":\"Not in manual mode\"}");
            return;
        }
        
        // Safety check: Cannot turn pump OFF if heating is ON
        if (state.heatingOn && state.pumpOn) {
            serialLogLn("[API] Cannot turn pump OFF while heating is ON");
            request->send(400, "application/json", "{\"error\":\"Cannot turn pump OFF while heating is ON\"}");
            return;
        }
        
        bool newPumpState = !state.pumpOn;
        
        serialLog("[API] Toggling pump from ");
        serialLog(state.pumpOn ? "ON" : "OFF");
        serialLog(" to ");
        serialLogLn(newPumpState ? "ON" : "OFF");

        // IMPORTANT: Set manual override flag BEFORE switching the GPIO.
        // The main loop can run concurrently and cooldown logic may otherwise turn the pump OFF immediately.
        state.pumpManualMode = newPumpState;
        if (state.pumpManualMode) {
            // Manual pump ON should not be affected by a stale heating cooldown timestamp.
            lastHeatingOffTime = 0;
        }
        setPump(newPumpState, true);  // Manual override
        
        // Save pump state to NVS
        prefs.begin("heater", false);
        prefs.putBool("pumpOn", state.pumpOn);
        prefs.putBool("pumpManualMode", state.pumpManualMode);
        prefs.end();
        
        lastToggleTime = millis();
        
        StaticJsonDocument<128> doc;
        doc["success"] = true;
        doc["pump"] = state.pumpOn;
        doc["pumpManualMode"] = state.pumpManualMode;
        
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
                    serialLogF("Mode changed to: %s\n", newMode.c_str());
                    
                    if (newMode != "manual") {
                        setHeater(false, false);
                        // Reset pump manual mode when leaving manual mode
                        state.pumpManualMode = false;
                    }
                }
            }

            // Relay configuration (validate types strictly)
            if (doc.containsKey("heaterRelayActiveLow") && doc["heaterRelayActiveLow"].is<bool>()) {
                bool v = doc["heaterRelayActiveLow"].as<bool>();
                if (v != state.heaterRelayActiveLow) {
                    state.heaterRelayActiveLow = v;
                    changed = true;
                }
            }
            if (doc.containsKey("pumpRelayActiveLow") && doc["pumpRelayActiveLow"].is<bool>()) {
                bool v = doc["pumpRelayActiveLow"].as<bool>();
                if (v != state.pumpRelayActiveLow) {
                    state.pumpRelayActiveLow = v;
                    changed = true;
                }
            }
            // New OFF mode (0..2)
            if (doc.containsKey("heaterRelayOffMode") && doc["heaterRelayOffMode"].is<int>()) {
                int v = doc["heaterRelayOffMode"].as<int>();
                if (v >= 0 && v <= 2 && (uint8_t)v != state.heaterRelayOffMode) {
                    state.heaterRelayOffMode = (uint8_t)v;
                    changed = true;
                }
            }
            if (doc.containsKey("pumpRelayOffMode") && doc["pumpRelayOffMode"].is<int>()) {
                int v = doc["pumpRelayOffMode"].as<int>();
                if (v >= 0 && v <= 2 && (uint8_t)v != state.pumpRelayOffMode) {
                    state.pumpRelayOffMode = (uint8_t)v;
                    changed = true;
                }
            }

            // Backward compatible booleans (map: true => INPUT(2), false => OUTPUT_HIGH(0))
            if (doc.containsKey("heaterRelayOpenDrainOff") && doc["heaterRelayOpenDrainOff"].is<bool>()) {
                bool v = doc["heaterRelayOpenDrainOff"].as<bool>();
                uint8_t mapped = v ? 2 : 0;
                if (mapped != state.heaterRelayOffMode) {
                    state.heaterRelayOffMode = mapped;
                    changed = true;
                }
            }
            if (doc.containsKey("pumpRelayOpenDrainOff") && doc["pumpRelayOpenDrainOff"].is<bool>()) {
                bool v = doc["pumpRelayOpenDrainOff"].as<bool>();
                uint8_t mapped = v ? 2 : 0;
                if (mapped != state.pumpRelayOffMode) {
                    state.pumpRelayOffMode = mapped;
                    changed = true;
                }
            }
            
            // Update pump manual mode (only in manual mode)
            if (doc.containsKey("pumpManualMode") && state.mode == "manual") {
                bool newPumpManualMode = doc["pumpManualMode"];
                if (newPumpManualMode != state.pumpManualMode) {
                    state.pumpManualMode = newPumpManualMode;
                    changed = true;
                    
                    // If setting manual mode to true and heating is OFF, turn pump ON
                    if (newPumpManualMode && !state.heatingOn) {
                        setPump(true, true);
                    } else if (!newPumpManualMode && !state.heatingOn) {
                        // If disabling manual mode and heating is OFF, turn pump OFF
                        setPump(false, false);
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
            
            // Update diesel consumption per hour
            if (doc.containsKey("dieselConsumptionPerHour")) {
                float consumption = doc["dieselConsumptionPerHour"];
                if (consumption > 0 && consumption <= 20) {  // Max 20 liters per hour
                    // Round to 1 decimal place to avoid float precision issues
                    state.dieselConsumptionPerHour = round(consumption * 10.0) / 10.0;
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
                // Apply relay configuration immediately to current outputs
                applyRelayOutput(state.heaterRelayPin, state.heatingOn, state.heaterRelayActiveLow, state.heaterRelayOffMode, "Heater");
                applyRelayOutput(state.pumpRelayPin, state.pumpOn, state.pumpRelayActiveLow, state.pumpRelayOffMode, "Pump");

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
        
        Serial.printf("[Geocode] Searching for: %s\n", query.c_str());
        
        HTTPClient http;
        
        // URL encode query parameter - encode all non-ASCII and special chars
        String encodedQuery = "";
        for (unsigned int i = 0; i < query.length(); i++) {
            unsigned char c = query.charAt(i);
            // Allow: a-z, A-Z, 0-9, -, _, ., ~
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                encodedQuery += (char)c;
            } else if (c == ' ') {
                encodedQuery += "+";
            } else {
                // URL encode all other characters (including Umlaute)
                char hex[4];
                sprintf(hex, "%%%02X", c);
                encodedQuery += hex;
            }
        }
        
        Serial.printf("[Geocode] Encoded query: %s\n", encodedQuery.c_str());
        
        // OpenStreetMap Nominatim API (forward geocoding)
        // Use HTTPS to avoid HTTP 301 redirect
        String url = "https://nominatim.openstreetmap.org/search?";
        url += "q=" + encodedQuery;
        url += "&format=json&limit=5";
        url += "&accept-language=de"; // Prefer German results
        
        Serial.printf("[Geocode] URL: %s\n", url.c_str());
        
        http.begin(url);
        http.addHeader("User-Agent", "ESP32-HeaterControl/2.3.0");
        http.setTimeout(10000); // Increased timeout
        
        int httpCode = http.GET();
        
        Serial.printf("[Geocode] HTTP Code: %d\n", httpCode);
        
        StaticJsonDocument<512> doc;
        
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            Serial.printf("[Geocode] Response length: %d\n", payload.length());
            
            // Parse array response - increase buffer size for larger responses
            StaticJsonDocument<2048> responseDoc;
            DeserializationError error = deserializeJson(responseDoc, payload);
            
            if (!error && responseDoc.is<JsonArray>() && responseDoc.size() > 0) {
                JsonObject firstResult = responseDoc[0];
                
                doc["found"] = true;
                doc["latitude"] = firstResult["lat"].as<float>();
                doc["longitude"] = firstResult["lon"].as<float>();
                
                // Extract city name from display_name (take only first part before comma)
                String displayName = firstResult["display_name"].as<String>();
                int commaIndex = displayName.indexOf(',');
                if (commaIndex > 0) {
                    displayName = displayName.substring(0, commaIndex);
                }
                doc["displayName"] = displayName;
                
                Serial.printf("[Geocode] Found: %s at %.6f,%.6f\n", 
                            displayName.c_str(), 
                            doc["latitude"].as<float>(), 
                            doc["longitude"].as<float>());
            } else {
                Serial.printf("[Geocode] Parse error or empty: %s\n", error.c_str());
                doc["found"] = false;
                doc["error"] = error ? String("Parse error: ") + error.c_str() : "Location not found";
            }
        } else {
            Serial.printf("[Geocode] HTTP error: %d\n", httpCode);
            doc["found"] = false;
            doc["error"] = "Geocoding service unavailable (HTTP " + String(httpCode) + ")";
        }
        
        http.end();
        
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });
    
    // API: Get weather data (updates on demand when requested)
    server.on("/api/weather", HTTP_GET, [](AsyncWebServerRequest *request) {
        // Update weather data on demand (when page loads or F5 is pressed)
        // Only fetch if data is old (> 10 min) or invalid, to avoid unnecessary API calls
        unsigned long now = millis();
        if (!weather.valid || (weather.valid && (now - weather.lastUpdate >= WEATHER_UPDATE_INTERVAL))) {
            // Fetch in background (don't wait for response to avoid blocking)
            if (WiFi.status() == WL_CONNECTED && state.locationName.length() > 0 && state.locationName != "Unbekannter Ort") {
                doFetchWeatherData(false);
            }
        }
        
        StaticJsonDocument<512> doc;
        
        // Always include locationName if available (even if weather data is invalid)
        // Only include if it's not the default "Unbekannter Ort"
        if (weather.locationName.length() > 0 && weather.locationName != "Unbekannter Ort") {
            doc["locationName"] = weather.locationName;
        }
        
        if (weather.valid) {
            doc["valid"] = true;
            doc["temperature"] = round(weather.temperature * 10) / 10.0;
            doc["weatherCode"] = weather.weatherCode;
            doc["humidity"] = weather.humidity;
            doc["windSpeed"] = round(weather.windSpeed * 10) / 10.0;
            
            // Tomorrow forecast
            JsonObject forecast = doc.createNestedObject("tomorrow");
            forecast["tempMin"] = round(weather.tempMin * 10) / 10.0;
            forecast["tempMax"] = round(weather.tempMax * 10) / 10.0;
            forecast["weatherCode"] = weather.forecastWeatherCode;
            forecast["precipitation"] = round(weather.precipitation * 10) / 10.0;
        } else {
            doc["valid"] = false;
            // Only include error message if location is set (to avoid spam)
            if (weather.locationName.length() > 0 && weather.locationName != "Unbekannter Ort") {
                doc["error"] = "No weather data available";
            }
        }
        
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });

    // API: Tank debug (diagnose JSN-SR04T wiring/levels)
    server.on("/api/tank-debug", HTTP_GET, [](AsyncWebServerRequest *request) {
        StaticJsonDocument<384> doc;
        doc["trigPin"] = TRIG_PIN;
        doc["echoPin"] = ECHO_PIN;
        doc["echoBefore"] = lastEchoBefore;
        doc["echoAfter"] = lastEchoAfter;
        doc["durationUs"] = lastUltrasonicDurationUs;
        doc["distanceCm"] = lastUltrasonicDistanceCm;
        doc["tankAvailable"] = state.tankSensorAvailable;

        const char* err = "OK";
        if (lastTankErrorCode == 1) err = "TIMEOUT_NO_ECHO";
        else if (lastTankErrorCode == 2) err = "OUT_OF_RANGE";
        doc["errorCode"] = lastTankErrorCode;
        doc["error"] = err;

        // Current pin reads (useful if ECHO is stuck HIGH/LOW)
        doc["echoReadNow"] = digitalRead(ECHO_PIN);
        doc["trigReadNow"] = digitalRead(TRIG_PIN);

        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    });
    
    // API: Stats history (no authentication required)
    server.on("/api/stats-history", HTTP_GET, [](AsyncWebServerRequest *request) {
        
        StaticJsonDocument<8192> doc;  // Increased for switch events
        
        // Return current statistics for aggregation
        doc["switchCount"] = stats.switchCount;
        doc["todaySwitches"] = stats.todaySwitches;
        doc["onTimeSeconds"] = stats.onTimeSeconds;
        doc["offTimeSeconds"] = stats.offTimeSeconds;
        
        // Calculate total diesel consumption (from total ON time)
        float totalDieselLiters = (stats.onTimeSeconds / 3600.0) * state.dieselConsumptionPerHour;
        doc["totalDieselLiters"] = round(totalDieselLiters * 10) / 10.0;
        
        // Today's data object
        JsonObject today = doc.createNestedObject("today");
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 100)) {
            char dateKey[9];
            sprintf(dateKey, "%04d%02d%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
            today["dateKey"] = dateKey;
            today["switches"] = stats.todaySwitches;
            
            // Calculate today's on/off times from switch events
            unsigned long todayOnSeconds = 0;
            unsigned long todayOffSeconds = 0;
            unsigned long todayStartTime = 0;
            bool todayStarted = false;
            float sumVorlauf = 0.0;
            float sumRuecklauf = 0.0;
            float minVorlauf = NAN;
            float maxVorlauf = NAN;
            float minRuecklauf = NAN;
            float maxRuecklauf = NAN;
            unsigned long sampleCount = 0;
            
            // Get today's timestamp at 00:00:00
            struct tm todayStart = timeinfo;
            todayStart.tm_hour = 0;
            todayStart.tm_min = 0;
            todayStart.tm_sec = 0;
            unsigned long todayStartTimestamp = mktime(&todayStart);
            
            // Get yesterday's timestamp at 00:00:00 (for events that started yesterday but ended today)
            struct tm yesterdayStart = timeinfo;
            yesterdayStart.tm_hour = 0;
            yesterdayStart.tm_min = 0;
            yesterdayStart.tm_sec = 0;
            // Subtract one day
            time_t yesterdayTime = mktime(&yesterdayStart);
            yesterdayTime -= 86400; // 24 hours in seconds
            unsigned long yesterdayStartTimestamp = (unsigned long)yesterdayTime;
            
            // Collect and sort today's events chronologically (including events from yesterday that affect today)
            struct EventWithIndex {
                const SwitchEvent* evt;
                int originalIdx;
                unsigned long sortKey; // timestamp or uptimeMs for sorting
            };
            EventWithIndex todayEvents[MAX_SWITCH_EVENTS];
            int todayEventCount = 0;
            
            for (int i = 0; i < MAX_SWITCH_EVENTS; ++i) {
                int idx = (switchEventIndex + i) % MAX_SWITCH_EVENTS;
                const SwitchEvent& evt = switchEvents[idx];
                if (evt.timestamp == 0 && evt.uptimeMs == 0) continue;
                
                // Check if event is from today or yesterday (for overnight runs)
                bool isRelevant = false;
                if (evt.timestamp > 0) {
                    // Include events from yesterday (for overnight heating cycles)
                    // and events from today
                    isRelevant = (evt.timestamp >= yesterdayStartTimestamp && evt.timestamp < (todayStartTimestamp + 86400));
                } else if (evt.uptimeMs > 0) {
                    // Fallback: if no timestamp, assume it's relevant if uptime is reasonable
                    unsigned long currentUptime = millis();
                    if (evt.uptimeMs <= currentUptime && (currentUptime - evt.uptimeMs) < 172800000) {
                        isRelevant = true; // Within last 48 hours (to catch overnight cycles)
                    }
                }
                
                if (!isRelevant) continue;
                
                todayEvents[todayEventCount].evt = &evt;
                todayEvents[todayEventCount].originalIdx = idx;
                todayEvents[todayEventCount].sortKey = evt.timestamp > 0 ? evt.timestamp : evt.uptimeMs;
                todayEventCount++;
            }
            
            // Sort events chronologically (oldest first)
            for (int i = 0; i < todayEventCount - 1; ++i) {
                for (int j = i + 1; j < todayEventCount; ++j) {
                    if (todayEvents[i].sortKey > todayEvents[j].sortKey) {
                        EventWithIndex temp = todayEvents[i];
                        todayEvents[i] = todayEvents[j];
                        todayEvents[j] = temp;
                    }
                }
            }
            
            // Calculate on/off times from sorted events
            // Track the state at the start of today (from yesterday's last event)
            unsigned long lastOnTime = 0;
            bool lastWasOn = false;
            bool startedBeforeToday = false;
            
            for (int i = 0; i < todayEventCount; ++i) {
                const SwitchEvent& evt = *todayEvents[i].evt;
                unsigned long eventTime = evt.timestamp > 0 ? evt.timestamp : (evt.uptimeMs / 1000);
                
                // If this event is from yesterday and it's an ON event, we started before today
                if (evt.timestamp > 0 && evt.timestamp < todayStartTimestamp && evt.isOn) {
                    startedBeforeToday = true;
                    lastOnTime = todayStartTimestamp; // Start counting from today 00:00
                    lastWasOn = true;
                    continue;
                }
                
                if (evt.isOn) {
                    // ON event
                    if (lastWasOn && lastOnTime > 0) {
                        // Previous ON period ended (shouldn't happen, but handle it)
                        unsigned long duration = eventTime - lastOnTime;
                        if (eventTime >= todayStartTimestamp) {
                            todayOffSeconds += duration;
                        }
                    }
                    lastOnTime = eventTime;
                    lastWasOn = true;
                } else {
                    // OFF event
                    if (lastWasOn && lastOnTime > 0) {
                        // Calculate ON duration
                        unsigned long duration = eventTime - lastOnTime;
                        // Only count time that's within today
                        if (eventTime >= todayStartTimestamp) {
                            if (lastOnTime < todayStartTimestamp) {
                                // Started before today, only count from today 00:00
                                todayOnSeconds += (eventTime - todayStartTimestamp);
                            } else {
                                // Entirely within today
                                todayOnSeconds += duration;
                            }
                        }
                    } else if (!lastWasOn && lastOnTime > 0) {
                        // Calculate OFF duration
                        unsigned long duration = eventTime - lastOnTime;
                        if (eventTime >= todayStartTimestamp) {
                            if (lastOnTime < todayStartTimestamp) {
                                // Started before today, only count from today 00:00
                                todayOffSeconds += (eventTime - todayStartTimestamp);
                            } else {
                                // Entirely within today
                                todayOffSeconds += duration;
                            }
                        }
                    }
                    lastOnTime = eventTime;
                    lastWasOn = false;
                }
                
                // Track temperatures from all events
                if (!isnan(evt.tempVorlauf)) {
                    sumVorlauf += evt.tempVorlauf;
                    if (isnan(minVorlauf) || evt.tempVorlauf < minVorlauf) minVorlauf = evt.tempVorlauf;
                    if (isnan(maxVorlauf) || evt.tempVorlauf > maxVorlauf) maxVorlauf = evt.tempVorlauf;
                }
                if (!isnan(evt.tempRuecklauf)) {
                    sumRuecklauf += evt.tempRuecklauf;
                    if (isnan(minRuecklauf) || evt.tempRuecklauf < minRuecklauf) minRuecklauf = evt.tempRuecklauf;
                    if (isnan(maxRuecklauf) || evt.tempRuecklauf > maxRuecklauf) maxRuecklauf = evt.tempRuecklauf;
                }
                sampleCount++;
            }
            
            // If currently ON, add time from last ON event to now
            if (lastWasOn && state.heatingOn && lastOnTime > 0) {
                unsigned long currentTime = 0;
                if (getLocalTime(&timeinfo, 100)) {
                    currentTime = mktime(&timeinfo);
                } else {
                    currentTime = millis() / 1000; // Fallback to uptime
                }
                if (currentTime > lastOnTime) {
                    unsigned long duration = currentTime - lastOnTime;
                    todayOnSeconds += duration;
                }
            }
            
            // Use calculated values or fallback to stats
            unsigned long finalOnSeconds = todayOnSeconds > 0 ? todayOnSeconds : stats.onTimeSeconds;
            unsigned long finalOffSeconds = todayOffSeconds > 0 ? todayOffSeconds : stats.offTimeSeconds;
            today["onSeconds"] = finalOnSeconds;
            today["offSeconds"] = finalOffSeconds;
            
            // Calculate diesel consumption (liters = hours * consumption per hour)
            float todayDieselLiters = (finalOnSeconds / 3600.0) * state.dieselConsumptionPerHour;
            today["dieselLiters"] = round(todayDieselLiters * 10) / 10.0;
            
            // Temperature statistics
            if (sampleCount > 0) {
                today["avgVorlauf"] = round((sumVorlauf / sampleCount) * 10) / 10.0;
                today["avgRuecklauf"] = round((sumRuecklauf / sampleCount) * 10) / 10.0;
                today["minVorlauf"] = round(minVorlauf * 10) / 10.0;
                today["maxVorlauf"] = round(maxVorlauf * 10) / 10.0;
                today["minRuecklauf"] = round(minRuecklauf * 10) / 10.0;
                today["maxRuecklauf"] = round(maxRuecklauf * 10) / 10.0;
            } else {
                // Fallback to current values
                if (!isnan(state.tempVorlauf)) {
                    today["avgVorlauf"] = round(state.tempVorlauf * 10) / 10.0;
                } else {
                    today["avgVorlauf"] = nullptr;
                }
                if (!isnan(state.tempRuecklauf)) {
                    today["avgRuecklauf"] = round(state.tempRuecklauf * 10) / 10.0;
                } else {
                    today["avgRuecklauf"] = nullptr;
                }
                today["minVorlauf"] = nullptr;
                today["maxVorlauf"] = nullptr;
                today["minRuecklauf"] = nullptr;
                today["maxRuecklauf"] = nullptr;
            }
            today["samples"] = sampleCount > 0 ? sampleCount : 1;
        } else {
            today["dateKey"] = "";
            today["switches"] = 0;
            today["onSeconds"] = 0;
            today["offSeconds"] = 0;
            today["avgVorlauf"] = nullptr;
            today["avgRuecklauf"] = nullptr;
            today["samples"] = 0;
        }
        
        // Historical days array (empty for now - future: NVS ring buffer with daily aggregated data)
        JsonArray daysArray = doc.createNestedArray("days");
        
        // Switch events array (last 50 events with temperatures)
        JsonArray eventsArray = doc.createNestedArray("switchEvents");
        // Start from oldest event (after current index) and wrap around
        for (int i = 0; i < MAX_SWITCH_EVENTS; ++i) {
            int idx = (switchEventIndex + i) % MAX_SWITCH_EVENTS;
            const SwitchEvent& evt = switchEvents[idx];
            // Skip empty entries (timestamp == 0 and uptimeMs == 0 means never written)
            if (evt.timestamp == 0 && evt.uptimeMs == 0) continue;
            
            JsonObject eventObj = eventsArray.createNestedObject();
            if (evt.timestamp > 0) {
                eventObj["timestamp"] = evt.timestamp;
            } else {
                eventObj["timestamp"] = nullptr;
            }
            eventObj["isOn"] = evt.isOn;
            eventObj["uptimeMs"] = evt.uptimeMs;
            if (!isnan(evt.tempVorlauf)) {
                eventObj["tempVorlauf"] = round(evt.tempVorlauf * 10) / 10.0;
            } else {
                eventObj["tempVorlauf"] = nullptr;
            }
            if (!isnan(evt.tempRuecklauf)) {
                eventObj["tempRuecklauf"] = round(evt.tempRuecklauf * 10) / 10.0;
            } else {
                eventObj["tempRuecklauf"] = nullptr;
            }
            if (!isnan(evt.tankLiters)) {
                eventObj["tankLiters"] = round(evt.tankLiters * 10) / 10.0;
            } else {
                eventObj["tankLiters"] = nullptr;
            }
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
            
            // Save location name if provided
            if (doc.containsKey("locationName")) {
                String locName = doc["locationName"].as<String>();
                if (locName.length() > 0) {
                    state.locationName = locName;
                    weather.locationName = locName;  // Also update weather location name
                    changed = true;
                }
            }
            
            if (changed) {
                saveSettings();
                // Force immediate weather update (reset fetch timer and clear cache)
                weather.valid = false;
                weather.lastUpdate = 0;
                lastWeatherFetch = 0;  // Force immediate fetch
                
                // Only clear location name if coordinates changed but no name was provided
                if (!doc.containsKey("locationName")) {
                    weather.locationName = "";  // Will be refetched with new coordinates
                }
                
                // Fetch weather data immediately with force refresh (don't wait for next loop cycle)
                doFetchWeatherData(true);
                
                request->send(200, "application/json", "{\"success\":true,\"message\":\"Location updated\"}");
            } else {
                request->send(400, "application/json", "{\"error\":\"Invalid location data\"}");
            }
        }
    );
    
    // API: Send test Telegram message
    server.on("/api/telegram/test", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->authenticate(AUTH_USER, AUTH_PASS)) {
            return request->requestAuthentication();
        }
        
        if (!isTelegramConfigured()) {
            request->send(400, "application/json", "{\"success\":false,\"message\":\"Telegram nicht konfiguriert. Bitte Bot Token in secrets.h eintragen.\"}");
            return;
        }
        
        String msg = "🔔 TEST-NACHRICHT\n\n";
        msg += "✅ Telegram funktioniert!\n";
        msg += "🌡️ Vorlauf: " + String(state.tempVorlauf, 1) + "°C\n";
        msg += "📊 Status: " + String(state.heatingOn ? "EIN" : "AUS");
        
        sendTelegramMessage(msg);
        
        request->send(200, "application/json", "{\"success\":true,\"message\":\"Testnachricht gesendet\"}");
    });
    
    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not found");
    });
    
    // Initialize WebSocket for Serial Monitor
    ws.onEvent(onWebSocketEvent);
    server.addHandler(&ws);
    serialLogLn("WebSocket initialized at /ws");
    
    // Initialize OTA Updates (custom handler)
    server.on("/update", HTTP_POST, 
        [](AsyncWebServerRequest *request) {
            // POST handler is called AFTER upload completes
            // Response is already sent in final handler, so we just check for reboot
            bool shouldReboot = !Update.hasError();
            if (shouldReboot) {
                Serial.println("OTA Update successful, scheduling reboot in 8 seconds...");
                Serial.flush();
                // Schedule reboot after a short delay to allow response to be sent
                scheduledRebootTime = millis() + 8000;  // 8 seconds
                rebootScheduled = true;
            } else {
                Serial.println("OTA Update FAILED - no reboot");
                Update.printError(Serial);
            }
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                Serial.printf("OTA Update Start: %s\n", filename.c_str());
                otaUpdateInProgress = true;  // Mark OTA as in progress to prevent WiFi reconnect
                if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
                    Update.printError(Serial);
                    otaUpdateInProgress = false;  // Reset on error
                }
            }
            if (!Update.hasError()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                    otaUpdateInProgress = false;  // Reset on write error
                }
            } else {
                // Update has error - reset flag on final chunk
                if (final) {
                    otaUpdateInProgress = false;
                }
            }
            if (final) {
                bool success = Update.end(true);
                otaUpdateInProgress = false;  // OTA complete (success or failure)
                if (success) {
                    Serial.printf("OTA Update Success: %u bytes\n", index + len);
                    // Send response IMMEDIATELY after successful update, before reboot
                    String status = "OK";
                    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", status);
                    response->addHeader("Connection", "close");
                    request->send(response);
                    Serial.println("Response sent to client");
                    Serial.flush();
                } else {
                    Update.printError(Serial);
                    request->send(500, "text/plain", "FAIL");
                }
            }
        }
    );
    Serial.println("OTA initialized at /update");
    
    // Initialize LittleFS OTA Updates (for HTML/CSS/JS)
    server.on("/update-fs", HTTP_POST, 
        [](AsyncWebServerRequest *request) {
            // POST handler is called AFTER upload completes
            // Response is already sent in final handler, so we just check for reboot
            bool shouldReboot = !Update.hasError();
            if (shouldReboot) {
                Serial.println("LittleFS OTA Update successful, scheduling reboot in 8 seconds...");
                Serial.flush();
                // Schedule reboot after a short delay to allow response to be sent
                scheduledRebootTime = millis() + 8000;  // 8 seconds
                rebootScheduled = true;
            } else {
                Serial.println("LittleFS OTA Update FAILED - no reboot");
                Update.printError(Serial);
            }
        },
        [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
            if (!index) {
                Serial.printf("LittleFS OTA Start: %s\n", filename.c_str());
                otaUpdateInProgress = true;  // Mark OTA as in progress to prevent WiFi reconnect
                // UPDATE_TYPE_FILESYSTEM = U_SPIFFS
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
                    Update.printError(Serial);
                    otaUpdateInProgress = false;  // Reset on error
                }
            }
            if (!Update.hasError()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                    otaUpdateInProgress = false;  // Reset on write error
                }
            } else {
                // Update has error - reset flag on final chunk
                if (final) {
                    otaUpdateInProgress = false;
                }
            }
            if (final) {
                bool success = Update.end(true);
                otaUpdateInProgress = false;  // OTA complete (success or failure)
                if (success) {
                    Serial.printf("LittleFS OTA Success: %u bytes\n", index + len);
                    // Send response IMMEDIATELY after successful update, before reboot
                    String status = "OK";
                    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain", status);
                    response->addHeader("Connection", "close");
                    request->send(response);
                    Serial.println("Response sent to client");
                    Serial.flush();
                } else {
                    Update.printError(Serial);
                    request->send(500, "text/plain", "FAIL");
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
    {
        String banner = String("\n\n=== ESP32 Heater Control ") + FIRMWARE_VERSION + " ===";
        serialLogLn(banner.c_str());
    }
    
    bootTime = millis();

    // Load relay configuration early (before setting GPIO directions)
    loadRelayConfigEarly();

    // Initialize both relays (heating and pump) to OFF state using configured polarity/off-mode
    applyRelayOutput(state.heaterRelayPin, false, state.heaterRelayActiveLow, state.heaterRelayOffMode, "Heater");
    serialLogLn("[Setup] Heating relay initialized to OFF");
    applyRelayOutput(state.pumpRelayPin, false, state.pumpRelayActiveLow, state.pumpRelayOffMode, "Pump");
    serialLogLn("[Setup] Pump relay initialized to OFF");
    // Initialize ultrasonic sensor pins
    pinMode(TRIG_PIN, OUTPUT);
    // Use pulldown to avoid floating ECHO when sensor is disconnected/miswired.
    // NOTE: JSN-SR04T ECHO is 5V on many boards -> requires a voltage divider to 3.3V for ESP32!
    pinMode(ECHO_PIN, INPUT_PULLDOWN);
    digitalWrite(TRIG_PIN, LOW);
    
    // IMPORTANT: On some ESP32 boards/cores, auto-format-on-fail can crash inside esp_littlefs_format_partition().
    // We avoid formatting here and simply continue without filesystem if mount fails.
    if (!LittleFS.begin(false)) {
        Serial.println("⚠️ WARNING: LittleFS mount failed!");
        Serial.println("Continuing without filesystem - Web server may not work properly");
        // DON'T return - continue anyway, maybe filesystem isn't critical
    } else {
        Serial.println("LittleFS mounted successfully");
    }
    
    // Initialize sensors
    initSensors();
    
    // Load settings
    loadSettings();
    
    // Restore heater and pump state based on mode
    if (state.mode == "manual") {
        // Manual mode: restore saved state, but ALWAYS enforce safety rule:
        // Heating ON => Pump ON (and apply GPIO state).
        bool desiredPump = state.pumpOn;
        if (state.heatingOn) desiredPump = true;
        setPump(desiredPump, state.pumpManualMode);
        setHeater(state.heatingOn, false);
    } else {
        // Auto/schedule modes: restore saved state so heater continues running after reboot
        // The control functions will adjust if needed based on current conditions
        // Restore pump state (should follow heating in auto/schedule modes)
        bool desiredPump = state.pumpOn;
        if (state.heatingOn) desiredPump = true;
        setPump(desiredPump, false);
        setHeater(state.heatingOn, false);
    }
    
    bool wifiConnected = setupWiFi();
    
    if (!wifiConnected) {
        Serial.println("WiFi connection failed, starting Access Point mode...");
        Serial.println("(You can still access via http://192.168.4.1 if AP mode starts)");
        setupAccessPoint();
    } else {
        // WiFi is connected, wait a bit more for full stability
        Serial.println("WiFi connected, waiting for stability...");
        delay(3000);  // Extended wait for VPN/network stability
        Serial.println("WiFi stable, setting up services...");
        Serial.printf("Final IP check: %s\n", WiFi.localIP().toString().c_str());
        setupMDNS();
        setupNTP();
        
        // Use saved location name or fetch initial location name if coordinates are set and location name is empty
        if (state.locationName.length() > 0 && state.locationName != "Unbekannter Ort") {
            weather.locationName = state.locationName;
            Serial.printf("[Setup] Using saved location name: %s\n", weather.locationName.c_str());
        } else if (weather.locationName == "" || weather.locationName == "Unbekannter Ort") {
            Serial.println("[Setup] Fetching initial location name...");
            weather.locationName = fetchLocationName(state.latitude, state.longitude);
            // Save fetched location name
            if (weather.locationName != "Unbekannter Ort" && weather.locationName.length() > 0) {
                state.locationName = weather.locationName;
                saveSettings();
            }
            Serial.printf("[Setup] Location: %s\n", weather.locationName.c_str());
        }
    }
    
    setupWebServer();
    
    // Final check: Ensure WiFi is still connected before declaring setup complete
    if (wifiConnected) {
        if (WiFi.status() == WL_CONNECTED) {
            serialLogLn("=== Setup complete ===");
            serialLogF("Access via: http://%s/\n", WiFi.localIP().toString().c_str());
            Serial.printf("Or via mDNS: http://%s.local/\n", HOSTNAME);
        } else {
            Serial.println("⚠️ WARNING: WiFi disconnected during setup!");
            Serial.println("Will retry in loop()...");
        }
    } else {
        serialLogLn("=== Setup complete (AP Mode) ===");
        serialLogLn("Access via: http://192.168.4.1/");
    }
    
    // Fetch weather data once at startup (only if WiFi connected and location is set)
    if (wifiConnected && state.locationName.length() > 0 && state.locationName != "Unbekannter Ort") {
        doFetchWeatherData(false);
    }
    
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
    
    // Now let control functions decide heater state based on current conditions
    if (state.frostProtectionEnabled) {
        frostProtection();  // Frost protection has highest priority
    } else if (state.mode == "auto") {
        automaticControl();  // Will turn on if tempRuecklauf <= tempOn
    } else if (state.mode == "schedule") {
        scheduleControl();  // Will turn on if in schedule time
    }
    
    Serial.println("\n");
}

// ========== LOOP ==========
void loop() {
    // Flush pending WebSocket messages
    flushWebSocketMessages();
    
    // TEST MODE - COMMENTED OUT (uncomment to test relay)
    /*
    static unsigned long lastToggle = 0;
    static bool relayState = false;
    unsigned long now = millis();
    
    if (lastToggle == 0) {
        lastToggle = now;
        relayState = false;
        Serial.println("\n[Loop] First toggle - Setting GPIO23 to LOW");
        pinMode(RELAY_PIN, OUTPUT);
        digitalWrite(RELAY_PIN, LOW);
        delay(100);
        return;
    }
    
    if (now - lastToggle >= 2000) {
        lastToggle = now;
        relayState = !relayState;
        
        if (relayState) {
            pinMode(RELAY_PIN, OUTPUT);
            digitalWrite(RELAY_PIN, LOW);
        } else {
            pinMode(RELAY_PIN, OUTPUT_OPEN_DRAIN);
            digitalWrite(RELAY_PIN, HIGH);
        }
        delay(100);
        return;
    }
    delay(10);
    */
    
    // NORMAL MODE
    unsigned long now = millis();
    
    // Handle scheduled reboot after OTA update
    if (rebootScheduled && now >= scheduledRebootTime) {
        Serial.println("=== Executing scheduled reboot after OTA update ===");
        Serial.println("Closing all connections...");
        
        // Close all WebSocket connections
        ws.closeAll();
        delay(100);
        
        // Stop the web server gracefully
        Serial.println("Stopping server...");
        server.end();
        delay(500);
        
        // CRITICAL: Reset WiFi state before reboot to ensure clean connection on next boot
        // Complete WiFi reset - setupWiFi() will handle everything fresh
        WiFi.persistent(false);  // Don't save anything
        WiFi.disconnect(true);   // Erase all stored credentials
        WiFi.mode(WIFI_OFF);     // Turn WiFi OFF
        delay(200);
        Serial.println("WiFi completely reset for clean boot");
        
        // Flush all serial output
        Serial.flush();
        delay(500);
        
        Serial.println("Rebooting in 1 second...");
        delay(1000);
        Serial.flush();
        
        ESP.restart();
        return;  // Should never reach here, but just in case
    }
    
    state.uptime = (now - bootTime) / 1000;
    
    // Weather data is only fetched on demand via /api/weather endpoint (not in loop to avoid WebSocket issues)
    
    // Sync NTP if not yet synced
    if (!state.ntpSynced && !state.apModeActive) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 100)) {
            state.ntpSynced = true;
            Serial.println("NTP time synced!");
        }
    }
    
    // Handle pump cooldown logic (check every second)
    handlePumpCooldown();
    
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
    
    // WiFi reconnect logic - but NOT during OTA update or scheduled reboot
    if (!state.apModeActive && 
        !otaUpdateInProgress && 
        !rebootScheduled && 
        WiFi.status() != WL_CONNECTED &&
        (now - lastWiFiReconnectAttempt) >= WIFI_RECONNECT_INTERVAL) {
        Serial.println("WiFi lost, attempting reconnect...");
        lastWiFiReconnectAttempt = now;
        // Don't block - just try once, will retry later if needed
        WiFi.disconnect();
        delay(100);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        // Give it a few seconds to connect before next check
        lastWiFiReconnectAttempt = now - (WIFI_RECONNECT_INTERVAL - 5000);
    }
    
    // Cleanup disconnected WebSocket clients
    ws.cleanupClients();
    
    delay(10);
}
