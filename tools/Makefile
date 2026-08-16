# Host-side build of the firmware's API, for developing the web interface
# without a board.  See host_server.cpp for what it is and is not.
FIRMWARE := ../firmware/src
ARDUINOJSON ?= /tmp/aj/src

SOURCES := \
  $(wildcard $(FIRMWARE)/core/*.cpp) \
  $(wildcard $(FIRMWARE)/services/*.cpp) \
  $(wildcard $(FIRMWARE)/buses/*.cpp) \
  $(wildcard $(FIRMWARE)/modules/*.cpp) \
  $(wildcard $(FIRMWARE)/modules/*/*.cpp) \
  $(wildcard $(FIRMWARE)/app/*.cpp) \
  $(wildcard $(FIRMWARE)/api/*.cpp) \
  $(FIRMWARE)/storage/ConfigStorage.cpp \
  $(FIRMWARE)/storage/ConfigApplier.cpp \
  $(FIRMWARE)/storage/CalibrationStore.cpp \
  $(FIRMWARE)/storage/ControlStore.cpp \
  $(FIRMWARE)/storage/ExperimentStore.cpp \
  $(FIRMWARE)/storage/RunLog.cpp \
  $(FIRMWARE)/storage/LogStore.cpp \
  $(FIRMWARE)/storage/DashboardStore.cpp \
  $(FIRMWARE)/storage/PosixBackend.cpp \
  $(FIRMWARE)/platform/host/HostClock.cpp \
  host_server.cpp

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
            -D LC_CONFIG_SCHEMA_VERSION=1 -D 'LC_FIRMWARE_VERSION="host"' \
            -I $(FIRMWARE) -I $(ARDUINOJSON)

host-server:
	$(CXX) $(CXXFLAGS) $(SOURCES) -o host_server -pthread

clean:
	rm -f host_server

.PHONY: host-server clean
