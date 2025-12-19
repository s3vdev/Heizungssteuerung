# Troubleshooting-Guide - ESP32 Heizungssteuerung

## 🔍 Systematische Fehlersuche

### Diagnose-Reihenfolge
1. **Power** → Hat ESP32 Strom?
2. **WiFi** → Verbindet ESP32 mit WLAN?
3. **Sensoren** → Werden beide DS18B20 erkannt?
4. **Relais** → Schaltet das Relais?
5. **Web** → Ist Dashboard erreichbar?

---

## ⚡ Problem: ESP32 bootet nicht

### Symptome
- Keine LED an ESP32
- Serial Monitor zeigt nichts
- Kein Lebenszeichen

### Lösungen

**1. Spannungsversorgung prüfen**
```
✓ LM2596S LED-Anzeige zeigt 5.0V?
✓ OUT+ mit Multimeter messen (sollte 5V sein)
✓ ESP32 5V Pin mit Multimeter messen
✓ USB-C Kabel intakt? (zum Testen direkt per USB)
```

**2. Falscher Boot-Modus**
```
→ BOOT-Button gedrückt halten
→ EN-Button kurz drücken (Reset)
→ BOOT-Button loslassen
→ Erneut flashen versuchen
```

**3. Defekter ESP32**
```
→ Mit anderem USB-Kabel testen
→ Direkt per USB am PC (ohne LM2596S)
→ Wenn immer noch nichts: ESP32 defekt → tauschen
```

---

## 📡 Problem: WiFi verbindet nicht

### Symptome
- Serial Monitor: "WiFi connection failed!"
- ESP32 startet Access Point "HeaterSetup"
- Dashboard nicht erreichbar

### Lösungen

**1. SSID/Passwort prüfen**
```cpp
// In secrets.h:
const char* WIFI_SSID = "Dein_WLAN_Name";      // ← Exakt wie im Router!
const char* WIFI_PASSWORD = "Dein_WLAN_PW";    // ← Groß-/Kleinschreibung!
```

**2. WLAN-Frequenz prüfen**
```
✗ ESP32 unterstützt NUR 2.4 GHz!
✗ NICHT 5 GHz!
→ Im Router: 2.4 GHz aktiviert?
→ SSID für 2.4 GHz sichtbar?
```

**3. Router-Einstellungen**
```
✓ WLAN ist eingeschaltet
✓ SSID wird gesendet (nicht versteckt)
✓ MAC-Filter deaktiviert (oder ESP32 erlaubt)
✓ DHCP aktiviert
✓ Keine Geräte-Beschränkung
```

**4. Signalstärke**
```
→ ESP32 näher an Router bringen
→ Im Dashboard: RSSI sollte besser als -80 dBm sein
→ Bei -90 dBm oder schlechter: zu weit weg!
```

**5. Fallback: Access Point Mode nutzen**
```
1. Warte 20 Sekunden nach Boot
2. ESP32 öffnet WLAN "HeaterSetup"
3. Passwort: 12345678
4. Verbinden mit http://192.168.4.1/
5. NUR für Tests! (kein NTP, kein Internet)
```

---

## 🌡️ Problem: Sensoren werden nicht erkannt

### Symptome
- Serial Monitor: "Found 0 DS18B20 sensor(s)"
- Dashboard zeigt "ERROR" bei Temperaturen
- Failsafe schaltet Heizung aus

### Lösungen

**1. Pull-Up Widerstand fehlt!**
```
⚠️ HÄUFIGSTER FEHLER!

✓ 4.7kΩ zwischen GPIO4 und 3.3V?
✓ Widerstand richtig eingelötet?
✓ Mit Multimeter durchmessen (sollte 4.7kΩ zeigen)

OHNE Pull-Up funktioniert OneWire GAR NICHT!
```

**2. Verkabelung prüfen**
```
DS18B20 Pins (wasserdicht, 1m Kabel):
- Rot    → ESP32 3.3V  (NICHT 5V!)
- Gelb   → ESP32 GPIO4
- Schwarz→ ESP32 GND

✓ Alle Verbindungen fest?
✓ Kalte Lötstellen?
✓ Kabel nicht vertauscht?
```

**3. Sensor defekt?**
```
→ Nur 1 Sensor anschließen zum Test
→ Wenn einer funktioniert: anderer kaputt
→ Mit Multimeter Durchgang prüfen:
   - VDD zu GND sollte >1kΩ sein (nicht Kurzschluss)
   - DATA zu GND sollte hochohmig sein
```

**4. GPIO4 belegt?**
```
→ Anderen Pin probieren (z.B. GPIO5)
→ In main.cpp ändern:
   #define ONE_WIRE_BUS 5  // statt 4
→ Neu kompilieren und flashen
```

**5. Code-Problem**
```
Serial Monitor genau lesen:
"Sensor 1 address: 28FF..." → Sensor wird erkannt!
"Found 2 DS18B20 sensor(s)" → Perfekt!
"Found 1 DS18B20 sensor(s)" → Einer defekt/nicht angeschlossen

Bei 1 Sensor: Kein Problem, wird für beide Werte genutzt!
```

---

## 🔌 Problem: Relais schaltet nicht

### Symptome
- Kein "Klick" beim Schalten
- LED am Relais leuchtet nicht
- Heizung bleibt aus

### Lösungen

**1. Relais-Versorgung**
```
✓ VCC mit 5V verbunden?
✓ GND mit Masse verbunden?
✓ Mit Multimeter messen: VCC = 5V?
✓ Relais-LED leuchtet (zeigt Versorgung)?
```

**2. Steuersignal prüfen**
```
✓ IN-Pin mit GPIO21 verbunden?
✓ Im Serial Monitor: "Heater ON (Relay: LOW)" ?

Test:
1. Manuellen Modus wählen
2. Toggle-Button klicken
3. Mit Multimeter GPIO21 messen:
   - Bei ON: sollte ~0V sein (LOW)
   - Bei OFF: sollte ~3.3V sein (HIGH)
```

**3. Active-Low Logik verstehen**
```
Dieses Relais ist Active-Low!
→ LOW (0V) = Relais EIN
→ HIGH (3.3V) = Relais AUS

Ist das Relais falsch herum geschaltet?
→ Heizung geht an wenn sie aus sein sollte?
→ Code ist richtig, Relais ist Active-Low!
```

**3.5. LED glimmt im AUS-Zustand (Relais schaltet nicht sauber)**
```
Symptom:
→ LED am Relais leuchtet im AUS-Zustand schwach ("glimmen")
→ Beim Toggle hört man kein sauberes Klicken

Ursache:
→ Der IN-Pin ist nicht "hart" HIGH (z.B. floating/Open-Drain + Leckströme)

Lösung:
→ Im Dashboard: "Relais-Einstellungen (Erweitert)"
   - Heizungsrelais OFF per Open-Drain (floating) = AUS
   - (Active-Low bleibt i.d.R. EIN)
→ Alternativ per API /api/settings:
   { "heaterRelayOpenDrainOff": false }
```

**4. Relais defekt?**
```
Test ohne ESP32:
→ IN-Pin direkt an GND → sollte schalten
→ IN-Pin an 5V → sollte ausschalten
→ Klick hörbar? LED leuchtet?
→ Wenn nichts passiert: Relais defekt
```

**5. Zu schwaches Signal**
```
Manche Relais brauchen stärkeres Signal:
→ Transistor als Treiber dazwischen
→ Oder: Relais mit Optokoppler nutzen
→ Billige Module funktionieren meist direkt
```

---

## 🌐 Problem: Dashboard nicht erreichbar

### Symptome
- Browser: "Seite nicht erreichbar"
- `http://heater.local/` funktioniert nicht
- IP-Adresse auch nicht

### Lösungen

**1. mDNS-Problem (heater.local)**
```
Windows:
→ Bonjour Service installieren (iTunes oder extra)
→ Oder direkt IP-Adresse nutzen

Mac/Linux:
→ Sollte funktionieren
→ Alternative: http://heater.fritz.box/ (bei Fritzbox)
```

**2. IP-Adresse herausfinden**
```
Methode 1: Serial Monitor
→ Nach Boot steht da: "IP: 192.168.x.x"
→ Diese IP im Browser öffnen

Methode 2: Router-Interface
→ Im Router nachsehen (z.B. fritz.box)
→ Liste der verbundenen Geräte
→ "ESP32" oder "heater" suchen

Methode 3: IP-Scanner
→ App "Fing" (Android/iOS)
→ Oder "Advanced IP Scanner" (Windows)
→ Netzwerk scannen nach Port 80
```

**3. Firewall blockiert**
```
→ Windows Firewall temporär deaktivieren (Test)
→ ESP32 muss im gleichen Netzwerk sein!
→ Gast-WLAN? → ESP32 da rein, PC auch!
```

**4. LittleFS nicht hochgeladen**
```
⚠️ HÄUFIGER FEHLER!

Du musst BEIDES hochladen:
1. pio run -t upload      (Firmware)
2. pio run -t uploadfs    (Web-Interface!)

Ohne uploadfs: 404 Error beim Öffnen!
```

**5. Port 80 belegt?**
```
→ Andere Software auf Port 80? (z.B. XAMPP, IIS)
→ ESP32 neu starten
→ Router neu starten
```

---

## 🔥 Problem: Heizung bleibt an/aus

### Symptome
- Automatik schaltet nicht
- Zeitplan funktioniert nicht
- Heizung reagiert nicht auf Temperatur

### Lösungen

**1. Falscher Modus**
```
✓ Richtiger Modus ausgewählt?
  - Manuell = nur über Toggle
  - Automatik = Temperatur-basiert
  - Zeitplan = Zeit-basiert

✓ Badge oben rechts zeigt aktuellen Modus
```

**2. Automatik: Temperatur-Schwellwerte**
```
Rücklauf-Temperatur wird für Steuerung genutzt!

✓ EIN-Temp < AUS-Temp ?
✓ Aktuelle Temp liegt im Bereich?

Beispiel: EIN=30°C, AUS=40°C, Temp=35°C
→ Heizung bleibt wie sie ist (Hysterese)
→ Erst bei ≤30°C oder ≥40°C wird geschaltet
```

**3. Zeitplan: NTP nicht synchronisiert**
```
✓ System-Info: "NTP-Status" = "Synced" ?
✗ Wenn "Pending": Keine Internet-Verbindung!

→ ESP32 braucht Internet für NTP
→ Router hat Internet?
→ ESP32 darf ins Internet? (Firewall)

Test:
→ Uhrzeit oben im Header korrekt?
→ Wenn "--:--": NTP funktioniert nicht
```

**4. Zeitplan: Falsche Zeitzone**
```
In main.cpp steht:
#define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"

Das ist Europa/Berlin.
Anderes Land? → Zeitzone anpassen!
```

**5. Sensor-Failsafe aktiv**
```
✓ Beide Temperaturen zeigen Werte?
✗ "ERROR" angezeigt?

→ Bei Sensorfehler schaltet Failsafe Heizung AUS
→ Sensor-Problem beheben (siehe oben)
```

---

## ⚙️ Problem: Einstellungen nicht gespeichert

### Symptome
- Nach Reboot sind Einstellungen weg
- Hysterese-Werte zurückgesetzt
- Zeitplan gelöscht

### Lösungen

**1. NVS-Problem**
```
→ ESP32 hat internen Flash für NVS
→ Normalerweise funktioniert das immer

Test:
1. Einstellungen speichern
2. ESP32 neu starten (EN-Button)
3. Sind Werte wieder da?

Wenn NEIN:
→ Flash defekt (selten)
→ ESP32 tauschen
```

**2. Code-Problem**
```
→ Neueste Version geflasht?
→ saveSettings() wird aufgerufen?
→ Serial Monitor: "Settings saved to NVS"?
```

---

## 📱 Problem: Mobile-Ansicht kaputt

### Symptome
- Auf Handy sieht's komisch aus
- Buttons überlagern sich
- Text zu klein/groß

### Lösungen

**1. Cache leeren**
```
→ Browser-Cache löschen
→ Hard-Reload: Strg+Shift+R (oder Cmd+Shift+R)
→ Seite neu laden
```

**2. Alte Version gecacht**
```
→ LittleFS neu hochladen:
   pio run -t uploadfs
→ ESP32 neu starten
→ Cache leeren
→ Seite neu laden
```

---

## 🔴 NOTFALL: Heizung schaltet unkontrolliert

### SOFORT-MAßNAHMEN

**1. SICHERUNG RAUS!**
```
→ Strom zur Heizung trennen
→ ERST DANN debuggen!
```

**2. Manuellen Modus erzwingen**
```
→ ESP32 vom Netz trennen
→ Über USB mit PC verbinden
→ Manuellen Modus einstellen
→ Heizung manuell steuern
→ Automatik/Zeitplan NICHT nutzen bis Problem gefunden!
```

**3. Ursachen prüfen**
```
✓ Sensoren zeigen korrekte Werte?
✓ Keine wilden Temperatur-Sprünge?
✓ Relais-Kontakte verschweißt? (bleibt immer an)
✓ Software-Bug? → Log im Serial Monitor

Bei Relais-Defekt: SOFORT TAUSCHEN!
```

---

## 📊 Diagnose-Kommandos

### Serial Monitor Ausgaben verstehen

**Beim Boot:**
```
=== ESP32 Heater Control v2.0 ===
LittleFS mounted                        ← Dateisystem OK
Found 2 DS18B20 sensor(s)              ← Beide Sensoren OK
Sensor 1 address: 28FF...              ← Adresse Sensor 1
Sensor 2 address: 28FF...              ← Adresse Sensor 2
Restored state from NVS: Heating OFF    ← Einstellungen geladen
WiFi connected! IP: 192.168.1.100      ← WLAN OK
RSSI: -65 dBm                          ← Signal gut
mDNS responder started: http://heater.local/  ← mDNS OK
NTP synced! Current time: 14:30:15     ← Zeit OK
Web server started                      ← Webserver läuft
Vorlauf: 45.5°C, Rücklauf: 35.2°C     ← Temperaturen OK
=== Setup complete ===
```

**Im Betrieb (Automatik):**
```
AUTO: Rücklauf 29.5°C <= 30.0°C, turning heater ON
Heater ON (Relay: LOW)                 ← Relais schaltet
...
AUTO: Rücklauf 40.1°C >= 40.0°C, turning heater OFF
Heater OFF (Relay: HIGH)               ← Relais aus
```

**Fehler:**
```
DS18B20 sensor error or disconnected!   ← Sensor-Problem
FAILSAFE: Sensor error detected...      ← Schutz aktiv
WiFi connection lost, reconnecting...   ← WLAN-Problem
```

---

## 🆘 Support-Checkliste

Wenn gar nichts hilft, sammle diese Infos:

**Hardware:**
- [ ] ESP32 Modell/Version
- [ ] Relais-Typ
- [ ] Anzahl funktionierender Sensoren
- [ ] Versorgungsspannung (mit Multimeter gemessen)

**Software:**
- [ ] Komplette Serial Monitor Ausgabe (Copy&Paste)
- [ ] Screenshot vom Dashboard
- [ ] WLAN-RSSI Wert
- [ ] Welcher Modus läuft (Manuell/Auto/Zeitplan)

**Problem:**
- [ ] Genaue Fehlerbeschreibung
- [ ] Seit wann tritt es auf?
- [ ] Was wurde zuletzt geändert?
- [ ] Reproduzierbar oder zufällig?

---

## ✅ Präventiv-Wartung

### Wöchentlich
- Dashboard öffnen, alle Werte prüfen
- RSSI-Signal checken (sollte stabil sein)
- Beide Temperaturen plausibel?

### Monatlich
- Relais-Schaltungen zählen (sollte nicht zu oft schalten)
- Gehäuse öffnen, Staub entfernen
- Schraubverbindungen nachziehen
- Lötstellen visuell prüfen

### Jährlich
- Kompletter Funktionstest
- Sensoren kalibrieren (mit Referenz-Thermometer)
- Backup der Einstellungen (Screenshot)
- Software-Update prüfen

---

**Bei weiteren Problemen: Projekt-README lesen oder Code-Kommentare durchgehen!**

