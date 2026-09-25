# Serial provisioning protocol

A deliberately trivial line-oriented text protocol over USB CDC at
115200 8N1. Both the webflasher (via Web Serial API) and a human in
a terminal program can talk to it. **No KISS, no binary framing, no
escape sequences.** The implementation lives in
`src/SerialConsole.cpp`; field parsing is `config::set_field()` in
`src/Config.cpp`.

Every command ends with `\n` or `\r\n`. Every response ends with a
line consisting of exactly `OK` or `ERR: <reason>`. The client reads
until it sees either terminator.

Command keywords are case-insensitive (`status` works); values keep
their case. The console echoes typed characters back, so each
response starts with the command line itself — clients should skip
lines until the payload. Lines are limited to 191 characters.

With `log_level` ≥ 1 the node also prints unsolicited log lines
(`[alive] ...`, `Radio: RX ...`, etc.) on the same port. Clients
should ignore lines they do not expect rather than treat them as part
of a response.

## Commands

### `PING`
→ `PONG`
→ `OK`

Liveness check.

### `VERSION`
→ `version=<git describe>`
→ `OK`

The version string is stamped at build time from
`git describe --tags --always` (`scripts/pre_build.py`), e.g.
`version=v0.6.5` for a tagged release build or
`version=v0.6.5-2-geaa40e0` for a build two commits past the tag. The
board name is printed in the boot banner, not here.

### `STATUS`
→ one `key=value` line per field
→ `OK`

```
uptime_s=219
radio=up
tx=enabled
packets_in=30
packets_out=15
paths=4
destinations=4
display_name=Rptr-723F88467151
battery_raw=2474
battery_mv=3787
batt_mult=1.5200
```

| Key | Meaning |
|---|---|
| `uptime_s` | Seconds since boot |
| `radio` | `up` / `down` |
| `tx` | `enabled` / `disabled` (RX-only — see `tx_enabled`) |
| `packets_in` / `packets_out` | Reticulum packets received / transmitted since boot |
| `paths` | Learned paths in the transport path table |
| `destinations` | Local destinations registered on this node |
| `display_name` | Live (not staged) display name |
| `battery_raw` | Averaged raw 12-bit ADC reading of the battery pin |
| `battery_mv` | `battery_raw × batt_mult` |
| `batt_mult` | Live battery scaling factor |

`paths` starts at 0 after boot and grows as announces are heard.

### `CONFIG GET`
→ one `key=value` line per config field
→ `OK`

Prints the **staging** config. Staging is seeded from the live config
at boot, so until you `CONFIG SET` something it matches what the node
is running. Keys are the same names `CONFIG SET` accepts.

### `CONFIG GETP`
→ all config fields on one `|`-delimited line
→ `OK`

Machine-readable form of `CONFIG GET` used by the BLE / web console.
Field order is documented at `print_fields_pipe()` in
`src/Config.cpp`.

### `CONFIG SET <key> <value>`
→ `OK` or `ERR: <reason>`

Writes to the **staging** config in RAM. Nothing hits flash until
`CONFIG COMMIT`.

The value is **everything after the first run of whitespace**, taken
literally — spaces are allowed and quotes are *not* stripped:

```
CONFIG SET display_name Solar Site A
```

Validation happens on each SET; invalid values return
`ERR: <reason>` and leave the staging copy unchanged. Booleans accept
`0`/`1`, `true`/`false`, `on`/`off`, `yes`/`no`.

Valid keys:

| Key | Type | Range |
|---|---|---|
| `display_name` | string | 1 – 31 bytes, must not contain `\|` |
| `freq_hz` | uint32 | 100000000 – 1100000000 |
| `bw_hz` | uint32 | 7800 – 500000 |
| `sf` | uint8 | 7 – 12 |
| `cr` | uint8 | 5 – 8 (denominator of 4/5..4/8) |
| `txp_dbm` | int8 | -9 – +22 |
| `tx_enabled` | bool | `0` = receive-only (fresh-flash default), `1` = transmit allowed |
| `batt_mult` | float | > 0 – 10.0 |
| `tele_interval_ms` | uint32 | `0` (off) or 10000 – 604800000 (7 days) |
| `lxmf_interval_ms` | uint32 | `0` (off) or 10000 – 604800000 (7 days) |
| `telemetry` | bool | LXMF telemetry push to `collector` |
| `lxmf` | bool | LXMF presence announces |
| `heartbeat` | bool | Heartbeat LED |
| `bt_enabled` | bool | BLE console (takes effect after commit/reboot) |
| `bt_pin` | uint32 | 0 – 999999 (BLE pairing PIN, 0 = none) |
| `latitude` | float | -90 – 90 degrees |
| `longitude` | float | -180 – 180 degrees |
| `altitude` | int32 | -100000 – 100000 m |
| `log_level` | uint8 | 0 = quiet, 1 = normal, 2 = verbose |
| `collector` | hex | 32 hex chars (16-byte `lxmf.delivery` hash), or `none`/`off`/`clear` |

### `CONFIG RESET`
→ `OK`

Restores the staging copy to the board's hardcoded defaults. Does
**not** touch flash. Follow with `CONFIG COMMIT` to persist.

### `CONFIG REVERT`
→ `OK`

Discards staged edits by re-seeding staging from the live config.

### `CONFIG COMMIT`
→ `committed, rebooting...`
→ `OK`
→ (node reboots ~50 ms later)

Validates the staging copy, computes its CRC, writes `/config.bin`,
and reboots so the new config takes effect via the normal boot path.
Returns `ERR: staging validation failed` or `ERR: save failed`
without rebooting on failure. **The webflasher should wait ~3 s and
then reconnect to USB CDC** to verify via `STATUS`.

### `CALIBRATE BATTERY <measured_mv>`
→ `battery_raw=<n>`
→ `measured_mv=<mV>`
→ `batt_mult=<float>`
→ `(staged -- run CONFIG COMMIT to persist)`
→ `OK`

Measure the battery with a multimeter and pass the reading
(500 – 10000 mV). The firmware takes a fresh ADC average, stages
`batt_mult = measured_mv / battery_raw`, and leaves persisting to
`CONFIG COMMIT`.

### `ANNOUNCE`
→ `firing LXMF presence announce...`
→ `sending telemetry message...`
→ `OK`

Forces an LXMF presence announce and a telemetry push now instead of
waiting for the next interval. `ERR: radio not online` if the radio
failed to start.

### `REBOOT`
→ `rebooting...`
→ `OK`
→ (node reboots)

Soft reboot without touching config.

### `DFU`
→ `entering DFU mode...`
→ `OK`
→ (node reboots into the serial DFU bootloader)

Used by the webflasher for one-click flashing. The bootloader
enumerates as a new USB serial port.

### `HELP`
→ (prints the command list)
→ `OK`

## Webflasher example session

```
→ PING
← PONG
← OK
→ VERSION
← version=v0.6.5
← OK
→ CONFIG RESET
← OK
→ CONFIG SET freq_hz 904375000
← OK
→ CONFIG SET bw_hz 250000
← OK
→ CONFIG SET sf 10
← OK
→ CONFIG SET cr 5
← OK
→ CONFIG SET txp_dbm 22
← OK
→ CONFIG SET tx_enabled 1
← OK
→ CONFIG SET display_name Roof Site North
← OK
→ CONFIG GET
← display_name=Roof Site North
← freq_hz=904375000
← bw_hz=250000
← sf=10
← cr=5
← txp_dbm=22
← tx_enabled=1
← batt_mult=1.2840
← tele_interval_ms=10800000
← lxmf_interval_ms=1800000
← telemetry=1
← lxmf=1
← heartbeat=1
← bt_enabled=0
← bt_pin=0
← latitude=0.000000
← longitude=0.000000
← altitude=0
← log_level=1
← collector=
← OK
→ CONFIG COMMIT
← committed, rebooting...
← OK
   (wait 3 s for reboot + reconnect)
→ STATUS
← uptime_s=4
← radio=up
← tx=enabled
← ...
← OK
```

(Echoed command lines and unsolicited log lines omitted.)

## Design notes

- Unknown commands return `ERR: unknown command (try HELP)`; unknown
  `CONFIG` / `CALIBRATE` subcommands and unknown keys also return
  `ERR`. No silent success; forces the webflasher to keep its command
  list in sync.
- Numeric values are decimal.
- Blank lines are ignored. Backspace/DEL edit the current line for
  humans typing in a terminal.
- The console processes at most 256 input bytes per main-loop pass so
  a chatty host cannot starve LoRa forwarding.
