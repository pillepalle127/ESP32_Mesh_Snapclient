# ESP32 Mesh Snapclient

ESP32-basierter Snapcast-Client fuer ein autonomes ESP-Mesh-Lite-Netz ohne externen Router. Der Client empfaengt Opus- oder PCM-Audio vom ESP32-S3-Snapserver und kann alternativ als Bluetooth-A2DP-Sink arbeiten. Die Quellenumschaltung erfolgt automatisch.

## Architektur

```text
ESP32-S3 Snapserver
  Mesh-Lite Root, Level 1
  SoftAP/Gateway 192.168.5.1
  Snapcast TCP 1704
           |
           | ESP-Mesh-Lite, No-Router-Modus
           v
ESP32-WROVER Snapclient
  Non-Root, Level 2 oder hoeher
  DHCP-Adresse vom Parent
           |
           +--> Snapcast Opus/PCM --+
           |                        |
           +--> Bluetooth A2DP -----+--> Source-Arbiter --> I2S
```

Der Snapclient darf nicht Root beziehungsweise Level 1 werden. Er sucht selbststaendig einen passenden Mesh-Lite-Parent mit identischer Mesh-ID und identischem Mesh-Passwort. Eine externe Router-SSID wird nicht verwendet.

## Funktionen

- ESP-Mesh-Lite 1.0.2 im autonomen No-Router-Betrieb
- ESP32-S3-Snapserver als einziger Root auf Level 1
- ESP32-Snapclient als Non-Root auf Level 2 oder hoeher
- Snapcast Protocol Version 2
- automatische Codec-Erkennung fuer Opus und PCM
- Opus-Dekodierung mit 48 kHz, 16 Bit und Stereo-Ausgabe
- Bluetooth-A2DP-Sink ueber Classic Bluetooth beziehungsweise BR/EDR
- SBC-Resampling auf 48 kHz
- automatische Quellenumschaltung zwischen Snapcast und A2DP
- unterbrechbarer, nichtblockierender TCP-Verbindungsaufbau
- Connect-Timeout von 1,5 Sekunden
- unmittelbare Reconnect-Freigabe nach erneutem Mesh-IP-Bezug
- sofortiger Socket-Abbruch bei Mesh-/IP-Verlust
- PSRAM-Nutzung fuer den Opus-PCM-Puffer

## Quellenprioritaet

```text
Aktiver A2DP-Audiostream
          |
          v
       Snapcast
          |
          v
        Stille
```

Eine reine Bluetooth-Verbindung schaltet die Quelle noch nicht um. Erst `ESP_A2D_AUDIO_STATE_STARTED` aktiviert A2DP. Dabei wird der Snapserver-Socket geschlossen. Nach Stopp, Pause oder Trennung des A2DP-Streams wird Snapcast automatisch wieder freigegeben und verbunden.

## Netzwerk und Reconnect

Der TCP-Snapclient wird nicht direkt in `app_main()` gestartet. `net_mesh.c` startet ihn erst nach `IP_EVENT_STA_GOT_IP`.

Bei einem Mesh-Abbruch gilt:

1. `WIFI_EVENT_STA_DISCONNECTED` meldet `NET DOWN` und unterbricht den laufenden Socket per `shutdown()`.
2. Solange keine gueltige Mesh-IP vorhanden ist, wird kein TCP-Verbindungsversuch gestartet.
3. Nach `IP_EVENT_STA_GOT_IP` meldet das Mesh `NET UP` und gibt den Reconnect sofort frei.
4. `connect()` arbeitet nichtblockierend und wird durch `select()` auf 1,5 Sekunden begrenzt.

Damit wird das zuvor beobachtete Blockieren von `connect()` ueber etwa 60 Sekunden vermieden.

## Projektkonfiguration

Die verbindlichen Projektwerte werden in `sdkconfig.defaults` gepflegt. Die vollstaendige `sdkconfig` wird von ESP-IDF erzeugt und sollte nicht manuell ausgeduennt werden.

```ini
CONFIG_IDF_TARGET="esp32"
CONFIG_MESH_SOFTAP_SSID_PREFIX="SnapMesh_sr"
CONFIG_MESH_SOFTAP_PASSWORD="<mesh-password>"
CONFIG_SNAPSERVER_HOST="192.168.5.1"
CONFIG_SNAPSERVER_PORT=1704
CONFIG_JOIN_MESH_WITHOUT_CONFIGURED_WIFI_INFO=y
CONFIG_JOIN_MESH_IGNORE_ROUTER_STATUS=y

CONFIG_BT_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BT_A2DP_ENABLE=y
CONFIG_BT_AVRCP_ENABLED=y
CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y
CONFIG_BTDM_CTRL_MODE_BTDM=n
CONFIG_BT_BLE_ENABLED=n
```

Passwoerter und andere projektspezifische Zugangsdaten sind vor einer oeffentlichen Veroeffentlichung zu ersetzen beziehungsweise aus dem Repository zu entfernen.

## Audio

- interne Verarbeitung: 48 kHz, 16 Bit, Stereo
- Snapcast: Opus oder PCM, automatische Erkennung aus dem CodecHeader
- A2DP: SBC-Eingang mit Resampling auf 48 kHz
- Ausgabe: gemeinsamer I2S-Masterpfad ueber den Source-Arbiter

Die verbindliche GPIO-Belegung wird im Modul `audio_i2s` gepflegt. Die README enthaelt bewusst keine separate Pin-Tabelle, damit Dokumentation und Firmware nicht auseinanderlaufen. Der aktuelle Laufzeitlog muss die tatsaechlich verwendeten I2S-Pins und den MCLK-Status ausgeben.

## Hardware

Empfohlen und getestet ist ein klassischer ESP32-WROVER mit PSRAM. Beim verwendeten Modul wurden 8 MB PSRAM erkannt; aufgrund des Adressraums des klassischen ESP32 werden davon 4 MB direkt in den Heap eingeblendet. Der Opus-Puffer wird bevorzugt im PSRAM angelegt.

Bluetooth A2DP benoetigt Bluetooth Classic. ESP32-S3-basierte Clients sind fuer diesen A2DP-Sink daher nicht geeignet; der ESP32-S3 wird in diesem Projekt als Snapserver und Mesh-Root eingesetzt.

## Verifizierter Stand vom 10. September 2026

Folgende Funktionen wurden im Laufzeitlog erfolgreich nachgewiesen:

- Mesh-Parent `SnapMesh_sr` gefunden
- Client als Level 2 verbunden
- DHCP-Adresse `192.168.5.2` mit Gateway `192.168.5.1` erhalten
- Snapserver-Verbindung auf `192.168.5.1:1704` aufgebaut
- Snapcast-Hello und Opus-CodecHeader verarbeitet
- Opus-Dekodierung ohne Decode-Fehler oder verworfene Bytes
- Umschaltung Snapcast zu A2DP bei Audiostart
- Pause des Snapserver-Sockets waehrend A2DP
- Rueckschaltung und Reconnect zu Snapcast nach A2DP-Ende
- Mesh-Reconnect nach WLAN-Abbruch
- TCP-Reconnect gegen lang blockierendes `connect()` ueberarbeitet

Der neue zeitlich begrenzte TCP-Reconnect muss noch gezielt durch wiederholte Server-, Root- und Funkunterbrechungen belastungsgetestet werden. Ein einzelner erfolgreicher Lauf gilt nicht als abschliessender Stabilitaetsnachweis.

## Bekannte Punkte

- Der WLAN-Abbruch mit `WIFI_REASON_SA_QUERY_TIMEOUT` beziehungsweise Reason 209 wurde beobachtet. Mesh-Lite stellte die WLAN- und DHCP-Verbindung wieder her.
- Der neue TCP-Reconnect verwendet einen Connect-Timeout von 1,5 Sekunden und 500 ms Retry-Abstand. Wiederholte Unterbrechungen sind noch zu testen.
- Das verwendete ESP32-Modul besitzt 8 MB Flash, während der Build aktuell 4 MB im Image-Header konfiguriert. Die vorhandene Partitionstabelle liegt innerhalb dieses Bereichs; die Flash-Konfiguration sollte bei Bedarf auf 8 MB vereinheitlicht werden.
- Die von ESP-IoT-Bridge beziehungsweise Mesh-Lite ausgegebenen Warnungen zu Kanal-Bitmaps und initial fehlenden NVS-Werten sind noch separat zu bewerten.

## Bauen und Flashen

ESP-IDF-Version: 5.4.3

```powershell
cd C:\Users\xxx\Documents\Elektronik\ESP\ESP32_Mesh_Snapclient
idf.py build
idf.py -p <CLIENT_COM_PORT> flash monitor
```

`<CLIENT_COM_PORT>` ist der tatsaechliche Port des klassischen ESP32-Snapclients. Der Port darf nicht mit dem Port des ESP32-S3-Snapservers verwechselt werden.
