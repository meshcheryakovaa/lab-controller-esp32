// =============================================================================
//  Milestone 2 — hardware drivers, tested without hardware.
//      pio test -e native
// =============================================================================
#include <unity.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "FakeBuses.h"
#include "buses/I2cScanner.h"
#include "core/Clock.h"
#include "modules/sensors/Aht20Driver.h"
#include "modules/sensors/BasicInputs.h"
#include "modules/sensors/Bmp280Driver.h"
#include "modules/sensors/Hx711Driver.h"
#include "services/DeviceManager.h"

using namespace lc;
using namespace lc::modules;

void setUp() {}
void tearDown() {}

namespace {

class MapConfig final : public IConfigView {
 public:
  MapConfig& set(const char* key, const std::string& value) {
    order_.push_back(key);
    scalars_[key] = value;
    return *this;
  }
  bool has(const char* key) const override { return scalars_.count(key) > 0; }
  std::int32_t getInt(const char* key, std::int32_t fallback) const override {
    auto it = scalars_.find(key);
    return it == scalars_.end() ? fallback
                                : static_cast<std::int32_t>(
                                      std::strtol(it->second.c_str(), nullptr, 0));
  }
  float getFloat(const char* key, float fallback) const override {
    auto it = scalars_.find(key);
    return it == scalars_.end() ? fallback
                                : static_cast<float>(std::atof(it->second.c_str()));
  }
  bool getBool(const char* key, bool fallback) const override {
    auto it = scalars_.find(key);
    return it == scalars_.end() ? fallback : (it->second == "true");
  }
  const char* getString(const char* key, const char* fallback) const override {
    auto it = scalars_.find(key);
    return it == scalars_.end() ? fallback : it->second.c_str();
  }
  std::size_t arraySize(const char*) const override { return 0; }
  float getFloatAt(const char*, std::size_t, float fallback) const override {
    return fallback;
  }
  std::size_t keyCount() const override { return order_.size(); }
  const char* keyAt(std::size_t index) const override {
    return index < order_.size() ? order_[index].c_str() : nullptr;
  }

 private:
  std::map<std::string, std::string> scalars_;
  std::vector<std::string> order_;
};

// Minimal harness: a channel manager plus a hand-built DeviceContext, so a
// driver can be exercised in isolation from DeviceManager.
struct DriverRig {
  ManualClock clock;
  ChannelManager channels{clock};
  ResourceManager resources{ChipProfile::esp32()};
  EventBus events;
  test::FakeBusProvider buses;
  ChannelHandle handles[4] = {kInvalidChannel};
  std::uint8_t handleCount = 0;

  ChannelHandle addChannel(const char* key) {
    ChannelDescriptor descriptor;
    descriptor.key.assign(key);
    descriptor.name.assign(key);
    const ChannelHandle handle = channels.create(descriptor).value();
    handles[handleCount++] = handle;
    return handle;
  }

  DeviceContext context(const ModuleManifest& manifest, const IConfigView& config) {
    DeviceContext ctx;
    ctx.self = 1;
    ctx.manifest = &manifest;
    ctx.config = &config;
    ctx.clock = &clock;
    ctx.resources = &resources;
    ctx.channels = &channels;
    ctx.events = &events;
    ctx.buses = &buses;
    ctx.channelHandles = handles;
    ctx.channelCount = handleCount;
    return ctx;
  }
};

// AHT20 frame builder: humidity and temperature as 20-bit raw values.
std::vector<std::uint8_t> aht20Frame(std::uint8_t status, std::uint32_t humidity,
                                     std::uint32_t temperature,
                                     bool corruptCrc = false) {
  std::vector<std::uint8_t> frame(7, 0);
  frame[0] = status;
  frame[1] = static_cast<std::uint8_t>((humidity >> 12) & 0xFF);
  frame[2] = static_cast<std::uint8_t>((humidity >> 4) & 0xFF);
  frame[3] = static_cast<std::uint8_t>(((humidity & 0x0F) << 4) |
                                       ((temperature >> 16) & 0x0F));
  frame[4] = static_cast<std::uint8_t>((temperature >> 8) & 0xFF);
  frame[5] = static_cast<std::uint8_t>(temperature & 0xFF);
  frame[6] = Aht20Protocol::crc8(frame.data(), 6);
  if (corruptCrc) frame[6] = static_cast<std::uint8_t>(frame[6] ^ 0xFF);
  return frame;
}

// ---------------------------------------------------------------------------
//  Independent floating-point reference for the BMP280, straight from the
//  datasheet's "double" listing.  Structurally different from the integer
//  implementation in the driver, so agreement between them is real evidence.
// ---------------------------------------------------------------------------
double referenceTemperature(std::int32_t adcT, const Bmp280Calibration& c,
                            double& tFine) {
  const double var1 = (static_cast<double>(adcT) / 16384.0 -
                       static_cast<double>(c.t1) / 1024.0) * static_cast<double>(c.t2);
  const double d = static_cast<double>(adcT) / 131072.0 -
                   static_cast<double>(c.t1) / 8192.0;
  const double var2 = d * d * static_cast<double>(c.t3);
  tFine = var1 + var2;
  return tFine / 5120.0;
}

double referencePressure(std::int32_t adcP, const Bmp280Calibration& c,
                         double tFine) {
  double var1 = tFine / 2.0 - 64000.0;
  double var2 = var1 * var1 * static_cast<double>(c.p6) / 32768.0;
  var2 = var2 + var1 * static_cast<double>(c.p5) * 2.0;
  var2 = var2 / 4.0 + static_cast<double>(c.p4) * 65536.0;
  var1 = (static_cast<double>(c.p3) * var1 * var1 / 524288.0 +
          static_cast<double>(c.p2) * var1) / 524288.0;
  var1 = (1.0 + var1 / 32768.0) * static_cast<double>(c.p1);
  if (var1 == 0.0) return 0.0;
  double pressure = 1048576.0 - static_cast<double>(adcP);
  pressure = (pressure - var2 / 4096.0) * 6250.0 / var1;
  var1 = static_cast<double>(c.p9) * pressure * pressure / 2147483648.0;
  var2 = pressure * static_cast<double>(c.p8) / 32768.0;
  return pressure + (var1 + var2 + static_cast<double>(c.p7)) / 16.0;
}

Bmp280Calibration referenceCalibration() {
  Bmp280Calibration c;
  c.t1 = 27504; c.t2 = 26435; c.t3 = -1000;
  c.p1 = 36477; c.p2 = -10685; c.p3 = 3024; c.p4 = 2855; c.p5 = 140;
  c.p6 = -7; c.p7 = 15500; c.p8 = -14600; c.p9 = 6000;
  return c;
}

std::vector<std::uint8_t> calibrationBytes(const Bmp280Calibration& c) {
  auto push = [](std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
  };
  std::vector<std::uint8_t> raw;
  push(raw, c.t1);
  push(raw, static_cast<std::uint16_t>(c.t2));
  push(raw, static_cast<std::uint16_t>(c.t3));
  push(raw, c.p1);
  push(raw, static_cast<std::uint16_t>(c.p2));
  push(raw, static_cast<std::uint16_t>(c.p3));
  push(raw, static_cast<std::uint16_t>(c.p4));
  push(raw, static_cast<std::uint16_t>(c.p5));
  push(raw, static_cast<std::uint16_t>(c.p6));
  push(raw, static_cast<std::uint16_t>(c.p7));
  push(raw, static_cast<std::uint16_t>(c.p8));
  push(raw, static_cast<std::uint16_t>(c.p9));
  return raw;
}

}  // namespace

// ===========================================================================
//  I²C scanner
// ===========================================================================
static void test_scanner_finds_devices_and_offers_careful_hints() {
  test::FakeI2cBus bus;
  bus.attach(0x38);
  bus.attach(0x76);
  bus.attach(0x5A);  // no hint for this one

  I2cScanEntry results[8];
  const std::size_t found = I2cScanner::scan(bus, results, 8);
  TEST_ASSERT_EQUAL_UINT(3, found);
  TEST_ASSERT_EQUAL_UINT(0x38, results[0].address);
  TEST_ASSERT_EQUAL_UINT(0x5A, results[1].address);
  TEST_ASSERT_EQUAL_UINT(0x76, results[2].address);

  // 0x38 is characteristic of the AHT20.
  TEST_ASSERT_EQUAL_UINT(1, results[0].hintCount);
  TEST_ASSERT_EQUAL_STRING("aht20", results[0].hints[0].moduleId);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HintConfidence::kLikely),
                        static_cast<int>(results[0].hints[0].confidence));

  // 0x76 is shared: the scanner must offer both and claim neither.
  TEST_ASSERT_EQUAL_UINT(2, results[2].hintCount);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HintConfidence::kPossible),
                        static_cast<int>(results[2].hints[0].confidence));

  // An unknown address is reported, just without a guess.
  TEST_ASSERT_EQUAL_UINT(0, results[1].hintCount);
  TEST_ASSERT_NULL(results[1].hints);
}

// ===========================================================================
//  AHT20
// ===========================================================================
static void test_aht20_crc_matches_the_standard_check_value() {
  // CRC-8/NRSC-5 (poly 0x31, init 0xFF): the catalogue check value for the
  // ASCII string "123456789" is 0xF7.  An external reference, not our own math.
  const std::uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_UINT(0xF7, Aht20Protocol::crc8(data, sizeof(data)));
}

static void test_aht20_decodes_a_known_frame() {
  // Half scale in both fields: 50 %RH and 50 °C by construction.
  const std::vector<std::uint8_t> frame = aht20Frame(0x1C, 0x80000, 0x80000);
  Aht20Reading reading;
  TEST_ASSERT_TRUE(Aht20Protocol::decode(frame.data(), reading));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, reading.humidityPercent);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.0f, reading.temperatureCelsius);

  // Zero scale: 0 %RH and -50 °C, the bottom of the range.
  const std::vector<std::uint8_t> cold = aht20Frame(0x1C, 0x00000, 0x00000);
  TEST_ASSERT_TRUE(Aht20Protocol::decode(cold.data(), reading));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, reading.humidityPercent);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -50.0f, reading.temperatureCelsius);
}

static void test_aht20_rejects_bad_crc_and_busy_frames() {
  Aht20Reading reading;
  const std::vector<std::uint8_t> corrupt = aht20Frame(0x1C, 0x40000, 0x40000, true);
  TEST_ASSERT_FALSE(Aht20Protocol::decode(corrupt.data(), reading));

  const std::vector<std::uint8_t> busy = aht20Frame(0x9C, 0x40000, 0x40000);
  TEST_ASSERT_FALSE(Aht20Protocol::decode(busy.data(), reading));
}

static void test_aht20_reports_a_missing_sensor_clearly() {
  DriverRig rig;
  rig.addChannel("t");
  rig.addChannel("rh");
  MapConfig config;
  config.set("bus", "0").set("address", "0x38");

  Aht20Driver driver;
  TEST_ASSERT_TRUE(driver.configure(rig.context(Aht20Driver::manifest(), config)).ok());

  // Nothing attached to the fake bus: begin() must say so, not hang.
  const Status status = driver.begin();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDeviceNotResponding),
                        static_cast<int>(status.code));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kError),
                        static_cast<int>(driver.state()));
  TEST_ASSERT_EQUAL_STRING("no ACK from 0x38", status.detail.c_str());
}

static void test_aht20_measures_without_ever_blocking() {
  DriverRig rig;
  const ChannelHandle temperature = rig.addChannel("t");
  const ChannelHandle humidity = rig.addChannel("rh");
  MapConfig config;
  config.set("bus", "0").set("address", "0x38");

  rig.buses.i2cBus.attach(0x38);
  rig.buses.i2cBus.queueRead(0x38, {0x1C});  // status: calibrated, not busy

  Aht20Driver driver;
  TEST_ASSERT_TRUE(driver.configure(rig.context(Aht20Driver::manifest(), config)).ok());
  TEST_ASSERT_TRUE(driver.begin().ok());

  // First poll only starts a conversion; nothing is published yet.
  driver.poll(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ChannelQuality::kUnknown),
                        static_cast<int>(rig.channels.value(temperature)->quality));

  // Polling again before the conversion is done must not read the part.
  rig.clock.advanceMicros(10000);
  driver.poll(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_UINT(0, rig.channels.value(temperature)->sequence);

  rig.buses.i2cBus.queueRead(0x38, aht20Frame(0x1C, 0x60000, 0x50000));
  rig.clock.advanceMicros(80000);
  driver.poll(rig.clock.nowMicros());

  TEST_ASSERT_EQUAL_UINT(1, rig.channels.value(temperature)->sequence);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 12.5f, rig.channels.value(temperature)->processed);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 37.5f, rig.channels.value(humidity)->processed);
}

static void test_aht20_degrades_before_it_fails() {
  DriverRig rig;
  rig.addChannel("t");
  rig.addChannel("rh");
  MapConfig config;
  config.set("bus", "0").set("address", "0x38");

  rig.buses.i2cBus.attach(0x38);
  rig.buses.i2cBus.queueRead(0x38, {0x1C});

  Aht20Driver driver;
  TEST_ASSERT_TRUE(driver.configure(rig.context(Aht20Driver::manifest(), config)).ok());
  TEST_ASSERT_TRUE(driver.begin().ok());

  // One failed transaction is a rattled cable, not a dead sensor.
  rig.buses.i2cBus.failNext(1);
  driver.poll(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kWarning),
                        static_cast<int>(driver.state()));

  rig.buses.i2cBus.failNext(2);
  driver.poll(rig.clock.nowMicros());
  driver.poll(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kError),
                        static_cast<int>(driver.state()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDeviceNotResponding),
                        static_cast<int>(driver.lastError().code));
}

// ===========================================================================
//  BMP280
// ===========================================================================
static void test_bmp280_integer_compensation_matches_the_double_reference() {
  const Bmp280Calibration c = referenceCalibration();

  // Sweep the plausible ADC range rather than checking one remembered pair.
  for (std::int32_t adcT = 300000; adcT <= 700000; adcT += 37000) {
    for (std::int32_t adcP = 250000; adcP <= 500000; adcP += 23000) {
      std::int32_t tFineInt = 0;
      const std::int32_t temperature =
          Bmp280Protocol::compensateTemperature(adcT, c, tFineInt);
      const std::uint32_t pressure =
          Bmp280Protocol::compensatePressure(adcP, c, tFineInt);

      double tFineDouble = 0.0;
      const double referenceT = referenceTemperature(adcT, c, tFineDouble);
      const double referenceP = referencePressure(adcP, c, tFineDouble);

      TEST_ASSERT_FLOAT_WITHIN(0.02f, static_cast<float>(referenceT),
                               static_cast<float>(temperature) * 0.01f);
      TEST_ASSERT_FLOAT_WITHIN(2.0f, static_cast<float>(referenceP),
                               static_cast<float>(pressure) / 256.0f);
    }
  }
}

static void test_bmp280_calibration_round_trips_through_the_register_block() {
  const Bmp280Calibration original = referenceCalibration();
  const std::vector<std::uint8_t> raw = calibrationBytes(original);
  const Bmp280Calibration parsed = Bmp280Calibration::parse(raw.data());

  TEST_ASSERT_EQUAL_INT(original.t1, parsed.t1);
  TEST_ASSERT_EQUAL_INT(original.t2, parsed.t2);
  TEST_ASSERT_EQUAL_INT(original.t3, parsed.t3);   // negative, sign matters
  TEST_ASSERT_EQUAL_INT(original.p6, parsed.p6);   // negative
  TEST_ASSERT_EQUAL_INT(original.p8, parsed.p8);   // negative
  TEST_ASSERT_EQUAL_INT(original.p9, parsed.p9);
  TEST_ASSERT_TRUE(parsed.plausible());

  std::uint8_t zeros[24] = {0};
  TEST_ASSERT_FALSE(Bmp280Calibration::parse(zeros).plausible());
}

static void test_bmp280_identifies_the_chip_before_trusting_it() {
  DriverRig rig;
  rig.addChannel("t");
  rig.addChannel("p");
  MapConfig config;
  config.set("bus", "0").set("address", "0x76");

  rig.buses.i2cBus.attach(0x76);
  rig.buses.i2cBus.setRegister(0x76, Bmp280Protocol::kRegChipId,
                               Bmp280Protocol::kChipIdBme280);

  Bmp280Driver driver;
  TEST_ASSERT_TRUE(driver.configure(rig.context(Bmp280Driver::manifest(), config)).ok());

  // The scan hint said "maybe BMP280". The chip ID says otherwise, and the
  // message says exactly what was found.
  const Status status = driver.begin();
  TEST_ASSERT_FALSE(status.ok());
  TEST_ASSERT_EQUAL_STRING("this is a BME280, not a BMP280", status.detail.c_str());
}

static void test_bmp280_publishes_compensated_values() {
  DriverRig rig;
  const ChannelHandle temperature = rig.addChannel("t");
  const ChannelHandle pressure = rig.addChannel("p");
  MapConfig config;
  config.set("bus", "0").set("address", "0x76");

  const Bmp280Calibration c = referenceCalibration();
  rig.buses.i2cBus.attach(0x76);
  rig.buses.i2cBus.setRegister(0x76, Bmp280Protocol::kRegChipId,
                               Bmp280Protocol::kChipIdBmp280);
  rig.buses.i2cBus.setRegisters(0x76, Bmp280Protocol::kRegCalibration,
                                calibrationBytes(c));

  // adc_P = 415148, adc_T = 519888 — a typical indoor reading.
  const std::int32_t adcP = 415148;
  const std::int32_t adcT = 519888;
  rig.buses.i2cBus.setRegisters(
      0x76, Bmp280Protocol::kRegData,
      {static_cast<std::uint8_t>(adcP >> 12),
       static_cast<std::uint8_t>((adcP >> 4) & 0xFF),
       static_cast<std::uint8_t>((adcP & 0x0F) << 4),
       static_cast<std::uint8_t>(adcT >> 12),
       static_cast<std::uint8_t>((adcT >> 4) & 0xFF),
       static_cast<std::uint8_t>((adcT & 0x0F) << 4)});

  Bmp280Driver driver;
  TEST_ASSERT_TRUE(driver.configure(rig.context(Bmp280Driver::manifest(), config)).ok());
  TEST_ASSERT_TRUE(driver.begin().ok());

  rig.clock.advanceMicros(200000);
  driver.poll(rig.clock.nowMicros());

  std::int32_t tFine = 0;
  const float expectedT =
      static_cast<float>(Bmp280Protocol::compensateTemperature(adcT, c, tFine)) * 0.01f;
  const float expectedP =
      static_cast<float>(Bmp280Protocol::compensatePressure(adcP, c, tFine)) / 256.0f;

  TEST_ASSERT_FLOAT_WITHIN(0.01f, expectedT, rig.channels.value(temperature)->processed);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, expectedP, rig.channels.value(pressure)->processed);
  // Sanity: a room, not the surface of Venus.
  TEST_ASSERT_TRUE(rig.channels.value(temperature)->processed > 0.0f);
  TEST_ASSERT_TRUE(rig.channels.value(temperature)->processed < 60.0f);
  TEST_ASSERT_TRUE(rig.channels.value(pressure)->processed > 80000.0f);
  TEST_ASSERT_TRUE(rig.channels.value(pressure)->processed < 110000.0f);
}

static void test_bmp280_does_not_publish_the_reset_value() {
  DriverRig rig;
  const ChannelHandle temperature = rig.addChannel("t");
  rig.addChannel("p");
  MapConfig config;
  config.set("bus", "0").set("address", "0x76");

  rig.buses.i2cBus.attach(0x76);
  rig.buses.i2cBus.setRegister(0x76, Bmp280Protocol::kRegChipId,
                               Bmp280Protocol::kChipIdBmp280);
  rig.buses.i2cBus.setRegisters(0x76, Bmp280Protocol::kRegCalibration,
                                calibrationBytes(referenceCalibration()));
  // 0x80000 in both fields: the part has not finished its first conversion.
  rig.buses.i2cBus.setRegisters(0x76, Bmp280Protocol::kRegData,
                                {0x80, 0x00, 0x00, 0x80, 0x00, 0x00});

  Bmp280Driver driver;
  TEST_ASSERT_TRUE(driver.configure(rig.context(Bmp280Driver::manifest(), config)).ok());
  TEST_ASSERT_TRUE(driver.begin().ok());
  driver.poll(rig.clock.nowMicros());

  // Publishing it would have put roughly -145 °C on the dashboard.
  TEST_ASSERT_EQUAL_UINT(0, rig.channels.value(temperature)->sequence);
}

// ===========================================================================
//  HX711
// ===========================================================================
static void test_hx711_sign_extension() {
  TEST_ASSERT_EQUAL_INT(0, Hx711Protocol::signExtend24(0x000000));
  TEST_ASSERT_EQUAL_INT(1, Hx711Protocol::signExtend24(0x000001));
  TEST_ASSERT_EQUAL_INT(8388607, Hx711Protocol::signExtend24(0x7FFFFF));
  // The line that everybody gets wrong: without sign extension this reads
  // +8388608 instead of the smallest negative value.
  TEST_ASSERT_EQUAL_INT(-8388608, Hx711Protocol::signExtend24(0x800000));
  TEST_ASSERT_EQUAL_INT(-1, Hx711Protocol::signExtend24(0xFFFFFF));
  TEST_ASSERT_EQUAL_INT(-2, Hx711Protocol::signExtend24(0xFFFFFE));
}

static void test_hx711_gain_selects_the_pulse_count() {
  TEST_ASSERT_EQUAL_UINT(1, Hx711Protocol::pulsesForGain(128));
  TEST_ASSERT_EQUAL_UINT(2, Hx711Protocol::pulsesForGain(32));
  TEST_ASSERT_EQUAL_UINT(3, Hx711Protocol::pulsesForGain(64));
}

static void test_hx711_reads_a_scripted_conversion() {
  DriverRig rig;
  const ChannelHandle mass = rig.addChannel("mass");
  MapConfig config;
  config.set("data_pin", "16").set("clock_pin", "17").set("gain", "64")
        .set("rate_hz", "10");

  Hx711Driver driver;
  TEST_ASSERT_TRUE(driver.configure(rig.context(Hx711Driver::manifest(), config)).ok());
  TEST_ASSERT_TRUE(driver.begin().ok());

  // DOUT low = ready, then 24 bits of 0xFFFFFE => -2 counts.
  std::vector<bool> script;
  script.push_back(false);
  const std::uint32_t raw = 0xFFFFFE;
  for (int bit = 23; bit >= 0; --bit) {
    script.push_back(((raw >> bit) & 1u) != 0);
  }
  rig.buses.gpioPort.queueReads(16, script);

  const std::uint32_t edgesBefore = rig.buses.gpioPort.risingEdges(17);
  driver.poll(rig.clock.nowMicros());

  TEST_ASSERT_EQUAL_FLOAT(-2.0f, rig.channels.value(mass)->processed);
  // 24 data pulses plus 3 for gain 64 — the gain selection is on the wire, not
  // in a comment.
  TEST_ASSERT_EQUAL_UINT(27, rig.buses.gpioPort.risingEdges(17) - edgesBefore);
}

static void test_hx711_never_waits_for_a_missing_sensor() {
  DriverRig rig;
  const ChannelHandle mass = rig.addChannel("mass");
  MapConfig config;
  config.set("data_pin", "16").set("clock_pin", "17").set("gain", "128")
        .set("rate_hz", "10");

  Hx711Driver driver;
  TEST_ASSERT_TRUE(driver.configure(rig.context(Hx711Driver::manifest(), config)).ok());
  TEST_ASSERT_TRUE(driver.begin().ok());

  rig.buses.gpioPort.setLevel(16, true);  // DOUT stuck high: nothing connected

  const std::uint32_t delayBefore = rig.buses.gpioPort.totalDelayMicros();
  for (int i = 0; i < 3; ++i) {
    rig.clock.advanceMicros(50000);
    driver.poll(rig.clock.nowMicros());
  }
  // Not one microsecond of busy-waiting, and nothing published.
  TEST_ASSERT_EQUAL_UINT(delayBefore, rig.buses.gpioPort.totalDelayMicros());
  TEST_ASSERT_EQUAL_UINT(0, rig.channels.value(mass)->sequence);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kRunning),
                        static_cast<int>(driver.state()));

  // After three missed conversion periods it is a fault, with a message that
  // tells the operator where to look.
  rig.clock.advanceMicros(400000);
  driver.poll(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kError),
                        static_cast<int>(driver.state()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDeviceNotResponding),
                        static_cast<int>(driver.lastError().code));
  TEST_ASSERT_EQUAL_STRING("DOUT never went low; check wiring and power",
                           driver.lastError().detail.c_str());
}

static void test_hx711_flags_saturation_instead_of_reporting_it() {
  DriverRig rig;
  rig.addChannel("mass");
  MapConfig config;
  config.set("data_pin", "16").set("clock_pin", "17").set("gain", "128");

  Hx711Driver driver;
  TEST_ASSERT_TRUE(driver.configure(rig.context(Hx711Driver::manifest(), config)).ok());
  TEST_ASSERT_TRUE(driver.begin().ok());

  std::vector<bool> script;
  script.push_back(false);
  for (int bit = 23; bit >= 0; --bit) script.push_back(bit != 23);  // 0x7FFFFF
  rig.buses.gpioPort.queueReads(16, script);

  driver.poll(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kWarning),
                        static_cast<int>(driver.state()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDeviceOutOfRange),
                        static_cast<int>(driver.lastError().code));
}

// ===========================================================================
//  Basic inputs
// ===========================================================================
static void test_analog_input_averages_the_requested_number_of_samples() {
  DriverRig rig;
  const ChannelHandle value = rig.addChannel("a");
  MapConfig config;
  config.set("pin", "34").set("samples", "4").set("output", "raw")
        .set("attenuation", "11");

  AnalogInputDriver driver;
  TEST_ASSERT_TRUE(
      driver.configure(rig.context(AnalogInputDriver::manifest(), config)).ok());
  TEST_ASSERT_TRUE(driver.begin().ok());

  rig.buses.adcPort.queueRaw({100, 200, 300, 400});
  driver.poll(rig.clock.nowMicros());

  TEST_ASSERT_EQUAL_FLOAT(250.0f, rig.channels.value(value)->processed);
  TEST_ASSERT_EQUAL_UINT(4, rig.buses.adcPort.readCount());
}

static void test_analog_input_admits_when_millivolts_are_approximate() {
  DriverRig rig;
  rig.addChannel("a");
  MapConfig config;
  config.set("pin", "34").set("output", "mv");

  rig.buses.adcPort.setCalibrated(false);

  AnalogInputDriver driver;
  TEST_ASSERT_TRUE(
      driver.configure(rig.context(AnalogInputDriver::manifest(), config)).ok());
  TEST_ASSERT_TRUE(driver.begin().ok());

  // Still usable, but the operator is told the numbers are not traceable.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kWarning),
                        static_cast<int>(driver.state()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kNotSupported),
                        static_cast<int>(driver.lastError().code));
}

static void test_digital_input_debounces_a_bouncing_contact() {
  DriverRig rig;
  const ChannelHandle state = rig.addChannel("d");
  MapConfig config;
  config.set("pin", "18").set("pull", "up").set("invert", "true")
        .set("debounce_ms", "20");

  DigitalInputDriver driver;
  TEST_ASSERT_TRUE(
      driver.configure(rig.context(DigitalInputDriver::manifest(), config)).ok());
  TEST_ASSERT_TRUE(driver.begin().ok());

  // Contact closes and bounces: low, high, low, then settles low.
  rig.buses.gpioPort.setLevel(18, false);
  driver.poll(rig.clock.nowMicros());          // candidate = closed
  rig.clock.advanceMicros(5000);
  rig.buses.gpioPort.setLevel(18, true);
  driver.poll(rig.clock.nowMicros());          // bounce resets the window
  rig.clock.advanceMicros(5000);
  rig.buses.gpioPort.setLevel(18, false);
  driver.poll(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_UINT(0, rig.channels.value(state)->sequence);

  // Held long enough: exactly one update, not one per poll.
  rig.clock.advanceMicros(25000);
  driver.poll(rig.clock.nowMicros());
  rig.clock.advanceMicros(25000);
  driver.poll(rig.clock.nowMicros());
  TEST_ASSERT_EQUAL_UINT(1, rig.channels.value(state)->sequence);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, rig.channels.value(state)->processed);
}

// ===========================================================================
//  Integration with DeviceManager
// ===========================================================================
static void test_a_bus_device_cannot_start_without_its_bus() {
  ManualClock clock;
  ModuleRegistry registry;
  registerBuiltinModules(registry);
  ResourceManager resources{ChipProfile::esp32()};
  ChannelManager channels{clock};
  Scheduler scheduler{clock};
  EventBus events;
  DeviceManager devices{clock, registry, resources, channels, scheduler, events};
  devices.begin();

  test::FakeBusProvider buses;
  buses.i2cConfigured = false;
  devices.setBusProvider(&buses);

  MapConfig config;
  config.set("bus", "0").set("address", "0x76");
  DeviceSpec spec;
  spec.key.assign("bmp_01");

  LabelString field;
  const auto added = devices.add("bmp280", spec, config, &field);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kBusNotConfigured),
                        static_cast<int>(added.code()));
  TEST_ASSERT_EQUAL_STRING("bus", field.c_str());
  // Nothing half-created.
  TEST_ASSERT_EQUAL_UINT(0, channels.activeCount());
  TEST_ASSERT_EQUAL_UINT(0, resources.claimCount());
}

static void test_two_aht20s_on_one_bus_collide_and_on_two_do_not() {
  ManualClock clock;
  ModuleRegistry registry;
  registerBuiltinModules(registry);
  ResourceManager resources{ChipProfile::esp32()};
  ChannelManager channels{clock};
  Scheduler scheduler{clock};
  EventBus events;
  DeviceManager devices{clock, registry, resources, channels, scheduler, events};
  devices.begin();

  // Two configured buses this time.
  struct TwoBusProvider final : IBusProvider {
    test::FakeI2cBus a, b;
    test::FakeGpioPort gpioPort;
    test::FakeAdcPort adcPort;
    II2cBus* i2c(std::uint8_t index) override {
      return index == 0 ? static_cast<II2cBus*>(&a)
                        : (index == 1 ? static_cast<II2cBus*>(&b) : nullptr);
    }
    std::uint8_t i2cBusCount() const override { return 2; }
    IGpioPort* gpio() override { return &gpioPort; }
    IAdcPort* adc() override { return &adcPort; }
  } buses;
  buses.a.attach(0x38);
  buses.a.queueRead(0x38, {0x1C});
  buses.b.attach(0x38);
  buses.b.queueRead(0x38, {0x1C});
  devices.setBusProvider(&buses);

  MapConfig onBus0;
  onBus0.set("bus", "0").set("address", "0x38");
  DeviceSpec first;
  first.key.assign("aht_01");
  TEST_ASSERT_TRUE(devices.add("aht20", first, onBus0).ok());

  // Same address, same bus: refused, naming the owner.
  MapConfig clash;
  clash.set("bus", "0").set("address", "0x38");
  DeviceSpec second;
  second.key.assign("aht_02");
  const auto collision = devices.add("aht20", second, clash);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kI2cAddressBusy),
                        static_cast<int>(collision.code()));

  // Same address on the other bus: a perfectly ordinary rig.
  MapConfig onBus1;
  onBus1.set("bus", "1").set("address", "0x38");
  DeviceSpec third;
  third.key.assign("aht_03");
  TEST_ASSERT_TRUE(devices.add("aht20", third, onBus1).ok());
  TEST_ASSERT_EQUAL_UINT(2, devices.activeCount());
  TEST_ASSERT_EQUAL_UINT(4, channels.activeCount());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_scanner_finds_devices_and_offers_careful_hints);
  RUN_TEST(test_aht20_crc_matches_the_standard_check_value);
  RUN_TEST(test_aht20_decodes_a_known_frame);
  RUN_TEST(test_aht20_rejects_bad_crc_and_busy_frames);
  RUN_TEST(test_aht20_reports_a_missing_sensor_clearly);
  RUN_TEST(test_aht20_measures_without_ever_blocking);
  RUN_TEST(test_aht20_degrades_before_it_fails);
  RUN_TEST(test_bmp280_integer_compensation_matches_the_double_reference);
  RUN_TEST(test_bmp280_calibration_round_trips_through_the_register_block);
  RUN_TEST(test_bmp280_identifies_the_chip_before_trusting_it);
  RUN_TEST(test_bmp280_publishes_compensated_values);
  RUN_TEST(test_bmp280_does_not_publish_the_reset_value);
  RUN_TEST(test_hx711_sign_extension);
  RUN_TEST(test_hx711_gain_selects_the_pulse_count);
  RUN_TEST(test_hx711_reads_a_scripted_conversion);
  RUN_TEST(test_hx711_never_waits_for_a_missing_sensor);
  RUN_TEST(test_hx711_flags_saturation_instead_of_reporting_it);
  RUN_TEST(test_analog_input_averages_the_requested_number_of_samples);
  RUN_TEST(test_analog_input_admits_when_millivolts_are_approximate);
  RUN_TEST(test_digital_input_debounces_a_bouncing_contact);
  RUN_TEST(test_a_bus_device_cannot_start_without_its_bus);
  RUN_TEST(test_two_aht20s_on_one_bus_collide_and_on_two_do_not);
  return UNITY_END();
}
