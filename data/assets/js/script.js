// ========== TOAST NOTIFICATION SYSTEM ==========
function showToast(message, type = 'info', duration = 3000) {
    const container = document.getElementById('toastContainer');
    const toast = document.createElement('div');
    toast.className = `toast ${type}`;

    // Icon based on type
    const icons = {
        success: '<i class="fas fa-check-circle"></i>',
        error: '<i class="fas fa-exclamation-circle"></i>',
        warning: '<i class="fas fa-exclamation-triangle"></i>',
        info: '<i class="fas fa-info-circle"></i>'
    };

    toast.innerHTML = `
        <div class="toast-icon">${icons[type]}</div>
        <div class="toast-content">
            <div class="toast-message">${message}</div>
        </div>
        <button class="toast-close" onclick="this.parentElement.remove()">×</button>
    `;

    container.appendChild(toast);

    // Auto remove after duration
    if (duration > 0) {
        setTimeout(() => {
            toast.classList.add('removing');
            setTimeout(() => toast.remove(), 300);
        }, duration);
    }
}

const MAX_SCHEDULES = 4;

let currentState = {
    version: 'v0.0.0',
    tempVorlauf: null,
    tempRuecklauf: null,
    heating: false,
    pump: false,
    pumpManualMode: false,
    mode: 'manual',
    tempOn: 30,
    tempOff: 40,
    rssi: null,
    uptime: 0,
    currentTime: null,
    ntpSynced: false,
    schedules: [],
    tempDiff: null,
    efficiency: 0,
    switchCount: 0,
    todaySwitches: 0,
    onTimeSeconds: 0,
    offTimeSeconds: 0,
    behaviorWarning: false,
    frostEnabled: false,
    frostTemp: 8,
    tankAvailable: false,
    tankDistance: null,
    tankLiters: null,
    tankPercent: null,
    tankHeight: 100,
    tankCapacity: 1000,
    latitude: 50.952149,
    longitude: 7.1229,
    locationName: null,
    heaterRelayActiveLow: true,
    pumpRelayActiveLow: true,
    heaterRelayOffMode: 0,
    pumpRelayOffMode: 0,
    weather: null
};

let isLocalMode = false;
let simulationInterval = null;
let themeModeMediaQuery = null;
let themeModeMediaQueryListener = null;

// ========== THEME (Light/Dark/System) ==========
function applyThemeMode(mode) {
    const prefersDark = () => window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches;
    const resolved = (mode === 'system') ? (prefersDark() ? 'dark' : 'light') : mode;
    document.documentElement.setAttribute('data-theme', resolved);
}

function setThemeMode(mode) {
    localStorage.setItem('ui:themeMode', mode);
    applyThemeMode(mode);
}

function initTheme() {
    const saved = localStorage.getItem('ui:themeMode') || 'system';
    const select = document.getElementById('themeSelect');
    if (select) select.value = saved;

    // Cleanup old listener (if any)
    if (themeModeMediaQuery && themeModeMediaQueryListener) {
        try { themeModeMediaQuery.removeEventListener('change', themeModeMediaQueryListener); } catch (_) { }
    }

    themeModeMediaQuery = window.matchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null;
    themeModeMediaQueryListener = () => {
        const mode = localStorage.getItem('ui:themeMode') || 'system';
        if (mode === 'system') applyThemeMode(mode);
    };
    if (themeModeMediaQuery && themeModeMediaQuery.addEventListener) {
        themeModeMediaQuery.addEventListener('change', themeModeMediaQueryListener);
    }

    applyThemeMode(saved);
}

for (let i = 0; i < MAX_SCHEDULES; i++) {
    currentState.schedules.push({
        enabled: false,
        start: '00:00',
        end: '00:00'
    });
}

function detectMode() {
    const hostname = window.location.hostname;
    const protocol = window.location.protocol;

    isLocalMode = protocol === 'file:' ||
        hostname === 'localhost' ||
        hostname === '127.0.0.1' ||
        hostname === '';

    if (isLocalMode) {
        document.getElementById('demoBadge').classList.add('active');
        document.getElementById('demoControls').classList.add('active');

        currentState.version = 'v2.2.7-demo';
        currentState.tempVorlauf = 45.3;
        currentState.tempRuecklauf = 22.2;
        currentState.ntpSynced = true;
        currentState.rssi = -68;
        currentState.schedules[0] = { enabled: true, start: '05:30', end: '23:30' };

        // Initialize temperature thresholds for demo (important!)
        currentState.tempOn = 30;
        currentState.tempOff = 40;

        // Initialize tank demo data
        currentState.tankAvailable = true;
        currentState.tankLiters = 650.0;
        currentState.tankPercent = 65;
        currentState.tankHeight = 100;
        currentState.tankCapacity = 1000;
        currentState.dieselConsumptionPerHour = 2.0;

        // Initialize with default weather demo data (Köln)
        currentState.weather = {
            valid: true,
            temperature: 5.1,
            weatherCode: 3,  // Bewölkt
            humidity: 88,
            windSpeed: 12.2,
            locationName: 'Köln',
            tomorrow: {
                tempMin: 0.1,
                tempMax: 5.8,
                weatherCode: 61,  // Leichter Regen
                precipitation: 4.0
            }
        };

        // Demo defaults for relay settings
        currentState.heaterRelayActiveLow = true;
        currentState.pumpRelayActiveLow = true;
        currentState.heaterRelayOffMode = 0;
        currentState.pumpRelayOffMode = 0;

        displayWeather();
        startSimulation();
    }
}

// WMO Weather Code Mapping
function getWeatherInfo(code) {
    const weatherMap = {
        0: { icon: '☀️', desc: 'Klar' },
        1: { icon: '🌤️', desc: 'Überwiegend klar' },
        2: { icon: '⛅', desc: 'Teilweise bewölkt' },
        3: { icon: '☁️', desc: 'Bewölkt' },
        45: { icon: '🌫️', desc: 'Nebel' },
        48: { icon: '🌫️', desc: 'Nebel' },
        51: { icon: '🌦️', desc: 'Leichter Regen' },
        53: { icon: '🌧️', desc: 'Regen' },
        55: { icon: '🌧️', desc: 'Starker Regen' },
        61: { icon: '🌦️', desc: 'Leichter Regen' },
        63: { icon: '🌧️', desc: 'Regen' },
        65: { icon: '🌧️', desc: 'Starker Regen' },
        71: { icon: '🌨️', desc: 'Leichter Schneefall' },
        73: { icon: '🌨️', desc: 'Schneefall' },
        75: { icon: '🌨️', desc: 'Starker Schneefall' },
        77: { icon: '🌨️', desc: 'Schneegriesel' },
        80: { icon: '🌦️', desc: 'Leichte Schauer' },
        81: { icon: '🌧️', desc: 'Schauer' },
        82: { icon: '🌧️', desc: 'Starke Schauer' },
        85: { icon: '🌨️', desc: 'Schneeschauer' },
        86: { icon: '🌨️', desc: 'Starke Schneeschauer' },
        95: { icon: '⛈️', desc: 'Gewitter' },
        96: { icon: '⛈️', desc: 'Gewitter mit Hagel' },
        99: { icon: '⛈️', desc: 'Starkes Gewitter' }
    };

    return weatherMap[code] || { icon: '🌤️', desc: 'Unbekannt' };
}

// Demo Serial Monitor Logs
let demoLogInterval = null;
const demoLogs = [
    '=== ESP32 Heater Control v2.2.0 ===',
    'LittleFS mounted',
    'Found 2 DS18B20 sensor(s)',
    'WiFi connected: FRITZ!Box 6890 MO',
    'IP address: 192.168.1.100',
    'mDNS responder started: heater.local',
    'NTP time synced!',
    'WebSocket initialized at /ws',
    'OTA initialized at /update',
    'Web server started',
    'Vorlauf: {temp1}°C, Rücklauf: {temp2}°C',
    'Heater {state} (Mode: {mode})',
    'Tank sensor detected: {tank} L ({percent}%)',
    'WiFi RSSI: {rssi} dBm',
    'Uptime: {uptime}',
    'Switch #{count}: Heater {switch_state}',
    'Automatic control: Temp = {temp}°C',
    'Schedule active: {schedule}',
    'Frost protection: {frost_state}'
];

function startDemoLogs() {
    if (!isLocalMode) return;

    // Initial boot logs
    appendSerialLog('// WebSocket verbunden\n', '#27ae60');
    setTimeout(() => appendSerialLog('=== ESP32 Heater Control v2.2.0-demo ===\n'), 500);
    setTimeout(() => appendSerialLog('LittleFS mounted\n'), 700);
    setTimeout(() => appendSerialLog('Found 2 DS18B20 sensor(s)\n'), 900);
    setTimeout(() => appendSerialLog('WiFi connected: Demo-Network\n'), 1100);
    setTimeout(() => appendSerialLog('IP address: 192.168.1.100\n'), 1300);
    setTimeout(() => appendSerialLog('mDNS responder started: heater.local\n'), 1500);
    setTimeout(() => appendSerialLog('NTP time synced!\n'), 1700);
    setTimeout(() => appendSerialLog('WebSocket initialized at /ws\n'), 1900);
    setTimeout(() => appendSerialLog('OTA initialized at /update\n'), 2100);
    setTimeout(() => appendSerialLog('Web server started\n'), 2300);
    setTimeout(() => appendSerialLog('=== Setup complete ===\n\n'), 2500);

    // Periodic logs every 5-10 seconds
    demoLogInterval = setInterval(() => {
        const rand = Math.random();

        if (rand < 0.3) {
            // Temperature log
            appendSerialLog(`Vorlauf: ${currentState.tempVorlauf.toFixed(1)}°C, Rücklauf: ${currentState.tempRuecklauf.toFixed(1)}°C\n`);
        } else if (rand < 0.5) {
            // Status log
            appendSerialLog(`Heater ${currentState.heating ? 'ON' : 'OFF'} (Mode: ${currentState.mode})\n`);
        } else if (rand < 0.6 && currentState.tankAvailable) {
            // Tank log
            appendSerialLog(`Tank: ${currentState.tankLiters.toFixed(1)} L (${currentState.tankPercent}%)\n`);
        } else if (rand < 0.7) {
            // WiFi log
            appendSerialLog(`WiFi RSSI: ${currentState.rssi} dBm\n`);
        } else if (rand < 0.85) {
            // Efficiency log
            if (currentState.tempDiff !== null) {
                appendSerialLog(`ΔT = ${currentState.tempDiff.toFixed(1)}°C, Heizleistung: ${Math.round(currentState.efficiency)}%\n`);
            }
        } else {
            // Stats log
            const hours = Math.floor(currentState.uptime / 3600);
            const minutes = Math.floor((currentState.uptime % 3600) / 60);
            appendSerialLog(`Uptime: ${hours}h ${minutes}m | Switches: ${currentState.todaySwitches} today, ${currentState.switchCount} total\n`);
        }
    }, 5000 + Math.random() * 5000); // Random interval between 5-10 seconds
}

function startSimulation() {
    if (!isLocalMode || simulationInterval) return;

    // Initialize temperatures to cold water (12.5°C) if not set
    if (currentState.tempVorlauf === null || currentState.tempRuecklauf === null) {
        currentState.tempVorlauf = 12.5;
        currentState.tempRuecklauf = 12.5;
    }

    simulationInterval = setInterval(() => {
        const now = new Date();
        currentState.currentTime = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;

        // Only simulate temperature changes when heating is ON
        // When heating is OFF, temperatures stay frozen at cold water level (12.5°C)
        if (currentState.tempVorlauf !== null && currentState.tempRuecklauf !== null) {
            if (currentState.heating) {
                // Heating ON: temperatures rise
                currentState.tempVorlauf += Math.random() * 0.5;
                currentState.tempRuecklauf += Math.random() * 0.4;

                // Clamp to realistic ranges
                currentState.tempVorlauf = Math.max(20, Math.min(80, currentState.tempVorlauf));
                currentState.tempRuecklauf = Math.max(15, Math.min(70, currentState.tempRuecklauf));
            } else {
                // Heating OFF: temperatures are frozen at cold water level (12.5°C)
                // No changes - temperatures stay exactly at 12.5°C
                currentState.tempVorlauf = 12.5;
                currentState.tempRuecklauf = 12.5;
            }
        }

        // Auto mode logic: only when heating is ON and temperatures are being simulated
        if (currentState.mode === 'auto' && currentState.heating && currentState.tempRuecklauf !== null) {
            // Check if we should turn heating OFF (when temp reaches tempOff)
            if (currentState.tempRuecklauf >= currentState.tempOff) {
                currentState.heating = false;
                // Pump can turn OFF after cooldown (simplified: immediately in demo, unless manual mode)
                if (currentState.mode !== 'manual' || !currentState.pumpManualMode) {
                    currentState.pump = false;
                }
            }
        }

        // Auto mode: check if we should turn heating ON (only if currently OFF)
        // Since temperatures are frozen at 12.5°C when heating is OFF,
        // we check if tempOn is above 12.5°C to trigger heating
        if (currentState.mode === 'auto' && !currentState.heating && currentState.tempOn > 12.5) {
            // tempOn is above cold water temp, so heating should turn ON
            currentState.heating = true;
            currentState.pump = true;
        }

        // Safety: Ensure pump is ON when heating is ON
        if (currentState.heating && !currentState.pump) {
            currentState.pump = true;
        }

        currentState.rssi = -68 + Math.floor(Math.random() * 8) - 4;
        currentState.uptime += 1;

        // Simulate stats
        if (currentState.heating && Math.random() > 0.99) {
            currentState.todaySwitches = Math.min(currentState.todaySwitches + 1, 99);
            currentState.switchCount++;
        }
        currentState.onTimeSeconds += currentState.heating ? 1 : 0;
        currentState.offTimeSeconds += !currentState.heating ? 1 : 0;

        // Calculate difference
        if (currentState.tempVorlauf !== null && currentState.tempRuecklauf !== null) {
            currentState.tempDiff = currentState.tempVorlauf - currentState.tempRuecklauf;
            const diff = currentState.tempDiff;
            if (diff >= 10 && diff <= 15) {
                currentState.efficiency = 100;
            } else if (diff > 15) {
                currentState.efficiency = Math.round(Math.max(0, 100 - ((diff - 15) * 5)));
            } else {
                currentState.efficiency = Math.round(Math.max(0, (diff / 10) * 100));
            }
        }

        // Simulate tank level (slowly decreasing when heating)
        if (currentState.tankAvailable) {
            if (currentState.heating && Math.random() > 0.95) {
                currentState.tankLiters = Math.max(0, currentState.tankLiters - 0.5);
                currentState.tankPercent = Math.round((currentState.tankLiters / currentState.tankCapacity) * 100);
            }
        }

        updateUI();
    }, 1000);
}

async function updateStatus() {
    if (isLocalMode) return;

    try {
        const response = await fetch('/api/status');
        if (!response.ok) throw new Error('API failed');

        const data = await response.json();

        // Ensure schedules array always exists with MAX_SCHEDULES entries
        const schedulesFromApi = Array.isArray(data.schedules) ? data.schedules : [];
        const safeSchedules = [];
        for (let i = 0; i < MAX_SCHEDULES; i++) {
            const s = schedulesFromApi[i] || (currentState.schedules && currentState.schedules[i]) || {};
            safeSchedules.push({
                enabled: !!s.enabled,
                start: (typeof s.start === 'string' && s.start) ? s.start : '00:00',
                end: (typeof s.end === 'string' && s.end) ? s.end : '00:00'
            });
        }

        currentState = {
            version: data.version || 'v0.0.0',
            tempVorlauf: data.tempVorlauf,
            tempRuecklauf: data.tempRuecklauf,
            heating: data.heating,
            pump: data.pump !== undefined ? data.pump : false,
            pumpManualMode: data.pumpManualMode !== undefined ? data.pumpManualMode : false,
            mode: data.mode,
            tempOn: data.tempOn,
            tempOff: data.tempOff,
            rssi: data.rssi,
            uptime: data.uptime,
            currentTime: data.currentTime,
            ntpSynced: data.ntpSynced,
            schedules: safeSchedules,
            tempDiff: data.tempDiff,
            efficiency: data.efficiency || 0,
            switchCount: data.switchCount || 0,
            todaySwitches: data.todaySwitches || 0,
            onTimeSeconds: data.onTimeSeconds || 0,
            offTimeSeconds: data.offTimeSeconds || 0,
            frostEnabled: data.frostEnabled || false,
            frostTemp: data.frostTemp || 8,
            tankAvailable: data.tankAvailable || false,
            tankDistance: data.tankDistance,
            tankLiters: data.tankLiters,
            tankPercent: data.tankPercent,
            tankHeight: data.tankHeight || 100,
            tankCapacity: data.tankCapacity || 1000,
            dieselConsumptionPerHour: data.dieselConsumptionPerHour !== undefined ? Math.round(data.dieselConsumptionPerHour * 10) / 10 : 2.0,
            latitude: data.latitude || 50.952149,
            longitude: data.longitude || 7.1229,
            locationName: data.locationName || null,
            heaterRelayActiveLow: (data.heaterRelayActiveLow !== undefined) ? data.heaterRelayActiveLow : true,
            pumpRelayActiveLow: (data.pumpRelayActiveLow !== undefined) ? data.pumpRelayActiveLow : true,
            heaterRelayOffMode: (data.heaterRelayOffMode !== undefined) ? data.heaterRelayOffMode : 0,
            pumpRelayOffMode: (data.pumpRelayOffMode !== undefined) ? data.pumpRelayOffMode : 0
        };

        // Update location name from status if available
        if (data.locationName) {
            document.getElementById('weatherLocation').textContent = data.locationName;
            document.getElementById('currentLocationName').textContent = data.locationName;
        }

        updateConnectionStatus(true);
        // Weather update is now handled in main interval to avoid spam

        // Update UI after status is loaded (so location input field gets filled)
        updateUI();
    } catch (error) {
        console.error('Status fetch failed:', error);
        updateConnectionStatus(false);
        updateUI();
    }
}

async function updateWeather() {
    if (isLocalMode) {
        // Use demo weather data
        return;
    }

    try {
        const response = await fetch('/api/weather');
        if (!response.ok) throw new Error('Weather API failed');

        const data = await response.json();

        // Update weather state even if not valid (to keep locationName)
        if (!currentState.weather) {
            currentState.weather = {};
        }
        currentState.weather = { ...currentState.weather, ...data };

        // Update location name immediately if available (even if weather data is invalid)
        // Ignore "Unbekannter Ort" as it's just a placeholder
        if (data.locationName && data.locationName !== "Unbekannter Ort") {
            document.getElementById('weatherLocation').textContent = data.locationName;
            document.getElementById('currentLocationName').textContent = data.locationName;
        }

        if (data.valid) {
            displayWeather();
            // Also update header if weather card is collapsed
            const weatherCard = document.getElementById('weatherCard');
            if (weatherCard) {
                const weatherContent = weatherCard.querySelector('.card-content');
                if (weatherContent && weatherContent.classList.contains('collapsed')) {
                    updateWeatherCardHeader(true);
                }
            }
            return true; // Return true if weather data is valid
        } else {
            // Check if we should show error or loading message
            const locationInput = document.getElementById('locationInput');
            const hasLocationInput = locationInput && locationInput.value.trim() !== '';
            const hasLocationName = (data.locationName && data.locationName !== "Unbekannter Ort");

            // Only show error if location is actually set, otherwise show loading
            if (hasLocationInput || hasLocationName) {
                // Location is set, show loading message (weather will be fetched soon)
                displayWeatherError();
            }
            // If no location is set, don't show anything (will be handled by initial state)
            return false; // Return false if weather data is not valid
        }
    } catch (error) {
        console.error('Weather fetch failed:', error);
        displayWeatherError();
        return false;
    }
}

function displayWeather() {
    if (!currentState.weather || !currentState.weather.valid) {
        displayWeatherError();
        return;
    }

    const w = currentState.weather;
    const currentInfo = getWeatherInfo(w.weatherCode);
    const forecastInfo = getWeatherInfo(w.tomorrow.weatherCode);

    // Update location name in header and settings info
    // BUT: Don't overwrite title if weather card is collapsed (it shows icon+temp)
    const weatherCardEl = document.getElementById('weatherCard');
    const weatherContent = weatherCardEl ? weatherCardEl.querySelector('.card-content') : null;
    const isCollapsed = weatherContent && weatherContent.classList.contains('collapsed');

    if (w.locationName) {
        const locationEl = document.getElementById('weatherLocation');
        if (locationEl && !isCollapsed) {
            locationEl.textContent = w.locationName;
        }
        document.getElementById('currentLocationName').textContent = w.locationName;
    }

    // If collapsed, update header to show icon+temp
    if (isCollapsed) {
        updateWeatherCardHeader(true);
    }

    // Helper: Format temperature (round to integer)
    const formatTemp = (temp) => Math.round(temp) + '°C';

    const html = `
        <div class="weather-current">
            <div class="weather-icon">${currentInfo.icon}</div>
            <div>
                <div class="weather-temp">${formatTemp(w.temperature)}</div>
                <div class="weather-desc">${currentInfo.desc}</div>
            </div>
        </div>
        
        <div class="weather-details">
            <div class="weather-detail">
                <i class="fas fa-droplet"></i> Luftfeuchtigkeit:
                <span class="weather-detail-value">${w.humidity}%</span>
            </div>
            <div class="weather-detail">
                <i class="fas fa-wind"></i> Wind:
                <span class="weather-detail-value">${Math.round(w.windSpeed)} km/h</span>
            </div>
        </div>
        
        <div class="weather-forecast">
            <div class="weather-forecast-title">
                <i class="fas fa-calendar-plus"></i> Morgen
            </div>
            <div class="weather-forecast-content">
                <div class="weather-forecast-icon">${forecastInfo.icon}</div>
                <div class="weather-forecast-temp">${Math.round(w.tomorrow.tempMin)}°/${Math.round(w.tomorrow.tempMax)}°C</div>
                <div class="weather-forecast-desc">${forecastInfo.desc}</div>
                ${w.tomorrow.precipitation > 0 ? `
                <div class="weather-forecast-rain"><i class="fas fa-droplet"></i> ${Math.round(w.tomorrow.precipitation)}mm Regen</div>
                ` : ''}
            </div>
        </div>
        ${w.temperature < 10 ? `
        <div class="weather-cold-warning">
            <i class="fas fa-temperature-low"></i> Kalt! Warme Kleidung empfohlen
        </div>
        ` : ''}
        <div class="weather-update-time">
            Zuletzt aktualisiert: ${new Date().toLocaleTimeString('de-DE', { hour: '2-digit', minute: '2-digit', second: '2-digit' })}
        </div>
    `;

    document.getElementById('weatherContent').innerHTML = html;

    // Always update header if card is collapsed (to show weather in header)
    // weatherCardEl is already declared above, so reuse it
    if (weatherCardEl) {
        const weatherContentCheck = weatherCardEl.querySelector('.card-content');
        if (weatherContentCheck && weatherContentCheck.classList.contains('collapsed')) {
            updateWeatherCardHeader(true);
        }
    }
}

function displayWeatherError() {
    // Check if location input is empty (user hasn't entered a location yet)
    const locationInput = document.getElementById('locationInput');
    const hasLocationInput = locationInput && locationInput.value.trim() !== '';
    const hasLocationName = (currentState.weather && currentState.weather.locationName && currentState.weather.locationName !== 'Unbekannter Ort') ||
        (currentState.locationName && currentState.locationName !== 'Unbekannter Ort');

    // Show appropriate message based on whether location is set
    let errorMsg = '';
    let errorIcon = '';

    // Only show "Bitte zuerst einen Standort..." if input is empty AND no location is set
    if (!hasLocationInput && !hasLocationName) {
        errorMsg = 'Bitte zuerst einen Standort in den Einstellungen eintragen, um Wetterdaten abzurufen.';
        errorIcon = '<i class="fas fa-map-marker-alt"></i>';
    } else {
        // Location is set, but weather data couldn't be loaded
        errorMsg = 'Wetterdaten werden geladen...';
        errorIcon = '<i class="fas fa-spinner fa-spin"></i>';
    }

    document.getElementById('weatherContent').innerHTML =
        '<div class="weather-error" id="weatherErrorMsg" style="text-align: center; padding: 20px;">' +
        '<div style="font-size: 32px; margin-bottom: 12px; color: var(--muted);">' + errorIcon + '</div>' +
        '<div style="font-size: 14px; line-height: 1.5; color: var(--muted);">' + errorMsg + '</div>' +
        '</div>';

    // Keep location name displayed even if weather data is invalid
    if (currentState.weather && currentState.weather.locationName) {
        document.getElementById('weatherLocation').textContent = currentState.weather.locationName;
        document.getElementById('currentLocationName').textContent = currentState.weather.locationName;
    } else if (currentState.locationName) {
        document.getElementById('weatherLocation').textContent = currentState.locationName;
        document.getElementById('currentLocationName').textContent = currentState.locationName;
    } else {
        document.getElementById('weatherLocation').textContent = '';
        document.getElementById('currentLocationName').textContent = '-';
    }
}

// German cities database for demo mode
const germanCities = [
    { name: 'Berlin', plz: '10115', lat: 52.520008, lon: 13.404954 },
    { name: 'München', plz: '80331', lat: 48.137154, lon: 11.576124 },
    { name: 'Hamburg', plz: '20095', lat: 53.551086, lon: 9.993682 },
    { name: 'Köln', plz: '50667', lat: 50.952149, lon: 7.1229 },
    { name: 'Frankfurt', plz: '60311', lat: 50.110922, lon: 8.682127 },
    { name: 'Stuttgart', plz: '70173', lat: 48.783333, lon: 9.183333 },
    { name: 'Düsseldorf', plz: '40210', lat: 51.227741, lon: 6.773456 },
    { name: 'Dortmund', plz: '44135', lat: 51.514244, lon: 7.468429 },
    { name: 'Essen', plz: '45127', lat: 51.455643, lon: 7.011555 },
    { name: 'Leipzig', plz: '04109', lat: 51.340333, lon: 12.360103 },
    { name: 'Bremen', plz: '28195', lat: 53.079296, lon: 8.801694 },
    { name: 'Dresden', plz: '01067', lat: 51.050407, lon: 13.737262 },
    { name: 'Hannover', plz: '30159', lat: 52.375892, lon: 9.732010 },
    { name: 'Nürnberg', plz: '90402', lat: 49.453872, lon: 11.077298 },
    { name: 'Bonn', plz: '53111', lat: 50.735851, lon: 7.101674 }
];

// Simple location name lookup for demo mode
function getDemoLocationName(lat, lon) {
    // Find closest city
    let closest = germanCities[0];
    let minDist = Math.sqrt(Math.pow(lat - closest.lat, 2) + Math.pow(lon - closest.lon, 2));

    for (let loc of germanCities) {
        const dist = Math.sqrt(Math.pow(lat - loc.lat, 2) + Math.pow(lon - loc.lon, 2));
        if (dist < minDist) {
            minDist = dist;
            closest = loc;
        }
    }

    return closest.name;
}

// Find location by name or PLZ (demo mode)
function findLocationByName(query) {
    query = query.toLowerCase().trim();

    // Try exact match first
    for (let loc of germanCities) {
        if (loc.name.toLowerCase() === query || loc.plz === query) {
            return loc;
        }
    }

    // Try partial match
    for (let loc of germanCities) {
        if (loc.name.toLowerCase().includes(query)) {
            return loc;
        }
    }

    return null;
}

// NEW: Save location by city name or PLZ
async function saveLocationByName() {
    const locationInputEl = document.getElementById('locationInput');
    const saveLocationBtn = document.getElementById('saveLocationBtn');
    const saveLocationIcon = document.getElementById('saveLocationIcon');
    const saveLocationText = document.getElementById('saveLocationText');
    const locationInput = locationInputEl.value.trim();

    if (!locationInput) {
        showToast('Bitte gib eine Stadt oder Postleitzahl ein', 'warning');
        return;
    }

    // Disable input and button, show loading
    locationInputEl.disabled = true;
    locationInputEl.style.opacity = '0.6';
    locationInputEl.style.cursor = 'not-allowed';
    saveLocationBtn.disabled = true;
    saveLocationBtn.style.opacity = '0.6';
    saveLocationBtn.style.cursor = 'not-allowed';
    saveLocationIcon.className = 'fas fa-spinner fa-spin';
    saveLocationText.textContent = 'Suche & speichere...';

    // Helper function to re-enable UI
    const enableUI = () => {
        locationInputEl.disabled = false;
        locationInputEl.style.opacity = '1';
        locationInputEl.style.cursor = 'text';
        saveLocationBtn.disabled = false;
        saveLocationBtn.style.opacity = '1';
        saveLocationBtn.style.cursor = 'pointer';
        saveLocationIcon.className = 'fas fa-search';
        saveLocationText.textContent = 'Standort suchen & speichern';
    };

    if (isLocalMode) {
        // Demo mode: Try to geocode using Nominatim API (if available)
        showToast('Suche Standort...', 'info', 2000);

        try {
            const geocodeUrl = `https://nominatim.openstreetmap.org/search?q=${encodeURIComponent(locationInput)}&format=json&limit=1&countrycodes=de&accept-language=de`;

            const response = await fetch(geocodeUrl, {
                headers: { 'User-Agent': 'HeizungssteuerungDemo/2.3.0' }
            });

            if (!response.ok) throw new Error('Geocoding failed');

            const data = await response.json();

            if (!data || data.length === 0) {
                showToast('Standort nicht gefunden: ' + locationInput, 'error');
                enableUI();
                return;
            }

            const result = data[0];
            const lat = parseFloat(result.lat);
            const lon = parseFloat(result.lon);
            const displayName = result.display_name.split(',')[0]; // Get city name only

            // Update hidden fields
            document.getElementById('latitude').value = lat;
            document.getElementById('longitude').value = lon;

            // Update state
            currentState.latitude = lat;
            currentState.longitude = lon;

            // Fetch REAL weather data for this location (not just simulated!)
            try {
                const weatherUrl = `https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum&timezone=Europe/Berlin&forecast_days=2`;

                const weatherResponse = await fetch(weatherUrl);

                if (weatherResponse.ok) {
                    const weatherData = await weatherResponse.json();

                    // Update weather with REAL data
                    currentState.weather = {
                        valid: true,
                        temperature: weatherData.current.temperature_2m,
                        weatherCode: weatherData.current.weather_code,
                        humidity: weatherData.current.relative_humidity_2m,
                        windSpeed: weatherData.current.wind_speed_10m,
                        locationName: displayName,
                        tomorrow: {
                            tempMin: weatherData.daily.temperature_2m_min[1],
                            tempMax: weatherData.daily.temperature_2m_max[1],
                            weatherCode: weatherData.daily.weather_code[1],
                            precipitation: weatherData.daily.precipitation_sum[1]
                        }
                    };

                    displayWeather();

                    showToast('⚠️ Standort & Wetter aktualisiert: ' + displayName + ' (Demo-Modus, nicht persistent!)', 'warning', 4000);
                } else {
                    throw new Error('Weather API failed');
                }
            } catch (weatherError) {
                console.error('Failed to fetch weather:', weatherError);
                // Fallback to simulated weather
                if (currentState.weather) {
                    currentState.weather.locationName = displayName;
                    const tempVariation = (lat - 50) * 0.3;
                    currentState.weather.temperature = 5.1 + tempVariation;
                    displayWeather();
                }
                showToast('⚠️ Standort aktualisiert: ' + displayName + ' (Wetter simuliert, Demo-Modus)', 'warning', 4000);
            }

            // Update input field to show clean city name
            document.getElementById('locationInput').value = displayName;
        } catch (error) {
            console.error('Geocoding error:', error);
            showToast('Fehler beim Suchen des Standorts. Bitte Internetverbindung prüfen.', 'error');
            enableUI();
        }

        enableUI();
        return;
    }

    // Real mode: call backend geocoding API
    try {
        // First, geocode the location name
        const geocodeResponse = await fetch(`/api/geocode?query=${encodeURIComponent(locationInput)}`);

        if (!geocodeResponse.ok) throw new Error('Geocoding failed');

        const geocodeData = await geocodeResponse.json();

        if (!geocodeData.found) {
            showToast('Standort nicht gefunden: ' + locationInput, 'error');
            enableUI();
            return;
        }

        // Update hidden fields
        document.getElementById('latitude').value = geocodeData.latitude;
        document.getElementById('longitude').value = geocodeData.longitude;

        // Save to backend
        const response = await fetch('/api/location', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': 'Basic ' + btoa('admin:admin')
            },
            body: JSON.stringify({
                latitude: geocodeData.latitude,
                longitude: geocodeData.longitude
            })
        });

        if (!response.ok) throw new Error('Failed to save location');

        const data = await response.json();

        // Extract city name from displayName (take only first part before comma)
        let cityName = geocodeData.displayName;
        if (cityName.includes(',')) {
            cityName = cityName.split(',')[0].trim();
        }

        showToast('Standort gespeichert: ' + cityName, 'success', 3000);

        // Update input field with short city name only
        document.getElementById('locationInput').value = cityName;

        // Update location name immediately in UI
        document.getElementById('currentLocationName').textContent = cityName;
        document.getElementById('weatherLocation').textContent = cityName;

        // Update state
        currentState.locationName = cityName;
        if (!currentState.weather) {
            currentState.weather = {};
        }
        currentState.weather.locationName = cityName;

        // Save location name to backend (so it's persisted in NVS)
        try {
            await fetch('/api/location', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'Authorization': 'Basic ' + btoa('admin:admin')
                },
                body: JSON.stringify({
                    latitude: geocodeData.latitude,
                    longitude: geocodeData.longitude,
                    locationName: cityName
                })
            });
        } catch (err) {
            console.error('Failed to save location name:', err);
        }

        // Update weather immediately (force refresh)
        // Try multiple times to get weather data (backend might need time to fetch)
        let retryCount = 0;
        const maxRetries = 6; // Try for ~12 seconds (2s intervals)
        const checkWeather = () => {
            updateStatus(); // Always update status first to get location name
            updateWeather();
            retryCount++;
            if (retryCount < maxRetries) {
                setTimeout(checkWeather, 2000);
            } else {
                enableUI();
            }
        };
        setTimeout(checkWeather, 2000);
    } catch (error) {
        console.error('Failed to save location:', error);
        showToast('Fehler beim Suchen des Standorts', 'error');
        enableUI();
    }
}

// Send Telegram test message
// ========== BACKUP & RESTORE ==========
function exportSettings() {
    try {
        const exportData = {
            version: '1.0',
            exportedAt: new Date().toISOString(),
            settings: {
                mode: currentState.mode,
                tempOn: currentState.tempOn,
                tempOff: currentState.tempOff,
                frostEnabled: currentState.frostEnabled,
                frostTemp: currentState.frostTemp,
                schedules: currentState.schedules || [],
                heaterRelayActiveLow: currentState.heaterRelayActiveLow,
                pumpRelayActiveLow: currentState.pumpRelayActiveLow,
                heaterRelayOffMode: currentState.heaterRelayOffMode,
                pumpRelayOffMode: currentState.pumpRelayOffMode,
                tankHeight: currentState.tankHeight,
                tankCapacity: currentState.tankCapacity,
                dieselConsumptionPerHour: currentState.dieselConsumptionPerHour
            }
        };

        const jsonStr = JSON.stringify(exportData, null, 2);
        const blob = new Blob([jsonStr], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `heizungssteuerung-backup-${new Date().toISOString().split('T')[0]}.json`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);

        showToast('✅ Einstellungen exportiert', 'success', 2000);
    } catch (error) {
        console.error('Export error:', error);
        showToast('Fehler beim Exportieren der Einstellungen', 'error');
    }
}

async function importSettings(event) {
    const file = event.target.files[0];
    if (!file) return;

    if (isLocalMode) {
        showToast('⚠️ Import nur im Live-Modus verfügbar', 'warning');
        return;
    }

    try {
        const text = await file.text();
        const importData = JSON.parse(text);

        if (!importData.settings) {
            throw new Error('Ungültiges Backup-Format');
        }

        // Confirm import
        if (!confirm('Einstellungen importieren? Dies überschreibt alle aktuellen Einstellungen.')) {
            event.target.value = '';
            return;
        }

        const settings = importData.settings;

        // Update current state
        if (settings.mode) currentState.mode = settings.mode;
        if (settings.tempOn !== undefined) currentState.tempOn = settings.tempOn;
        if (settings.tempOff !== undefined) currentState.tempOff = settings.tempOff;
        if (settings.frostEnabled !== undefined) currentState.frostEnabled = settings.frostEnabled;
        if (settings.frostTemp !== undefined) currentState.frostTemp = settings.frostTemp;
        if (settings.schedules) currentState.schedules = settings.schedules;
        if (settings.heaterRelayActiveLow !== undefined) currentState.heaterRelayActiveLow = settings.heaterRelayActiveLow;
        if (settings.pumpRelayActiveLow !== undefined) currentState.pumpRelayActiveLow = settings.pumpRelayActiveLow;
        if (settings.heaterRelayOffMode !== undefined) currentState.heaterRelayOffMode = settings.heaterRelayOffMode;
        if (settings.pumpRelayOffMode !== undefined) currentState.pumpRelayOffMode = settings.pumpRelayOffMode;
        if (settings.tankHeight !== undefined) currentState.tankHeight = settings.tankHeight;
        if (settings.tankCapacity !== undefined) currentState.tankCapacity = settings.tankCapacity;
        if (settings.dieselConsumptionPerHour !== undefined) currentState.dieselConsumptionPerHour = settings.dieselConsumptionPerHour;

        // Save to ESP32
        await saveSettings();

        // Clear file input
        event.target.value = '';

        showToast('✅ Einstellungen importiert und gespeichert', 'success', 3000);

        // Reload UI
        setTimeout(() => {
            updateUI();
        }, 500);
    } catch (error) {
        console.error('Import error:', error);
        showToast('Fehler beim Importieren: ' + error.message, 'error');
        event.target.value = '';
    }
}

async function sendTelegramTest() {
    if (isLocalMode) {
        showToast('⚠️ Telegram-Test nur im Live-Modus verfügbar', 'warning');
        return;
    }

    const btn = document.getElementById('telegramTestBtn');
    btn.disabled = true;
    btn.innerHTML = '<i class="fas fa-spinner fa-spin"></i> Sende...';

    try {
        const response = await fetch('/api/telegram/test', {
            method: 'POST',
            headers: {
                'Authorization': 'Basic ' + btoa('admin:admin')
            }
        });

        const data = await response.json();

        if (data.success) {
            showToast('✅ Test-Nachricht gesendet! Prüfe Telegram.', 'success', 5000);
        } else {
            showToast('❌ ' + data.message, 'error');
        }
    } catch (error) {
        console.error('Telegram test error:', error);
        showToast('Fehler beim Senden der Test-Nachricht', 'error');
    } finally {
        btn.disabled = false;
        btn.innerHTML = '<i class="fas fa-paper-plane"></i> Test-Nachricht senden';
    }
}

function updateUI() {
    // Version
    document.getElementById('versionBadge').textContent = currentState.version;

    // Temperatures
    const vl = document.getElementById('tempVorlauf');
    const rl = document.getElementById('tempRuecklauf');

    vl.textContent = currentState.tempVorlauf !== null ? currentState.tempVorlauf.toFixed(1) + '°C' : 'ERROR';
    rl.textContent = currentState.tempRuecklauf !== null ? currentState.tempRuecklauf.toFixed(1) + '°C' : 'ERROR';

    // Relay badge
    const badge = document.getElementById('relayBadge');
    badge.textContent = currentState.heating ? 'Aktiv' : 'Aus';
    badge.className = 'card-badge ' + (currentState.heating ? 'badge-active' : 'badge-inactive');

    // Mode
    const modeBadge = document.getElementById('modeBadge');
    let modeText = 'Manuell', modeClass = 'badge-manual';
    if (currentState.mode === 'auto') { modeText = 'Automatik'; modeClass = 'badge-auto'; }
    else if (currentState.mode === 'schedule') { modeText = 'Zeitplan'; modeClass = 'badge-schedule'; }

    modeBadge.textContent = modeText;
    modeBadge.className = 'card-badge ' + modeClass;

    document.getElementById('modeManual').classList.toggle('active', currentState.mode === 'manual');
    document.getElementById('modeAuto').classList.toggle('active', currentState.mode === 'auto');
    document.getElementById('modeSchedule').classList.toggle('active', currentState.mode === 'schedule');

    // Show/hide controls
    document.getElementById('manualControl').style.display = currentState.mode === 'manual' ? 'flex' : 'none';
    document.getElementById('pumpControl').style.display = currentState.mode === 'manual' ? 'flex' : 'none';
    document.getElementById('autoInfo').style.display = currentState.mode === 'auto' ? 'block' : 'none';
    document.getElementById('scheduleInfo').style.display = currentState.mode === 'schedule' ? 'block' : 'none';

    document.getElementById('hystereseCard').style.display = currentState.mode === 'auto' ? 'block' : 'none';
    document.getElementById('scheduleCard').style.display = currentState.mode === 'schedule' ? 'block' : 'none';

    // Toggle & settings
    document.getElementById('heatingToggle').checked = currentState.heating;
    document.getElementById('pumpToggle').checked = currentState.pump;

    // Update pump toggle state - disable if heating is ON (safety: pump must be ON when heating is ON)
    const pumpToggle = document.getElementById('pumpToggle');
    if (currentState.heating) {
        pumpToggle.disabled = true;
        pumpToggle.checked = true;  // Force checked when heating is ON
    } else {
        pumpToggle.disabled = false;
    }

    // Update status displays in auto/schedule modes
    document.getElementById('autoHeatingStatus').textContent = currentState.heating ? 'EIN' : 'AUS';
    document.getElementById('autoPumpStatus').textContent = currentState.pump ? 'EIN' : 'AUS';
    document.getElementById('scheduleHeatingStatus').textContent = currentState.heating ? 'EIN' : 'AUS';
    document.getElementById('schedulePumpStatus').textContent = currentState.pump ? 'EIN' : 'AUS';
    document.getElementById('statTempOn').textContent = currentState.tempOn + '°C';
    document.getElementById('statTempOff').textContent = currentState.tempOff + '°C';
    // Don't overwrite user edits while typing / until saved (status refresh calls updateUI frequently)
    const tempOnEl = document.getElementById('tempOn');
    const tempOffEl = document.getElementById('tempOff');
    if (tempOnEl && !tempOnEl.dataset.userEditing && !tempOnEl.dataset.userDirty && document.activeElement !== tempOnEl) {
        tempOnEl.value = currentState.tempOn;
    }
    if (tempOffEl && !tempOffEl.dataset.userEditing && !tempOffEl.dataset.userDirty && document.activeElement !== tempOffEl) {
        tempOffEl.value = currentState.tempOff;
    }

    // Schedule list
    updateScheduleList();

    // Temperature difference & efficiency
    if (currentState.tempDiff !== null && currentState.tempDiff !== undefined) {
        document.getElementById('tempDiff').textContent = 'Δ ' + currentState.tempDiff.toFixed(1) + '°C';
        const efficiencyRounded = Math.round(currentState.efficiency);
        document.getElementById('efficiencyValue').textContent = efficiencyRounded + '%';
        document.getElementById('efficiencyBar').style.width = efficiencyRounded + '%';
    } else {
        document.getElementById('tempDiff').textContent = 'Δ --°C';
        document.getElementById('efficiencyValue').textContent = '--%';
        document.getElementById('efficiencyBar').style.width = '0%';
    }

    // Update compact statistics
    const todaySwitchesCompact = document.getElementById('todaySwitchesCompact');
    const totalSwitchesCompact = document.getElementById('totalSwitchesCompact');
    const onTimeCompact = document.getElementById('onTimeCompact');
    const offTimeCompact = document.getElementById('offTimeCompact');
    if (todaySwitchesCompact) todaySwitchesCompact.textContent = currentState.todaySwitches + 'x';
    if (totalSwitchesCompact) totalSwitchesCompact.textContent = currentState.switchCount + 'x';
    if (onTimeCompact) onTimeCompact.textContent = formatUptime(currentState.onTimeSeconds);
    if (offTimeCompact) offTimeCompact.textContent = formatUptime(currentState.offTimeSeconds);

    // Behavior warning
    const behaviorWarningCard = document.getElementById('behaviorWarningCard');
    if (behaviorWarningCard) {
        behaviorWarningCard.style.display = currentState.behaviorWarning ? 'block' : 'none';
        if (currentState.behaviorWarning && !window.lastBehaviorWarningState) {
            showBrowserNotification('⚠️ Ungewöhnliches Verhalten', 'Zu häufiges Schalten erkannt - bitte prüfen!');
            window.lastBehaviorWarningState = true;
        } else if (!currentState.behaviorWarning) {
            window.lastBehaviorWarningState = false;
        }
    }

    // Heater ON/OFF notifications
    if (window.lastHeatingState !== undefined && window.lastHeatingState !== currentState.heating) {
        if (currentState.heating) {
            showBrowserNotification('🔥 Heizung EIN', 'Die Heizung wurde eingeschaltet');
        } else {
            showBrowserNotification('❄️ Heizung AUS', 'Die Heizung wurde ausgeschaltet');
        }
    }
    window.lastHeatingState = currentState.heating;

    // Sensor error notifications
    const hasSensorError = (currentState.tempVorlauf === null || currentState.tempVorlauf === undefined) ||
        (currentState.tempRuecklauf === null || currentState.tempRuecklauf === undefined);
    if (window.lastSensorErrorState === undefined) window.lastSensorErrorState = false;
    if (hasSensorError && !window.lastSensorErrorState) {
        showBrowserNotification('⚠️ Sensor-Fehler', 'Beide Temperatursensoren ausgefallen');
        window.lastSensorErrorState = true;
    } else if (!hasSensorError && window.lastSensorErrorState) {
        showBrowserNotification('✅ Sensoren wieder OK', 'Temperatursensoren funktionieren wieder');
        window.lastSensorErrorState = false;
    }

    // Tank low notifications
    if (currentState.tankAvailable && currentState.tankPercent !== null && currentState.tankPercent !== undefined) {
        const isTankLow = currentState.tankPercent < 20;
        if (window.lastTankLowState === undefined) window.lastTankLowState = false;
        if (isTankLow && !window.lastTankLowState) {
            showBrowserNotification('🪫 Tank niedrig', `Füllstand: ${currentState.tankPercent}% (< 20%) - Bitte nachfüllen!`);
            window.lastTankLowState = true;
        } else if (!isTankLow && window.lastTankLowState) {
            window.lastTankLowState = false;
        }
    }

    // Update statistics card header if collapsed
    const statisticsCard = document.getElementById('statisticsCard');
    if (statisticsCard) {
        const statisticsContent = statisticsCard.querySelector('.card-content');
        if (statisticsContent && statisticsContent.classList.contains('collapsed')) {
            updateStatisticsCardHeader(true);
        }
    }

    // Frost protection
    document.getElementById('frostEnabled').checked = currentState.frostEnabled;
    document.getElementById('frostTemp').value = currentState.frostTemp;
    document.getElementById('frostTempSetting').style.display = currentState.frostEnabled ? 'block' : 'none';

    // Relay config (advanced)
    const hAct = document.getElementById('heaterRelayActiveLow');
    const pAct = document.getElementById('pumpRelayActiveLow');
    const hOff = document.getElementById('heaterRelayOffMode');
    const pOff = document.getElementById('pumpRelayOffMode');
    if (hAct && pAct && hOff && pOff) {
        hAct.checked = !!currentState.heaterRelayActiveLow;
        pAct.checked = !!currentState.pumpRelayActiveLow;
        hOff.value = String(currentState.heaterRelayOffMode ?? 0);
        pOff.value = String(currentState.pumpRelayOffMode ?? 0);
    }

    // Tank level
    // Don't overwrite user edits while typing (status refresh calls updateUI frequently)
    const tankHeightEl = document.getElementById('tankHeight');
    const tankCapacityEl = document.getElementById('tankCapacity');
    const dieselPerHourEl = document.getElementById('dieselConsumptionPerHour');
    if (tankHeightEl && !tankHeightEl.dataset.userEditing && document.activeElement !== tankHeightEl) {
        tankHeightEl.value = currentState.tankHeight;
    }
    if (tankCapacityEl && !tankCapacityEl.dataset.userEditing && document.activeElement !== tankCapacityEl) {
        tankCapacityEl.value = currentState.tankCapacity;
    }
    if (dieselPerHourEl && !dieselPerHourEl.dataset.userEditing && document.activeElement !== dieselPerHourEl) {
        const value = currentState.dieselConsumptionPerHour || 2.0;
        dieselPerHourEl.value = Math.round(value * 10) / 10; // Round to 1 decimal place
    }

    if (currentState.tankAvailable) {
        document.getElementById('tankAvailableContainer').style.display = 'block';
        document.getElementById('tankNotAvailableContainer').style.display = 'none';

        const liters = currentState.tankLiters || 0;
        const percent = currentState.tankPercent || 0;

        document.getElementById('tankLiters').textContent = liters.toFixed(1) + ' L';
        document.getElementById('tankPercent').textContent = percent + '%';
        document.getElementById('tankFillBar').style.height = percent + '%';

        // Color based on fill level
        if (percent < 20) {
            document.getElementById('tankFillBar').style.background = 'linear-gradient(180deg, #e74c3c 0%, #c0392b 100%)';
            document.getElementById('tankWarning').style.display = 'block';
        } else if (percent < 50) {
            document.getElementById('tankFillBar').style.background = 'linear-gradient(180deg, #f39c12 0%, #e67e22 100%)';
            document.getElementById('tankWarning').style.display = 'none';
        } else {
            document.getElementById('tankFillBar').style.background = 'linear-gradient(180deg, #3498db 0%, #2980b9 100%)';
            document.getElementById('tankWarning').style.display = 'none';
        }
    } else {
        document.getElementById('tankAvailableContainer').style.display = 'none';
        document.getElementById('tankNotAvailableContainer').style.display = 'block';
    }

    // System info in header
    const rssiElement = document.getElementById('rssiValue');
    const wifiIcon = document.getElementById('wifiIcon');
    if (currentState.rssi !== null) {
        rssiElement.textContent = currentState.rssi + ' dBm';
        // Update WiFi icon color based on signal strength
        wifiIcon.className = 'fas fa-wifi wifi-icon';
        if (currentState.rssi >= -50) {
            wifiIcon.classList.add('excellent');
        } else if (currentState.rssi >= -60) {
            wifiIcon.classList.add('good');
        } else if (currentState.rssi >= -70) {
            wifiIcon.classList.add('fair');
        } else {
            wifiIcon.classList.add('poor');
        }
    } else {
        rssiElement.textContent = '--';
        wifiIcon.className = 'fas fa-wifi wifi-icon';
    }

    document.getElementById('uptimeValue').textContent = formatUptime(currentState.uptime);

    const ntpElement = document.getElementById('ntpStatus');
    if (currentState.ntpSynced) {
        ntpElement.textContent = 'Synced';
        ntpElement.className = 'ntp-status synced';
    } else {
        ntpElement.textContent = 'Pending';
        ntpElement.className = 'ntp-status pending';
    }

    document.getElementById('currentTime').textContent = currentState.currentTime || '--:--';

    // Location settings - only update if fields are not focused (to avoid overwriting user input)
    const latField = document.getElementById('latitude');
    const lonField = document.getElementById('longitude');
    const locationInput = document.getElementById('locationInput');

    if (document.activeElement !== latField && document.activeElement !== lonField && document.activeElement !== locationInput) {
        latField.value = currentState.latitude;
        lonField.value = currentState.longitude;

        // Only update location input if it's empty, default value, or we have a VALID location name
        // Don't overwrite user input or set "Unbekannter Ort"
        const currentInputValue = locationInput.value.trim();
        if (currentInputValue === '' || currentInputValue === 'Köln' || currentInputValue === 'Unbekannter Ort' ||
            currentInputValue === 'z.B. Berlin oder 10115' || !currentInputValue) {
            if (currentState.locationName && currentState.locationName !== 'Unbekannter Ort' && currentState.locationName !== '') {
                locationInput.value = currentState.locationName;
            } else if (currentState.weather && currentState.weather.locationName && currentState.weather.locationName !== 'Unbekannter Ort') {
                locationInput.value = currentState.weather.locationName;
            } else {
                locationInput.value = '';
            }
        }
    }
}

function updateScheduleList() {
    const container = document.getElementById('scheduleList');
    container.innerHTML = '';

    for (let i = 0; i < MAX_SCHEDULES; i++) {
        const s = (currentState.schedules && currentState.schedules[i]) ? currentState.schedules[i] : { enabled: false, start: '00:00', end: '00:00' };
        const startVal = (typeof s.start === 'string' && s.start) ? s.start : '00:00';
        const endVal = (typeof s.end === 'string' && s.end) ? s.end : '00:00';
        const item = document.createElement('div');
        item.className = 'schedule-item';
        item.innerHTML = `
            <div class="schedule-header">
                <div class="schedule-title">Zeitfenster ${i + 1}</div>
                <div class="checkbox-control">
                    <input type="checkbox" id="sched${i}En" ${s.enabled ? 'checked' : ''}>
                    <label for="sched${i}En">Aktiv</label>
                </div>
            </div>
            <div class="schedule-times">
                <input type="time" class="time-input" id="sched${i}Start" value="${startVal}">
                <input type="time" class="time-input" id="sched${i}End" value="${endVal}">
            </div>
        `;
        container.appendChild(item);
    }
}

function updateConnectionStatus(connected) {
    document.getElementById('statusDot').classList.toggle('connected', connected);
    document.getElementById('connectionText').textContent = connected ? (isLocalMode ? 'Demo' : 'Verbunden') : 'Fehler';
}

function formatUptime(sec) {
    const h = Math.floor(sec / 3600);
    const m = Math.floor((sec % 3600) / 60);
    return `${h}h ${m}m`;
}

async function setMode(mode) {
    currentState.mode = mode;
    updateUI();

    if (!isLocalMode) {
        try {
            await fetch('/api/settings', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json', 'Authorization': 'Basic ' + btoa('admin:admin') },
                body: JSON.stringify({ mode: mode })
            });
        } catch (e) { console.error(e); }
    } else {
        // Demo log
        const modeNames = { manual: 'Manual', auto: 'Automatic', schedule: 'Schedule' };
        appendSerialLog(`Mode changed to: ${modeNames[mode]}\n`, '#3498db');
    }
}

async function toggleHeating() {
    if (currentState.mode !== 'manual') {
        console.log('[Toggle] Not in manual mode, ignoring toggle');
        // Revert toggle state
        document.getElementById('heatingToggle').checked = !document.getElementById('heatingToggle').checked;
        return;
    }

    console.log('[Toggle] Toggling heater, current state:', currentState.heating);

    if (!isLocalMode) {
        try {
            console.log('[Toggle] Sending API request to /api/toggle');
            const response = await fetch('/api/toggle', {
                method: 'GET',
                headers: { 'Authorization': 'Basic ' + btoa('admin:admin') }
            });

            console.log('[Toggle] Response status:', response.status);

            if (!response.ok) {
                console.error('[Toggle] API call failed:', response.status, await response.text());
                // Revert toggle state
                document.getElementById('heatingToggle').checked = !document.getElementById('heatingToggle').checked;
                return;
            }

            const data = await response.json();
            console.log('[Toggle] API response:', data);

            // Update state from server response
            currentState.heating = data.heating;

            // Update UI WITHOUT changing the toggle (it's already in the right state)
            const toggle = document.getElementById('heatingToggle');
            const oldChecked = toggle.checked;
            updateUI();
            // Restore the toggle state after updateUI might have changed it
            toggle.checked = currentState.heating;

            // Also update status to sync
            await updateStatus();
        } catch (e) {
            console.error('[Toggle] Error:', e);
            // Revert toggle state on error
            document.getElementById('heatingToggle').checked = !document.getElementById('heatingToggle').checked;
        }
    } else {
        // Demo mode
        currentState.heating = document.getElementById('heatingToggle').checked;
        updateUI();
        appendSerialLog(`Manual toggle: Heater ${currentState.heating ? 'ON' : 'OFF'}\n`, '#e67e22');
    }
}

async function togglePump() {
    if (currentState.mode !== 'manual') {
        console.log('[Toggle] Not in manual mode, ignoring pump toggle');
        // Revert toggle state
        document.getElementById('pumpToggle').checked = !document.getElementById('pumpToggle').checked;
        return;
    }

    // Safety check: Cannot turn pump OFF if heating is ON
    if (currentState.heating && document.getElementById('pumpToggle').checked === false) {
        console.log('[Toggle] Cannot turn pump OFF while heating is ON');
        showToast('⚠️ Pumpe kann nicht ausgeschaltet werden, solange die Heizung läuft!', 'warning', 3000);
        // Revert toggle state
        document.getElementById('pumpToggle').checked = true;
        return;
    }

    console.log('[Toggle] Toggling pump, current state:', currentState.pump);

    if (!isLocalMode) {
        try {
            console.log('[Toggle] Sending API request to /api/toggle-pump');
            const response = await fetch('/api/toggle-pump', {
                method: 'GET',
                headers: { 'Authorization': 'Basic ' + btoa('admin:admin') }
            });

            console.log('[Toggle] Response status:', response.status);

            if (!response.ok) {
                const errorText = await response.text();
                console.error('[Toggle] API call failed:', response.status, errorText);
                // Revert toggle state
                document.getElementById('pumpToggle').checked = !document.getElementById('pumpToggle').checked;
                try {
                    const errorData = JSON.parse(errorText);
                    if (errorData.error) {
                        showToast('⚠️ ' + errorData.error, 'warning', 3000);
                    }
                } catch (e) { }
                return;
            }

            const data = await response.json();
            console.log('[Toggle] API response:', data);

            // Update state from server response
            currentState.pump = data.pump;
            currentState.pumpManualMode = data.pumpManualMode;

            // Update UI
            updateUI();

            // Also update status to sync
            await updateStatus();
        } catch (e) {
            console.error('[Toggle] Error:', e);
            // Revert toggle state on error
            document.getElementById('pumpToggle').checked = !document.getElementById('pumpToggle').checked;
            showToast('Fehler beim Umschalten der Pumpe', 'error');
        }
    } else {
        // Demo mode
        currentState.pump = document.getElementById('pumpToggle').checked;
        currentState.pumpManualMode = currentState.pump;
        updateUI();
        appendSerialLog(`Manual toggle: Pump ${currentState.pump ? 'ON' : 'OFF'}\n`, '#3498db');
    }
}

async function saveRelayConfig() {
    // Read UI values
    const heaterRelayActiveLow = document.getElementById('heaterRelayActiveLow').checked;
    const pumpRelayActiveLow = document.getElementById('pumpRelayActiveLow').checked;
    const heaterRelayOffMode = parseInt(document.getElementById('heaterRelayOffMode').value, 10);
    const pumpRelayOffMode = parseInt(document.getElementById('pumpRelayOffMode').value, 10);

    // Update local state immediately
    currentState.heaterRelayActiveLow = heaterRelayActiveLow;
    currentState.pumpRelayActiveLow = pumpRelayActiveLow;
    currentState.heaterRelayOffMode = heaterRelayOffMode;
    currentState.pumpRelayOffMode = pumpRelayOffMode;
    updateUI();

    if (isLocalMode) {
        showToast('⚠️ Demo-Modus: Relais-Einstellungen werden nicht gespeichert.', 'warning', 3000);
        return;
    }

    try {
        const response = await fetch('/api/settings', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json', 'Authorization': 'Basic ' + btoa('admin:admin') },
            body: JSON.stringify({
                heaterRelayActiveLow,
                pumpRelayActiveLow,
                heaterRelayOffMode,
                pumpRelayOffMode
            })
        });
        if (!response.ok) {
            const txt = await response.text();
            console.error('Relay config save failed:', response.status, txt);
            showToast('Fehler beim Speichern der Relais-Einstellungen', 'error');
            return;
        }
        showToast('Relais-Einstellungen gespeichert', 'success', 2000);
        await updateStatus();
    } catch (e) {
        console.error('Relay config save error:', e);
        showToast('Fehler beim Speichern der Relais-Einstellungen', 'error');
    }
}

async function saveSettings() {
    const tempOn = parseFloat(document.getElementById('tempOn').value);
    const tempOff = parseFloat(document.getElementById('tempOff').value);
    const warn = document.getElementById('hystereseWarning');

    if (tempOff <= tempOn) {
        warn.style.display = 'block';
        return;
    }
    warn.style.display = 'none';

    currentState.tempOn = tempOn;
    currentState.tempOff = tempOff;

    for (let i = 0; i < MAX_SCHEDULES; i++) {
        currentState.schedules[i].enabled = document.getElementById(`sched${i}En`).checked;
        currentState.schedules[i].start = document.getElementById(`sched${i}Start`).value;
        currentState.schedules[i].end = document.getElementById(`sched${i}End`).value;
    }

    updateUI();

    if (!isLocalMode) {
        try {
            await fetch('/api/settings', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json', 'Authorization': 'Basic ' + btoa('admin:admin') },
                body: JSON.stringify({ tempOn, tempOff, schedules: currentState.schedules })
            });
            // Clear "dirty" flags after successful save so values can refresh from backend again.
            const tempOnEl = document.getElementById('tempOn');
            const tempOffEl = document.getElementById('tempOff');
            if (tempOnEl) { delete tempOnEl.dataset.userDirty; delete tempOnEl.dataset.userEditing; }
            if (tempOffEl) { delete tempOffEl.dataset.userDirty; delete tempOffEl.dataset.userEditing; }
            showToast('Einstellungen gespeichert', 'success');
        } catch (e) {
            showToast('Fehler beim Speichern', 'error');
        }
    } else {
        showToast('⚠️ Speichern im Demo-Modus nicht möglich. Nach Reload wieder Standard.', 'warning', 4000);
    }
}

async function saveFrostProtection() {
    const enabled = document.getElementById('frostEnabled').checked;
    const temp = parseFloat(document.getElementById('frostTemp').value);

    currentState.frostEnabled = enabled;
    currentState.frostTemp = temp;

    updateUI();

    if (!isLocalMode) {
        try {
            await fetch('/api/settings', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json', 'Authorization': 'Basic ' + btoa('admin:admin') },
                body: JSON.stringify({ frostEnabled: enabled, frostTemp: temp })
            });
            showToast('Frostschutz gespeichert', 'success');
        } catch (e) {
            console.error('Failed to save frost protection:', e);
            showToast('Fehler beim Speichern', 'error');
        }
    } else {
        showToast('⚠️ Speichern im Demo-Modus nicht möglich. Nach Reload wieder Standard.', 'warning', 4000);
    }
}

async function saveTankConfig() {
    const height = parseFloat(document.getElementById('tankHeight').value);
    const capacity = parseFloat(document.getElementById('tankCapacity').value);
    const dieselPerHour = Math.round(parseFloat(document.getElementById('dieselConsumptionPerHour').value) * 10) / 10; // Round to 1 decimal place

    currentState.tankHeight = height;
    currentState.tankCapacity = capacity;
    currentState.dieselConsumptionPerHour = dieselPerHour;

    if (!isLocalMode) {
        try {
            await fetch('/api/settings', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json', 'Authorization': 'Basic ' + btoa('admin:admin') },
                body: JSON.stringify({ tankHeight: height, tankCapacity: capacity, dieselConsumptionPerHour: dieselPerHour })
            });
            showToast('Tank-Konfiguration gespeichert', 'success');
        } catch (e) {
            console.error('Failed to save tank config:', e);
            showToast('Fehler beim Speichern', 'error');
        }
    } else {
        showToast('⚠️ Speichern im Demo-Modus nicht möglich. Nach Reload wieder Standard.', 'warning', 4000);
    }
}

// Prevent tank inputs from being reset by periodic status updates while user is typing.
(function setupTankInputGuards() {
    const tankHeightEl = document.getElementById('tankHeight');
    const tankCapacityEl = document.getElementById('tankCapacity');
    if (!tankHeightEl || !tankCapacityEl) return;

    const markEditing = (el) => { el.dataset.userEditing = '1'; };
    const clearEditing = (el) => { delete el.dataset.userEditing; };

    // Update local state on each keystroke so UI stays consistent even before onchange fires
    tankHeightEl.addEventListener('input', () => {
        markEditing(tankHeightEl);
        const v = parseFloat(tankHeightEl.value);
        if (!Number.isNaN(v)) currentState.tankHeight = v;
    });
    tankCapacityEl.addEventListener('input', () => {
        markEditing(tankCapacityEl);
        const v = parseFloat(tankCapacityEl.value);
        if (!Number.isNaN(v)) currentState.tankCapacity = v;
    });
    const dieselPerHourEl = document.getElementById('dieselConsumptionPerHour');
    if (dieselPerHourEl) {
        dieselPerHourEl.addEventListener('input', () => {
            markEditing(dieselPerHourEl);
            const v = parseFloat(dieselPerHourEl.value);
            if (!Number.isNaN(v)) currentState.dieselConsumptionPerHour = Math.round(v * 10) / 10; // Round to 1 decimal place
        });
        dieselPerHourEl.addEventListener('focus', () => markEditing(dieselPerHourEl));
        dieselPerHourEl.addEventListener('blur', () => clearEditing(dieselPerHourEl));
    }

    tankHeightEl.addEventListener('focus', () => markEditing(tankHeightEl));
    tankCapacityEl.addEventListener('focus', () => markEditing(tankCapacityEl));
    tankHeightEl.addEventListener('blur', () => clearEditing(tankHeightEl));
    tankCapacityEl.addEventListener('blur', () => clearEditing(tankCapacityEl));
})();

// Prevent auto temperature inputs from being reset by periodic status updates while user is typing.
(function setupAutoTempInputGuards() {
    const tempOnEl = document.getElementById('tempOn');
    const tempOffEl = document.getElementById('tempOff');
    if (!tempOnEl || !tempOffEl) return;

    const markEditing = (el) => { el.dataset.userEditing = '1'; };
    const clearEditing = (el) => { delete el.dataset.userEditing; };
    const markDirty = (el) => { el.dataset.userDirty = '1'; };

    tempOnEl.addEventListener('input', () => {
        markEditing(tempOnEl);
        markDirty(tempOnEl);
        const v = parseFloat(tempOnEl.value);
        if (!Number.isNaN(v)) currentState.tempOn = v;
    });
    tempOffEl.addEventListener('input', () => {
        markEditing(tempOffEl);
        markDirty(tempOffEl);
        const v = parseFloat(tempOffEl.value);
        if (!Number.isNaN(v)) currentState.tempOff = v;
    });

    tempOnEl.addEventListener('focus', () => markEditing(tempOnEl));
    tempOffEl.addEventListener('focus', () => markEditing(tempOffEl));
    tempOnEl.addEventListener('blur', () => clearEditing(tempOnEl));
    tempOffEl.addEventListener('blur', () => clearEditing(tempOffEl));
})();

function simulateSensorError() {
    if (!isLocalMode) return;
    currentState.tempVorlauf = currentState.tempVorlauf === null ? 45.3 : null;
    currentState.tempRuecklauf = currentState.tempRuecklauf === null ? 22.2 : null;
    updateUI();
}

function simulateTempChange() {
    if (!isLocalMode) return;
    currentState.tempVorlauf = Math.random() * 60 + 20;
    currentState.tempRuecklauf = Math.random() * 50 + 15;
    updateUI();
}

function simulateSchedule() {
    if (!isLocalMode) return;
    currentState.mode = 'schedule';
    updateUI();
}

function resetDemo() {
    if (!isLocalMode) return;
    currentState.tempVorlauf = 45.3;
    currentState.tempRuecklauf = 22.2;
    currentState.heating = false;
    currentState.mode = 'manual';
    currentState.tempOn = 30;
    currentState.tempOff = 40;
    updateUI();
}

// ========== SERIAL MONITOR (WebSocket) ==========
let ws = null;
let wsReconnectTimer = null;
let waitingForReboot = false; // Flag to indicate we're waiting for ESP32 to reboot after OTA
let rebootCheckInterval = null; // Interval for checking WebSocket reconnection after reboot
let reloadTriggered = false; // Flag to prevent multiple page reloads
let overlayShown = false; // Flag to prevent showing reboot overlay multiple times
let reconnectAttempts = 0; // Counter for reconnection attempts
let isConnecting = false; // Prevent multiple simultaneous connection attempts
const MAX_RECONNECT_DELAY = 30000; // Maximum delay: 30 seconds
const BASE_RECONNECT_DELAY = 2000; // Base delay: 2 seconds

function connectWebSocket() {
    if (isLocalMode) return;

    // Prevent multiple simultaneous connection attempts
    if (isConnecting) {
        console.log('WebSocket connection already in progress, skipping...');
        return;
    }

    // If already connected or connecting, don't create another connection
    if (ws) {
        if (ws.readyState === WebSocket.CONNECTING) {
            console.log('WebSocket connection already in progress, skipping...');
            return;
        }
        if (ws.readyState === WebSocket.OPEN) {
            console.log('WebSocket already connected, skipping...');
            return;
        }
        if (ws.readyState === WebSocket.CLOSING) {
            console.log('WebSocket is closing, waiting before reconnect...');
            return;
        }
        // If closed, clean up
        if (ws.readyState === WebSocket.CLOSED) {
            ws = null;
        }
    }

    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}/ws`;

    isConnecting = true;
    ws = new WebSocket(wsUrl);

    ws.onopen = () => {
        console.log('WebSocket connected');
        isConnecting = false; // Connection successful
        appendSerialLog('// WebSocket verbunden\n', '#27ae60');

        // Reset reconnect attempts on successful connection
        reconnectAttempts = 0;

        // If we were waiting for reboot, ESP32 is back online
        if (waitingForReboot) {
            console.log('ESP32 rebooted and WebSocket reconnected');

            // Clear any check intervals
            if (rebootCheckInterval) {
                clearInterval(rebootCheckInterval);
                rebootCheckInterval = null;
            }

            // Wait for "=== Setup complete ===" message to ensure ESP32 is fully ready
            // This message comes via WebSocket after all initialization is done
            let setupCompleteReceived = false;
            let setupCompleteTimeout = null;
            const originalOnMessage = ws.onmessage;
            ws.onmessage = (event) => {
                // Call original handler
                if (originalOnMessage) {
                    originalOnMessage(event);
                }

                // Check if setup is complete
                if (waitingForReboot && !setupCompleteReceived && !reloadTriggered) {
                    const message = event.data;
                    if (message.includes('=== Setup complete ===') || message.includes('Setup complete')) {
                        setupCompleteReceived = true;
                        waitingForReboot = false;

                        // Cancel fallback timeout
                        if (setupCompleteTimeout) {
                            clearTimeout(setupCompleteTimeout);
                            setupCompleteTimeout = null;
                        }

                        console.log('ESP32 setup complete, closing modal and reloading page...');
                        hideRebootOverlay();
                        // Close modal if open
                        closeOtaModal();

                        // Prevent multiple reloads
                        if (!reloadTriggered) {
                            reloadTriggered = true;
                            showToast('ESP32 ist wieder online! Lade Seite neu...', 'success', 3000);
                            setTimeout(() => {
                                location.reload();
                            }, 1000);
                        }
                    }
                }
            };

            // Fallback: If no "Setup complete" message received within 8 seconds, reload anyway
            setupCompleteTimeout = setTimeout(() => {
                if (waitingForReboot && !setupCompleteReceived && !reloadTriggered) {
                    console.log('Timeout waiting for setup complete, closing modal and reloading anyway...');
                    setupCompleteReceived = true;
                    waitingForReboot = false;
                    hideRebootOverlay();
                    // Close modal if open
                    closeOtaModal();

                    // Prevent multiple reloads
                    if (!reloadTriggered) {
                        reloadTriggered = true;
                        showToast('ESP32 ist wieder online! Lade Seite neu...', 'success', 3000);
                        setTimeout(() => {
                            location.reload();
                        }, 1000);
                    }
                }
            }, 8000);
        }
    };

    ws.onmessage = (event) => {
        // Handle both single-line and multi-line messages
        const data = event.data;
        // Split by newlines and add each line separately
        const lines = data.split('\n');
        lines.forEach((line) => {
            // Skip empty lines only if they're the last line and data ends with \n
            if (line.length > 0) {
                appendSerialLog(line);
            }
        });
    };

    ws.onclose = (event) => {
        console.log('WebSocket disconnected', event.code, event.reason);
        isConnecting = false; // Reset connection flag

        // Don't auto-reconnect if we're waiting for reboot (we handle that separately)
        if (!waitingForReboot) {
            // Only reconnect if this was an unexpected disconnect (not a clean close)
            // Normal close codes: 1000 (normal), 1001 (going away)
            // Don't reconnect on normal closes
            if (event.code !== 1000 && event.code !== 1001) {
                // Only show reconnect message if not already reconnecting
                if (!wsReconnectTimer) {
                    appendSerialLog('// WebSocket getrennt, versuche Wiederverbindung...\n', '#e74c3c');

                    // Calculate reconnect delay with exponential backoff
                    // Also consider WiFi signal strength for longer delays on weak signals
                    let signalDelayMultiplier = 1;
                    if (currentState.rssi !== null && currentState.rssi < -70) {
                        // Weak signal: increase delay multiplier
                        signalDelayMultiplier = 1.5;
                    }

                    const delay = Math.min(
                        BASE_RECONNECT_DELAY * Math.pow(2, reconnectAttempts) * signalDelayMultiplier,
                        MAX_RECONNECT_DELAY
                    );

                    reconnectAttempts++;

                    wsReconnectTimer = setTimeout(() => {
                        wsReconnectTimer = null;
                        // Clear ws reference before reconnecting
                        ws = null;
                        // Only reconnect if no active connection exists
                        if (!isConnecting) {
                            connectWebSocket();
                        }
                    }, delay);
                }
            }
        } else {
            console.log('WebSocket closed, waiting for ESP32 reboot...');
        }
        // Clear ws reference after handling
        ws = null;
    };

    ws.onerror = (error) => {
        console.error('WebSocket error:', error);
        isConnecting = false; // Reset on error
    };
}

function appendSerialLog(message, color = '#d4d4d4') {
    const monitor = document.getElementById('serialMonitor');
    const line = document.createElement('div');
    line.style.color = color;
    line.textContent = message.replace(/\n$/, '');

    // Remove first child if buffer is too large (> 200 lines)
    if (monitor.children.length > 200) {
        monitor.removeChild(monitor.firstChild);
    }

    monitor.appendChild(line);

    // Auto-scroll if enabled
    if (document.getElementById('serialAutoScroll').checked) {
        monitor.scrollTop = monitor.scrollHeight;
    }
}

function clearSerialMonitor() {
    document.getElementById('serialMonitor').innerHTML = '<div style="color: #608b4e;">// Log gelöscht</div>';
}

// ========== REBOOT/UPDATE OVERLAY ==========
function showRebootOverlay(title, sub) {
    // Prevent showing overlay multiple times
    if (overlayShown) return;
    overlayShown = true;

    const overlay = document.getElementById('rebootOverlay');
    if (!overlay) return;
    const titleEl = document.getElementById('rebootOverlayTitle');
    const subEl = document.getElementById('rebootOverlaySub');
    if (titleEl && title) titleEl.textContent = title;
    if (subEl && sub) subEl.innerHTML = sub;
    overlay.classList.add('active');
}

function hideRebootOverlay() {
    overlayShown = false; // Reset flag when hiding
    const overlay = document.getElementById('rebootOverlay');
    if (!overlay) return;
    overlay.classList.remove('active');
}

// ========== STATS MODAL ==========
function openStatsModal(event) {
    if (event && event.stopPropagation) event.stopPropagation();
    const modal = document.getElementById('statsModal');
    if (!modal) return;
    modal.classList.add('active');
    refreshStatsModal();
}

function closeStatsModal() {
    const modal = document.getElementById('statsModal');
    if (!modal) return;
    modal.classList.remove('active');
}

function formatSecondsHMS(seconds) {
    const s = Math.max(0, Number(seconds) || 0);
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    return `${h}h ${m}m`;
}

function formatDateKey(dateKey) {
    const s = String(dateKey || '');
    if (s.length !== 8) return s || '-';
    // German format: DD.MM.YYYY
    return `${s.slice(6, 8)}.${s.slice(4, 6)}.${s.slice(0, 4)}`;
}

async function refreshStatsModal() {
    if (isLocalMode) {
        showToast('⚠️ Demo-Modus: Statistik-History ist nur auf dem ESP32 verfügbar', 'warning', 3000);
        return;
    }
    
    // Show loading indicator
    const loadingEl = document.getElementById('statsModalLoading');
    const daysListEl = document.getElementById('statsModalDaysList');
    const eventsEl = document.getElementById('statsModalSwitchEvents');
    
    if (loadingEl) loadingEl.style.display = 'block';
    if (daysListEl) daysListEl.style.display = 'none';
    if (eventsEl) eventsEl.style.display = 'none';
    
    try {
        const res = await fetch('/api/stats-history');
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const data = await res.json();
        window.__lastStatsHistory = data;
        
        // Hide loading, show content
        if (loadingEl) loadingEl.style.display = 'none';
        if (daysListEl) daysListEl.style.display = 'block';
        if (eventsEl) eventsEl.style.display = 'block';
        
        renderStatsModal(data);
        
        // Show warning toast if MySQL is not available
        if (data.mysqlAvailable === false) {
            showToast('⚠️ MySQL-Verbindung nicht verfügbar. Zeige lokale Daten.', 'warning', 4000);
        }
    } catch (e) {
        console.error('stats-history failed:', e);
        
        // Hide loading on error
        if (loadingEl) loadingEl.style.display = 'none';
        if (daysListEl) daysListEl.style.display = 'block';
        if (eventsEl) eventsEl.style.display = 'block';
        
        showToast('Fehler beim Laden der Statistik-History', 'error');
        
        // Try to render with cached data if available
        if (window.__lastStatsHistory) {
            renderStatsModal(window.__lastStatsHistory);
        } else {
            // Show MySQL warning even on error if we have no cached data
            const mysqlWarningEl = document.getElementById('statsModalMySQLWarning');
            if (mysqlWarningEl) {
                mysqlWarningEl.style.display = 'block';
            }
        }
    }
}

function renderStatsModal(data) {
    const summaryEl = document.getElementById('statsModalSummary');
    if (!summaryEl) return;
    
    // Show/hide MySQL warning
    const mysqlWarningEl = document.getElementById('statsModalMySQLWarning');
    if (mysqlWarningEl) {
        const mysqlAvailable = data.mysqlAvailable !== undefined ? data.mysqlAvailable : true;
        mysqlWarningEl.style.display = mysqlAvailable ? 'none' : 'block';
    }

    const rangeSelect = document.getElementById('statsRangeSelect');
    const rangeDays = rangeSelect ? parseInt(rangeSelect.value, 10) : 14;

    const totalSwitches = data.switchCount ?? currentState.switchCount ?? 0;
    const onSec = data.onTimeSeconds ?? currentState.onTimeSeconds ?? 0;
    const offSec = data.offTimeSeconds ?? currentState.offTimeSeconds ?? 0;

    // Build rows array: today first, then history
    const rows = [];
    if (data.today && data.today.dateKey) rows.push({ ...data.today, _today: true });
    if (Array.isArray(data.days)) {
        for (const d of data.days) rows.push(d);
    }

    // Weekly aggregation over selected range (today + last N-1 days)
    const slice = rows.slice(0, Math.max(1, Math.min(rangeDays, 14)));
    const agg = {
        switches: 0,
        onSeconds: 0,
        offSeconds: 0,
        sumVorlauf10: 0,
        sumRuecklauf10: 0,
        minVorlauf: null,
        maxVorlauf: null,
        minRuecklauf: null,
        maxRuecklauf: null
    };
    const updateMinMax = (keyMin, keyMax, v) => {
        if (v === null || v === undefined || Number.isNaN(Number(v))) return;
        const n = Number(v);
        agg[keyMin] = (agg[keyMin] === null) ? n : Math.min(agg[keyMin], n);
        agg[keyMax] = (agg[keyMax] === null) ? n : Math.max(agg[keyMax], n);
    };
    for (const r of slice) {
        agg.switches += Number(r.switches || 0);
        agg.onSeconds += Number(r.onSeconds || 0);
        agg.offSeconds += Number(r.offSeconds || 0);
        // Aggregate temperature data (if available)
        // MySQL returns temperatures as strings, so we need to parse them
        const avgVorlaufVal = r.avgVorlauf !== null && r.avgVorlauf !== undefined ? parseFloat(r.avgVorlauf) : null;
        const avgRuecklaufVal = r.avgRuecklauf !== null && r.avgRuecklauf !== undefined ? parseFloat(r.avgRuecklauf) : null;
        
        if (avgVorlaufVal !== null && !isNaN(avgVorlaufVal)) {
            agg.sumVorlauf10 += avgVorlaufVal * 10;
        }
        if (avgRuecklaufVal !== null && !isNaN(avgRuecklaufVal)) {
            agg.sumRuecklauf10 += avgRuecklaufVal * 10;
        }
        updateMinMax('minVorlauf', 'maxVorlauf', r.minVorlauf);
        updateMinMax('minVorlauf', 'maxVorlauf', r.maxVorlauf);
        updateMinMax('minRuecklauf', 'maxRuecklauf', r.minRuecklauf);
        updateMinMax('minRuecklauf', 'maxRuecklauf', r.maxRuecklauf);
    }

    // Calculate averages (divide by number of days with data)
    const daysWithVorlauf = slice.filter(r => {
        const val = r.avgVorlauf !== null && r.avgVorlauf !== undefined ? parseFloat(r.avgVorlauf) : null;
        return val !== null && !isNaN(val) && val > 0;
    }).length;
    const daysWithRuecklauf = slice.filter(r => {
        const val = r.avgRuecklauf !== null && r.avgRuecklauf !== undefined ? parseFloat(r.avgRuecklauf) : null;
        return val !== null && !isNaN(val) && val > 0;
    }).length;
    
    const avgVorlauf = (daysWithVorlauf > 0 && agg.sumVorlauf10 > 0) ? (agg.sumVorlauf10 / (10 * daysWithVorlauf)) : null;
    const avgRuecklauf = (daysWithRuecklauf > 0 && agg.sumRuecklauf10 > 0) ? (agg.sumRuecklauf10 / (10 * daysWithRuecklauf)) : null;

    const weeklyLabel = (rangeDays === 7) ? '7 Tage' : '14 Tage';
    summaryEl.innerHTML = `
        <div style="border:1px solid var(--border); border-radius:10px; padding:10px; background: rgba(255,255,255,0.04);">
            <div style="font-size:11px; color: var(--muted);">Gesamt Schaltungen</div>
            <div style="font-size:18px; font-weight:800; color: var(--text);">${totalSwitches}x</div>
        </div>
        <div style="border:1px solid var(--border); border-radius:10px; padding:10px; background: rgba(255,255,255,0.04);">
            <div style="font-size:11px; color: var(--muted);">Heizung ON Gesamt</div>
            <div style="font-size:18px; font-weight:800; color: var(--text);">${formatSecondsHMS(onSec)}</div>
        </div>
        <div style="border:1px solid var(--border); border-radius:10px; padding:10px; background: rgba(255,255,255,0.04);">
            <div style="font-size:11px; color: var(--muted);">Heizung OFF Gesamt</div>
            <div style="font-size:18px; font-weight:800; color: var(--text);">${formatSecondsHMS(offSec)}</div>
        </div>
        <div style="grid-column: 1 / -1; border:1px solid var(--border); border-radius:10px; padding:10px; background: rgba(255,255,255,0.04);">
            <div style="display:flex; justify-content: space-between; gap: 10px; align-items: baseline;">
                <div style="font-size:11px; color: var(--muted);">Übersicht (${weeklyLabel})</div>
            </div>
            <div style="display:grid; grid-template-columns: repeat(5, 1fr); gap: 8px; margin-top: 8px;">
                <div><div style="font-size:11px; color: var(--muted);">Schaltungen</div><div style="font-weight:800; color: var(--text);">${agg.switches}x</div></div>
                <div><div style="font-size:11px; color: var(--muted);">Heizung ON</div><div style="font-weight:800; color: var(--text);">${formatSecondsHMS(agg.onSeconds)}</div></div>
                <div><div style="font-size:11px; color: var(--muted);">Diesel</div><div style="font-weight:800; color: var(--text);">${((agg.onSeconds / 3600.0) * (currentState.dieselConsumptionPerHour || 2.0)).toFixed(1)}L</div></div>
                <div><div style="font-size:11px; color: var(--muted);">Ø Vorlauf</div><div style="font-weight:800; color: var(--text);">${avgVorlauf === null ? '—' : avgVorlauf.toFixed(1) + '°C'}</div></div>
                <div><div style="font-size:11px; color: var(--muted);">Ø Rücklauf</div><div style="font-weight:800; color: var(--text);">${avgRuecklauf === null ? '—' : avgRuecklauf.toFixed(1) + '°C'}</div></div>
            </div>
            <div style="display:grid; grid-template-columns: repeat(2, 1fr); gap: 8px; margin-top: 8px;">
                <div style="font-size:12px; color: var(--text);">
                    Vorlauf Min/Max: <b>${agg.minVorlauf === null ? '—' : agg.minVorlauf.toFixed(1) + '°C'}</b> / <b>${agg.maxVorlauf === null ? '—' : agg.maxVorlauf.toFixed(1) + '°C'}</b>
                </div>
                <div style="font-size:12px; color: var(--text);">
                    Rücklauf Min/Max: <b>${agg.minRuecklauf === null ? '—' : agg.minRuecklauf.toFixed(1) + '°C'}</b> / <b>${agg.maxRuecklauf === null ? '—' : agg.maxRuecklauf.toFixed(1) + '°C'}</b>
                </div>
            </div>
        </div>
    `;

    // Render daily values as cards (scrollable list)
    const daysListEl = document.getElementById('statsModalDaysList');
    if (daysListEl) {
        // Filter: Only show days with actual data (switches > 0 OR onSeconds > 0)
        const displayRows = rows.filter(r => {
            const hasSwitches = (r.switches ?? 0) > 0;
            const hasOnTime = (r.onSeconds ?? 0) > 0;
            return hasSwitches || hasOnTime;
        });
        
        if (displayRows.length > 0) {
            daysListEl.innerHTML = `
                <div style="font-weight: 700; color: var(--text); margin-bottom: 10px;">Tageswerte</div>
                <div style="display: flex; flex-direction: column; gap: 8px;">
                    ${displayRows.map((r) => {
                const date = formatDateKey(r.dateKey);
                const sw = r.switches ?? 0;
                const on = formatSecondsHMS(r.onSeconds ?? 0);
                const av = (r.avgVorlauf === null || r.avgVorlauf === undefined) ? '—' : `${Number(r.avgVorlauf).toFixed(1)}°C`;
                const ar = (r.avgRuecklauf === null || r.avgRuecklauf === undefined) ? '—' : `${Number(r.avgRuecklauf).toFixed(1)}°C`;
                const todayLabel = r._today ? ' <span style="font-size: 10px; color: var(--muted);">(heute)</span>' : '';
                return `
                            <div style="border:1px solid var(--border); border-radius:8px; padding:12px; background: rgba(255,255,255,0.04); flex-shrink: 0;">
                                <div style="display:flex; justify-content: space-between; align-items: center; margin-bottom: 10px;">
                                    <div style="font-weight: 600; color: var(--text); font-size: 14px;">${date}${todayLabel}</div>
                                    <div style="font-size: 12px; color: var(--text); font-weight: 600;">${sw}x</div>
                                </div>
                                <div style="display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; font-size: 12px;">
                                    <div>
                                        <div style="color: var(--muted); font-size: 11px; margin-bottom: 4px;">ON Zeit</div>
                                        <div style="color: var(--text); font-weight: 600;">${on}</div>
                                    </div>
                                    <div>
                                        <div style="color: var(--muted); font-size: 11px; margin-bottom: 4px;">Diesel</div>
                                        <div style="color: var(--text); font-weight: 600;">${(r.dieselLiters !== null && r.dieselLiters !== undefined) ? Number(r.dieselLiters).toFixed(1) + 'L' : '—'}</div>
                                    </div>
                                    <div>
                                        <div style="color: var(--muted); font-size: 11px; margin-bottom: 4px;">Ø Vorlauf</div>
                                        <div style="color: var(--text); font-weight: 600;">${av}</div>
                                    </div>
                                    <div>
                                        <div style="color: var(--muted); font-size: 11px; margin-bottom: 4px;">Ø Rücklauf</div>
                                        <div style="color: var(--text); font-weight: 600;">${ar}</div>
                                    </div>
                                </div>
                            </div>
                        `;
            }).join('')}
                </div>
            `;
        } else {
            daysListEl.innerHTML = '<div style="color: var(--muted); text-align: center; padding: 20px;">Keine Tageswerte verfügbar</div>';
        }
    }

    // Render heating periods (ON -> OFF cycles)
    const eventsEl = document.getElementById('statsModalSwitchEvents');
    if (eventsEl) {
        // Check if we have switch events
        if (!Array.isArray(data.switchEvents) || data.switchEvents.length === 0) {
            eventsEl.innerHTML = '<div style="color: var(--muted); text-align: center; padding: 10px;">Keine Heizungsperioden vorhanden.<br><span style="font-size: 10px;">Switch-Events werden automatisch gespeichert, wenn die Heizung ein- und ausgeschaltet wird.</span></div>';
        } else {
            // Sort events chronologically (oldest first)
            let sortedEvents = [...data.switchEvents].sort((a, b) => {
                if (a.timestamp && b.timestamp) return a.timestamp - b.timestamp;
                if (a.uptimeMs && b.uptimeMs) return a.uptimeMs - b.uptimeMs;
                return 0;
            });

            // Remove duplicates: if multiple events have the same timestamp and isOn value,
            // prefer the one with tankLiters
            const eventMap = new Map();
            for (const evt of sortedEvents) {
                const key = `${evt.timestamp || evt.uptimeMs}_${evt.isOn ? '1' : '0'}`;
                const existing = eventMap.get(key);
                if (!existing) {
                    eventMap.set(key, evt);
                } else {
                    // Prefer event with tankLiters
                    const evtHasTank = evt.tankLiters !== null && evt.tankLiters !== undefined && !isNaN(evt.tankLiters);
                    const existingHasTank = existing.tankLiters !== null && existing.tankLiters !== undefined && !isNaN(existing.tankLiters);
                    if (evtHasTank && !existingHasTank) {
                        eventMap.set(key, evt);
                    }
                }
            }
            sortedEvents = Array.from(eventMap.values()).sort((a, b) => {
                if (a.timestamp && b.timestamp) return a.timestamp - b.timestamp;
                if (a.uptimeMs && b.uptimeMs) return a.uptimeMs - b.uptimeMs;
                return 0;
            });

            // Group events into periods (ON -> OFF)
            const periods = [];
            let currentPeriod = null;

            for (const evt of sortedEvents) {
                if (evt.isOn) {
                    // Start of a new period
                    if (currentPeriod && currentPeriod.endTime === null) {
                        // Previous period never ended, skip it
                    }
                    // Parse tankLiters - handle both number and string from MySQL
                    let startTank = null;
                    if (evt.tankLiters !== null && evt.tankLiters !== undefined) {
                        const parsed = typeof evt.tankLiters === 'string' ? parseFloat(evt.tankLiters) : Number(evt.tankLiters);
                        if (!isNaN(parsed) && parsed >= 0) {
                            startTank = parsed;
                        }
                    }
                    
                    currentPeriod = {
                        startTime: evt.timestamp ? evt.timestamp * 1000 : evt.uptimeMs,
                        endTime: null,
                        startTempVorlauf: evt.tempVorlauf,
                        startTempRuecklauf: evt.tempRuecklauf,
                        endTempVorlauf: null,
                        endTempRuecklauf: null,
                        startTankLiters: startTank,
                        endTankLiters: null,
                        hasTimestamp: evt.timestamp > 0
                    };
                } else if (currentPeriod && currentPeriod.endTime === null) {
                    // End of current period
                    // Parse tankLiters - handle both number and string from MySQL
                    let endTank = null;
                    if (evt.tankLiters !== null && evt.tankLiters !== undefined) {
                        const parsed = typeof evt.tankLiters === 'string' ? parseFloat(evt.tankLiters) : Number(evt.tankLiters);
                        if (!isNaN(parsed) && parsed >= 0) {
                            endTank = parsed;
                        }
                    }
                    
                    currentPeriod.endTime = evt.timestamp ? evt.timestamp * 1000 : evt.uptimeMs;
                    currentPeriod.endTempVorlauf = evt.tempVorlauf;
                    currentPeriod.endTempRuecklauf = evt.tempRuecklauf;
                    currentPeriod.endTankLiters = endTank;
                    periods.push(currentPeriod);
                    currentPeriod = null;
                }
            }

            // If currently ON, add the ongoing period
            if (currentPeriod && currentPeriod.endTime === null && currentState.heatingOn) {
                const now = Date.now();
                currentPeriod.endTime = now;
                currentPeriod.endTempVorlauf = currentState.tempVorlauf;
                currentPeriod.endTempRuecklauf = currentState.tempRuecklauf;
                currentPeriod.endTankLiters = currentState.tankLiters;
                periods.push(currentPeriod);
            }

            // Sort periods by start time (newest first)
            periods.sort((a, b) => b.startTime - a.startTime);

            if (periods.length > 0) {
                eventsEl.innerHTML = `
                <div style="font-weight: 700; color: var(--text); margin-bottom: 10px;">Heizungsperioden (letzte ${periods.length})</div>
                <div style="display: grid; grid-template-columns: 1fr; gap: 8px; max-height: 400px; overflow-y: auto;">
                    ${periods.map((period) => {
                    const startDate = new Date(period.startTime);
                    const endDate = new Date(period.endTime);
                    const durationMs = period.endTime - period.startTime;
                    const durationHours = Math.floor(durationMs / 3600000);
                    const durationMinutes = Math.floor((durationMs % 3600000) / 60000);

                    let startTimeStr = '—';
                    let endTimeStr = '—';
                    if (period.hasTimestamp) {
                        startTimeStr = startDate.toLocaleString('de-DE', {
                            day: '2-digit',
                            month: '2-digit',
                            hour: '2-digit',
                            minute: '2-digit'
                        });
                        endTimeStr = endDate.toLocaleString('de-DE', {
                            day: '2-digit',
                            month: '2-digit',
                            hour: '2-digit',
                            minute: '2-digit'
                        });
                    } else {
                        const startHours = Math.floor(period.startTime / 3600000);
                        const startMins = Math.floor((period.startTime % 3600000) / 60000);
                        const endHours = Math.floor(period.endTime / 3600000);
                        const endMins = Math.floor((period.endTime % 3600000) / 60000);
                        startTimeStr = `Uptime: ${startHours}h ${startMins}m`;
                        endTimeStr = `Uptime: ${endHours}h ${endMins}m`;
                    }

                    const durationStr = `${durationHours}h ${durationMinutes}m`;
                    // Show end temperatures (when heating turned off) as primary values
                    const endTempV = (period.endTempVorlauf !== null && period.endTempVorlauf !== undefined)
                        ? Number(period.endTempVorlauf).toFixed(1)
                        : (period.startTempVorlauf !== null ? Number(period.startTempVorlauf).toFixed(1) : '—');
                    const endTempR = (period.endTempRuecklauf !== null && period.endTempRuecklauf !== undefined)
                        ? Number(period.endTempRuecklauf).toFixed(1)
                        : (period.startTempRuecklauf !== null ? Number(period.startTempRuecklauf).toFixed(1) : '—');

                    // Calculate diesel consumption
                    const consumptionPerHour = currentState.dieselConsumptionPerHour || 1.3;
                    const calculatedConsumption = (durationMs / 3600000.0) * consumptionPerHour;

                    // Calculate measured consumption from tank level change (if available)
                    let measuredConsumption = null;
                    let consumptionComparison = null;
                    if (period.startTankLiters !== null && period.startTankLiters !== undefined &&
                        period.endTankLiters !== null && period.endTankLiters !== undefined &&
                        !isNaN(period.startTankLiters) && !isNaN(period.endTankLiters)) {
                        // Tank level decreases when diesel is consumed
                        measuredConsumption = Number(period.startTankLiters) - Number(period.endTankLiters);
                        if (measuredConsumption > 0) {
                            consumptionComparison = ((measuredConsumption - calculatedConsumption) / calculatedConsumption * 100).toFixed(1);
                        } else if (measuredConsumption < 0) {
                            // Tank was refilled during heating period - show as negative
                            measuredConsumption = Math.abs(measuredConsumption);
                            consumptionComparison = null; // Can't compare if tank was refilled
                        }
                    }

                    return `
                            <div style="border:1px solid var(--border); border-radius:8px; padding:12px; background: rgba(255,255,255,0.04);">
                                <div style="display:flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
                                    <div style="font-weight: 600; color: var(--text); font-size: 13px;">🔥 Heizung gelaufen</div>
                                    <div style="color: var(--muted); font-size: 11px;">${durationStr}</div>
                                </div>
                                <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-bottom: 8px; font-size: 11px;">
                                    <div>
                                        <div style="color: var(--muted);">Von</div>
                                        <div style="color: var(--text); font-weight: 600;">${startTimeStr}</div>
                                    </div>
                                    <div>
                                        <div style="color: var(--muted);">Bis</div>
                                        <div style="color: var(--text); font-weight: 600;">${endTimeStr}</div>
                                    </div>
                                </div>
                                <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-bottom: 8px; font-size: 11px;">
                                    <div>
                                        <div style="color: var(--muted);">Vorlauf</div>
                                        <div style="color: var(--text); font-weight: 600;">${endTempV}${endTempV !== '—' ? '°C' : ''}</div>
                                    </div>
                                    <div>
                                        <div style="color: var(--muted);">Rücklauf</div>
                                        <div style="color: var(--text); font-weight: 600;">${endTempR}${endTempR !== '—' ? '°C' : ''}</div>
                                    </div>
                                </div>
                                <div style="border-top: 1px solid var(--border); padding-top: 8px; margin-top: 8px;">
                                    <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 8px; font-size: 11px;">
                                        <div>
                                            <div style="color: var(--muted);">Diesel (berechnet)</div>
                                            <div style="color: var(--text); font-weight: 600;">${calculatedConsumption.toFixed(2)}L</div>
                                            <div style="color: var(--muted); font-size: 9px;">(${consumptionPerHour.toFixed(1)}L/h × ${durationStr})</div>
                                        </div>
                                        ${measuredConsumption !== null && measuredConsumption > 0 ? `
                                        <div>
                                            <div style="color: var(--muted);">Diesel (gemessen)</div>
                                            <div style="color: var(--text); font-weight: 600;">${measuredConsumption.toFixed(2)}L</div>
                                            ${consumptionComparison !== null ? `
                                            <div style="color: ${Math.abs(Number(consumptionComparison)) > 20 ? '#e74c3c' : '#f39c12'}; font-size: 9px;">
                                                ${Number(consumptionComparison) > 0 ? '+' : ''}${consumptionComparison}% Abweichung
                                            </div>
                                            ` : ''}
                                        </div>
                                        ` : `
                                        <div>
                                            <div style="color: var(--muted);">Diesel (gemessen)</div>
                                            <div style="color: var(--muted); font-size: 10px;">— (Sensor ungenau)</div>
                                        </div>
                                        `}
                                    </div>
                                </div>
                            </div>
                        `;
                }).join('')}
                </div>
            `;
            } else {
                eventsEl.innerHTML = '<div style="color: var(--muted); text-align: center; padding: 10px;">Keine vollständigen Heizungsperioden gefunden.<br><span style="font-size: 10px;">Es wurden ' + sortedEvents.length + ' Switch-Events gefunden, aber keine vollständigen ON->OFF-Zyklen.</span></div>';
            }
        }
    }
}

// ========== COLLAPSIBLE CARDS (persisted in localStorage) ==========
function initCollapsibleCards() {
    const STORAGE_PREFIX = 'ui:cardCollapse:';

    const getDirectChild = (parent, className) => {
        for (const el of parent.children) {
            if (el.classList && el.classList.contains(className)) return el;
        }
        return null;
    };

    const slugify = (s) => (s || '')
        .toLowerCase()
        .trim()
        .replace(/\s+/g, '_')
        .replace(/[^a-z0-9_äöüß-]/g, '')
        .slice(0, 60);

    const setCollapsed = (content, arrow, collapsed, cardId) => {
        content.classList.toggle('collapsed', collapsed);
        content.classList.toggle('expanded', !collapsed);
        if (arrow) arrow.classList.toggle('expanded', !collapsed);

        // Special handling for weather and statistics cards
        if (cardId === 'weatherCard') {
            // Use setTimeout to ensure DOM is updated before calling updateWeatherCardHeader
            setTimeout(() => updateWeatherCardHeader(collapsed), 10);
        } else if (cardId === 'statisticsCard') {
            updateStatisticsCardHeader(collapsed);
        }
    };

    const getStorageKey = (card, titleText, index) => {
        if (card.id) return STORAGE_PREFIX + card.id;
        const base = slugify(titleText) || `card_${index}`;
        return STORAGE_PREFIX + base + '_' + index; // index makes it stable even for duplicate titles
    };

    const isInteractiveHeaderClick = (target) => {
        return !!target.closest('input, select, textarea, button, a, label');
    };

    const cards = document.querySelectorAll('.container .card');
    cards.forEach((card, index) => {
        // Determine header + content parent
        let contentParent = card;
        let header = getDirectChild(card, 'card-header');
        if (!header) {
            const body = getDirectChild(card, 'card-body');
            if (body) {
                const bodyHeader = getDirectChild(body, 'card-header');
                if (bodyHeader) {
                    header = bodyHeader;
                    contentParent = body;
                }
            }
        }
        if (!header) return;
        if (header.dataset.collapseInited === '1') return;
        header.dataset.collapseInited = '1';

        header.classList.add('collapsible');

        // Ensure arrow exists
        const titleEl = header.querySelector('.card-title');
        const titleText = titleEl ? titleEl.textContent.trim() : '';
        let arrow = titleEl ? titleEl.querySelector('.collapse-arrow') : null;
        if (titleEl && !arrow) {
            arrow = document.createElement('i');
            arrow.className = 'fas fa-chevron-right collapse-arrow';
            titleEl.prepend(arrow);
        }

        // Ensure we have a .card-content sibling (wrap everything after header if needed)
        let content = header.nextElementSibling;
        if (!(content && content.classList && content.classList.contains('card-content'))) {
            const wrapper = document.createElement('div');
            wrapper.className = 'card-content expanded';

            // Move all nodes after header into wrapper (including text nodes)
            while (header.nextSibling) {
                wrapper.appendChild(header.nextSibling);
            }
            contentParent.appendChild(wrapper);
            content = wrapper;
        }

        const storageKey = getStorageKey(card, titleText, index);
        const stored = localStorage.getItem(storageKey);
        const defaultCollapsed = (card.dataset.collapseDefault === 'collapsed') || content.classList.contains('collapsed');
        const collapsed = stored ? (stored === 'collapsed') : defaultCollapsed;

        setCollapsed(content, arrow, collapsed, card.id || null);

        header.addEventListener('click', (e) => {
            if (isInteractiveHeaderClick(e.target)) return;
            const nowCollapsed = !content.classList.contains('collapsed');
            setCollapsed(content, arrow, nowCollapsed, card.id || null);
            localStorage.setItem(storageKey, nowCollapsed ? 'collapsed' : 'expanded');
            // Force update weather header after toggle to ensure it shows correctly
            if (card.id === 'weatherCard') {
                // Use longer timeout to ensure DOM is fully updated and prevent race conditions
                setTimeout(() => {
                    const wc = document.getElementById('weatherCard');
                    if (wc) {
                        const wcContent = wc.querySelector('.card-content');
                        if (wcContent) {
                            const isCollapsedNow = wcContent.classList.contains('collapsed');
                            updateWeatherCardHeader(isCollapsedNow);
                        }
                    }
                }, 150);
            }
        });
    });

    // Initial update for weather and statistics cards
    const weatherCard = document.getElementById('weatherCard');
    const statisticsCard = document.getElementById('statisticsCard');
    if (weatherCard) {
        const weatherContent = weatherCard.querySelector('.card-content');
        if (weatherContent) {
            updateWeatherCardHeader(weatherContent.classList.contains('collapsed'));
        }
    }
    if (statisticsCard) {
        const statisticsContent = statisticsCard.querySelector('.card-content');
        if (statisticsContent) {
            updateStatisticsCardHeader(statisticsContent.classList.contains('collapsed'));
        }
    }
}

// Update weather card header based on collapsed state
function updateWeatherCardHeader(collapsed) {
    const titleEl = document.getElementById('weatherTitle');
    const locationEl = document.getElementById('weatherLocation');
    const headerMiniEl = document.getElementById('headerWeatherMini');
    const headerIconEl = document.getElementById('headerWeatherIcon');
    const headerTempEl = document.getElementById('headerWeatherTemp');
    if (!titleEl) return;

    if (collapsed) {
        // Show weather icon + temperature when collapsed
        // Always check currentState.weather, don't lose data on collapse
        const w = currentState.weather;
        if (w && w.valid && w.temperature !== undefined) {
            const currentInfo = getWeatherInfo(w.weatherCode);
            const temp = Math.round(w.temperature);
            titleEl.textContent = `${currentInfo.icon} ${temp}°C`;
            titleEl.style.fontSize = '14px';
            if (locationEl) locationEl.style.display = 'none';

            // Show mini weather in header
            if (headerMiniEl) headerMiniEl.style.display = 'flex';
            if (headerIconEl) headerIconEl.textContent = currentInfo.icon;
            if (headerTempEl) headerTempEl.textContent = `${temp}°C`;
        } else {
            // Still show location or icon even if temp not available
            const loc = w && w.locationName ? w.locationName : (currentState.locationName || '');
            if (loc && loc !== 'Unbekannter Ort') {
                titleEl.textContent = `🌤️ ${loc}`;
            } else {
                titleEl.textContent = '🌤️ Wetter';
            }
            titleEl.style.fontSize = '14px';
            if (locationEl) locationEl.style.display = 'none';

            // Hide mini weather in header if no valid data
            if (headerMiniEl) headerMiniEl.style.display = 'none';
        }
    } else {
        // Show "🌤️ Wetter" when expanded (always, never show location in title)
        titleEl.textContent = '🌤️ Wetter';
        titleEl.style.fontSize = '';
        // Show location in subtitle when expanded
        if (locationEl) {
            const w = currentState.weather;
            const loc = (w && w.locationName) ? w.locationName : (currentState.locationName || '');
            if (loc && loc !== 'Unbekannter Ort') {
                locationEl.textContent = loc;
                locationEl.style.display = '';
            } else {
                locationEl.style.display = 'none';
            }
        }

        // Hide mini weather in header when card is expanded
        if (headerMiniEl) headerMiniEl.style.display = 'none';
    }
}

// Update statistics card header based on collapsed state
function updateStatisticsCardHeader(collapsed) {
    const titleEl = document.getElementById('statisticsTitle');
    if (!titleEl) return;

    if (collapsed) {
        // Show "Heute: X Schaltungen" when collapsed
        const todaySwitches = currentState.todaySwitches || 0;
        titleEl.textContent = `Heute: ${todaySwitches}x`;
        titleEl.style.fontSize = '14px';
    } else {
        // Show "Statistik" when expanded
        titleEl.textContent = 'Statistik';
        titleEl.style.fontSize = '';
    }
}

// ========== FILE TYPE VALIDATION ==========
// ESP32 Firmware magic bytes: First byte is typically 0xE9 (ESP32) or 0xEA (ESP8266)
// LittleFS filesystem has different structure
async function validateFirmwareFile(file) {
    return new Promise((resolve, reject) => {
        // Check filename
        const filename = file.name.toLowerCase();
        if (!filename.includes('firmware') && !filename.includes('esp32')) {
            reject(new Error('Dateiname enthält nicht "firmware" oder "esp32". Bitte Firmware-Datei verwenden!'));
            return;
        }

        // Check file extension
        if (!filename.endsWith('.bin')) {
            reject(new Error('Datei muss eine .bin Datei sein!'));
            return;
        }

        // Check file size (firmware typically > 100KB)
        if (file.size < 100 * 1024) {
            reject(new Error('Datei ist zu klein für eine Firmware-Datei. Mindestens 100 KB erwartet.'));
            return;
        }

        // Read first bytes to check magic bytes
        const reader = new FileReader();
        reader.onload = function (e) {
            const arrayBuffer = e.target.result;
            const uint8Array = new Uint8Array(arrayBuffer);

            // ESP32 firmware typically starts with 0xE9 or 0xEA
            // LittleFS starts with different bytes
            const firstByte = uint8Array[0];

            // ESP32 bootloader header check (typical values: 0xE9, 0xEA)
            if (firstByte !== 0xE9 && firstByte !== 0xEA && firstByte !== 0xE0) {
                // Check if it might be LittleFS (starts with filesystem signature)
                if (uint8Array[0] === 0x6C && uint8Array[1] === 0x69 && uint8Array[2] === 0x74 && uint8Array[3] === 0x74) {
                    reject(new Error('Diese Datei ist eine LittleFS-Datei (Frontend), keine Firmware! Bitte firmware.bin verwenden.'));
                } else {
                    reject(new Error('Datei scheint keine gültige ESP32-Firmware zu sein. Bitte firmware.bin verwenden.'));
                }
                return;
            }

            resolve(true);
        };
        reader.onerror = function () {
            reject(new Error('Fehler beim Lesen der Datei'));
        };
        reader.readAsArrayBuffer(file.slice(0, 16)); // Read first 16 bytes
    });
}

async function validateLittleFSFile(file) {
    return new Promise((resolve, reject) => {
        // Check filename
        const filename = file.name.toLowerCase();
        if (!filename.includes('littlefs') && !filename.includes('spiffs') && !filename.includes('filesystem')) {
            reject(new Error('Dateiname enthält nicht "littlefs", "spiffs" oder "filesystem". Bitte Frontend-Datei verwenden!'));
            return;
        }

        // Check file extension
        if (!filename.endsWith('.bin')) {
            reject(new Error('Datei muss eine .bin Datei sein!'));
            return;
        }

        // Read first bytes to check for LittleFS signature
        const reader = new FileReader();
        reader.onload = function (e) {
            const arrayBuffer = e.target.result;
            const uint8Array = new Uint8Array(arrayBuffer);

            // Check if it's firmware instead (ESP32 starts with 0xE9/0xEA)
            if (uint8Array[0] === 0xE9 || uint8Array[0] === 0xEA || uint8Array[0] === 0xE0) {
                reject(new Error('Diese Datei ist eine Firmware-Datei, keine LittleFS-Datei! Bitte littlefs.bin verwenden.'));
                return;
            }

            // LittleFS might start with different signatures, so we're less strict
            // Just make sure it's not firmware
            resolve(true);
        };
        reader.onerror = function () {
            reject(new Error('Fehler beim Lesen der Datei'));
        };
        reader.readAsArrayBuffer(file.slice(0, 16)); // Read first 16 bytes
    });
}

// ========== MODAL FUNCTIONS ==========
function openOtaModal() {
    document.getElementById('otaModal').classList.add('active');
}

function closeOtaModal() {
    document.getElementById('otaModal').classList.remove('active');
}

// Update file input labels when files are selected
(function setupFileInputLabels() {
    const firmwareFile = document.getElementById('firmwareFile');
    const frontendFile = document.getElementById('frontendFile');
    const firmwareFileName = document.getElementById('firmwareFileName');
    const frontendFileName = document.getElementById('frontendFileName');
    const firmwareFileLabel = document.getElementById('firmwareFileLabel');
    const frontendFileLabel = document.getElementById('frontendFileLabel');

    if (firmwareFile && firmwareFileName && firmwareFileLabel) {
        firmwareFile.addEventListener('change', function (e) {
            const file = e.target.files[0];
            if (file) {
                firmwareFileName.textContent = file.name;
                firmwareFileLabel.style.borderColor = '#3498db';
                firmwareFileLabel.style.background = 'rgba(52, 152, 219, 0.1)';
                firmwareFileName.style.color = 'var(--text)';
            } else {
                firmwareFileName.textContent = 'Datei auswählen';
                firmwareFileLabel.style.borderColor = 'var(--border)';
                firmwareFileLabel.style.background = 'rgba(255,255,255,0.02)';
                firmwareFileName.style.color = 'var(--muted)';
            }
        });
    }

    if (frontendFile && frontendFileName && frontendFileLabel) {
        frontendFile.addEventListener('change', function (e) {
            const file = e.target.files[0];
            if (file) {
                frontendFileName.textContent = file.name;
                frontendFileLabel.style.borderColor = '#9b59b6';
                frontendFileLabel.style.background = 'rgba(155, 89, 182, 0.1)';
                frontendFileName.style.color = 'var(--text)';
            } else {
                frontendFileName.textContent = 'Datei auswählen';
                frontendFileLabel.style.borderColor = 'var(--border)';
                frontendFileLabel.style.background = 'rgba(255,255,255,0.02)';
                frontendFileName.style.color = 'var(--muted)';
            }
        });
    }
})();

// Close modal on overlay click and ESC key
function initModalHandlers() {
    const otaModal = document.getElementById('otaModal');
    const statsModal = document.getElementById('statsModal');

    const wireOverlayClose = (modalEl, closeFn) => {
        if (!modalEl) return;
        modalEl.addEventListener('click', function (e) {
            if (e.target === modalEl) closeFn();
        });
    };

    wireOverlayClose(otaModal, closeOtaModal);
    wireOverlayClose(statsModal, closeStatsModal);

    document.addEventListener('keydown', function (e) {
        if (e.key !== 'Escape') return;
        if (otaModal && otaModal.classList.contains('active')) closeOtaModal();
        if (statsModal && statsModal.classList.contains('active')) closeStatsModal();
    });
}

// Initialize modal handlers when DOM is ready
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', initModalHandlers);
} else {
    // DOM already loaded
    initModalHandlers();
}

// ========== OTA FIRMWARE UPLOAD ==========
async function uploadFirmware(event) {
    event.preventDefault();

    if (isLocalMode) {
        showToast('OTA Update ist nur auf dem ESP32 verfügbar, nicht im Demo-Modus', 'warning');
        return;
    }

    const fileInput = document.getElementById('firmwareFile');
    const file = fileInput.files[0];

    if (!file) {
        showToast('Bitte wähle eine .bin Datei aus', 'warning');
        return;
    }

    // Validate file type
    try {
        await validateFirmwareFile(file);
    } catch (error) {
        showToast(error.message, 'error');
        return;
    }

    // Reset overlay flag at start of upload
    overlayShown = false;

    const uploadBtn = document.getElementById('uploadBtn');
    const progressContainer = document.getElementById('uploadProgress');
    const progressBar = document.getElementById('uploadProgressBar');
    const progressPercent = document.getElementById('uploadProgressPercent');
    const statusText = document.getElementById('uploadStatus');

    uploadBtn.disabled = true;
    uploadBtn.textContent = '⏳ Wird hochgeladen...';
    progressContainer.style.display = 'block';
    progressBar.style.width = '0%';
    if (progressPercent) progressPercent.textContent = '0%';
    statusText.textContent = 'Bereite Upload vor...';

    try {
        let lastPercentComplete = 0;
        const formData = new FormData();
        formData.append('update', file);

        const xhr = new XMLHttpRequest();

        xhr.upload.addEventListener('progress', (e) => {
            if (e.lengthComputable) {
                const percentComplete = Math.round((e.loaded / e.total) * 100);
                lastPercentComplete = percentComplete;
                progressBar.style.width = percentComplete + '%';
                if (progressPercent) progressPercent.textContent = percentComplete + '%';
                statusText.textContent = `Hochgeladen: ${(e.loaded / 1024 / 1024).toFixed(1)} MB von ${(e.total / 1024 / 1024).toFixed(1)} MB`;

                // If upload reaches 100%, assume success (response might not arrive due to reboot)
                if (percentComplete === 100 && e.loaded === e.total) {
                    // Wait a moment to see if response arrives
                    setTimeout(() => {
                        // Check if we haven't already shown success
                        if (statusText.textContent.indexOf('erfolgreich') === -1 && statusText.textContent.indexOf('neustartet') === -1 && statusText.textContent.indexOf('Warte') === -1) {
                            progressBar.style.width = '100%';
                            if (progressPercent) progressPercent.textContent = '100%';
                            statusText.textContent = '✅ Upload erfolgreich! Warte auf ESP32-Neustart...';
                            statusText.style.color = '#27ae60';
                            showToast('Firmware wurde erfolgreich hochgeladen! Warte auf ESP32-Neustart...', 'success', 5000);

                            // Set flag and wait for WebSocket to reconnect
                            waitingForReboot = true;
                            reloadTriggered = false; // Reset reload flag
                            if (!overlayShown) {
                                showRebootOverlay('Firmware-Update…', 'Der ESP32 startet neu und ist gleich wieder erreichbar. Bitte ~20–40s warten. Wir verbinden automatisch neu…');
                            }

                            // Clear any existing reconnect timer
                            if (wsReconnectTimer) {
                                clearTimeout(wsReconnectTimer);
                                wsReconnectTimer = null;
                            }

                            // Clear any existing reboot check interval
                            if (rebootCheckInterval) {
                                clearInterval(rebootCheckInterval);
                                rebootCheckInterval = null;
                            }

                            // Close current WebSocket
                            if (ws) {
                                ws.onclose = null; // Prevent normal reconnect handler
                                ws.close();
                                ws = null;
                            }

                            // ESP32 waits 8 seconds before reboot, then needs ~15-20 seconds for full boot
                            // Timeline: Upload complete → 8s wait → reboot → boot sequence → WebSocket ready (~23-28s total)
                            setTimeout(() => {
                                // Start checking for WebSocket reconnection every 2 seconds
                                rebootCheckInterval = setInterval(() => {
                                    if (!waitingForReboot) {
                                        clearInterval(rebootCheckInterval);
                                        rebootCheckInterval = null;
                                        return;
                                    }

                                    // Try to reconnect
                                    if (!ws || ws.readyState === WebSocket.CLOSED) {
                                        console.log('Attempting WebSocket reconnect after reboot...');
                                        connectWebSocket();
                                    }
                                }, 2000);
                            }, 10000); // Start trying after 10s (8s delay + 2s buffer)
                        }
                    }, 500);
                }
            }
        });

        xhr.addEventListener('load', () => {
            if (xhr.status === 200) {
                progressBar.style.width = '100%';
                if (progressPercent) progressPercent.textContent = '100%';
                statusText.textContent = '✅ Upload erfolgreich! Warte auf ESP32-Neustart...';
                statusText.style.color = '#27ae60';
                showToast('Firmware wurde erfolgreich hochgeladen! Warte auf ESP32-Neustart...', 'success', 5000);

                // Set flag and wait for WebSocket to reconnect
                waitingForReboot = true;
                reloadTriggered = false; // Reset reload flag
                if (!overlayShown) {
                    showRebootOverlay('Firmware-Update…', 'Der ESP32 startet neu und ist gleich wieder erreichbar. Bitte ~20–40s warten. Wir verbinden automatisch neu…');
                }

                // Clear any existing reconnect timer
                if (wsReconnectTimer) {
                    clearTimeout(wsReconnectTimer);
                    wsReconnectTimer = null;
                }

                // Clear any existing reboot check interval
                if (rebootCheckInterval) {
                    clearInterval(rebootCheckInterval);
                    rebootCheckInterval = null;
                }

                // Close current WebSocket
                if (ws) {
                    ws.onclose = null; // Prevent normal reconnect handler
                    ws.close();
                    ws = null;
                }

                // ESP32 waits 8 seconds before reboot, then needs ~15-20 seconds for full boot
                // Timeline: Upload complete → 8s wait → reboot → boot sequence → WebSocket ready (~23-28s total)
                setTimeout(() => {
                    // Start checking for WebSocket reconnection every 2 seconds
                    rebootCheckInterval = setInterval(() => {
                        if (!waitingForReboot) {
                            clearInterval(rebootCheckInterval);
                            rebootCheckInterval = null;
                            return;
                        }

                        // Try to reconnect
                        if (!ws || ws.readyState === WebSocket.CLOSED) {
                            console.log('Attempting WebSocket reconnect after reboot...');
                            connectWebSocket();
                        }
                    }, 2000);
                }, 10000); // Start trying after 10s (8s delay + 2s buffer)
            } else {
                throw new Error('Upload fehlgeschlagen: ' + xhr.statusText);
            }
        });

        xhr.addEventListener('loadend', () => {
            // This fires even if connection is closed
            if (statusText.textContent.indexOf('erfolgreich') === -1 && statusText.textContent.indexOf('neustartet') === -1 && statusText.textContent.indexOf('Warte') === -1) {
                // If we reached 100% but didn't get response, assume success
                if (progressBar.style.width === '100%' || xhr.status === 200) {
                    statusText.textContent = '✅ Upload erfolgreich! Warte auf ESP32-Neustart...';
                    statusText.style.color = '#27ae60';
                    showToast('Firmware wurde erfolgreich hochgeladen! Warte auf ESP32-Neustart...', 'success', 5000);

                    // Set flag and wait for WebSocket to reconnect
                    waitingForReboot = true;
                    if (!overlayShown) {
                        showRebootOverlay('Firmware-Update…', 'Der ESP32 startet neu und ist gleich wieder erreichbar. Bitte ~20–40s warten. Wir verbinden automatisch neu…');
                    }

                    // Close current WebSocket so it will reconnect when ESP32 is back
                    if (ws && ws.readyState === WebSocket.OPEN) {
                        ws.close();
                    }

                    // Start checking for WebSocket reconnection
                    const checkInterval = setInterval(() => {
                        if (ws && ws.readyState === WebSocket.OPEN && !waitingForReboot) {
                            clearInterval(checkInterval);
                        }
                        // Auto-reconnect attempt
                        if (ws && ws.readyState === WebSocket.CLOSED) {
                            connectWebSocket();
                        }
                    }, 1000);
                }
            }
        });

        xhr.addEventListener('timeout', () => {
            // If request times out but upload was 100%, assume success
            if (progressBar.style.width === '100%' && statusText.textContent.indexOf('Warte') === -1) {
                statusText.textContent = '✅ Upload erfolgreich! Warte auf ESP32-Neustart...';
                statusText.style.color = '#27ae60';
                showToast('Firmware wurde erfolgreich hochgeladen! Warte auf ESP32-Neustart...', 'success', 5000);

                // Set flag and wait for WebSocket to reconnect
                waitingForReboot = true;
                reloadTriggered = false; // Reset reload flag
                if (!overlayShown) {
                    showRebootOverlay('Firmware-Update…', 'Der ESP32 startet neu und ist gleich wieder erreichbar. Bitte ~20–40s warten. Wir verbinden automatisch neu…');
                }

                // Clear any existing reconnect timer
                if (wsReconnectTimer) {
                    clearTimeout(wsReconnectTimer);
                    wsReconnectTimer = null;
                }

                // Clear any existing reboot check interval
                if (rebootCheckInterval) {
                    clearInterval(rebootCheckInterval);
                    rebootCheckInterval = null;
                }

                // Close current WebSocket
                if (ws) {
                    ws.onclose = null; // Prevent normal reconnect handler
                    ws.close();
                    ws = null;
                }

                // ESP32 waits 8 seconds before reboot, then needs ~15-20 seconds for full boot
                // Timeline: Upload complete → 8s wait → reboot → boot sequence → WebSocket ready (~23-28s total)
                setTimeout(() => {
                    // Start checking for WebSocket reconnection every 2 seconds
                    rebootCheckInterval = setInterval(() => {
                        if (!waitingForReboot) {
                            clearInterval(rebootCheckInterval);
                            rebootCheckInterval = null;
                            return;
                        }

                        // Try to reconnect
                        if (!ws || ws.readyState === WebSocket.CLOSED) {
                            console.log('Attempting WebSocket reconnect after reboot...');
                            connectWebSocket();
                        }
                    }, 2000);
                }, 10000); // Start trying after 10s (8s delay + 2s buffer)
            }
        });

        xhr.addEventListener('error', () => {
            // If the ESP32 reboots right after finishing the upload, browsers sometimes report a network error.
            // Treat this as success if we already reached 100%.
            if ((progressBar.style.width === '100%' || lastPercentComplete >= 98) && statusText.textContent.indexOf('Warte') === -1) {
                statusText.textContent = '✅ Upload erfolgreich! Warte auf ESP32-Neustart...';
                statusText.style.color = '#27ae60';
                showToast('Firmware wurde erfolgreich hochgeladen! Warte auf ESP32-Neustart...', 'success', 5000);
                waitingForReboot = true;
                if (!overlayShown) {
                    showRebootOverlay('Firmware-Update…', 'Der ESP32 startet neu und ist gleich wieder erreichbar. Bitte ~20–40s warten. Wir verbinden automatisch neu…');
                }
                if (ws && ws.readyState === WebSocket.OPEN) {
                    ws.close();
                }
                return;
            }
            throw new Error('Netzwerkfehler beim Upload');
        });

        xhr.open('POST', '/update');
        xhr.send(formData);

    } catch (error) {
        console.error('Upload error:', error);
        showToast('Fehler beim Upload: ' + error.message, 'error');

        uploadBtn.disabled = false;
        uploadBtn.textContent = '📤 Firmware hochladen';
        progressContainer.style.display = 'none';
        statusText.textContent = '';
        statusText.style.color = getComputedStyle(document.documentElement).getPropertyValue('--muted').trim();
    }
}

// ========== OTA FRONTEND/LITTLEFS UPLOAD ==========
async function uploadFrontend(event) {
    event.preventDefault();

    if (isLocalMode) {
        showToast('OTA Update ist nur auf dem ESP32 verfügbar, nicht im Demo-Modus', 'warning');
        return;
    }

    const fileInput = document.getElementById('frontendFile');
    const file = fileInput.files[0];

    if (!file) {
        showToast('Bitte wähle eine .bin Datei aus', 'warning');
        return;
    }

    // Validate file type
    try {
        await validateLittleFSFile(file);
    } catch (error) {
        showToast(error.message, 'error');
        return;
    }

    const uploadBtn = document.getElementById('uploadBtnFS');
    const progressContainer = document.getElementById('uploadProgressFS');
    const progressBar = document.getElementById('uploadProgressBarFS');
    const progressPercent = document.getElementById('uploadProgressPercentFS');
    const statusText = document.getElementById('uploadStatusFS');

    uploadBtn.disabled = true;
    uploadBtn.textContent = '⏳ Wird hochgeladen...';
    progressContainer.style.display = 'block';
    progressBar.style.width = '0%';
    if (progressPercent) progressPercent.textContent = '0%';
    statusText.textContent = 'Bereite Frontend-Upload vor...';

    try {
        let lastPercentComplete = 0;
        const formData = new FormData();
        formData.append('update', file);

        const xhr = new XMLHttpRequest();

        xhr.upload.addEventListener('progress', (e) => {
            if (e.lengthComputable) {
                const percentComplete = Math.round((e.loaded / e.total) * 100);
                lastPercentComplete = percentComplete;
                progressBar.style.width = percentComplete + '%';
                if (progressPercent) progressPercent.textContent = percentComplete + '%';
                statusText.textContent = `Hochgeladen: ${(e.loaded / 1024).toFixed(1)} KB von ${(e.total / 1024).toFixed(1)} KB`;

                // If upload reaches 100%, assume success (response might not arrive due to reboot)
                if (percentComplete === 100 && e.loaded === e.total) {
                    // Wait a moment to see if response arrives
                    setTimeout(() => {
                        // Check if we haven't already shown success
                        if (statusText.textContent.indexOf('erfolgreich') === -1 && statusText.textContent.indexOf('neustartet') === -1 && statusText.textContent.indexOf('Warte') === -1) {
                            progressBar.style.width = '100%';
                            if (progressPercent) progressPercent.textContent = '100%';
                            statusText.textContent = 'Frontend-Upload erfolgreich! Warte auf ESP32-Neustart...';
                            statusText.style.color = '#27ae60';
                            showToast('Frontend wurde erfolgreich hochgeladen! Warte auf ESP32-Neustart...', 'success', 5000);

                            // Set flag and wait for WebSocket to reconnect
                            waitingForReboot = true;
                            reloadTriggered = false; // Reset reload flag
                            if (!overlayShown) {
                                showRebootOverlay('Frontend (LittleFS) Update…', 'Der ESP32 startet neu und ist gleich wieder erreichbar. Bitte ~20–40s warten. Wir verbinden automatisch neu…');
                            }

                            // Clear any existing reconnect timer
                            if (wsReconnectTimer) {
                                clearTimeout(wsReconnectTimer);
                                wsReconnectTimer = null;
                            }

                            // Clear any existing reboot check interval
                            if (rebootCheckInterval) {
                                clearInterval(rebootCheckInterval);
                                rebootCheckInterval = null;
                            }

                            // Close current WebSocket
                            if (ws) {
                                ws.onclose = null; // Prevent normal reconnect handler
                                ws.close();
                                ws = null;
                            }

                            // ESP32 waits 8 seconds before reboot, then needs ~15-20 seconds for full boot
                            // Timeline: Upload complete → 8s wait → reboot → boot sequence → WebSocket ready (~23-28s total)
                            setTimeout(() => {
                                // Start checking for WebSocket reconnection every 2 seconds
                                rebootCheckInterval = setInterval(() => {
                                    if (!waitingForReboot) {
                                        clearInterval(rebootCheckInterval);
                                        rebootCheckInterval = null;
                                        return;
                                    }

                                    // Try to reconnect
                                    if (!ws || ws.readyState === WebSocket.CLOSED) {
                                        console.log('Attempting WebSocket reconnect after reboot...');
                                        connectWebSocket();
                                    }
                                }, 2000);
                            }, 10000); // Start trying after 10s (8s delay + 2s buffer)
                        }
                    }, 500);
                }
            }
        });

        xhr.addEventListener('load', () => {
            if (xhr.status === 200) {
                progressBar.style.width = '100%';
                if (progressPercent) progressPercent.textContent = '100%';
                statusText.textContent = 'Frontend-Upload erfolgreich! Warte auf ESP32-Neustart...';
                statusText.style.color = '#27ae60';
                showToast('Frontend wurde erfolgreich hochgeladen! Warte auf ESP32-Neustart...', 'success', 5000);

                // Set flag and wait for WebSocket to reconnect
                waitingForReboot = true;
                reloadTriggered = false; // Reset reload flag
                if (!overlayShown) {
                    showRebootOverlay('Frontend (LittleFS) Update…', 'Der ESP32 startet neu und ist gleich wieder erreichbar. Bitte ~20–40s warten. Wir verbinden automatisch neu…');
                }

                // Clear any existing reconnect timer
                if (wsReconnectTimer) {
                    clearTimeout(wsReconnectTimer);
                    wsReconnectTimer = null;
                }

                // Clear any existing reboot check interval
                if (rebootCheckInterval) {
                    clearInterval(rebootCheckInterval);
                    rebootCheckInterval = null;
                }

                // Close current WebSocket
                if (ws) {
                    ws.onclose = null; // Prevent normal reconnect handler
                    ws.close();
                    ws = null;
                }

                // ESP32 waits 8 seconds before reboot, then needs ~15-20 seconds for full boot
                // Timeline: Upload complete → 8s wait → reboot → boot sequence → WebSocket ready (~23-28s total)
                setTimeout(() => {
                    // Start checking for WebSocket reconnection every 2 seconds
                    rebootCheckInterval = setInterval(() => {
                        if (!waitingForReboot) {
                            clearInterval(rebootCheckInterval);
                            rebootCheckInterval = null;
                            return;
                        }

                        // Try to reconnect
                        if (!ws || ws.readyState === WebSocket.CLOSED) {
                            console.log('Attempting WebSocket reconnect after reboot...');
                            connectWebSocket();
                        }
                    }, 2000);
                }, 10000); // Start trying after 10s (8s delay + 2s buffer)
            } else {
                throw new Error('Upload fehlgeschlagen: ' + xhr.statusText);
            }
        });

        xhr.addEventListener('loadend', () => {
            // This fires even if connection is closed
            if (statusText.textContent.indexOf('erfolgreich') === -1 && statusText.textContent.indexOf('neustartet') === -1 && statusText.textContent.indexOf('Warte') === -1) {
                // If we reached 100% but didn't get response, assume success
                if (progressBar.style.width === '100%' || xhr.status === 200) {
                    statusText.textContent = 'Frontend-Upload erfolgreich! Warte auf ESP32-Neustart...';
                    statusText.style.color = '#27ae60';
                    showToast('Frontend wurde erfolgreich hochgeladen! Warte auf ESP32-Neustart...', 'success', 5000);

                    // Set flag and wait for WebSocket to reconnect
                    waitingForReboot = true;
                    reloadTriggered = false; // Reset reload flag
                    if (!overlayShown) {
                        showRebootOverlay('Frontend (LittleFS) Update…', 'Der ESP32 startet neu und ist gleich wieder erreichbar. Bitte ~20–40s warten. Wir verbinden automatisch neu…');
                    }

                    // Clear any existing reconnect timer
                    if (wsReconnectTimer) {
                        clearTimeout(wsReconnectTimer);
                        wsReconnectTimer = null;
                    }

                    // Clear any existing reboot check interval
                    if (rebootCheckInterval) {
                        clearInterval(rebootCheckInterval);
                        rebootCheckInterval = null;
                    }

                    // Close current WebSocket
                    if (ws) {
                        ws.onclose = null; // Prevent normal reconnect handler
                        ws.close();
                        ws = null;
                    }

                    // ESP32 waits 8 seconds before reboot, then needs ~15-20 seconds for full boot
                    // Timeline: Upload complete → 8s wait → reboot → boot sequence → WebSocket ready (~23-28s total)
                    setTimeout(() => {
                        // Start checking for WebSocket reconnection every 2 seconds
                        rebootCheckInterval = setInterval(() => {
                            if (!waitingForReboot) {
                                clearInterval(rebootCheckInterval);
                                rebootCheckInterval = null;
                                return;
                            }

                            // Try to reconnect
                            if (!ws || ws.readyState === WebSocket.CLOSED) {
                                console.log('Attempting WebSocket reconnect after reboot...');
                                connectWebSocket();
                            }
                        }, 2000);
                    }, 10000); // Start trying after 10s (8s delay + 2s buffer)
                }
            }
        });

        xhr.addEventListener('timeout', () => {
            // If request times out but upload was 100%, assume success
            if (progressBar.style.width === '100%' && statusText.textContent.indexOf('Warte') === -1) {
                statusText.textContent = 'Frontend-Upload erfolgreich! Warte auf ESP32-Neustart...';
                statusText.style.color = '#27ae60';
                showToast('Frontend wurde erfolgreich hochgeladen! Warte auf ESP32-Neustart...', 'success', 5000);

                // Set flag and wait for WebSocket to reconnect
                waitingForReboot = true;
                reloadTriggered = false; // Reset reload flag
                if (!overlayShown) {
                    showRebootOverlay('Frontend (LittleFS) Update…', 'Der ESP32 startet neu und ist gleich wieder erreichbar. Bitte ~20–40s warten. Wir verbinden automatisch neu…');
                }

                // Clear any existing reconnect timer
                if (wsReconnectTimer) {
                    clearTimeout(wsReconnectTimer);
                    wsReconnectTimer = null;
                }

                // Clear any existing reboot check interval
                if (rebootCheckInterval) {
                    clearInterval(rebootCheckInterval);
                    rebootCheckInterval = null;
                }

                // Close current WebSocket
                if (ws) {
                    ws.onclose = null; // Prevent normal reconnect handler
                    ws.close();
                    ws = null;
                }

                // ESP32 waits 8 seconds before reboot, then needs ~15-20 seconds for full boot
                // Timeline: Upload complete → 8s wait → reboot → boot sequence → WebSocket ready (~23-28s total)
                setTimeout(() => {
                    // Start checking for WebSocket reconnection every 2 seconds
                    rebootCheckInterval = setInterval(() => {
                        if (!waitingForReboot) {
                            clearInterval(rebootCheckInterval);
                            rebootCheckInterval = null;
                            return;
                        }

                        // Try to reconnect
                        if (!ws || ws.readyState === WebSocket.CLOSED) {
                            console.log('Attempting WebSocket reconnect after reboot...');
                            connectWebSocket();
                        }
                    }, 2000);
                }, 10000); // Start trying after 10s (8s delay + 2s buffer)
            }
        });

        xhr.addEventListener('error', () => {
            // If the ESP32 reboots right after finishing the upload, browsers sometimes report a network error.
            // Treat this as success if we already reached 100%.
            if ((progressBar.style.width === '100%' || lastPercentComplete >= 98) && statusText.textContent.indexOf('Warte') === -1) {
                statusText.textContent = 'Frontend-Upload erfolgreich! Warte auf ESP32-Neustart...';
                statusText.style.color = '#27ae60';
                showToast('Frontend wurde erfolgreich hochgeladen! Warte auf ESP32-Neustart...', 'success', 5000);
                waitingForReboot = true;
                reloadTriggered = false; // Reset reload flag
                if (!overlayShown) {
                    showRebootOverlay('Frontend (LittleFS) Update…', 'Der ESP32 startet neu und ist gleich wieder erreichbar. Bitte ~20–40s warten. Wir verbinden automatisch neu…');
                }

                // Clear any existing reconnect timer
                if (wsReconnectTimer) {
                    clearTimeout(wsReconnectTimer);
                    wsReconnectTimer = null;
                }

                // Clear any existing reboot check interval
                if (rebootCheckInterval) {
                    clearInterval(rebootCheckInterval);
                    rebootCheckInterval = null;
                }

                // Close current WebSocket
                if (ws) {
                    ws.onclose = null; // Prevent normal reconnect handler
                    ws.close();
                    ws = null;
                }

                // ESP32 waits 8 seconds before reboot, then needs ~15-20 seconds for full boot
                // Timeline: Upload complete → 8s wait → reboot → boot sequence → WebSocket ready (~23-28s total)
                setTimeout(() => {
                    // Start checking for WebSocket reconnection every 2 seconds
                    rebootCheckInterval = setInterval(() => {
                        if (!waitingForReboot) {
                            clearInterval(rebootCheckInterval);
                            rebootCheckInterval = null;
                            return;
                        }

                        // Try to reconnect
                        if (!ws || ws.readyState === WebSocket.CLOSED) {
                            console.log('Attempting WebSocket reconnect after reboot...');
                            connectWebSocket();
                        }
                    }, 2000);
                }, 10000); // Start trying after 10s (8s delay + 2s buffer)
                return;
            }
            throw new Error('Netzwerkfehler beim Upload');
        });

        xhr.open('POST', '/update-fs');
        xhr.send(formData);

    } catch (error) {
        console.error('Frontend upload error:', error);
        showToast('Fehler beim Frontend-Upload: ' + error.message, 'error');

        uploadBtn.disabled = false;
        uploadBtn.textContent = 'Frontend hochladen';
        progressContainer.style.display = 'none';
        statusText.textContent = '';
        statusText.style.color = getComputedStyle(document.documentElement).getPropertyValue('--muted').trim();
    }
}

// ========== SERVICE WORKER REGISTRATION ==========
if ('serviceWorker' in navigator && !isLocalMode) {
    navigator.serviceWorker.register('/sw.js').then((registration) => {
        console.log('Service Worker registered:', registration.scope);
    }).catch((error) => {
        console.log('Service Worker registration failed:', error);
    });
}

// ========== PUSH NOTIFICATIONS ==========
async function requestNotificationPermission() {
    if (!('Notification' in window)) {
        console.log('This browser does not support notifications');
        return false;
    }

    if (Notification.permission === 'granted') {
        return true;
    }

    if (Notification.permission !== 'denied') {
        const permission = await Notification.requestPermission();
        return permission === 'granted';
    }

    return false;
}

// Request notification permission on first load
if (!isLocalMode) {
    requestNotificationPermission();
}

function showBrowserNotification(title, body, icon = '🔥') {
    if (Notification.permission === 'granted') {
        new Notification(title, { body, icon, badge: icon, tag: 'heating-notification' });
    }
}

// ========== INITIALIZATION ==========
initTheme();
initCollapsibleCards();
setInterval(() => {
    if (!isLocalMode) {
        // During OTA reboot we expect the backend to be unreachable. Don't spam console with failed requests.
        if (!waitingForReboot) {
            updateStatus();
        }

        // Weather is only updated on page load and F5 refresh, not in loop
    }
}, 1000);

// Update weather on page load and F5 refresh
function refreshWeather() {
    if (!isLocalMode) {
        updateWeather();
    }
}

// Update weather on F5 (keydown event)
document.addEventListener('keydown', function (e) {
    if (e.key === 'F5' || (e.key === 'r' && (e.metaKey || e.ctrlKey))) {
        refreshWeather();
    }
});

detectMode();
updateConnectionStatus(true);
if (!isLocalMode) {
    // Update status first (which will call updateUI after loading data)
    updateStatus().then(() => {
        // Update weather once on page load - if it's already cached, it will display right away
        updateWeather();

        // Also try a few times in case backend is still fetching
        let retryCount = 0;
        const maxRetries = 5;
        const checkWeather = async () => {
            const success = await updateWeather();
            retryCount++;

            // Check if we have a location set
            const locationInput = document.getElementById('locationInput');
            const hasLocationInput = locationInput && locationInput.value.trim() !== '' &&
                locationInput.value.trim() !== 'z.B. Berlin oder 10115';
            const hasLocationName = (currentState.locationName && currentState.locationName !== 'Unbekannter Ort') ||
                (currentState.weather && currentState.weather.locationName &&
                    currentState.weather.locationName !== 'Unbekannter Ort');

            const hasLocation = hasLocationInput || hasLocationName;

            // Continue retrying if weather is not valid and we have a location
            if (!success && retryCount < maxRetries && hasLocation) {
                setTimeout(checkWeather, 2000);
            } else {
                lastWeatherUpdate = Date.now();
            }
        };
        // Start retry checks after 2 seconds
        setTimeout(checkWeather, 2000);
    });
    connectWebSocket();
} else {
    // In local mode, update UI immediately
    updateUI();
    // Start demo logs in local mode
    startDemoLogs();
}