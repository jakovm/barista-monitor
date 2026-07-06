#!/usr/bin/env bash
set -euo pipefail

export PATH="${HOME}/.local/bin:${PATH}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="m5stack:esp32:m5stack_coreink"
SKETCH="${ROOT}/firmware/barista_monitor"
BUILD="${ROOT}/build"
PORT="${1:-}"
WAIT_SECS="${2:-90}"
UPLOAD_BAUD="${UPLOAD_BAUD:-115200}"

if ! command -v arduino-cli >/dev/null; then
  echo "arduino-cli nicht gefunden." >&2
  exit 1
fi

find_port() {
  python3 - <<'PY'
import glob
ports = sorted(glob.glob("/dev/cu.usbserial-*"))
print(ports[0] if ports else "", end="")
PY
}

if [[ -z "${PORT}" ]]; then
  PORT="$(find_port)"
fi

if [[ -z "${PORT}" ]]; then
  echo "Warte bis ${WAIT_SECS}s auf CoreInk-USB..."
  for _ in $(seq 1 "${WAIT_SECS}"); do
    PORT="$(find_port)"
    if [[ -n "${PORT}" ]]; then
      break
    fi
    sleep 1
  done
fi

if [[ -z "${PORT}" ]]; then
  echo "Kein CoreInk-Port gefunden." >&2
  exit 1
fi

python3 "${ROOT}/scripts/write_rtc_stamp.py"

echo "Kompiliere mit aktueller Lokalzeit..."
arduino-cli compile --fqbn "${FQBN}" --build-path "${BUILD}" "${SKETCH}"

pulse_reset() {
  python3 - <<PY
import serial
import time

port = "${PORT}"
ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.dtr = False
ser.rts = False
ser.open()
ser.setRTS(True)
time.sleep(0.12)
ser.setRTS(False)
time.sleep(0.5)
ser.setDTR(True)
time.sleep(0.1)
ser.close()
PY
}

echo "Flashe nach ${PORT} (${UPLOAD_BAUD} baud)..."
for attempt in 1 2 3 4 5; do
  echo "Upload-Versuch ${attempt}/5"
  pulse_reset || true
  if arduino-cli upload -p "${PORT}" --fqbn "${FQBN}" --build-path "${BUILD}" \
    --upload-property "upload.speed=${UPLOAD_BAUD}" "${SKETCH}"; then
    echo "Sync fertig (RTC = Lokalzeit beim Build)."
    exit 0
  fi
  sleep 2
done

echo "Sync fehlgeschlagen." >&2
exit 1