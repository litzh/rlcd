#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FQBN='esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB'
export UV_CACHE_DIR="${UV_CACHE_DIR:-$PROJECT_DIR/build/uv-cache}"
uv run --no-project "$PROJECT_DIR/scripts/generate_wifi_config.py" \
  "$PROJECT_DIR/firmware/rlcd/wifi_defaults.h"
exec arduino-cli compile --fqbn "$FQBN" \
  --library "$PROJECT_DIR/libraries/U8g2" \
  --build-path "$PROJECT_DIR/build" \
  "$PROJECT_DIR/firmware/rlcd"
