# ESP32 Heizungssteuerung - Schaltplan

## Übersicht

```
                    ┌─────────────────────────────────────────────┐
                    │         12V/24V Netzteil                     │
                    │         (von Heizungsanlage)                 │
                    └──────────────┬──────────────────────────────┘
                                   │
                    ┌──────────────┴──────────────────────────────┐
                    │                                              │
                ┌───▼────────────────────────┐                    │
                │   LM2596S DC-DC            │                    │
                │   Spannungsregler          │                    │
                │   (auf 5V einstellen!)     │                    │
                │                            │                    │
                │  IN+  ←─── 12V/24V        │                    │
                │  IN-  ←─── GND            │                    │
                │  OUT+ ───→ 5V             │                    │
                │  OUT- ───→ GND            │                    │
                └────────────┬───────────────┘                    │
                             │                                    │
                   ┌─────────┴──────────┐                        │
                   │                    │                        │
        ┌──────────▼──────────┐  ┌─────▼──────────────┐  ┌─────▼──────────────┐
        │                     │  │                     │  │                     │
        │    ESP32 DevKit     │  │ Relais #1 (Heizung) │  │ Relais #2 (Pumpe)  │
        │                     │  │   (Active-Low!)     │  │   (Active-Low!)    │
        │  ┌───────────────┐  │  │                     │  │                     │
        │  │ PIN-Belegung: │  │  │  VCC ←── 5V        │  │  VCC ←── 5V        │
        │  │               │  │  │  GND ←── GND       │  │  GND ←── GND       │
        │  │ 5V   ←── 5V   │  │  │  IN  ←── GPIO23   │  │  IN  ←── GPIO22   │
        │  │ GND  ←── GND  │  │  │                     │  │                     │
        │  │               │  │  │  COM ────────┐     │  │  COM ────────┐     │
        │  │ 3.3V ──→ +    │  │  │  NO  ────────┼─────┼─│  NO  ────────┼─────┼─
        │  │               │  │  │              │     │ │              │     │ │
        │  │ GPIO4 ──→ BUS │  │  └──────────────┼─────┘ └──────────────┼─────┘ │
        │  │               │  │                  │                      │       │
        │  │ GPIO23 ──→ RLY│  │                  │                      │       │
        │  │ GPIO22 ──→ RLY│  │                  │                      │       │
        │  └───────────────┘  │            +12V ─┘              Phase ─┘       │
        │                     │           (12V)                 (230V!)        │
        └──────────┬──────────┘                                              │
                   │                                                          │
                   │                    zu Heizgerät        zu Umwälzpumpe ───┘
                   │
                   │ GPIO4 (OneWire Bus)
                   │
        ┌──────────┴──────────────────────────────┐
        │                                          │
        │  [4.7kΩ Pull-Up Widerstand]             │
        │  zwischen GPIO4 und 3.3V                │
        │                                          │
        ├─────────────┬────────────────────────────┤
        │             │                            │
    ┌───▼────────┐ ┌──▼───────────┐              │
    │ DS18B20 #1 │ │  DS18B20 #2  │              │
    │ (Vorlauf)  │ │  (Rücklauf)  │              │
    │            │ │              │              │
    │ Rot   → 3.3V│ │ Rot   → 3.3V│              │
    │ Gelb  → GPIO4│ │Gelb  → GPIO4│ (parallel!) │
    │ Schwarz→ GND│ │Schwarz→ GND │              │
    └────────────┘ └──────────────┘              │
                                                  │
                          GND ←───────────────────┘
                          (gemeinsame Masse!)
```

## Detaillierte Pin-Belegung

### ESP32 DevKit V1

| ESP32 Pin | Verbindung | Beschreibung |
|-----------|------------|--------------|
| **5V** | USB-Netzteil (5V, 1-2A) ODER LM2596S OUT+ | Stromversorgung |
| **GND** | Gemeinsame Masse | GND zu allem! |
| **3.3V** | DS18B20 VDD (beide) + Pull-Up | Versorgung Sensoren |
| **5V** (Ausgang) | Relais VCC (beide) + JSN-SR04T VCC (optional) | Versorgung für 5V-Komponenten |
| **GPIO4** | DS18B20 DATA (beide) | OneWire Bus |
| **GPIO5** | JSN-SR04T TRIG | Ultraschall Trigger (optional) |
| **GPIO18** | JSN-SR04T ECHO | Ultraschall Echo (optional) |
| **GPIO23** | Relais #1 IN (Heizung) | Steuerung (Active-Low!) |
| **GPIO22** | Relais #2 IN (Pumpe) | Steuerung (Active-Low!) |

### DS18B20 Sensoren (beide identisch verdrahtet)

| Kabel | Farbe | Verbindung |
|-------|-------|------------|
| **VDD** | Rot | ESP32 3.3V |
| **DATA** | Gelb | ESP32 GPIO4 |
| **GND** | Schwarz | ESP32 GND |

**WICHTIG:** 4.7kΩ Widerstand zwischen GPIO4 und 3.3V!

### Relais-Module (2x 1-Kanal, Active-Low)

#### Relais #1: Heizung (GPIO23) - 12V DC

| Relais Pin | Verbindung | Beschreibung |
|------------|------------|--------------|
| **VCC** | ESP32 5V Pin (direkt möglich) ODER LM2596S OUT+ | Stromversorgung |
| **GND** | GND | Masse |
| **IN** | ESP32 GPIO23 | Steuersignal |
| **COM** | +12V (gelbes Kabel) | Eingang 12V DC |
| **NO** | zu Heizgerät (+12V) | Ausgang (Normal Open) |

#### Relais #2: Umwälzpumpe (GPIO22) - 230V AC

| Relais Pin | Verbindung | Beschreibung |
|------------|------------|--------------|
| **VCC** | ESP32 5V Pin (direkt möglich) ODER LM2596S OUT+ | Stromversorgung |
| **GND** | GND | Masse |
| **IN** | ESP32 GPIO22 | Steuersignal |
| **COM** | Phase (L) | Eingang 230V AC |
| **NO** | zu Umwälzpumpe | Ausgang (Normal Open) |

**WICHTIG:** 
- **Relais #1 (Heizung)**: Schaltet 12V DC (gelbes Kabel) - GND durchgehend verbunden!
- **Relais #2 (Pumpe)**: Schaltet 230V AC - **NIEMALS N oder PE durch Relais!**

**Active-Low Logik mit Open-Drain-Mode (beide Relais identisch):**
- GPIO23/GPIO22 = **LOW** (OUTPUT-Mode, 0V) → Relais **EIN** → Heizung/Pumpe läuft
- GPIO23/GPIO22 = **HIGH** (OUTPUT_OPEN_DRAIN-Mode, floating) → Relais **AUS** → Heizung/Pumpe aus

**WICHTIG:** Das HW-307 Relais-Modul erkennt 3.3V HIGH nicht zuverlässig! Daher muss Open-Drain-Mode verwendet werden:
- HIGH wird als "floating" gesetzt → wird vom internen Pull-Up des Relais-Moduls auf HIGH gezogen
- LOW wird im normalen OUTPUT-Mode gesetzt → Pin zieht aktiv auf GND

**Pumpensteuerung:**
- **Sicherheitsregel**: Heizung EIN → Pumpe MUSS automatisch EIN sein
- **Nachlauf**: Pumpe bleibt nach Heizung AUS noch 180 Sekunden (3 Minuten) an
- **Manueller Modus**: Pumpe kann unabhängig geschaltet werden (nur wenn Heizung AUS ist)
- **Automatik/Zeitplan**: Pumpe folgt automatisch der Heizung

### JSN-SR04T Ultraschall-Sensor (optional)

| Sensor Pin | Verbindung | Beschreibung |
|------------|------------|--------------|
| **VCC** | ESP32 5V Pin (direkt möglich) ODER LM2596S OUT+ | Stromversorgung |
| **TRIG** | ESP32 GPIO5 | Trigger-Signal |
| **ECHO** | ESP32 GPIO18 | Echo-Rückmeldung |
| **GND** | GND | Masse |

**Funktion:**
- Misst Abstand zur Flüssigkeitsoberfläche im Tank
- Berechnet Füllstand in Litern & Prozent
- Messbereich: 25 cm - 450 cm
- Wasserdicht (IP67)
- Von **oben** in den Tank montieren

**Montage-Hinweise:**
- Sensor zeigt senkrecht nach unten auf die Flüssigkeit
- Mindestabstand: 25 cm zur Oberfläche
- Maximaler Abstand: 450 cm
- Konfiguration im Dashboard: Tankhöhe & Kapazität eingeben
- Fallback: Falls nicht angeschlossen → Dashboard zeigt "N/A"

### Stromversorgung (2 Optionen)

#### Option 1: USB-Netzteil (empfohlen für einfache Installation)
**Alle Komponenten können direkt am ESP32 betrieben werden:**

| Komponente | Anschluss | Stromaufnahme |
|------------|-----------|---------------|
| ESP32 | USB-Netzteil (5V, 1-2A) | ~100-250mA |
| Relais #1 VCC (Heizung) | ESP32 5V Pin | ~70-150mA |
| Relais #2 VCC (Pumpe) | ESP32 5V Pin | ~70-150mA |
| JSN-SR04T VCC (optional) | ESP32 5V Pin | ~15-30mA |
| DS18B20 (beide) | ESP32 3.3V Pin | ~3-5mA |
| **Gesamt** | **USB-Netzteil** | **~258-585mA** ✅ |

**Vorteile:**
- Einfache Installation (nur USB-Kabel + Netzteil)
- Keine zusätzliche Elektronik nötig
- USB-Netzteile sind günstig und überall erhältlich

#### Option 2: LM2596S Spannungsregler (optional, bei externer 12V/24V Versorgung)

| Pin | Verbindung | Spannung |
|-----|------------|----------|
| **IN+** | Netzteil + | 12V oder 24V |
| **IN-** | Netzteil - | GND |
| **OUT+** | ESP32 5V + Relais VCC (beide) + JSN-SR04T VCC | 5V (einstellen!) |
| **OUT-** | Gemeinsame Masse | GND |

**Einstellung:** Potentiometer drehen, bis LED-Anzeige **5.0V** zeigt!

**Wann nötig:**
- Externe 12V/24V Versorgung (z.B. von der Heizung)
- Kein USB-Netzteil verfügbar
- Besonders hohe Stromaufnahme (z.B. mehrere Relais)

## Schritt-für-Schritt Aufbau

### Phase 1: Spannungsversorgung (OHNE Netzspannung!)

#### Option A: USB-Netzteil (empfohlen - einfacher!)

1. **USB-Netzteil anschließen:**
   - USB-Kabel an ESP32 USB-C Port
   - USB-Netzteil (5V, 1-2A) in die Steckdose
   - ESP32 sollte booten (LED leuchtet) ✅

**Alle Komponenten können direkt am ESP32 angeschlossen werden:**
- Relais #1 VCC (Heizung) → ESP32 5V Pin
- Relais #2 VCC (Pumpe) → ESP32 5V Pin
- JSN-SR04T VCC → ESP32 5V Pin
- DS18B20 VDD → ESP32 3.3V Pin

#### Option B: LM2596S Spannungsregler (optional, bei 12V/24V Versorgung)

1. **LM2596S einstellen:**
   - Mit USB-Netzteil (12V) an IN+ / IN- anschließen
   - Potentiometer drehen bis Display **5.0V** zeigt
   - Abziehen und merken (evt. mit Kleber fixieren)

2. **ESP32 testen:**
   - LM2596S OUT+ an ESP32 5V
   - LM2596S OUT- an ESP32 GND
   - 12V anlegen → ESP32 sollte booten (LED leuchtet)

### Phase 2: Sensoren anschließen

3. **Pull-Up Widerstand:**
   - 4.7kΩ zwischen ESP32 GPIO4 und 3.3V löten
   - Am besten auf kleiner Lochrasterplatine

4. **DS18B20 #1 (Vorlauf):**
   - Rot → 3.3V
   - Gelb → GPIO4
   - Schwarz → GND

5. **DS18B20 #2 (Rücklauf):**
   - Rot → 3.3V (parallel zu #1)
   - Gelb → GPIO4 (parallel zu #1)
   - Schwarz → GND (parallel zu #1)

6. **Test:**
   - Software flashen
   - Serial Monitor öffnen
   - Sollte "Found 2 DS18B20 sensor(s)" anzeigen

### Phase 3: Relais (OHNE Last!)

7. **Relais #1 (Heizung) anschließen:**
   - VCC → ESP32 5V Pin (direkt möglich) ODER LM2596S OUT+
   - GND → Gemeinsame Masse
   - IN → ESP32 GPIO23

8. **Relais #2 (Pumpe) anschließen:**
   - VCC → ESP32 5V Pin (direkt möglich) ODER LM2596S OUT+
   - GND → Gemeinsame Masse
   - IN → ESP32 GPIO22

9. **Test:**
   - Manuellen Modus im Dashboard aktivieren
   - Heizung-Toggle klicken → Relais #1 sollte **klicken**!
   - Pumpe-Toggle klicken → Relais #2 sollte **klicken**!
   - **Sicherheitstest**: Heizung EIN schalten → Pumpe sollte automatisch EIN gehen

### Phase 3.5: JSN-SR04T Sensor (optional)

9. **Sensor anschließen:**
   - VCC → ESP32 5V Pin (direkt möglich) ODER LM2596S OUT+
   - GND → Gemeinsame Masse
   - TRIG → ESP32 GPIO5
   - ECHO → ESP32 GPIO18

10. **Test:**
    - Software neuflashen (falls schon geflasht)
    - Serial Monitor: "Tank sensor detected" oder "Tank sensor not available"
    - Dashboard öffnen: Tankinhalt-Card sollte sichtbar sein
    - Falls "Sensor nicht verfügbar" → optional, kein Problem

11. **Konfiguration im Tank:**
    - Sensor von oben in Tank montieren (senkrecht nach unten)
    - Im Dashboard: Tankhöhe (cm) und Tankkapazität (Liter) eingeben
    - Füllstand sollte jetzt angezeigt werden

### Phase 4: Heizung und Pumpe

12. **Alte Steuerung ausbauen:**
    - Alle Verbindungen dokumentieren (Foto!)
    - Abklemmen
    - Beiseite legen

13. **Relais #1 (Heizung) einbauen - 12V DC:**
    - +12V (gelbes Kabel) von Versorgung → Relais COM
    - Relais NO → Heizgerät (+12V)
    - GND durchgehend verbunden (nicht durch Relais!)
    - **Polung beachten**: +12V / GND

14. **Relais #2 (Pumpe) einbauen - 230V AC:**
    - ⚠️ **STROMZUFUHR AUSSCHALTEN!** Sicherung raus, Spannungsfreiheit prüfen!
    - Phase (L) von Verteilung → Relais COM
    - Relais NO → Umwälzpumpe
    - **NIEMALS N oder PE durch Relais!**
    - Neutral (N) und Schutzleiter (PE) durchgehend verbunden

15. **Erste Inbetriebnahme:**
    - Nochmals ALLE Verbindungen prüfen
    - **Heizung**: Polung prüfen (+12V / GND)
    - **Pumpe**: Isolationsprüfung, FI-Schalter prüfen
    - Sicherung wieder rein (nur für Pumpe-Bereich)
    - Im manuellen Modus testen
    - Kurze Test-Intervalle (5 Min ein, 5 Min aus)

## Sicherheitshinweise

### ⚠️ LEBENSGEFAHR durch 230V Netzspannung (Pumpe)!

✅ **IMMER beachten bei Pumpe (230V AC):**
- Vor Arbeiten **Sicherung rausdrehen**
- Mit **Spannungsprüfer** Spannungsfreiheit prüfen
- **NIE** unter Spannung arbeiten
- Nur **Phase (L)** durch Relais schalten
- **Neutral (N)** und **Schutzleiter (PE)** DURCHGEHEND
- Gehäuse mit **PE** verbinden
- Nach Montage: **Isolationsprüfung**

✅ **Bei Heizung (12V DC) beachten:**
- Vor Arbeiten **Stromzufuhr trennen**
- Mit **Multimeter** Spannung prüfen
- **Polung beachten**: +12V (gelbes Kabel) / GND
- **GND (Masse)** DURCHGEHEND verbinden (nicht durch Relais!)
- Nach Montage: **Polarität prüfen**

✅ **Relais-Spezifikationen prüfen:**
- **Relais #1 (Heizung)**: Mindestens **10A bei 12V DC**
- **Relais #2 (Pumpe)**: Mindestens **10A bei 230V AC**
- Für **Widerstandslast** geeignet
- Bei induktiver Last (Motor): höhere Spezifikation!

✅ **Sicherheit:**
- Alle Verbindungen **fest verschrauben**
- **Aderendhülsen** verwenden
- ESP32 in **nicht brennbarem Gehäuse**
- **Sicherung (max. 10A)** vorgeschaltet empfohlen
- **Fehlerstromschutzschalter (FI)** für 230V-Bereich vorgeschaltet

## Montage-Tipps

### Gehäuse
- IP65-Gehäuse für Hutschiene
- Kabelverschraubungen für Sensoren
- Lüftungsschlitze (ESP32 wird warm)
- Beschriftung aller Anschlüsse

### Kabelführung
- Sensorkabel ordentlich führen
- Zugentlastung an allen Kabeln
- Beschriftung: "Vorlauf", "Rücklauf", "+12V", "GND"
- Reserve-Länge einplanen

### Befestigung
- ESP32 auf DIN-Schiene (mit Adapter) oder
- Auf Lochrasterplatine löten
- Relais auf DIN-Schiene
- LM2596S fixieren (wird warm!)

## Test-Checkliste vor Inbetriebnahme

- [ ] Alle Lötstellen geprüft
- [ ] 4.7kΩ Pull-Up vorhanden
- [ ] LM2596S auf 5V eingestellt
- [ ] Beide Sensoren werden erkannt
- [ ] Beide Relais klicken im Test
- [ ] Sicherheitstest: Heizung EIN → Pumpe automatisch EIN
- [ ] Alle Schrauben fest
- [ ] Keine blanken Drähte
- [ ] Gehäuse geschlossen
- [ ] **Heizung**: Polung geprüft (+12V / GND)
- [ ] **Pumpe**: FI-Schalter vorhanden, Isolationsprüfung durchgeführt
- [ ] Sicherung (max. 10A) vorhanden

## Erstes Monitoring (nach Installation)

**Tag 1-3:** Intensiv beobachten
- Alle 2h Temperaturen prüfen
- Dashboard-Zugriff testen
- Relais-Schaltung beobachten
- Gehäuse-Temperatur fühlen (sollte lauwarm sein)

**Woche 1:** Täglich prüfen
- RSSI-Signal stabil?
- NTP-Zeit korrekt?
- Automatik funktioniert?
- Keine ungewöhnlichen Geräusche?

**Ab Woche 2:** Normal nutzen
- Bei Problemen: Troubleshooting-Guide

---

**Zum Speichern als PDF:**
1. Diese Datei in Browser öffnen (mit Markdown-Viewer)
2. Drucken → "Als PDF speichern"
3. Oder: In VS Code Markdown-PDF Extension nutzen

**Bei Fragen oder Problemen: Troubleshooting-Guide lesen!**

