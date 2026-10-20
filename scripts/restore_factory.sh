#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
BACKUP_FILE="$(dirname "$0")/../backups/factory_firmware_4mb.bin"
ESPTOOL="${ESPTOOL:-/home/fcurrie/.platformio/packages/tool-esptoolpy/esptool.py}"

if [[ ! -f "$BACKUP_FILE" ]]; then
    echo "Error: Factory backup not found at $BACKUP_FILE" >&2
    exit 1
fi

echo "Verifying SHA-256 checksum..."
(cd "$(dirname "$0")/.." && sha256sum -c backups/SHA256SUMS)

echo "Restoring 4MB factory firmware to $PORT..."
timeout 30s python3 "$ESPTOOL" --chip esp32c6 --port "$PORT" --baud 921600 write_flash 0x0 "$BACKUP_FILE"
echo "Factory firmware restored successfully."
