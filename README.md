# ESP32 Mesh Snapclient

ESP32-basierter Multiroom-Audioclient auf Basis von Snapcast, ESP-Mesh-Lite und Opus.

Das Projekt ermöglicht die synchrone Wiedergabe von Audio über mehrere ESP32-Knoten. Die Audioverteilung erfolgt über Snapcast, die Netzwerkanbindung über ESP-Mesh-Lite. Zusätzlich wird Bluetooth A2DP als lokale Audioquelle unterstützt.

---

# Features

- Snapcast Client
- Opus Decoder
- ESP-Mesh-Lite
- Bluetooth A2DP Sink
- Automatische Quellenumschaltung
- I2S Audioausgabe
- PCM5102A Unterstützung
- ADAU1701 Unterstützung
- Automatischer Reconnect
- Mehrknotenbetrieb
- 48 kHz Stereo Audio Pipeline

---

# Systemarchitektur

```text
                Snapserver
                     │
                     ▼
                   Opus
                     │
                     ▼
              Snapclient Glue
                     │
                     ▼
              Source Arbiter
                 ▲       ▲
                 │       │
          Snapcast     A2DP
                 │       │
                 └───┬───┘
                     │
                     ▼
                    I2S
                     │
          ┌──────────┴──────────┐
          │                     │
          ▼                     ▼
      PCM5102A             ADAU1701
```

---

# Audioquellen

## Snapcast

Eigenschaften:

```text
Codec:      Opus
Samplerate: 48 kHz
Kanäle:     Stereo
```

Die Codec-Erkennung erfolgt automatisch anhand der vom Snapserver übertragenen Streaminformationen.

---

## Bluetooth A2DP

Eigenschaften:

```text
Bluetooth Classic A2DP Sink
Automatische Quellumschaltung
Automatische Rückkehr zu Snapcast
```

Der Bluetooth-Name wird automatisch aus der ESP32-MAC-Adresse erzeugt.

Beispiel:

```text
Snap-Blth-9904
```

---

# Automatische Quellenumschaltung

Der Source-Arbiter stellt sicher, dass immer nur eine Audioquelle aktiv ist.

Priorität:

```text
Bluetooth A2DP
        ↓
     Snapcast
        ↓
      Stille
```

Verhalten:

```text
A2DP Audio STARTED
        ↓
Snapcast wird pausiert
        ↓
Bluetooth wird wiedergegeben
```

```text
A2DP Audio STOPPED
        ↓
Snapcast verbindet erneut
        ↓
Snapcast wird wiedergegeben
```

Eine gleichzeitige Wiedergabe mehrerer Quellen findet nicht statt.

---

# PCM5102A Anschluss

## Pinning

```text
ESP32      PCM5102A
--------------------
GPIO21 --> DIN
GPIO22 --> LRCK
GPIO23 --> BCK
3V3    --> VCC
GND    --> GND
```

## Hinweis

```text
Der PCM5102A wird ohne MCLK betrieben.

Der SCK/MCLK-Pin des PCM5102A bleibt unbeschaltet.
```

---

# ADAU1701 Anschluss

## Pinning

```text
ESP32      ADAU1701
--------------------
GPIO21 --> SDATA_IN
GPIO22 --> LRCLK
GPIO23 --> BCLK
3V3    --> VCC
GND    --> GND
```

Die Taktversorgung erfolgt über den ESP32 als I2S-Master.

---

# Mesh-Lite

Das Projekt verwendet ESP-Mesh-Lite zur automatischen Bildung eines vermaschten WLAN-Netzes.

Beispiel:

```text
Root
 ├─ Child
 ├─ Child
 └─ Child
```

Der Root-Knoten verbindet sich mit dem Router und stellt die Verbindung zum Snapserver bereit.

---

# WLAN-Konfiguration

Die Zugangsdaten werden über die Projektkonfiguration hinterlegt:

```text
CONFIG_ROUTER_SSID
CONFIG_ROUTER_PASSWORD
```

Beispiel:

```text
# CONFIG_ROUTER_SSID removed
# CONFIG_ROUTER_PASSWORD removed
```

---

# Snapserver-Konfiguration

Standardparameter:

```text
CONFIG_SNAPSERVER_HOST="192.168.1.4"
CONFIG_SNAPSERVER_PORT=1704
```

---

# Build

Projekt kompilieren:

```bash
idf.py build
```

Firmware flashen:

```bash
idf.py flash
```

Monitor starten:

```bash
idf.py monitor
```

Flashen und Monitor gemeinsam:

```bash
idf.py flash monitor
```

---

# Getestete Funktionen

```text
✓ Snapcast
✓ Opus
✓ ESP-Mesh-Lite
✓ WLAN-Reconnect
✓ PCM5102A
✓ ADAU1701
✓ Bluetooth A2DP
✓ Snapcast ↔ A2DP Umschaltung
✓ Mehrknotenbetrieb
✓ ESP32-WROVER
```

---

# Empfohlene Hardware

## ESP32

Getestet:

```text
ESP32-WROVER
```

## DAC / DSP

Unterstützt:

```text
PCM5102A
ADAU1701
```

---

# Bekannte Einschränkungen

Für stabile Audiowiedergabe wird empfohlen:

```text
RSSI > -80 dBm
```

Sehr schwache WLAN-Verbindungen können zu:

```text
Reconnects
Audioaussetzern
erhöhter Latenz
```

führen.

---

# Projektstatus

```text
Snapcast:            stabil
Opus:                stabil
Mesh-Lite:           stabil
PCM5102A:            stabil
ADAU1701:            stabil
Bluetooth A2DP:      stabil
Quellenumschaltung:  stabil
Mehrknotenbetrieb:   stabil
```

---

# Lizenz

Projektinternes Entwicklungsprojekt.