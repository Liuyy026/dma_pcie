#!/usr/bin/env bash
set -euo pipefail

PIDFILE="/tmp/pcie_demo_pid"
LOG="/tmp/pcie_runtime.log"

if [[ ! -f "$PIDFILE" ]]; then
  echo "PID file not found: $PIDFILE" >&2
  exit 1
fi

pid=$(cat "$PIDFILE")
if ps -p "$pid" > /dev/null 2>&1; then
  echo "Stopping pid $pid"
  kill "$pid"
  # wait up to 5s
  for i in {1..10}; do
    if ! ps -p "$pid" > /dev/null 2>&1; then break; fi
    sleep 0.5
  done
  if ps -p "$pid" > /dev/null 2>&1; then
    echo "Process still alive, sending SIGKILL"
    kill -9 "$pid" || true
  fi
else
  echo "Process $pid not running"
fi

rm -f "$PIDFILE"
echo "Tail of runtime log ($LOG):"
tail -n 200 "$LOG" || true
