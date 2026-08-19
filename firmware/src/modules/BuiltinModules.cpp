// =============================================================================
//  modules/BuiltinModules.cpp — THE list of module types in this firmware.
//
//  Adding a supported device is a two-line change here plus the driver itself.
//  Nothing else in the firmware, and nothing at all in the frontend, needs to
//  know the new module exists: the UI is generated from its manifest.
//
//  Milestone 0 ships the software modules only (they need no hardware and are
//  fully unit-tested).  Milestone 2 adds HX711 / AHT20 / BMP280 / DS18B20 /
//  analog input; Milestone 7 the outputs; Milestone 8 PID and rules.
// =============================================================================
#include "core/ModuleRegistry.h"
#include "modules/outputs/AnalogOutputs.h"
#include "modules/outputs/DigitalOutputs.h"
#include "modules/processing/CalibrationProcessor.h"
#include "modules/processing/FilterProcessors.h"
#include "modules/processing/MathProcessors.h"
#include "modules/processing/MovingAverageProcessor.h"
#include "modules/sensors/Aht20Driver.h"
#include "modules/sensors/AdditionalSensors.h"
#include "modules/sensors/BasicInputs.h"
#include "modules/sensors/Bmp280Driver.h"
#include "modules/sensors/Hx711Driver.h"
#include "modules/sensors/SignalSimulator.h"

namespace lc {

void registerBuiltinModules(ModuleRegistry& registry) {
  using namespace modules;

  // --- sensors -------------------------------------------------------------
  registry.add(ModuleDescriptor{&SignalSimulator::manifest(),
                                &SignalSimulator::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&Hx711Driver::manifest(),
                                &Hx711Driver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&Aht20Driver::manifest(),
                                &Aht20Driver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&Bmp280Driver::manifest(),
                                &Bmp280Driver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&Bme280Driver::manifest(),
                                &Bme280Driver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&Ds18b20Driver::manifest(),
                                &Ds18b20Driver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&Dht11Driver::manifest(),
                                &Dht11Driver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&Vl53l0xDriver::manifest(),
                                &Vl53l0xDriver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&Mlx90614Driver::manifest(),
                                &Mlx90614Driver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&AnalogInputDriver::manifest(),
                                &AnalogInputDriver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&DigitalInputDriver::manifest(),
                                &DigitalInputDriver::create, nullptr, nullptr});

  // --- outputs (§27) -------------------------------------------------------
  // Every one of these declares a safe state in its manifest; DeviceManager
  // registers it with the safety layer before the driver instance is even
  // created, so a driver cannot forget (ADR-0016).
  registry.add(ModuleDescriptor{&DigitalOutputDriver::manifest(),
                                &DigitalOutputDriver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&RelayDriver::manifest(),
                                &RelayDriver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&PwmOutputDriver::manifest(),
                                &PwmOutputDriver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&HeaterDriver::manifest(),
                                &HeaterDriver::create, nullptr, nullptr});
  registry.add(ModuleDescriptor{&FanDriver::manifest(),
                                &FanDriver::create, nullptr, nullptr});

  // --- processing ----------------------------------------------------------
  registry.add(ModuleDescriptor{&CalibrationProcessor::manifest(), nullptr,
                                &CalibrationProcessor::create, nullptr});
  registry.add(ModuleDescriptor{&MovingAverageProcessor::manifest(), nullptr,
                                &MovingAverageProcessor::create, nullptr});
  registry.add(ModuleDescriptor{&MedianProcessor::manifest(), nullptr,
                                &MedianProcessor::create, nullptr});
  registry.add(ModuleDescriptor{&LowPassProcessor::manifest(), nullptr,
                                &LowPassProcessor::create, nullptr});
  registry.add(ModuleDescriptor{&DeadbandProcessor::manifest(), nullptr,
                                &DeadbandProcessor::create, nullptr});
  registry.add(ModuleDescriptor{&DerivativeProcessor::manifest(), nullptr,
                                &DerivativeProcessor::create, nullptr});
  registry.add(ModuleDescriptor{&IntegralProcessor::manifest(), nullptr,
                                &IntegralProcessor::create, nullptr});
  registry.add(ModuleDescriptor{&StatisticsProcessor::manifest(), nullptr,
                                &StatisticsProcessor::create, nullptr});
  registry.add(ModuleDescriptor{&ClampProcessor::manifest(), nullptr,
                                &ClampProcessor::create, nullptr});

}

}  // namespace lc
