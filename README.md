# ESP32 Mesh-Lite Snapclient (Minimal Edition)

Minimaler Snapcast-Client für ESP32 mit ESP-Mesh-Lite.

## Ziel

Diese Branch-Variante konzentriert sich ausschließlich auf den Audio-Pfad:

```text
Mesh-Lite
↓
Snapcast
↓
Opus
↓
I2S
```

Folgende Komponenten wurden entfernt:

- Bluetooth
- A2DP
- AVRCP
- BLE
- Bluedroid

Dadurch werden RAM-Verbrauch, CPU-Last und mögliche WLAN-/Bluetooth-Koexistenzprobleme reduziert.

---

## Funktionen

- Snapcast-Client
- Opus-Dekodierung
- ESP-Mesh-Lite
- I2S-Audioausgabe
- Root- und Child-Betrieb im Mesh

---

## Nicht enthalten

```text
Bluetooth
A2DP
AVRCP
BLE
Mikrofon
Paging
AEC
```

---

## Audiopfad

```text
Snapserver
    ↓
   Opus
    ↓
 Snapclient
    ↓
 Source Arbiter
    ↓
    I2S
    ↓
 PCM5102A / ADAU1701
```

---

## Hardware

### Erfolgreich getestet

- ESP32
- ESP-IDF 5.4.3
- ESP-Mesh-Lite
- Snapcast (Opus)
- I2S-Audioausgabe

### Flash

Der aktuelle Teststand wurde mit folgender Build-Konfiguration betrieben:

```text
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
```

### PSRAM

Die getestete Hardware verfügte über PSRAM.

Ob PSRAM für diesen Branch tatsächlich erforderlich ist, wurde bislang nicht untersucht.

---

## Aktueller Status

Erfolgreich getestet:

- Mesh-Lite verbindet sich mit dem Router
- Root-Betrieb funktioniert
- Snapserver-Verbindung funktioniert
- Opus-Dekodierung funktioniert
- I2S-Ausgabe funktioniert

Beobachtete Laufzeitwerte:

```text
decode_errors = 0
I2S errors    = 0
dropped       = überwiegend 0
```

Der Branch dient als minimaler Referenzstand für weitere Optimierungen und Analysen.

---

## Build

```bash
idf.py set-target esp32
idf.py reconfigure
idf.py build
idf.py -p COM5 flash monitor
```

---

## Konfiguration

Projektparameter werden über `menuconfig` bzw. die Projektkonfiguration gesetzt:

```bash
idf.py menuconfig
```

Wichtige Parameter:

```text
Router SSID
Router Passwort
Snapserver IP-Adresse
Snapserver Port
Mesh-Parameter
```

---

## Branch-Hintergrund

Die Vollversion enthält zusätzlich:

```text
Bluetooth
A2DP
AVRCP
automatische Quellenumschaltung
```

Diese Komponenten wurden in diesem Branch bewusst entfernt, um den Ressourcenbedarf zu minimieren und die Snapcast-/Mesh-Funktion isoliert testen zu können.

---

## Lizenz

Dieses Projekt wird zu Evaluierungs- und Entwicklungszwecken bereitgestellt.