# CellMgr

CellMgr is a lightweight cellular module management backend written in C, with a dependency-free native HTML/CSS/JavaScript frontend.

The current default device profile targets Fibocom FM650, while the backend is designed to support more modules through JSON profiles.

## Features

- Device overview: CPU, memory, disk, temperature, uptime, WAN RX/TX counters.
- Modem overview through ofono D-Bus wrappers.
- AT console through `org.ofono.Modem.SendAtcmd`.
- Profile CRUD from the frontend.
- FM650 profile-driven network operations:
  - band discovery with `AT+GTACT=?`
  - band lock with `AT+GTACT=...`
  - cell lock with `AT+GTCELLLOCK=1,<rat>,0,<earfcn>,<pci>`
  - frequency lock with `AT+GTCELLLOCK=1,<rat>,1,<earfcn>`
  - unlock with `AT+GTCELLLOCK=0`
- SMS list/read/send/delete.
- SMS forwarding through plain SMTP relay or HTTP webhook.
- D-Bus event capture through `dbus-monitor`.
- Immediate reboot and scheduled reboot.
- Optional session authentication.

## Layout

```text
src/                  C backend source
config/cellmgrd.conf  default runtime config
profiles/fm650.json   default FM650 profile
web/mock.html         static UI mock preview
DESIGN.md             design notes
.github/workflows/    GitHub Actions builds
```

## Dependency Versions

GitHub Actions builds use a pinned SQLite amalgamation instead of the runner's system SQLite package.

| Dependency | Version | Notes |
| --- | --- | --- |
| SQLite | 3.46.1, amalgamation id `3460100` | Downloaded from the official SQLite archive in CI and compiled into `cellmgrd`. |
| C standard | C99 | Built with POSIX APIs. |
| pthread | glibc/pthread from target toolchain | Required by scheduler and D-Bus monitor threads. |
| D-Bus tools | target runtime `dbus-send`, `dbus-monitor` | Runtime dependency, not linked. |
| ofono | target runtime | Must expose `/ril_0` or configured modem path and `org.ofono.Modem.SendAtcmd`. |
| curl | target runtime, HTTP/HTTPS only | Used only for HTTP webhook SMS forwarding. SMTP does not use curl. |

For the FM650 target described during development, the known runtime baseline is glibc 2.27 on aarch64.

## Build

### Reproducible build with pinned SQLite

This is the same style used by GitHub Actions:

```sh
export SQLITE_VERSION=3460100
export SQLITE_YEAR=2024
mkdir -p third_party/sqlite .deps
curl -fL "https://www.sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_VERSION}.zip" -o .deps/sqlite.zip
unzip -q .deps/sqlite.zip -d .deps
cp ".deps/sqlite-amalgamation-${SQLITE_VERSION}/sqlite3.c" third_party/sqlite/
cp ".deps/sqlite-amalgamation-${SQLITE_VERSION}/sqlite3.h" third_party/sqlite/

make USE_BUNDLED_SQLITE=1 SQLITE_DIR=third_party/sqlite
```

### Build with system SQLite

Use this only when your toolchain provides a known-good `sqlite3.h` and `libsqlite3`:

```sh
make
```

### FM650/vendor cross build

```sh
make clean
make CC=aarch64-unisoc-linux-gnu-gcc USE_BUNDLED_SQLITE=1 SQLITE_DIR=third_party/sqlite
```

GitHub Actions also builds a generic aarch64 Linux binary with `aarch64-linux-gnu-gcc`. For production on the module, prefer the vendor toolchain when available.

## Run

```sh
./cellmgrd -c config/cellmgrd.conf
```

Open:

```text
http://<device-ip>:4242/
```

The default config has authentication disabled:

```ini
auth_enabled=0
```

To enable login:

```ini
auth_enabled=1
auth_user=admin
auth_pass=admin
```

## UI Mock

Open the static mock directly in a browser:

```text
web/mock.html
```

This file does not call the backend. It is only for layout and interaction preview.

## Runtime Notes

- The backend never shells through `system()` or `popen()`.
- D-Bus calls are executed through `fork + execvp` with argument arrays.
- AT commands are filtered unless `allow_dangerous_at=1`.
- TLS SMTP is intentionally not implemented because the target curl build does not support SMTP and OpenSSL development headers are not guaranteed. Use a local SMTP relay or HTTP webhook for forwarding.
- `FM650_AT_Commands_full.txt` is not committed because it is a vendor manual with copyright restrictions.

## Important API Groups

- `GET /api/overview`
- `GET /api/device/status`
- `POST /api/device/drop-caches`
- `GET /api/modem/status`
- `GET /api/modem/radio`
- `POST /api/modem/online`
- `POST /api/modem/power`
- `GET /api/network/bands`
- `POST /api/network/lock-band`
- `POST /api/network/lock-cell`
- `POST /api/network/lock-frequency`
- `POST /api/network/unlock`
- `GET /api/sms/list`
- `GET /api/sms/read?index=<index>`
- `POST /api/sms/send`
- `POST /api/sms/delete`
- `POST /api/sms/forward-test`
- `POST /api/at/send`
- `GET /api/at/history`
- `GET /api/profiles`
- `POST /api/profiles/save`
- `POST /api/profiles/activate`
- `POST /api/system/settings-save`
- `POST /api/system/reboot`
- `POST /api/system/scheduled-reboot`

## Current Status

The functional paths are implemented, but final validation still needs to happen on the target module:

- Build with the vendor aarch64 toolchain.
- Verify `/ril_0` or configure the correct modem path.
- Verify exact FM650 AT responses for `GTACT`, `GTCELLLOCK`, `CMGL`, and `CMGR`.
- Tune parsers using real device output.
