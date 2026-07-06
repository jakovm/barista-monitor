#!/usr/bin/env bash
# Freigibt den CoreInk-Serial-Port vor Upload/Flash.
# Beendet u.a. agent-button (matrix_daemon), der sonst den ersten /dev/cu.usbserial-* belegt.
set -euo pipefail

PORT="${1:-}"
COREINK_PORT="${COREINK_PORT:-/dev/cu.usbserial-5A6D0127411}"
AGENT_BUTTON_LABEL="com.jakov.agent-button"

find_port() {
  python3 - <<'PY'
import glob
import os

preferred = os.environ.get("COREINK_PORT", "/dev/cu.usbserial-5A6D0127411")
if preferred and os.path.exists(preferred):
    print(preferred, end="")
else:
    ports = sorted(glob.glob("/dev/cu.usbserial-*"))
    print(ports[0] if ports else "", end="")
PY
}

if [[ -z "${PORT}" ]]; then
  PORT="$(find_port)"
fi

if [[ -z "${PORT}" ]]; then
  echo "ensure_serial_port: kein USB-Serial-Port gefunden." >&2
  exit 1
fi

echo "ensure_serial_port: Ziel ${PORT}"

stop_agent_button() {
  launchctl bootout "gui/$(id -u)/${AGENT_BUTTON_LABEL}" 2>/dev/null || true
  pkill -f matrix_daemon.py 2>/dev/null || true
}

list_blockers() {
  lsof -nP "${PORT}" 2>/dev/null || true
}

kill_port_holders() {
  local pids
  pids="$(lsof -t "${PORT}" 2>/dev/null || true)"
  if [[ -z "${pids}" ]]; then
    return 0
  fi

  for pid in ${pids}; do
    local cmd
    cmd="$(ps -p "${pid}" -o command= 2>/dev/null || true)"
    if [[ -z "${cmd}" ]]; then
      continue
    fi
    if [[ "${cmd}" == *"ensure_serial_port.sh"* ]] || [[ "${cmd}" == *"flash.sh"* && "${cmd}" != *"matrix_daemon"* ]]; then
      continue
    fi
    echo "ensure_serial_port: beende PID ${pid} (${cmd%% *})"
    kill "${pid}" 2>/dev/null || kill -9 "${pid}" 2>/dev/null || true
  done
}

stop_agent_button

if [[ -n "$(list_blockers)" ]]; then
  echo "ensure_serial_port: Port blockiert:"
  list_blockers
  kill_port_holders
  sleep 0.5
fi

if [[ -n "$(list_blockers)" ]]; then
  echo "ensure_serial_port: erneuter Versuch..."
  stop_agent_button
  kill_port_holders
  sleep 0.5
fi

if [[ -n "$(list_blockers)" ]]; then
  echo "ensure_serial_port: Port weiterhin blockiert:" >&2
  list_blockers >&2
  exit 1
fi

echo "ensure_serial_port: Port frei."