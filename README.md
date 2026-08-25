# ESP32 Mesh Snapclient with Opus and ADAU1701

ESP32-basierter Snapcast-Client mit ESP-Mesh-Lite, Opus-Decodierung, Bluetooth A2DP Sink und ADAU1701 Audio-DSP.

Der ESP32 empfängt Audiostreams vom Snapserver über WLAN bzw. ESP-Mesh-Lite, dekodiert Opus direkt auf dem Mikrocontroller und gibt das resultierende PCM-Signal über I2S an einen ADAU1701 DSP aus.

Zusätzlich kann das Gerät als Bluetooth-Lautsprecher (A2DP Sink) betrieben werden. Die Umschaltung zwischen Snapcast und Bluetooth erfolgt automatisch über einen Source-Arbiter.

---

# Branches

- `main` – stabile Basis
- `feature/opus_ADAU` – Opus + ADAU1701
- `feature/opus_PCM5102` – Opus + PCM5102A

---

# Features

- Snapcast Client (Protocol Version 2)
- Opus Decoder (78/esp-opus)
- PCM Unterstützung
- ESP-Mesh-Lite
- Bluetooth A2DP Sink
- ADAU1701 Audio DSP
- SigmaStudio Live Download
- Automatische Quellenumschaltung (Snapcast ↔ A2DP)
- Source Arbiter
- ESP32 als I2S Master
- 48 kHz Stereo Audio Pipeline
- WLAN STA + SoftAP
- PSRAM Unterstützung
- Automatischer Snapclient-Start nach `IP_EVENT_STA_GOT_IP`
- Reale ESP32 STA MAC als Snapcast Client-ID
- Eindeutige Gerätenamen über MAC-Suffix

---

# Systemarchitektur

```text
Snapserver
     |
     v
   Opus
     |
     v

ESP-Mesh-Lite
     |
     v

+----------------------+
| ESP32                |
|                      |
| Snapclient           |
|     |                |
|     v                |
| Opus Decoder         |
|     |                |
|     v                |
| PCM 48k Stereo       |
|     |                |
|     v                |
| Source Arbiter       |
+-----+-----------+----+
      |           |
      |           |
      |           +------ A2DP Sink
      |
      v

I2S Master
      |
      v

ADAU1701
      |
      v

DAC
      |
      v

Verstärker
      |
      v

Lautsprecher
```

---

# Unterstützte Audioformate

## Snapserver → Opus

```text
48 kHz
16 Bit
Stereo
Opus
```

## Interne Audioverarbeitung

```text
48 kHz
16 Bit
Stereo PCM
```

---

# Hardware

## ESP32

Getestet mit:

```text
ESP32-WROVER
ESP32-D0WD-V3
240 MHz
4 MB Flash
8 MB physisches PSRAM
```

Bootlog:

```text
Detected flash size: 4MB
Found 8MB PSRAM device
4MB is mapped
```

---

## Audio DSP

```text
ADAU1701
```

Features:

```text
I2S Slave
DSP
EQ
Limiter
Crossovers
Mixer
SigmaStudio Support
```

---

# ESP32 ↔ ADAU1701 Verdrahtung

## Hardware-Verkabelung

| Funktion | ESP32 | ADAU1701 | Beschreibung |
|----------|--------|----------|--------------|
| MCLK | GPIO0 | MCLKI | Master Clock |
| BCLK | GPIO23 | MP5 | Input Bit Clock |
| LRCLK | GPIO22 | MP4 | Input LR Clock |
| SDATA | GPIO21 | MP0 | SDATA_IN0 |
| GND | GND | DGND | Masse |

---

## Übersicht

```text
ESP32                       ADAU1701
-----                       --------

GPIO0   MCLK ----------->   MCLKI

GPIO23  BCLK ----------->   MP5

GPIO22  LRCLK ---------->   MP4

GPIO21  SDATA ---------->   MP0 (SDATA_IN0)

GND -------------------->   DGND
```

---

# Clocking

## Audioformat

```text
48 kHz
16 Bit
Stereo
```

## Taktraten

```text
LRCLK = 48.000 Hz

BCLK  = 3.072 MHz

MCLK  = 12.288 MHz
```

---

# ADAU1701 Konfiguration

## PLL

```text
PLLMODE0 = GND
PLLMODE1 = VDD
```

---

## Clock Source

```text
External MCLK
12.288 MHz
```

---

## MP-Konfiguration

```text
MP0 = SDATA_IN0

MP4 = INPUT_LRCLK

MP5 = INPUT_BCLK
```

---

# SigmaStudio

Dieses Projekt nutzt aktuell:

```text
SigmaStudio Live Download
```

Es wird kein Selfboot-EEPROM benötigt.

---

## Serial Input

```text
I2S
Slave
48 kHz
Stereo
```

---

## Minimales Audio Routing

```text
SDATA_IN0 Left
        |
        v
      DAC0

SDATA_IN0 Right
        |
        v
      DAC1
```

---

## Typisches Routing

```text
Input Left
     |
 Volume
     |
    EQ
     |
 Limiter
     |
   DAC0

Input Right
     |
 Volume
     |
    EQ
     |
 Limiter
     |
   DAC1
```

---

# Snapserver Konfiguration

Beispiel:

```ini
stream = pipe:///tmp/snapfifo?name=Mopidy&sampleformat=48000:16:2&codec=opus&chunk_ms=20

sampleformat = 48000:16:2
codec = opus
chunk_ms = 20
buffer = 500
```

Neustart:

```bash
sudo systemctl restart snapserver
```

Status:

```bash
sudo systemctl status snapserver
```

Logs:

```bash
sudo journalctl -u snapserver -f
```

---

# Netzwerk

## ESP-Mesh-Lite

Unterstützt:

```text
Root Node

Child Node

Automatische Topologie

Selbstheilung

Eigene IP-Adresse je Node

Eigener TCP/IP Stack je Node
```

---

# Snapclient Identität

Die Snapcast-ID wird automatisch aus der STA-MAC erzeugt.

Beispiel:

```text
STA MAC

A8:42:E3:AE:88:44
```

Snapcast:

```text
Client Name

ESP32-SnapMesh-8844
```

```text
Client ID

A8:42:E3:AE:88:44
```

Der gleiche Build kann auf beliebig viele ESP32 geflasht werden.

Jeder Client erscheint separat im Snapserver.

---

# Build

Abhängigkeiten laden:

```bash
idf.py reconfigure
```

Build:

```bash
idf.py build
```

---

# Flashen

```bash
idf.py -p COM5 flash
```

---

# Monitor

```bash
idf.py -p COM5 monitor
```

Monitor verlassen:

```text
Ctrl + ]
```

---

# Erwartete Logmeldungen

## WLAN

```text
GOT IP: 192.168.x.x
```

---

## Snapclient

```text
Snapclient gestartet
```

```text
verbunden mit Snapserver
```

---

## Opus

```text
CodecHeader: codec=opus
```

```text
Opus-Decoder bereit
```

---

## Erfolgreiche Wiedergabe

```text
Stream stats/5s:

codec=opus

decoded≈960000 B

arbiter≈960000 B

dropped=0

decode_errors=0
```

---

## I2S

```text
audio_i2s:

written≈960000 B

errors=0
```

---

# Bluetooth

Bluetooth Name:

```text
SnapMesh-Speaker
```

---

# Source Arbiter

Priorität:

```text
Bluetooth aktiv
       ↓
Snapcast pausieren

Bluetooth beendet
       ↓
Snapcast automatisch reconnecten
```

---

# Troubleshooting

## Kein Ton

Prüfen:

```text
CodecHeader empfangen

Opus-Decoder bereit

decoded > 0

arbiter > 0

I2S written > 0
```

---

## Keine Verbindung zum Snapserver

Prüfen:

```text
IP-Adresse vorhanden

Server erreichbar

Port 1704 erreichbar

Snapserver läuft
```

---

## ADAU zeigt keinen Pegel

Prüfen:

```text
MCLK vorhanden

BCLK vorhanden

LRCLK vorhanden

SDATA vorhanden

SigmaStudio Routing korrekt

PLL richtig konfiguriert
```

---

# Speicher

Opus PCM Buffer:

```text
23040 Byte
```

PSRAM wird verwendet, ist aber nicht ausschließlich für diesen Puffer erforderlich.

---

# Release Historie

## v1.0

- Mesh-Lite
- ADAU1701
- A2DP
- PCM Audio

## v1.1

- Opus Decoder
- Automatischer Start nach GOT_IP
- Echte STA-MAC
- Eindeutige Clientnamen
- Deutlich reduzierte Netzwerkbandbreite
- Vollständige Opus-Wiedergabe

---

# Commit und Push

README übernehmen:

```powershell
git add README.md
git commit -m "Update ADAU1701 README"
git push
```

Branch kontrollieren:

```powershell
git branch --show-current
```

Erwartet:

```text
feature/opus_ADAU
```