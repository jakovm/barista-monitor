#!/usr/bin/env bash
set -euo pipefail

export PATH="${HOME}/.local/bin:${PATH}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="m5stack:esp32:m5stack_coreink"
SKETCH="${ROOT}/firmware/barista_monitor"
PORT="${1:-}"

if ! command -v arduino-cli >/dev/null; then
  echo "arduino-cli nicht gefunden." >&2
  exit 1
fi

if [[ -z "${PORT}" ]]; then
  PORT="$(arduino-cli board list | awk '/m5stack_coreink|serial|usbserial/{print $1; exit}')"
fi

if [[ -z "${PORT}" ]]; then
  echo "Kein CoreInk-Port gefunden. Port als Argument angeben." >&2
  exit 1
fi

echo "Kompiliere barista-monitor..."
arduino-cli compile --fqbn "${FQBN}" "${SKETCH}"

echo "Flashe nach ${PORT}..."
arduino-cli upload -p "${PORT}" --fqbn "${FQBN}" "${SKETCH}"

echo "Fertig."