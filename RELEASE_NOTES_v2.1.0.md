# Release v2.1.0 - ESP32 Heizungssteuerung

## 📦 Release-Paket

- **ZIP-Datei**: `ESP32-Heizungssteuerung-v2.1.0.zip`
- **Größe**: ~727 KB (komprimiert)
- **Inhalt**:
  - `firmware.bin` (1.1 MB) - Kompilierte Firmware
  - `littlefs.bin` (1.4 MB) - Frontend (HTML/CSS/JS)
  - `README.md` - Release-Informationen

## 🚀 Installation

1. ZIP-Datei entpacken
2. Mit esptool.py oder PlatformIO auf ESP32 flashen
3. Siehe `release/README.md` für detaillierte Anweisungen

## ✨ Neue Features

### UI-Verbesserungen
- **System-Informationen im Header**: WiFi-Signal, Betriebszeit und NTP-Status direkt im Header für bessere Übersicht
- **Farblich kodiertes WiFi-Icon**: 
  - 🟢 Grün: > -50 dBm (exzellent)
  - 🟠 Orange: -50 bis -60 dBm (gut)
  - 🟠 Dunkelorange: -60 bis -70 dBm (befriedigend)
  - 🔴 Rot: < -70 dBm (schlecht)
- **Klappbare Statistik**: Statistik-Karte kann ein- und ausgeklappt werden (standardmäßig eingeklappt)
- **Platzsparende UI**: 
  - Frostschutz in Steuerungskarte integriert
  - OTA-Update Button im Serial Monitor
  - Tankinhalt unter Serial Monitor verschoben

### Sicherheit
- **Upload-Validierung**: Robustere Validierung von Firmware- und Frontend-Dateien
  - Prüft Dateinamen (firmware.bin vs littlefs.bin)
  - Prüft Magic Bytes (ESP32-Header)
  - Prüft Dateigröße
  - Verhindert versehentliche Uploads

### OTA-Updates
- **Modal-System**: OTA-Updates in kompaktem Modal-Dialog
- **Automatische Modal-Schließung**: Modal schließt automatisch nach erfolgreichem Upload und WebSocket-Wiederverbindung

## 🐛 Bugfixes

- **Modal-Verhalten**: Modal reagiert korrekt auf WebSocket-Nachrichten und schließt nach Upload
- **Datei-Validierung**: Robustere Validierung von Firmware- und Frontend-Dateien

## 📝 Technische Details

- **Firmware-Version**: v2.1.0
- **ESP32**: DevKit V1
- **Framework**: Arduino
- **Build-System**: PlatformIO

## 🔗 Weitere Informationen

- **Repository**: https://github.com/s3vdev/Heizungssteuerung
- **Dokumentation**: Siehe `README.md` und `SCHALTPLAN.md`

## 📋 Changelog

Vollständiger Changelog siehe `release/README.md`
