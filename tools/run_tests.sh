#!/usr/bin/env bash
# =============================================================================
#  tools/run_tests.sh — the whole firmware test suite on a host, no board.
#
#  Six suites, built from the real firmware sources with the POSIX storage
#  backend swapped in for LittleFS.  `pio test -e native` runs the same files
#  through Unity; this script exists because it needs nothing but a compiler,
#  which is what makes it the thing that actually gets run.
#
#  Usage
#      tools/run_tests.sh                 # plain build, warnings are failures
#      SAN=1 tools/run_tests.sh           # AddressSanitizer + UBSan
#
#  Set ARDUINOJSON_SRC if the library is not at /tmp/aj/src.
#
#  This does NOT compile the ESP32-only files; tools/esp32_typecheck/check.sh
#  does, and both are worth running before calling a change finished.
# =============================================================================
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
firmware="$(cd "$here/../firmware" && pwd)"

ARDUINOJSON_SRC="${ARDUINOJSON_SRC:-/tmp/aj/src}"
UNITY_SHIM="${UNITY_SHIM:-/tmp/unityshim}"

if [ ! -d "$ARDUINOJSON_SRC" ]; then
  echo "ArduinoJson sources not found at $ARDUINOJSON_SRC" >&2
  echo "  git clone --depth 1 --branch v7.4.3 https://github.com/bblanchon/ArduinoJson /tmp/aj" >&2
  exit 2
fi

CXX="${CXX:-g++}"
FLAGS="-std=c++17 -Wall -Wextra -Wshadow -Wno-unused-parameter
  -D LC_TARGET_HOST=1 -D LC_CONFIG_SCHEMA_VERSION=1
  -D LC_FIRMWARE_VERSION='\"test\"'
  -I $firmware/src -I $ARDUINOJSON_SRC"

# Unity itself is not vendored.  Point UNITY_SHIM at a real unity.h (PlatformIO
# keeps one under ~/.platformio/packages) or at the minimal shim used in
# development.
if [ -d "$UNITY_SHIM" ]; then
  FLAGS="$FLAGS -I $UNITY_SHIM"
fi

if [ "${SAN:-0}" = "1" ]; then
  FLAGS="$FLAGS -g -fsanitize=address,undefined -fno-sanitize-recover=all"
fi

SRC="$firmware/src/core/*.cpp
  $firmware/src/services/*.cpp
  $firmware/src/modules/*/*.cpp
  $firmware/src/modules/BuiltinModules.cpp
  $firmware/src/storage/ConfigStorage.cpp
  $firmware/src/storage/ConfigApplier.cpp
  $firmware/src/storage/CalibrationStore.cpp
  $firmware/src/storage/ControlStore.cpp
  $firmware/src/storage/ExperimentStore.cpp
  $firmware/src/storage/RunLog.cpp
  $firmware/src/storage/LogStore.cpp
  $firmware/src/storage/CloudUploadQueue.cpp
  $firmware/src/storage/DashboardStore.cpp
  $firmware/src/storage/PosixBackend.cpp
  $firmware/src/app/*.cpp
  $firmware/src/api/*.cpp
  $firmware/src/buses/*.cpp
  $firmware/src/platform/host/*.cpp"

status=0
for suite in core services devices storage modules api cloud; do
  out="$(mktemp -t "lc_test_${suite}.XXXXXX")"
  warnings="$(mktemp -t "lc_warn_${suite}.XXXXXX")"
  # shellcheck disable=SC2086
  if ! eval "$CXX" $FLAGS $SRC "$firmware/test/test_$suite/test_$suite.cpp" \
      -o "$out" 2> "$warnings"; then
    echo "BUILD FAIL $suite"
    tail -40 "$warnings"
    exit 1
  fi
  if [ -s "$warnings" ]; then
    echo "--- warnings in $suite ---"
    cat "$warnings"
    status=1
  fi
  printf '%-10s ' "$suite"
  # Run it once, remember the verdict, and KEEP GOING.  `set -euo pipefail`
  # used to end the script at the first failing suite, so a failure in `core`
  # meant the other six were never even built — the run said less the more
  # there was wrong with the tree.
  log="$out.log"
  if "$out" > "$log" 2>&1; then tail -1 "$log"; else tail -1 "$log"; status=1; fi
  rm -f "$out" "$warnings" "$log"
done
exit $status
