# ESP32 Mesh-Lite Snapclient + A2DP → ADAU1701

Übungsprojekt: ESP32 empfängt Audio über **Snapcast** (via ESP-Mesh-Lite) **oder**
**Bluetooth A2DP**, gibt es als **I2S-Master** an einen **ADAU1701**-DSP aus.
Automatische Quellenumschaltung mit Priorität.

## Rahmenbedingungen (fixiert)

| Punkt | Entscheidung |
|---|---|
| SoC | **originaler ESP32-WROVER** (BT Classic für A2DP + PSRAM) |
| ESP-IDF | **v5.5.x** (neuer `i2s_std`-Treiber) |
| Sample-Rate | **48 kHz durchgängig** |
| I2S-Rolle | **ESP32 = Master**, ADAU1701 = Input-Port-Slave |
| Takt | ESP32 gibt **MCLK 12.288 MHz (256×fs)** aus, ADAU-PLL lockt darauf |
| Quellen | Snapcast **oder** A2DP (nie gleichzeitig streamend) |

## Taktkette @ 48 kHz

```
ESP32 APLL ──MCLK 12.288 MHz──► ADAU1701 MCLKI ──PLL×4──► Core 49.152 MHz
           ──BCLK  3.072 MHz──► (Input-Slave)
           ──LRCLK   48 kHz ──►
           ──SDATA         ──► SDATA_IN
```
ADAU1701: **PLLMODE0=GND, PLLMODE1=VDD** (256×fs-Modus). Quarz weglassen.

## Pinout (originaler ESP32)

| Signal | GPIO | Hinweis |
|---|---|---|
| MCLK | **GPIO0** | MCLK-Ausgabe nur auf GPIO0/1/3 möglich |
| BCLK | GPIO27 | |
| LRCLK/WS | GPIO25 | |
| SDATA_OUT | GPIO26 | → ADAU SDATA_IN |

## Build

```bash
idf.py set-target esp32
idf.py reconfigure         # zieht managed component espressif/mesh_lite
idf.py menuconfig          # Router-SSID/-PW, Snapserver optional
idf.py build flash monitor
```

## Projektstruktur

```
esp32_mesh_snapclient/
├── CMakeLists.txt
├── sdkconfig.defaults        # PSRAM, BTDM, Koexistenz, APLL-freundlich
├── partitions.csv
├── main/
│   ├── main.c                # Init-Reihenfolge
│   ├── net_mesh.*            # ESP-Mesh-Lite bring-up  (STUB)
│   ├── snapclient_glue.*     # Snapcast -> Arbiter     (STUB + Demo)
│   ├── a2dp_sink_glue.*      # A2DP -> Arbiter         (STUB)
│   └── idf_component.yml     # dependency: espressif/mesh_lite
└── components/
    ├── audio_i2s/            # I2S-Master + MCLK (fertig)
    └── source_arbiter/       # Priorität + Fade/Mute + Ringpuffer (fertig)
```

## Was fertig ist

Alle drei Kernmodule sind voll implementiert:

- **I2S-Master** (`audio_i2s`) — APLL, MCLK 12.288 MHz, 32-bit-Slots.
- **Source-Arbiter** (`source_arbiter`) — Prioritätsregel, Fade/Mute-Umschaltung,
  Ringpuffer, Player-Task auf Core 1, Pause-Callback-Kopplung.
- **Resampler** (`resampler`) — linear, 16.16-Fixpoint, 44.1k→48k.
- **Mesh-Lite** (`net_mesh.c`) — vollständige Startsequenz (`esp_bridge_create_all_netif`
  → `esp_mesh_lite_init` → Router-Config → `esp_mesh_lite_start`), Level-/Root-Logging.
- **A2DP-Sink** (`a2dp_sink_glue.c`) — BTDM-Controller + Bluedroid, A2DP + AVRC,
  SBC-Sample-Rate-Erkennung, Resampling→48k, Connection-State → Arbiter.
- **Snapclient** (`snapclient_glue.c`) — TCP zum Snapserver, Snapcast-Base-Message-
  Framing, Hello-Handshake, WireChunk/CodecHeader/ServerSettings, PCM→Arbiter,
  Pause/Resume für die Koexistenz.

### Verbleibende Integrations-Hooks (klar markiert im Code)

- `snapclient_glue.c` → **FLAC/OPUS-Decode**: PCM läuft direkt; für FLAC/OPUS die
  Decoder aus CarlosDerSeher/snapclient am `handle_codec_header`/`handle_wire_chunk`
  einhängen.
- `snapclient_glue.c` → **Zeitsync** (`SNAP_MSG_TIME`) für exaktes Multiroom
  optional auswerten.
- `a2dp_sink_glue.c` → **AVRC-Metadaten** (Titel/Play-Pause) optional nutzen.

## Kritische Punkte (nicht ignorieren)

1. **Koexistenz WLAN/BT:** ein Funkmodul, Time-Sharing. Bei A2DP-Verbindung
   muss der Arbiter den **Snapclient-Socket pausieren** (TODO in `switch_to()`),
   sonst Aussetzer. Deshalb streamt immer nur EINE Quelle.
2. **A2DP-Rate:** SBC liefert meist 44.1 kHz → **Resampling auf 48 kHz** nötig,
   da die I2S-Kette fix 48 kHz fährt.
3. **PSRAM zwingend:** WLAN/BT-Puffer in SPIRAM (siehe `sdkconfig.defaults`).
4. **Nur eine APLL:** nicht anderweitig belegen, sonst schlägt die I2S-Clock-
   Konfiguration fehl.

## Externe Referenzen

- Snapclient-Portierung: github.com/CarlosDerSeher/snapclient (IDF v5.x)
- ESP-Mesh-Lite: components.espressif.com/components/espressif/mesh_lite
- ADAU1701 Datasheet (PLL 64/256/384/512×fs), Analog Devices
- ESP-IDF I2S (i2s_std, APLL, mclk_multiple)
