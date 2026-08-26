# Stabilitätspatch: Testhinweise

## Geänderte Bereiche

- A2DP-Datencallback puffert nur noch und blockiert nicht.
- Resampling läuft in einem eigenen Task auf CPU 1.
- Arbiter-Feed ist nicht blockierend und gegen den Quellenwechsel serialisiert.
- Snapclient verwendet Socket-Timeouts und schließt den Socket nur im Snap-Task.
- Opus-Decoder und PCM-Puffer bleiben über Reconnects erhalten.
- Pauschales `vTaskDelay(1)` nach jeder Nachricht wurde durch kontrolliertes `taskYIELD()` ersetzt.

## Erwartete Logs

Snapcast:

```text
arbiter: switch 0 -> 1
snap: CodecHeader: codec=opus
snap: Opus-Decoder bereit
```

A2DP startet:

```text
a2dp: A2DP audio state=1
arbiter: switch 1 -> 2
snap: PAUSE: A2DP aktiv
```

A2DP stoppt:

```text
a2dp: A2DP audio state=0
arbiter: switch 2 -> 0
snap: RESUME: Snapclient wird wieder verbunden
arbiter: switch 0 -> 1
```

## Abbruchkriterien

Patch nicht committen, wenn eines davon auftritt:

- Task-Watchdog
- A2DP-Paketdrops nehmen gegenüber dem bisherigen Stand zu
- dauerhafte `dropped`-Werte bei stabilem WLAN
- keine Rückschaltung zu Snapcast
- Knacken oder Wiedergabe von Frames der vorherigen Quelle
- Build-Warnungen zu impliziten Deklarationen oder inkompatiblen Callback-Typen
