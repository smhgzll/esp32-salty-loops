#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Set up a Nobara / Fedora computer for this project: system packages, ESP-IDF v5.5.5 with the
# ESP32-S3 toolchain and QEMU, the web UI's dependencies, Playwright for the browser tests.
#   scripts/bootstrap-nobara.sh [--idf PATH] [--no-dnf] [--no-browser-tests]
#     --idf PATH   use an existing ESP-IDF v5.5.x checkout (recorded in .idf_path, not modified
#                  beyond its own install.sh); default: ~/esp/v5.5.5/esp-idf, cloned if missing
# Re-running is safe. scripts/doctor.sh shows what is still missing afterwards.
source "$(dirname "$0")/common.sh"
IDF_DIR=""; DNF=1; BROWSER=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --idf) IDF_DIR="$2"; shift ;;
    --no-dnf) DNF=0 ;;
    --no-browser-tests) BROWSER=0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

if [[ $DNF == 1 ]]; then
  echo "== system packages (sudo dnf)"
  sudo dnf install -y git wget flex bison gperf python3 python3-pip cmake ninja-build ccache dfu-util libusb1 \
    gcc gcc-c++ alsa-lib-devel alsa-utils nodejs npm libgcrypt pixman SDL2 libslirp curl
fi

echo "== ESP-IDF v5.5.5"
if [[ -z "$IDF_DIR" ]]; then
  IDF_DIR="$(find_idf 2>/dev/null || true)"
  if [[ -z "$IDF_DIR" ]]; then
    IDF_DIR="$HOME/esp/v5.5.5/esp-idf"
    mkdir -p "$(dirname "$IDF_DIR")"
    git clone --branch v5.5.5 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git "$IDF_DIR"
  fi
fi
[[ -f "$IDF_DIR/export.sh" ]] || { echo "$IDF_DIR is not an ESP-IDF checkout" >&2; exit 1; }
"$IDF_DIR/install.sh" esp32s3
python3 "$IDF_DIR/tools/idf_tools.py" install qemu-xtensa
echo "$IDF_DIR" > "$ROOT_DIR/.idf_path"

echo "== web UI"
(cd "$ROOT_DIR/web" && npm ci --no-audit --no-fund)

if [[ $BROWSER == 1 ]]; then
  echo "== browser tests (Playwright, chromium)"
  python3 -m pip install --user playwright
  python3 -m playwright install chromium
fi

if ! id -nG | grep -qwE "dialout|uucp"; then
  echo "note: to flash over USB, add yourself to the serial group and log in again:"
  echo "      sudo usermod -aG dialout \$USER"
fi
[[ -f "$ROOT_DIR/config/local.env" ]] || echo "next: cp config/local.env.example config/local.env (Wi-Fi, admin password; never committed)"
"$ROOT_DIR/scripts/doctor.sh" || true
