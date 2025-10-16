#!/usr/bin/env bash
set -euo pipefail

# Stable runner for PCIE_Demo_QT. Uses setsid to detach from the invoking shell/IDE
# and writes a PID file + log in /tmp. Supports optional CSV recording via
# --csv and --csv-path.

DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$DIR/build/bin/PCIE_Demo_QT"
LOG="/tmp/pcie_runtime.log"
PIDFILE="/tmp/pcie_demo_pid"

CSV_ENABLE=0
CSV_PATH=""

usage() {
  cat <<EOF
Usage: $(basename "$0") [--csv] [--csv-path /path/to/file]

Options:
  --csv             Enable per-write CSV logging (sets PCIE_WRITE_CSV=1)
  --csv-path PATH   Set CSV output path (sets PCIE_WRITE_CSV_PATH)
  -h, --help        Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --csv)
      CSV_ENABLE=1; shift;;
    --csv-path)
      CSV_PATH="$2"; shift 2;;
    -h|--help)
      usage; exit 0;;
    *)
      break;;
  esac
done

if [[ ! -x "$BIN" ]]; then
  echo "Error: binary not found or not executable: $BIN" >&2
  exit 1
fi

if [[ $CSV_ENABLE -ne 0 ]]; then
  export PCIE_WRITE_CSV=1
  if [[ -n "$CSV_PATH" ]]; then
    export PCIE_WRITE_CSV_PATH="$CSV_PATH"
  fi
fi

# Start detached
setsid "$BIN" > "$LOG" 2>&1 < /dev/null &
sleep 0.1
echo $! > "$PIDFILE"
echo "Started: pid=$(cat $PIDFILE), log=$LOG"
