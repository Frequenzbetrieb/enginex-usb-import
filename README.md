# USB Import Mod for EngineX OS

Import music files from USB drives directly on your Denon DJ deck — no laptop required.

## What it does

When you plug in a USB drive, a dialog appears on the touchscreen asking whether to import music files. If you tap **Import**, the mod:

1. **Backs up** the EngineX database automatically before any changes
2. **Copies** all music files (MP3, FLAC, WAV, M4A, OGG, AAC, AIFF) from the USB drive to the internal SATA drive
3. **Registers** each track in the EngineX database (including files that already exist)
4. **Creates a "USB Import" playlist** containing all imported tracks
5. **Shows copy progress** on screen (file count and current filename)
6. **Asks whether to restart EngineX** — you decide when playback stops
7. Skips files that already exist on the internal drive

## Target drive selection

On first use (or when multiple Engine Library drives are detected), the mod asks you to select your internal drive. You can optionally save this choice so it remembers across reboots.

To reset the saved target drive: `http://<deck-ip>:8082/api/reset-target`

## Compatibility

- Any Denon DJ deck running [EngineX OS](https://github.com/Cd1s/EngineX-OS)
- Binaries for **aarch64** (Prime 4+ G2, SC LIVE 4, Rane SYSTEM ONE) and **armv7** (Prime 4, SC5000, SC6000, Prime 2, Prime GO, Mixstream Pro)
- `start.sh` auto-detects the architecture

## Installation

1. Open the EngineX web console at `http://<deck-ip>:8080`
2. Go to **Mods** → Upload `usb-import-7.0.0.tar.gz` → Enable

## Safety

- **Database backup** via SQLite backup API while holding the write lock; only the last 3 kept
- `BEGIN IMMEDIATE` + `ROLLBACK` on any statement error — no partial writes
- Every INSERT, UPDATE, and COMMIT result is checked
- Both copied and already-existing files are registered in the database
- Uses `DELETE` journal mode (never WAL)
- Files written to temp name, fsynced, then atomically renamed
- Free space checked before copying; symlinks ignored; no fixed file limit
- Drive parameter validated; concurrent imports rejected
- Watchdog: if daemon stops responding for ~7.5s, dialog shows error instead of hanging
- EngineX is **never restarted without explicit user confirmation**

### Restoring from backup

```sh
ssh root@<deck-ip> -p 2222
cp '/media/<Volume>/Engine Library/Database2/m.db.usb-import-backup-<timestamp>' \
   '/media/<Volume>/Engine Library/Database2/m.db'
```

## How it works

- **`usb-import-aarch64` / `usb-import-arm32`** — Statically linked C daemon with embedded SQLite. Polls `/media/` for USB drives every 3s. Local HTTP API on `127.0.0.1:8082`. No network connections.
- **`UsbImportDialog.qml`** — QML overlay using EngineX's Planck prompt system.
- **`start.sh`** — Copies binary to `/run/usb-import/`, chmod 755, verifies start, checks port, logs to `/run/usb-import/daemon.log`.

### Drive detection

1. Scans `/media/` for `Engine Library/Database2/m.db`
2. Excludes removable devices (`/sys/block/*/removable`) and USB-attached devices (sysfs path containing `/usb`)
3. Uses saved target if still mounted; auto-selects if exactly one candidate; asks user if ambiguous
4. Optionally saves choice to `/data/enginex/mods/usb-import/config`

## Building from source

Requirements: [Zig](https://ziglang.org/), [SQLite amalgamation](https://www.sqlite.org/download.html) (3.45.3)

```sh
curl -L https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip -o sqlite.zip
unzip sqlite.zip

zig cc -target aarch64-linux-musl -static -Os \
  -I sqlite-amalgamation-3450300 \
  -o usb-import-aarch64 usb-import-server.c \
  sqlite-amalgamation-3450300/sqlite3.c \
  -lpthread -DSQLITE_THREADSAFE=1

zig cc -target arm-linux-musleabihf -static -Os \
  -I sqlite-amalgamation-3450300 \
  -o usb-import-arm32 usb-import-server.c \
  sqlite-amalgamation-3450300/sqlite3.c \
  -lpthread -DSQLITE_THREADSAFE=1
```

Note: Zig uses clang/LLD internally — the ELF `.comment` shows the bundled clang version.

## API (127.0.0.1:8082)

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | Current state |
| `/api/select-target` | POST | Select internal drive (`{"drive":"name","remember":true}`) |
| `/api/reset-target` | POST | Clear saved target drive |
| `/api/import` | POST | Start import (`{"drive":"name"}`) |
| `/api/dismiss` | POST | Dismiss a detected drive |
| `/api/ack` | POST | Acknowledge done result |
| `/api/restart` | POST | Restart EngineX (only when pending) |
| `/api/later` | POST | Dismiss restart, resume detection |

## License

MIT — see [LICENSE](LICENSE)

## Author

**DJ MR. BROWN** — [Source on GitHub](https://github.com/Frequenzbetrieb/enginex-usb-import)
