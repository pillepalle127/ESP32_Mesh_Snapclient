# ESP32 Mesh-Lite Snapclient mit A2DP

ESP32-WROVER-basierter Audio-Client für Snapcast-Audio über ESP-Mesh-Lite sowie lokales Bluetooth-A2DP. Die Ausgabe erfolgt über I2S an einen PCM5102A oder ADAU1701.

## Funktionen

- Snapcast-Client mit Opus- und PCM-Wiedergabe
- ESP-Mesh-Lite für Root-/Child-Topologien
- lokaler Bluetooth-A2DP-Sink
- automatische Quellenumschaltung
- 48 kHz, 16 Bit, Stereo
- Ausgabe an PCM5102A oder ADAU1701
- eindeutige Namen aus den letzten vier Stellen der WLAN-STA-MAC

## Quellenpriorität

```text
A2DP-Audiostream aktiv
        ↓
      A2DP
        ↓
kein A2DP-Audiostream
        ↓
    Snapcast
        ↓
keine Netzwerkquelle
        ↓
      Stille
```

Eine reine Bluetooth-Verbindung schaltet die Quelle nicht um. Erst `ESP_A2D_AUDIO_STATE_STARTED` aktiviert A2DP. Beim Stoppen, Pausieren oder Trennen wird der Snapclient wieder freigegeben und verbindet sich erneut mit dem Snapserver.

## Audiopfad

```text
Snapserver -> Opus -> ESP32 Opus-Decoder --+
                                             +-> Source-Arbiter -> I2S -> DAC/DSP
Smartphone -> A2DP/SBC -> Resampler -------+
```

A2DP liefert üblicherweise 44,1 kHz. Der ESP32 resampelt den dekodierten SBC-PCM-Strom auf die feste I2S-Samplerate von 48 kHz.

## Hardware

### Empfohlen

- klassischer ESP32-WROVER
- Bluetooth Classic für A2DP
- PSRAM für Mesh-Lite, Netzwerk, Bluetooth und Opus
- ESP-IDF 5.4.3 als getesteter Stand

ESP32-WROOM ohne PSRAM ist für die vollständige Kombination aus Mesh-Lite, Opus und A2DP nicht vorgesehen.

## Gemeinsames I2S-Pinning

| Signal | ESP32 GPIO |
|---|---:|
| BCLK | GPIO27 |
| LRCLK / WS | GPIO25 |
| SDATA_OUT | GPIO26 |

## PCM5102A

| ESP32 | PCM5102A |
|---|---|
| GPIO27 | BCK |
| GPIO25 | LCK / LRCK |
| GPIO26 | DIN |
| 3,3 V bzw. passende Modulversorgung | VCC |
| GND | GND |

**Der getestete PCM5102A-Aufbau benötigt keine MCLK-Verbindung. Der SCK/MCLK-Pin bleibt unbeschaltet.**

## ADAU1701

| ESP32 | ADAU1701 |
|---|---|
| GPIO0 | MCLKI |
| GPIO27 | BCLK |
| GPIO25 | LRCLK |
| GPIO26 | SDATA_IN |
| GND | GND |

Der ESP32 arbeitet als I2S-Master und erzeugt für den ADAU1701 12,288 MHz MCLK bei 48 kHz. Die ADAU1701-PLL- und SigmaStudio-Konfiguration muss zum verwendeten Hardwareaufbau passen.

## Bluetooth

Der Gerätename folgt dem Schema:

```text
Snap-Blth-XXXX
```

Beispiel:

```text
Snap-Blth-8844
```

`XXXX` entspricht den letzten beiden Bytes der WLAN-STA-MAC.

## Snapcast

Der Client sendet seine WLAN-STA-MAC als Client-ID und erkennt den vom Server gelieferten CodecHeader. Unterstützt werden derzeit:

- Opus, 48 kHz, Stereo
- PCM-Direktpfad

Der Snap-Task läuft auf CPU 1 unterhalb der Priorität des I2S-Player-Tasks. Netzwerk-, Mesh- und Bluetooth-Systemaufgaben verbleiben überwiegend auf CPU 0.

## Build

ESP-IDF-Terminal öffnen und im Projektverzeichnis ausführen:

```cmd
idf.py set-target esp32
idf.py reconfigure
idf.py build
idf.py -p COM5 flash monitor
```

Den COM-Port an die lokale Umgebung anpassen.

## Konfiguration

Routerdaten, Snapserver-Adresse und weitere Projektparameter werden über `menuconfig` bzw. die Projektkonfiguration gesetzt:

```cmd
idf.py menuconfig
```

Zugangsdaten dürfen nicht in Logs, Commits oder NVS-Abbildern veröffentlicht werden.

## Projektstruktur

```text
components/
  audio_i2s/          I2S-Master und Ausgabe
  resampler/          Sample-Rate-Konvertierung
  source_arbiter/     Quellenwahl und Audiopuffer
main/
  main.c              Initialisierung
  net_mesh.c          Mesh-Lite und Netzwerk
  snapclient_glue.c   Snapcast, Opus und Reconnect
  a2dp_sink_glue.c    Bluetooth-A2DP
```

## Bekannte Einschränkungen

- Der klassische ESP32 teilt ein 2,4-GHz-Funkmodul zwischen WLAN und Bluetooth.
- Schwacher WLAN-RSSI, WPA3/PMF und 40-MHz-Kanalbreite können den Verbindungsaufbau erschweren.
- Mesh-Lite-Scans können kurzfristig Funkzeit beanspruchen.
- Ein vollständiger serverzeitbasierter Snapcast-Zeitsync ist noch nicht implementiert.
- Das Projekt unterstützt aktuell keine Mikrofon-, Paging- oder AEC-Funktion.

## Testkriterien

Nach Änderungen mindestens prüfen:

1. Snapcast startet und spielt mindestens zwei Minuten ohne Watchdog.
2. `decode_errors=0` und im stabilen Betrieb `dropped=0`.
3. A2DP startet erst bei aktivem Audiostream.
4. Wechsel Snapcast -> A2DP -> Snapcast mindestens zehnmal wiederholen.
5. Bluetooth trennen, während A2DP spielt.
6. Snapserver während aktiver A2DP-Wiedergabe stoppen und neu starten.
7. WLAN-Reconnect prüfen.
8. PCM5102A und ADAU1701 getrennt testen.
