#!/bin/sh
MOD_DIR="$(cd "$(dirname "$0")" && pwd)"
STATE_DIR="/run/usb-import"
PIDFILE="$STATE_DIR/server.pid"
LOGFILE="$STATE_DIR/daemon.log"
ARCH="$(uname -m)"

case "$ARCH" in
  aarch64|arm64) SRC="$MOD_DIR/usb-import-aarch64" ;;
  *)             SRC="$MOD_DIR/usb-import-arm32" ;;
esac

BINARY="$STATE_DIR/usb-import"

is_running() {
    [ -f "$PIDFILE" ] || return 1
    PID="$(cat "$PIDFILE")"
    [ -d "/proc/$PID" ] || return 1
    grep -q usb-import "/proc/$PID/cmdline" 2>/dev/null
}

is_our_process() {
    [ -d "/proc/$1" ] || return 1
    grep -q usb-import "/proc/$1/cmdline" 2>/dev/null
}

case "$1" in
  start)
    if is_running; then
        exit 0
    fi
    rm -f "$PIDFILE"
    mkdir -p "$STATE_DIR"
    cp "$SRC" "$BINARY"
    chmod 755 "$BINARY"
    "$BINARY" >> "$LOGFILE" 2>&1 &
    echo $! > "$PIDFILE"
    sleep 1
    if is_running; then
        echo "$(date): started pid=$(cat "$PIDFILE")" >> "$LOGFILE"
        # Verify port is listening
        if command -v ss >/dev/null 2>&1; then
            ss -tln 2>/dev/null | grep -q ':8082 ' || echo "$(date): WARNING port 8082 not listening" >> "$LOGFILE"
        fi
        exit 0
    else
        echo "$(date): FAILED to start" >> "$LOGFILE"
        rm -f "$PIDFILE"
        exit 1
    fi
    ;;
  stop)
    if [ -f "$PIDFILE" ]; then
        PID="$(cat "$PIDFILE")"
        if is_our_process "$PID"; then
            kill "$PID" 2>/dev/null
        fi
        rm -f "$PIDFILE"
    fi
    exit 0
    ;;
  restart)
    "$0" stop
    sleep 1
    "$0" start
    ;;
esac
