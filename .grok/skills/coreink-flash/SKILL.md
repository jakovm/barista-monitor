---
name: coreink-flash
description: >
  CoreInk (barista-monitor) flashen oder syncen. Vor jedem Upload Serial-Port freigeben
  (agent-button/matrix_daemon blockiert sonst /dev/cu.usbserial-*). Bei fehlgeschlagenem
  Flash zuerst Port prüfen, dann erneut flashen. Triggers: flashen, flash, gerät flashen,
  upload, sync, serial port, port blockiert, /coreink-flash, barista-monitor flash.
---

# CoreInk Flash (barista-monitor)

## Wann anwenden

- Nutzer will Firmware auf den CoreInk laden (`flashen`, `sync`, `gerät flashen`)
- Upload schlägt fehl: `port is busy`, `Resource temporarily unavailable`, `Device not configured`,
  `chip stopped responding`, `Serial data stream stopped`
- Nach fehlgeschlagenem erstem Versuch: **immer** Port-Blocker prüfen, bevor erneut geflasht wird

## Ursache (häufig)

Der **agent-button**-Daemon (`matrix_daemon.py`) nutzt als Fallback den ersten
`/dev/cu.usbserial-*` — das ist oft der **CoreInk**, nicht der Atom Matrix.
Dann konkurriert er mit `esptool`/`arduino-cli` um den Port.

## Pflicht vor jedem Upload

```bash
bash /Users/jakov/Documents/GitHub/barista-monitor/scripts/ensure_serial_port.sh /dev/cu.usbserial-5A6D0127411
```

Das Skript:
1. stoppt `com.jakov.agent-button` (LaunchAgent)
2. beendet `matrix_daemon.py`
3. beendet andere Prozesse, die den Port halten
4. bricht mit Fehler ab, wenn der Port noch belegt ist

**Nicht** nur dem Nutzer sagen — **selbst ausführen**.

## Flash / Sync

```bash
cd /Users/jakov/Documents/GitHub/barista-monitor
bash scripts/ensure_serial_port.sh /dev/cu.usbserial-5A6D0127411
bash scripts/flash.sh /dev/cu.usbserial-5A6D0127411
```

Nur Upload (bereits kompiliert):

```bash
bash scripts/ensure_serial_port.sh /dev/cu.usbserial-5A6D0127411
bash scripts/sync.sh /dev/cu.usbserial-5A6D0127411
```

`flash.sh` und `sync.sh` rufen `ensure_serial_port.sh` vor **jedem** Upload-Versuch automatisch auf.

## Erfolg trotz USB-Abbruch (wichtig)

CoreInk trennt USB oft **nach** erfolgreichem Write (Hard-Reset / Deep-Sleep).  
esptool/arduino-cli melden dann Exit ≠ 0, obwohl die Firmware schon drauf ist.

`flash.sh` / `sync.sh` werten deshalb als **Erfolg**, wenn:
- App-Partition geschrieben (`Wrote … at 0x00010000`) und
- mindestens ein `Hash of data verified`

→ **keine** weiteren Retries nach verifiziertem Write. Chat/Agent nicht minutenlang blockieren lassen.

## Nach erfolgreichem Flash

Agent-Button wieder starten (Atom Matrix, separater Port):

```bash
zsh -lic '/Users/jakov/Documents/Smart\ Devices/scripts/ensure_daemon.sh'
python3 "${HOME}/.grok/skills/agent-button/scripts/matrix_status.py" I
```

## Diagnose

```bash
ls /dev/cu.usbserial*
lsof /dev/cu.usbserial-5A6D0127411
pgrep -fl matrix_daemon
```

CoreInk-Port: `/dev/cu.usbserial-5A6D0127411`  
Atom-Matrix-Port: `/dev/cu.usbserial-855251E6E2`

## Retry-Ablauf

1. `ensure_serial_port.sh` ausführen
2. CoreInk per Knopf kurz wecken, USB prüfen
3. `flash.sh` erneut starten (bis zu 5 Versuche intern; bricht bei Hash-ok ab)
4. Schlägt es wieder fehl → `lsof` + USB-Kabel/Hub prüfen, dann Schritt 1