# ESP32 Mesh Snapclient

Multiroom-Audio-Client auf Basis eines ESP32-WROVER mit ESP-Mesh-Lite, Snapcast, Opus-Decodierung, Bluetooth A2DP Sink und ADAU1701 Audio-DSP.

Das Projekt ermöglicht die Wiedergabe von Snapcast-Audiostreams auf einem ESP32. Die Audiodaten werden über WLAN oder ESP-Mesh-Lite empfangen, auf dem ESP32 dekodiert und über I2S an einen ADAU1701 DSP ausgegeben.

Zusätzlich kann das Gerät als Bluetooth-Audioempfänger (A2DP Sink) betrieben werden. Die Umschaltung zwischen Snapcast und Bluetooth erfolgt automatisch über einen Source-Arbiter.

---

# Features

- Snapcast Client (Protocol Version 2)
- Opus Decoder auf dem ESP32
- ESP-Mesh-Lite Unterstützung
- Bluetooth A2DP Sink
- ADAU1701 DSP Audioausgabe
- Automatische Quellenumschaltung
- ESP32 als I2S-Master
- 48 kHz Stereo Audio Pipeline
- PSRAM-Unterstützung
- Automatischer Start nach erfolgreicher WLAN-Verbindung
- Reale ESP32 STA-MAC im Snapcast Hello
- Opus-Wiedergabe erfolgreich getestet

---

# Systemarchitektur

```text
                        Snapserver
                             |
                             |
                           Opus
                             |
                             v

                     +---------------+
                     |   Mesh-Lite   |
                     +-------+-------+
                             |
                             v

                     +---------------+
                     |    ESP32      |
                     |  Snapclient   |
                     +-------+-------+
                             |
                       Opus Decoder
                             |
                             v

                     +---------------+
                     | Source Arbiter|
                     +-------+-------+
                             |
                 +-----------+-----------+
                 |                       |
                 v                       v

            Snapcast                A2DP Sink

                             |
                             v

                     +---------------+
                     |      I2S      |
                     +-------+-------+
                             |
                             v

                     +---------------+
                     |   ADAU1701    |
                     +-------+-------+
                             |
                             v

                         Audio Out
```

---

# Hardware

## Unterstützte Plattform

Getestet mit:

```text
ESP32-WROVER
8 MB PSRAM
ESP-IDF 5.4.x
```

Andere ESP32-Varianten können funktionieren, wurden jedoch nicht getestet.

---

# Benötigte Hardware

## Pflichtkomponenten

- ESP32-WROVER
- ADAU1701 DSP Board
- Lautsprecherverstärker
- Lautsprecher
- WLAN-Infrastruktur
- Snapserver

## Optional

- Bluetooth Audioquelle
- ESP-Mesh-Lite weitere Nodes

---

# Verdrahtung

## ESP32 → ADAU1701

```text
ESP32                     ADAU1701

GPIO0      MCLK      ---> MCLKI

GPIO23     BCLK      ---> MP5
GPIO22     LRCLK     ---> MP4
GPIO21     SDATA     ---> MP0

GND                  ---> DGND
```

---

# I2S Konfiguration

Der ESP32 arbeitet als I2S-Master.

```text
Sample Rate : 48 kHz
MCLK        : 12.288 MHz
BCLK        : 3.072 MHz
LRCLK       : 48 kHz
```

Audioformat:

```text
48 kHz
16 Bit
Stereo
```

---

# ADAU1701 Konfiguration

## PLL

```text
PLLMODE0 = GND
PLLMODE1 = VDD
```

## Clock Source

Der ADAU1701 erhält seinen Master Clock direkt vom ESP32.

```text
MCLK = 12.288 MHz
```

---

# SigmaStudio

## Verwendeter Betriebsmodus

```text
I2S Slave
48 kHz
Stereo
```

## Serial Input

```text
MP0 = SDATA_IN0
MP4 = LRCLK
MP5 = BCLK
```

## Minimales Routing

```text
Input Left  -> DAC Left
Input Right -> DAC Right
```

Der erste Funktionstest sollte immer ohne EQ, Limiter oder zusätzliche DSP-Blöcke erfolgen.

---

# ESP-IDF Installation

Projekt basiert auf:

```text
ESP-IDF 5.4.x
```

Beispiel:

```bash
C:\esp\v5.4.3
```

Espressif Installation Guide:

https://docs.espressif.com/projects/esp-idf

---

# Repository klonen

```bash
git clone https://github.com/pillepalle127/ESP32_Mesh_Snapclient.git
cd ESP32_Mesh_Snapclient
```

---

# Abhängigkeiten herunterladen

```bash
idf.py reconfigure
```

Dabei werden benötigte Komponenten automatisch geladen.

Unter anderem:

```text
espressif/mesh_lite
78/esp-opus
```

---

# WLAN konfigurieren

Vor dem Build müssen die lokalen WLAN-Daten eingetragen werden.

Nicht im Repository enthalten:

```text
CONFIG_ROUTER_SSID
CONFIG_ROUTER_PASSWORD
```

Konfiguration über:

```bash
idf.py menuconfig
```

oder direkt über die lokale sdkconfig.

---

# Build

```bash
idf.py build
```

---

# Flashen

```bash
idf.py -p COM5 flash
```

Mit Monitor:

```bash
idf.py -p COM5 flash monitor
```

---

# Betriebsablauf

Nach dem Einschalten:

```text
1. WLAN verbinden
2. Mesh-Lite starten
3. IP-Adresse beziehen
4. Snapclient starten
5. Verbindung zum Snapserver herstellen
6. Codec erkennen
7. Opus dekodieren
8. Audio über I2S ausgeben
```

---

# Erwartete Logmeldungen

## WLAN verbunden

```text
GOT IP: 192.168.x.x
```

## Snapserver verbunden

```text
verbunden mit Snapserver
```

## Codec erkannt

```text
CodecHeader: codec=opus
```

## Decoder aktiv

```text
Opus-Decoder bereit
```

## Erfolgreiche Wiedergabe

```text
Stream stats/5s:
codec=opus
decoded=960000 B
arbiter=960000 B
dropped=0 B
decode_errors=0
```

---

# Snapserver Konfiguration

Beispiel:

```ini
stream = pipe:///tmp/snapfifo?name=Mopidy

sampleformat = 48000:16:2
codec = opus
chunk_ms = 20
buffer = 200
```

---

# Bluetooth Betrieb

Der ESP32 kann zusätzlich als Bluetooth-Lautsprecher verwendet werden.

Bluetooth Name:

```text
SnapMesh-Speaker
```

---

# Source Arbiter

Priorisierung:

```text
Bluetooth aktiv
    ↓
Snapcast pausiert

Bluetooth beendet
    ↓
Snapcast verbindet erneut
```

Dadurch wird immer nur eine Quelle wiedergegeben.

---

# ESP-Mesh-Lite

Das Projekt unterstützt:

```text
Root Node
Child Node
Automatische Topologie
Selbstheilung
```

Jeder Knoten besitzt einen eigenen TCP/IP Stack und eine eigene IP-Adresse.

---

# Speicherbedarf

Typischer Betrieb:

```text
ESP32-WROVER
8 MB PSRAM
```

Opus-PCM-Puffer:

```text
23040 Byte
```

---

# Bekannte Einschränkungen

- Aktuell auf 48 kHz Stereo ausgelegt.
- ADAU1701 benötigt externen MCLK vom ESP32.
- WLAN-Zugangsdaten müssen lokal konfiguriert werden.
- SigmaStudio-Projekt ist nicht Bestandteil des Repositories.
- Opus ist der primär getestete Codec.

---

# Verifizierte Funktionen

## Netzwerk

- ✅ WLAN STA
- ✅ SoftAP
- ✅ ESP-Mesh-Lite
- ✅ Root Node
- ✅ Child Node

## Snapcast

- ✅ Verbindung zum Snapserver
- ✅ Snapcast Protocol Version 2
- ✅ Reconnect
- ✅ Opus Streaming

## Audio

- ✅ Opus Decoder
- ✅ PCM Pipeline
- ✅ Source Arbiter
- ✅ A2DP Umschaltung
- ✅ I2S Ausgabe
- ✅ ADAU1701 Eingang

## System

- ✅ PSRAM Nutzung
- ✅ Automatischer Start nach GOT_IP
- ✅ Reale STA-MAC im Snapcast Hello

---

# Release Historie

## v1.0

- Mesh-Lite
- Snapcast PCM
- ADAU1701
- A2DP

## v1.1

- Opus Decoder
- Automatischer Start nach GOT_IP
- Reale ESP32 STA-MAC
- Reduzierte Netzwerkbandbreite
- Vollständige Opus-Wiedergabe
