# tools/esp32_typecheck — compiling the ESP32-only code without an ESP32

Everything in `firmware/src` that is Arduino-free is compiled and tested on the
host by `tools/run_tests.sh`.  Six files were not:

    src/buses/esp32/Esp32BusProvider.cpp
    src/buses/esp32/WireI2cBus.cpp
    src/api/esp32/PsychicHttpAdapter.cpp
    src/platform/esp32/WifiManager.cpp
    src/storage/LittleFsBackend.cpp
    src/main.cpp

They were written, reviewed, and never compiled by anything, because compiling
them needed a toolchain and a package registry this project's CI does not have.
The first real bring-up on hardware found, in these six files alone: a missing
pair of closing braces, four calls to a web-server API that had changed between
major versions, and a filesystem API used with the wrong argument.  Every one of
them is a compile error, and every one of them cost an afternoon because nothing
ever tried to compile.

This directory fixes that.  `stubs/` contains declaration-only versions of the
Arduino and ESP-IDF headers those files include — `String`, `fs::File`,
`Preferences`, `httpd_req_t` and friends.  Nothing here runs; nothing here is
linked.  `check.sh` runs the compiler in `-fsyntax-only` mode over the six files
with `stubs/` on the include path and the **real** PsychicHttp and ArduinoJson
headers next to it.

That distinction is the whole point:

* The Arduino and ESP-IDF stubs are ours, so they only prove the code is
  internally consistent — a brace, a type, a member that does not exist.
* PsychicHttp and ArduinoJson are the genuine articles at the pinned versions,
  so the check also proves the code agrees with the libraries it is compiled
  against.  A handler with the wrong signature, a constructor that lost an
  argument, an enumerator that was deleted two releases ago — all of it fails
  here, on a laptop, in four seconds.

What it still cannot prove: that the firmware fits in DRAM, that a peripheral
behaves, or that ESP-IDF's HTTP server does what its header says.  Those need
the board.  It is a syntax and API check, and it is worth exactly what it says
on the tin — which is a great deal more than nothing, which is what was there
before.

## Running it

    tools/esp32_typecheck/check.sh

Set `PSYCHIC_SRC` and `ARDUINOJSON_SRC` to point at the libraries; the script
tells you where it expects them if they are missing.
