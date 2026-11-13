# EngineX USB Import Mod

Import music files from USB drives to the internal SATA drive on Denon DJ Prime 4+ (and other EngineX-based devices).

## Features

- **On-screen dialogs** via Planck prompt system
- **ID3 tag reading** (ID3v2 + ID3v1): title, artist, album, genre, year, bitrate, duration
- **Cover art extraction** from APIC frames with hash-based deduplication
- **Automatic database registration** with full metadata fields
- **USB Import playlist** creation with proper EngineX linked-list structure
- **Smart source detection**: recognizes Engine Library/Music/ structure on USB sticks
- **Database backup** before every import with automatic rotation

## Dialog Flow

1. USB stick detected → "Import music?" Yes/No
2. If Yes and no target configured → select internal drive from list
3. Copy progress with cancel option
4. Import complete → Restart EngineX now or later

## Build

Requires [Zig](https://ziglang.org/) for cross-compilation:

```bash
# Download SQLite amalgamation
curl -sL "https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip" -o sqlite.zip
unzip sqlite.zip

# Build for aarch64 (Prime 4+ G2)
zig cc -target aarch64-linux-musl -static -Os \
  -I sqlite-amalgamation-3450300 \
  -o usb-import-aarch64 usb-import-server.c sqlite-amalgamation-3450300/sqlite3.c \
  -lpthread
```

## Installation

Copy the mod directory to `/data/enginex/mods/usb-import/` on your device.

## License

MIT — DJ MR. BROWN
