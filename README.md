# ESP32 Mesh Snapclient

ESP32-basierter Snapcast-Client mit Bluetooth A2DP, automatischer Quellenumschaltung und Unterstützung für PCM5102A sowie ADAU1701.

## Features

- Snapcast Client (PCM und Opus)
- ESP-Mesh-Lite
- Bluetooth A2DP Sink
- Automatische Quellenumschaltung
- Linkwitz-Riley-Frequenzweiche (LR4)
- Stereo-zu-Mono Summierung
- 48 kHz Audio-Pipeline
- PCM5102A Unterstützung
- ADAU1701 Unterstützung
- ESP32-WROVER empfohlen

---

## Audio-Pipeline

```text
Snapcast (PCM/Opus)
          │
          ▼
Bluetooth A2DP
          │
          ▼
     Source Arbiter
          │
          ▼
     Stereo → Mono
          │
          ▼
 Linkwitz-Riley LR4
 Frequenzweiche
          │
 ┌────────┴────────┐
 │                 │
 ▼                 ▼
Hochpass       Tiefpass
 │                 │
 ▼                 ▼
Links         Rechts
Breitband     Subwoofer
          │
          ▼
          I2S