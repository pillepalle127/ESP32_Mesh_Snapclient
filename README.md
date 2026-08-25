# ESP32 Mesh Snapclient with Opus and PCM5102A

ESP32-based Snapcast client for bandwidth-efficient multiroom audio using ESP-Mesh-Lite, Opus decoding, Bluetooth A2DP, automatic source arbitration, and direct I2S output to a PCM5102/PCM5102A stereo DAC.

This branch replaces the ADAU1701 used by `feature/opus_ADAU` with a PCM5102A. SigmaStudio is not required.

## Branches

- `main`: stable project base
- `feature/opus_ADAU`: validated Opus output through ADAU1701
- `feature/opus_PCM5102`: Opus output through PCM5102A

## Features

- Snapcast protocol version 2 client
- Opus decoding through `78/esp-opus`
- PCM stream support
- ESP-Mesh-Lite 1.0.2
- Wi-Fi STA and SoftAP operation
- Mesh root and child operation
- Bluetooth A2DP sink
- Automatic Snapcast/A2DP source arbitration
- ESP32 I2S master
- 48 kHz, 16-bit, stereo audio
- Real STA MAC as Snapcast client ID
- Unique names such as `ESP32-SnapMesh-8844`
- Snapclient start only after `IP_EVENT_STA_GOT_IP`
- Automatic reconnect after network or server interruption

## Architecture

```text
Audio source
    |
    v
Snapserver, codec=opus
    |
    v
Wi-Fi / ESP-Mesh-Lite
    |
    v
ESP32 Snapclient
    |
    v
Opus decoder
    |
    v
PCM 48 kHz / 16 bit / stereo
    |
    v
Source arbiter <---- Bluetooth A2DP
    |
    v
ESP32 I2S master
    |
    v
PCM5102A stereo DAC
    |
    v
Amplifier or active speakers
```

## Hardware

### Required

- ESP32 with classic Bluetooth support
- PCM5102 or PCM5102A DAC module
- Suitable power supply for the selected module
- Common ground between ESP32, DAC, and amplifier
- Stereo amplifier or active speakers
- Snapserver reachable through the network

### Tested ESP32

```text
Chip: ESP32-D0WD-V3 revision 3.1
CPU: dual core, 240 MHz
Flash: 4 MB
Physical PSRAM: 8 MB
Mapped PSRAM: 4 MB
Crystal: 40 MHz
```

The firmware does not require 8 MB of mapped PSRAM. The Opus PCM buffer is 23,040 bytes. Minimum memory requirements for other boards have not yet been determined.

## ESP32 to PCM5102A wiring

The ESP32 is the I2S master. PCM5102A module labels vary, so verify the module schematic before connecting it.

| Function | ESP32 | PCM5102A counterpart | Direction |
|---|---:|---|---|
| BCLK | GPIO23 | `BCK`, `BCLK` | ESP32 to DAC |
| LRCLK | GPIO22 | `LCK`, `LRCK`, `WS` | ESP32 to DAC |
| Serial data | GPIO21 | `DIN`, `DATA`, `SDIN` | ESP32 to DAC |
| Ground | GND | `GND` | Common |
| MCLK, optional | GPIO0 | `SCK` or `MCLK`, if required | ESP32 to DAC |

Typical overview:

```text
ESP32                              PCM5102A
GPIO23  BCLK --------------------> BCK / BCLK
GPIO22  LRCLK -------------------> LCK / LRCK / WS
GPIO21  SDATA -------------------> DIN / DATA
GPIO0   MCLK --------------------> optional SCK/MCLK
GND     GND  --------------------> GND
module supply -------------------> VIN/VCC/3V3 as specified

PCM5102A LOUT -------------------> amplifier left input
PCM5102A ROUT -------------------> amplifier right input
PCM5102A GND  -------------------> amplifier signal ground
```

Many PCM5102A modules recover their internal system clock from BCLK and do not require MCLK. Leave GPIO0 disconnected unless the selected module explicitly requires an external system clock.

Do not connect passive loudspeakers directly to the PCM5102A line outputs. Use an amplifier or active speakers.

## PCM5102A control pins

Some modules expose additional pins:

- `FMT`: select standard I2S format
- `XSMT`: must permit normal operation and must not hold the DAC muted
- `FLT`: digital-filter selection, normally the module default is sufficient
- `DEMP`: de-emphasis, normally disabled for this project

Many breakout boards already contain suitable pull resistors. Follow the documentation for the exact board.

## I2S format

```text
ESP32 role: master
PCM5102A role: slave
Protocol: Philips I2S
Sample rate: 48,000 Hz
Sample depth: signed 16-bit
Channels: two, interleaved stereo
LRCLK: 48 kHz
MCLK: optional, depending on module
```

The PCM5102A branch must validate the required I2S slot width for the selected module. Keep the working network and Opus path unchanged during that test.

## Snapserver configuration

Example `/etc/snapserver.conf`:

```ini
[stream]
stream = pipe:///tmp/snapfifo?name=Mopidy&sampleformat=48000:16:2&codec=opus&chunk_ms=20
sampleformat = 48000:16:2
codec = opus
chunk_ms = 20
buffer = 500
```

Avoid conflicting values between URI parameters and default settings.

Restart and inspect Snapserver:

```bash
sudo systemctl restart snapserver
sudo systemctl status snapserver --no-pager
sudo journalctl -u snapserver -f
```

## Observed bandwidth

```text
Opus transport: about 120,000 bytes per 5 seconds
Decoded PCM: about 960,000 bytes per 5 seconds
Transport reduction versus PCM: about 87.5 percent
```

Actual Opus bitrate depends on the signal and encoder settings.

## Software requirements

- ESP-IDF 5.4.x
- Git
- ESP32 USB serial driver
- Snapserver with Opus support

Main ESP-IDF components:

- `espressif/mesh_lite` 1.0.2
- `espressif/iot_bridge`
- `78/esp-opus`
- ESP-IDF Bluetooth A2DP stack
- ESP-IDF standard I2S driver

## Clone and select this branch

```bash
git clone https://github.com/pillepalle127/ESP32_Mesh_Snapclient.git
cd ESP32_Mesh_Snapclient
git switch feature/opus_PCM5102
```

## Resolve dependencies

```bash
idf.py reconfigure
```

## Local configuration

The local `sdkconfig` is excluded from Git because it can contain credentials and board-specific settings.

```bash
idf.py menuconfig
```

Configure at least:

- Router SSID and password
- Snapserver address
- Snapserver port, normally 1704
- Mesh-Lite parameters
- PSRAM parameters for the selected ESP32

Recommended `.gitignore` entries:

```gitignore
build/
sdkconfig
sdkconfig.old
.vscode/
.idea/
*.log
__pycache__/
```

## Build

```bash
idf.py build
```

## Find the serial port on Windows

```powershell
Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Description, Manufacturer
```

## Flash and monitor

```bash
idf.py -p COM5 flash monitor
```

Exit the monitor with `Ctrl+]`.

## Startup sequence

```text
1. Initialize I2S and continuous clocks
2. Start the source arbiter
3. Initialize Wi-Fi STA and SoftAP
4. Start ESP-Mesh-Lite
5. Obtain an IP address
6. Start Snapclient after IP_EVENT_STA_GOT_IP
7. Connect to Snapserver port 1704
8. Send Hello with the real STA MAC
9. Receive the Opus CodecHeader
10. Initialize the Opus decoder
11. Decode WireChunks to PCM
12. Feed PCM to the source arbiter
13. Transmit PCM over I2S to PCM5102A
```

## Unique client names

Example:

```text
STA MAC: A8:42:E3:AE:88:44
Client name: ESP32-SnapMesh-8844
Client ID: A8:42:E3:AE:88:44
```

The same firmware can be flashed to multiple ESP32 devices. Every device receives a unique client ID and readable name from its factory STA MAC.

## Expected logs

Network:

```text
GOT IP: 192.168.x.x (Uplink steht)
Snapclient nach GOT IP gestartet
```

Snapcast and Opus:

```text
verbunden mit Snapserver 192.168.x.x:1704
Snapcast-Hello: Client=ESP32-SnapMesh-8844, ID=A8:42:E3:AE:88:44
CodecHeader: codec=opus
Opus-Decoder bereit: 48000 Hz, 2 Kanaele, PCM-Puffer=23040 B
```

Healthy stream:

```text
Stream stats/5s: codec=opus packets=250 wire=120000 B decoded_frames=250 decoded=960000 B arbiter=960000 B dropped=0 B decode_errors=0 src=1
```

Healthy I2S path:

```text
I2S stats/5s: written=960000 B, errors=0, muted=0
```

Small deviations are normal because logging windows and packet boundaries are not perfectly aligned.

## Bluetooth A2DP

Default advertised name:

```text
SnapMesh-Speaker
```

Source behavior:

```text
A2DP active -> arbiter selects A2DP -> Snapcast pauses
A2DP stopped -> Snapcast reconnects automatically
```

Wi-Fi and Bluetooth share the 2.4 GHz radio. Validate simultaneous Mesh-Lite, SoftAP, ESP-NOW, Snapcast, and A2DP operation under realistic conditions.

## First PCM5102A test

1. Confirm branch `feature/opus_PCM5102`.
2. Connect BCLK, LRCLK, SDATA, and common ground.
3. Power the module according to its documentation.
4. Leave MCLK open unless required.
5. Connect line outputs to an amplifier or active speakers.
6. Build and flash.
7. Confirm `CodecHeader: codec=opus`.
8. Confirm `decode_errors=0` and `dropped=0`.
9. Confirm BCLK, LRCLK, and SDATA with an oscilloscope if needed.
10. Confirm that `XSMT` or another control pin is not muting the DAC.

## Troubleshooting

### No Snapserver connection

Check IP acquisition, Snapserver host, port 1704, service status, firewall, and client group assignment.

### Opus received but no decoded output

`wire` increases while `decoded` remains zero. Verify `78/esp-opus`, decoder initialization, and `decode_errors`.

### Decoded audio but no analog output

Check GPIO23 to BCLK, GPIO22 to LRCLK, GPIO21 to DIN, common ground, supply voltage, `XSMT`, amplifier connection, and I2S slot compatibility.

### Distorted audio or wrong speed

Confirm 48 kHz throughout, `48000:16:2` on Snapserver, standard I2S format, correct clocks, and disabled de-emphasis unless explicitly required.

### Bluetooth packet-drop warnings

Messages such as `BT_APPL: Pkt dropped` concern the A2DP path. Check Snapcast statistics separately before attributing them to Opus or Mesh-Lite.

## Security and Git hygiene

Check whether local configuration files are tracked:

```powershell
git ls-files sdkconfig sdkconfig.old
```

Check for a known secret in current files and reachable history:

```powershell
git grep "YOUR_SECRET"
git log -p --all | Select-String "YOUR_SECRET"
```

## Commit and push this README

Confirm the correct branch:

```powershell
git branch --show-current
```

Expected:

```text
feature/opus_PCM5102
```

Stage, commit, and push:

```powershell
git add README.md
git commit -m "Document PCM5102A Opus branch"
git push -u origin feature/opus_PCM5102
```

After the upstream is configured, later pushes require only:

```powershell
git push
```

## Release criteria

- Opus decoder initializes successfully
- `decode_errors=0`
- `dropped=0`
- Decoded PCM remains near 960,000 bytes per five seconds
- I2S errors remain zero
- Audible stereo output exists at the PCM5102A line outputs
- A2DP source switching works
- Snapcast reconnects after A2DP
- Multiple ESP32 clients have unique names
- Dynamic movement and RF obstruction tests remain stable

## Known limitations

- Current audio format is 48 kHz stereo.
- PCM5102A module labels and supply requirements vary.
- Output is line level and needs amplification.
- MCLK requirement depends on the module.
- Dynamic RF performance requires application-specific validation.
- Exact sample-time synchronization across multiple clients requires further validation.

## License

Add or confirm the project license before wider redistribution. Retain all third-party notices, including the license files of `78/esp-opus`.

## Acknowledgements

- Espressif ESP-IDF
- Espressif ESP-Mesh-Lite
- Espressif IoT Bridge
- Xiph.Org Opus and `78/esp-opus`
- Snapcast and Snapserver
