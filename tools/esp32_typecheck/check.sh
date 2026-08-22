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

# ARDUINO is defined by the real PlatformIO build, and PsychicHttp 3.x branches
# on it: without it the library selects its native-ESP-IDF filesystem shim and
# PsychicFileResponse loses the fs::FS overload the adapter calls.  Compiling
# without it would report a break that does not exist on the target — and, worse,
# could hide one that does.
FLAGS="-std=gnu++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter
  -D ARDUINO=200
  -D LC_TARGET_ESP32=1 -D LC_HAS_PSRAM=0
  -D PSYCHIC_WS_MAX_PENDING_FRAMES=4
  -D LC_FIRMWARE_VERSION=\"typecheck\" -D LC_CONFIG_SCHEMA_VERSION=1
  -D ARDUINOJSON_ENABLE_STD_STRING=1
  -I $root/firmware/src
  -I $here/stubs
  -I $PSYCHIC_SRC
  -I $ARDUINOJSON_SRC"

CXX="${CXX:-g++}"
status=0

# Optionally compile the WebSocket adapter against a SECOND PsychicHttp as well.
# 0.15.2-m15 moved from 2.2.0 to 3.1.2 for the bounded WebSocket send queue, and
# 3.x changed the PsychicFileResponse constructor.  Checking both means the
# upgrade stays reversible: if 3.x turns out to misbehave on the bench, reverting
# the pin is a one-line change that is known to still compile.
#   PSYCHIC_ALT_SRC=/path/to/other/src ./check.sh
PSYCHIC_ALT_SRC="${PSYCHIC_ALT_SRC:-}"

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

if [ -n "$PSYCHIC_ALT_SRC" ]; then
  if [ ! -d "$PSYCHIC_ALT_SRC" ]; then
    echo "PSYCHIC_ALT_SRC is set but $PSYCHIC_ALT_SRC does not exist" >&2
    exit 2
  fi
  ALT_FLAGS="${FLAGS/-I $PSYCHIC_SRC/-I $PSYCHIC_ALT_SRC}"
  echo
  echo "against the alternate PsychicHttp ($PSYCHIC_ALT_SRC):"
  for file in src/api/esp32/PsychicHttpAdapter.cpp src/main.cpp; do
    printf '%-45s ' "$file"
    # shellcheck disable=SC2086
    if $CXX $ALT_FLAGS "$root/firmware/$file" 2> /tmp/typecheck.err; then
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
fi

exit $status
