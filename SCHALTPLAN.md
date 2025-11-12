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
        ┌──────────▼──────────┐  ┌─────▼──────────────┐        │
        │                     │  │                     │        │
        │    ESP32 DevKit     │  │   Relais-Modul     │        │
        │                     │  │   (Active-Low!)    │        │
        │  ┌───────────────┐  │  │                     │        │
        │  │ PIN-Belegung: │  │  │  VCC ←── 5V        │        │
        │  │               │  │  │  GND ←── GND       │        │
        │  │ 5V   ←── 5V   │  │  │  IN  ←── GPIO23    │────────┤
        │  │ GND  ←── GND  │  │  │                     │        │
        │  │               │  │  │  COM ────────┐      │        │
        │  │ 3.3V ──→ +    │  │  │  NO  ────────┼──────┼────────┘
        │  │               │  │  │              │      │    zu Heizgerät
        │  │ GPIO4 ──→ BUS │  │  └──────────────┼──────┘
        │  │               │  │                  │
        │  │ GPIO23 ──→ RLY│  │                  │
        │  └───────────────┘  │           Phase ─┘
        │                     │           (230V!)
        └──────────┬──────────┘
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
| **5V** | LM2596S OUT+ | Stromversorgung (5V) |
| **GND** | Gemeinsame Masse | GND zu allem! |
| **3.3V** | DS18B20 VDD (beide) + Pull-Up | Versorgung Sensoren |
| **GPIO4** | DS18B20 DATA (beide) | OneWire Bus |
| **GPIO23** | Relais IN | Steuerung (Active-Low!) |

### DS18B20 Sensoren (beide identisch verdrahtet)

| Kabel | Farbe | Verbindung |
|-------|-------|------------|
| **VDD** | Rot | ESP32 3.3V |
| **DATA** | Gelb | ESP32 GPIO4 |
| **GND** | Schwarz | ESP32 GND |

**WICHTIG:** 4.7kΩ Widerstand zwischen GPIO4 und 3.3V!

### Relais-Modul (1-Kanal, Active-Low)

| Relais Pin | Verbindung | Beschreibung |
|------------|------------|--------------|
| **VCC** | 5V | Stromversorgung |
| **GND** | GND | Masse |
| **IN** | ESP32 GPIO23 | Steuersignal |
| **COM** | Phase (L) | Eingang 230V |
| **NO** | zu Heizgerät | Ausgang (Normal Open) |

**Active-Low Logik:**
- GPIO23 = **LOW** (0V) → Relais **EIN** → Heizung läuft
- GPIO23 = **HIGH** (3.3V) → Relais **AUS** → Heizung aus

### LM2596S Spannungsregler

| Pin | Verbindung | Spannung |
|-----|------------|----------|
| **IN+** | Netzteil + | 12V oder 24V |
| **IN-** | Netzteil - | GND |
| **OUT+** | ESP32 5V + Relais VCC | 5V (einstellen!) |
| **OUT-** | Gemeinsame Masse | GND |

**Einstellung:** Potentiometer drehen, bis LED-Anzeige **5.0V** zeigt!

## Schritt-für-Schritt Aufbau

### Phase 1: Spannungsversorgung (OHNE Netzspannung!)

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

7. **Relais anschließen:**
   - VCC → 5V (vom LM2596S)
   - GND → Gemeinsame Masse
   - IN → ESP32 GPIO23

8. **Test:**
   - Manuellen Modus im Dashboard aktivieren
   - Toggle-Button klicken
   - Relais sollte **klicken**!

### Phase 4: Heizung (MIT VORSICHT!)

⚠️ **ACHTUNG: AB HIER 230V NETZSPANNUNG!** ⚠️

9. **STROMZUFUHR AUSSCHALTEN!**
   - Sicherung raus
   - Mit Spannungsprüfer testen
   - Zweimal prüfen!

10. **W1209 ausbauen:**
    - Alle Verbindungen dokumentieren (Foto!)
    - Abklemmen
    - Beiseite legen

11. **Relais einbauen:**
    - Phase (L) von Verteilung → Relais COM
    - Relais NO → Heizgerät
    - **NIEMALS N oder PE durch Relais!**

12. **Erste Inbetriebnahme:**
    - Nochmals ALLE Verbindungen prüfen
    - Sicherung wieder rein
    - Im manuellen Modus testen
    - Kurze Test-Intervalle (5 Min ein, 5 Min aus)

## Sicherheitshinweise

### ⚠️ LEBENSGEFAHR durch 230V Netzspannung!

✅ **IMMER beachten:**
- Vor Arbeiten **Sicherung rausdrehen**
- Mit **Spannungsprüfer** Spannungsfreiheit prüfen
- **NIE** unter Spannung arbeiten
- Nur **Phase (L)** durch Relais schalten
- **Neutral (N)** und **Schutzleiter (PE)** DURCHGEHEND
- Gehäuse mit **PE** verbinden
- Nach Montage: **Isolationsprüfung**

✅ **Relais-Spezifikationen prüfen:**
- Mindestens **10A bei 230V AC**
- Für **Widerstandslast** geeignet
- Bei induktiver Last (Motor): höhere Spezifikation!

✅ **Brand-Schutz:**
- Alle Verbindungen **fest verschrauben**
- **Aderendhülsen** verwenden
- ESP32 in **nicht brennbarem Gehäuse**
- **Fehlerstromschutzschalter (FI)** vorgeschaltet

## Montage-Tipps

### Gehäuse
- IP65-Gehäuse für Hutschiene
- Kabelverschraubungen für Sensoren
- Lüftungsschlitze (ESP32 wird warm)
- Beschriftung aller Anschlüsse

### Kabelführung
- Sensorkabel **vom** 230V-Bereich trennen
- Zugentlastung an allen Kabeln
- Beschriftung: "Vorlauf", "Rücklauf"
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
- [ ] Relais klickt im Test
- [ ] Alle Schrauben fest
- [ ] Keine blanken Drähte
- [ ] Gehäuse geschlossen
- [ ] FI-Schalter vorhanden
- [ ] Erdung geprüft

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

