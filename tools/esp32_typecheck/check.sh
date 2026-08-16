#!/usr/bin/env bash
# =============================================================================
#  tools/esp32_typecheck/check.sh — compile the ESP32-only sources on the host.
#
#  -fsyntax-only, so nothing is linked and nothing runs.  What it proves is
#  narrow and was worth an afternoon of hardware bring-up: the ESP32 files
#  parse, their braces balance, and they agree with the REAL PsychicHttp and
#  ArduinoJson headers at the versions platformio.ini pins.
#
#  See README.md in this directory for what it deliberately cannot prove.
# =============================================================================
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"

PSYCHIC_SRC="${PSYCHIC_SRC:-/tmp/psychic/src}"
ARDUINOJSON_SRC="${ARDUINOJSON_SRC:-/tmp/aj/src}"

missing=0
for dir in "$PSYCHIC_SRC" "$ARDUINOJSON_SRC"; do
  if [ ! -d "$dir" ]; then
    echo "missing library sources: $dir" >&2
    missing=1
  fi
done
if [ "$missing" != "0" ]; then
  cat >&2 <<'HINT'

This check compiles against the real libraries, not against stubs of them.
Fetch them at the versions pinned in firmware/platformio.ini:

  git clone --depth 1 --branch v7.4.3 https://github.com/bblanchon/ArduinoJson /tmp/aj
  git clone https://github.com/hoeken/PsychicHttp /tmp/psychic && \
      git -C /tmp/psychic checkout 2.2.0

then re-run, or set PSYCHIC_SRC / ARDUINOJSON_SRC.
HINT
  exit 2
fi

FILES="
  src/buses/esp32/Esp32BusProvider.cpp
  src/buses/esp32/WireI2cBus.cpp
  src/api/esp32/PsychicHttpAdapter.cpp
  src/platform/esp32/WifiManager.cpp
  src/storage/LittleFsBackend.cpp
  src/main.cpp
"

FLAGS="-std=gnu++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter
  -D LC_TARGET_ESP32=1 -D LC_HAS_PSRAM=0
  -D LC_FIRMWARE_VERSION=\"typecheck\" -D LC_CONFIG_SCHEMA_VERSION=1
  -D ARDUINOJSON_ENABLE_STD_STRING=1
  -I $root/firmware/src
  -I $here/stubs
  -I $PSYCHIC_SRC
  -I $ARDUINOJSON_SRC"

CXX="${CXX:-g++}"
status=0
for file in $FILES; do
  printf '%-45s ' "$file"
  # shellcheck disable=SC2086
  if $CXX $FLAGS "$root/firmware/$file" 2> /tmp/typecheck.err; then
    if [ -s /tmp/typecheck.err ]; then
      echo "WARN"
      cat /tmp/typecheck.err
      status=1
    else
      echo "ok"
    fi
  else
    echo "FAIL"
    head -40 /tmp/typecheck.err
    status=1
  fi
done
exit $status
