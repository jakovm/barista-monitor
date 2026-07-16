#!/usr/bin/env bash
# Compile + flash barista-monitor onto CoreInk.
# CoreInk drops USB after hard-reset even when the write succeeded — treat
# verified writes as success so we do not burn minutes on useless retries.
set -euo pipefail

export PATH="${HOME}/.local/bin:${PATH}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENSURE_PORT="${ROOT}/scripts/ensure_serial_port.sh"
COREINK_PORT="${COREINK_PORT:-/dev/cu.usbserial-5A6D0127411}"

bash "${ENSURE_PORT}" "${COREINK_PORT}" 2>/dev/null || true
FQBN="m5stack:esp32:m5stack_coreink"
SKETCH="${ROOT}/firmware/barista_monitor"
BUILD="${ROOT}/build"
PORT="${1:-}"
WAIT_SECS="${2:-90}"
UPLOAD_BAUD="${UPLOAD_BAUD:-115200}"
MAX_ATTEMPTS="${MAX_ATTEMPTS:-5}"

if ! command -v arduino-cli >/dev/null; then
  echo "arduino-cli nicht gefunden." >&2
  exit 1
fi

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
  echo "Kein CoreInk-Port gefunden. USB prüfen, ggf. Port als Argument angeben." >&2
  exit 1
fi

python3 "${ROOT}/scripts/write_rtc_stamp.py"

echo "Kompiliere barista-monitor..."
arduino-cli compile --fqbn "${FQBN}" --build-path "${BUILD}" "${SKETCH}"

pulse_reset() {
  local port="$1"
  python3 - "${port}" <<'PY'
import serial
import time
import sys

port = sys.argv[1]
with serial.Serial(port, 115200) as ser:
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.12)
    ser.setRTS(False)
    time.sleep(0.5)
    ser.setDTR(True)
    time.sleep(0.1)
PY
}

# True if esptool finished writing+verifying the app image, even if the final
# hard-reset loses the USB port (normal on CoreInk after reboot / deep sleep).
upload_looks_verified() {
  local log="$1"
  # App partition write (0x10000) completed
  echo "${log}" | grep -qE 'Wrote .+ at 0x00010000' || return 1
  # At least one region hash verified (bootloader/partitions/app)
  local hashes
  hashes="$(echo "${log}" | grep -c 'Hash of data verified' || true)"
  [[ "${hashes}" -ge 1 ]] || return 1
  # Prefer strong signal: hard-reset attempted after write, or post-reset port loss
  if echo "${log}" | grep -q 'Hard resetting'; then
    return 0
  fi
  if echo "${log}" | grep -qiE 'could not open port|No such file or directory'; then
    return 0
  fi
  # All typical regions verified without explicit hard-reset line
  [[ "${hashes}" -ge 3 ]]
}

upload_once() {
  local port="$1"
  local log rc
  set +e
  log="$(arduino-cli upload -p "${port}" --fqbn "${FQBN}" --build-path "${BUILD}" \
    --upload-property "upload.speed=${UPLOAD_BAUD}" "${SKETCH}" 2>&1)"
  rc=$?
  set -e
  printf '%s\n' "${log}"

  if [[ "${rc}" -eq 0 ]]; then
    return 0
  fi

  if upload_looks_verified "${log}"; then
    echo "Upload verifiziert (Hash ok). USB nach Hard-Reset weg — CoreInk normal, gilt als Erfolg."
    return 0
  fi

  return "${rc}"
}

echo "Flashe nach ${PORT} (${UPLOAD_BAUD} baud)..."
for attempt in $(seq 1 "${MAX_ATTEMPTS}"); do
  echo "Upload-Versuch ${attempt}/${MAX_ATTEMPTS}"

  # Port can vanish between attempts — re-resolve
  if [[ ! -e "${PORT}" ]]; then
    PORT="$(find_port)"
    if [[ -z "${PORT}" ]]; then
      echo "Port weg — warte 3s auf USB-Reconnect..."
      sleep 3
      PORT="$(find_port)"
    fi
  fi
  if [[ -z "${PORT}" || ! -e "${PORT}" ]]; then
    echo "Kein Port (Versuch ${attempt}/${MAX_ATTEMPTS})."
    if [[ "${attempt}" -lt "${MAX_ATTEMPTS}" ]]; then
      sleep 3
      continue
    fi
    break
  fi

  bash "${ENSURE_PORT}" "${PORT}"
  pulse_reset "${PORT}" || true

  if upload_once "${PORT}"; then
    echo "Fertig (RTC = Lokalzeit beim Build)."
    exit 0
  fi

  if [[ "${attempt}" -lt "${MAX_ATTEMPTS}" ]]; then
    echo "Warte 3s auf USB-Reconnect..."
    sleep 3
  fi
done

echo "Upload fehlgeschlagen. CoreInk kurz per Knopf wecken und Skript erneut starten." >&2
exit 1
