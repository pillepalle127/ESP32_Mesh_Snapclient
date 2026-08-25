# ESP32 Mesh Snapclient

ESP32-WROVER basierter Snapcast-Client für Multiroom-Audio mit ESP-Mesh-Lite.

## Features

- Snapcast Client
- Opus Decoder (ESP-IDF + esp-opus)
- ESP Mesh Lite
- Bluetooth A2DP Sink
- ADAU1701 Audio DSP
- Automatische Quellenumschaltung (Snapcast / A2DP)
- 48 kHz Stereo Audio Pipeline
- ESP32 als I2S Master

## Audio Pipeline

Snapserver
→ Opus
→ ESP32
→ Opus Decoder
→ PCM 48 kHz Stereo
→ Source Arbiter
→ I2S
→ ADAU1701

## Hardware

### ESP32-WROVER

### ADAU1701

### I2S Belegung

GPIO0  -> MCLKI

GPIO23 -> MP5 (BCLK)

GPIO22 -> MP4 (LRCLK)

GPIO21 -> MP0 (SDATA_IN0)

GND    -> DGND

## Audio Format

- 48 kHz
- 16 Bit
- Stereo

## Verwendete Komponenten

- ESP-IDF 5.4.x
- ESP-Mesh-Lite 1.0.2
- esp-opus
- esp-iot-bridge

## Status

✅ Mesh-Lite funktioniert

✅ Snapcast funktioniert

✅ Opus Streaming funktioniert

✅ Opus Decoder integriert

✅ ADAU1701 empfängt Audiodaten

✅ A2DP Umschaltung funktioniert

✅ Automatischer Snapclient-Start nach GOT_IP

✅ Echte ESP32 STA-MAC im Snapcast Hello

## Bekannte Einschränkungen

- WLAN-Zugangsdaten müssen lokal konfiguriert werden
- ADAU1701 benötigt externen MCLK vom ESP32