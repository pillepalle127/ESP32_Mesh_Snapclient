# ESP32 Mesh Snapclient

ESP32-basierter Snapcast-Client mit Bluetooth A2DP, automatischer Quellenumschaltung und Unterstützung für PCM5102A sowie ADAU1701.

## Features

- Snapcast Client (Opus)
- ESP-Mesh-Lite
- Bluetooth A2DP Sink
- Automatische Quellenumschaltung
- 48 kHz Stereo Audio Pipeline
- PCM5102A Unterstützung
- ADAU1701 Unterstützung
- ESP32-WROVER empfohlen

---

## Audio-Pipeline

```text
Snapcast (Opus)
          │
          ▼
Bluetooth A2DP
          │
          ▼
     Source Arbiter
          │
          ▼
          I2S
          │
 ┌────────┴────────┐
 │                 │
 ▼                 ▼
PCM5102A       ADAU1701
```

---

## Quellenpriorität

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
    → Snapcast wird pausiert

A2DP Audio STOPPED
    → Snapcast wird automatisch wieder verbunden
```

Es wird immer nur eine Audioquelle gleichzeitig ausgegeben.

---

## PCM5102A Anschluss

### Pinning

```text
ESP32      PCM5102A
--------------------
GPIO21 --> DIN
GPIO22 --> LRCK
GPIO23 --> BCK

3V3    --> VCC
GND    --> GND
```

### Hinweise

```text
MCLK wird nicht benötigt.

Der SCK/MCLK-Pin des PCM5102A bleibt unbeschaltet.
```

Audioformat:

```text
48 kHz
16 Bit
Stereo
```

---

## ADAU1701 Anschluss

### Pinning

```text
ESP32      ADAU1701
--------------------
GPIO21 --> SDATA_IN
GPIO22 --> LRCLK
GPIO23 --> BCLK

3V3    --> VCC
GND    --> GND
```

### Hinweise

```text
Der ADAU1701 kann als DSP-Stufe verwendet werden.

Die Audioquelle (Snapcast oder Bluetooth)
wird vollständig auf dem ESP32 verwaltet.
```

---

## Bluetooth

Bluetooth-Gerätename:

```text
Snap-Blth-XXXX
```

Beispiel:

```text
Snap-Blth-9904
```

Die letzten vier Zeichen entsprechen den letzten beiden Bytes der WLAN-MAC-Adresse.

---

## Snapcast

Standardparameter:

```text
Codec       : Opus
Samplerate  : 48 kHz
Kanäle      : Stereo
Bitbreite   : 16 Bit
```

Die Codec-Erkennung erfolgt automatisch anhand der vom Snapserver übertragenen Streaminformationen.

---

## ESP32 Hardware

### Empfohlen

```text
ESP32-WROVER
8 MB PSRAM
```

Getestet:

```text
✓ Snapcast
✓ Opus
✓ Bluetooth A2DP
✓ Mesh-Lite
✓ PCM5102A
✓ ADAU1701
✓ Automatische Quellenumschaltung
```

### Nicht empfohlen

```text
ESP32 ohne PSRAM
```

Die Kombination aus

- Bluetooth A2DP
- Mesh-Lite
- Snapcast
- Opus-Decoding

beansprucht erhebliche Speicherressourcen.

---

## Aktueller Status

```text
✓ Snapcast stabil
✓ Opus-Decoding stabil
✓ Bluetooth A2DP stabil
✓ A2DP ↔ Snapcast Umschaltung stabil
✓ PCM5102A getestet
✓ ADAU1701 getestet
✓ ESP32-WROVER getestet
```

---

## Projektstart

ESP-IDF Umgebung aktivieren:

```cmd
C:\esp\v5.4.3\esp-idf\export.bat
```

Projekt bauen:

```cmd
idf.py build
```

Flashen:

```cmd
idf.py -p COM5 flash
```

Flashen und Monitor starten:

```cmd
idf.py -p COM5 flash monitor
```