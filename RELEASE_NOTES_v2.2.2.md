# Release v2.2.2

## Fixes
- **Dashboard**: Tankhöhe/Tankkapazität werden beim Tippen nicht mehr durch Status-Updates zurückgesetzt.
- **Pumpen-Logik**: Verhindert ON/OFF‑„Flattern“ der Pumpe bei fehlenden Temperatursensoren (Failsafe respektiert manuelle Pumpen-Übersteuerung).
- **Tank-Sensor**: Stabilere Anzeige bei sporadisch fehlendem Echo (kurze Grace-Period statt sofort „Sensor nicht verfügbar“).

## Hardware/Wiring Updates
- **DS18B20 (OneWire)**: DATA-Pin ist jetzt **GPIO27** (statt GPIO4), um Boot-Probleme durch falsche Pinbelegung zu vermeiden.
- **JSN-SR04T**: TRIG **GPIO16**, ECHO **GPIO18** (wie im Dashboard/Debug angezeigt).

