#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -ne 1 ]]; then
  echo "用法: bash scripts/upload.sh <串口>" >&2
  exit 2
fi
# 每次上传前构建，避免烧录过期产物。
bash "$PROJECT_DIR/scripts/build.sh"
exec arduino-cli upload \
  --fqbn 'esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB' \
  --port "$1" --input-dir "$PROJECT_DIR/build" \
  "$PROJECT_DIR/firmware/rlcd_demo"
