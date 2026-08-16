// =============================================================================
//  Unit tests for the services layer and the software modules.
//      pio test -e native
// =============================================================================
#include <unity.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/Clock.h"
#include "modules/processing/CalibrationProcessor.h"
#include "modules/processing/FilterProcessors.h"
#include "modules/processing/MathProcessors.h"
#include "modules/processing/MovingAverageProcessor.h"
#include "modules/sensors/SignalSimulator.h"
#include "services/CalibrationSolver.h"
#include "services/ChannelManager.h"

using namespace lc;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
//  A map-backed IConfigView.  This is the whole reason modules do not see
//  ArduinoJson: configuring a driver in a test is three lines.
// ---------------------------------------------------------------------------
namespace {
class MapConfig final : public IConfigView {
 public:
  MapConfig& set(const char* key, const std::string& value) {
    scalars_[key] = value;
    return *this;
  }
  MapConfig& setArray(const char* key, std::vector<float> values) {
    arrays_[key] = std::move(values);
    return *this;
  }

  bool has(const char* key) const override { return scalars_.count(key) > 0; }
  std::int32_t getInt(const char* key, std::int32_t fallback) const override {
    auto it = scalars_.find(key);
    return it == scalars_.end() ? fallback : std::atoi(it->second.c_str());
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
  std::size_t arraySize(const char* key) const override {
    auto it = arrays_.find(key);
    return it == arrays_.end() ? 0 : it->second.size();
  }
  float getFloatAt(const char* key, std::size_t index, float fallback) const override {
    auto it = arrays_.find(key);
    if (it == arrays_.end() || index >= it->second.size()) return fallback;
    return it->second[index];
  }

 private:
  std::map<std::string, std::string> scalars_;
  std::map<std::string, std::vector<float>> arrays_;
};

ChannelDescriptor makeDescriptor(const char* key, const char* unit = "") {
  ChannelDescriptor descriptor;
  descriptor.key.assign(key);
  descriptor.name.assign(key);
  descriptor.unit.assign(unit);
  return descriptor;
}

int g_listenerCalls = 0;
float g_lastProcessed = 0.0f;
ChannelQuality g_lastQuality = ChannelQuality::kUnknown;
void recordSample(ChannelHandle, const ChannelValue& value, void*) {
  ++g_listenerCalls;
  g_lastProcessed = value.processed;
  g_lastQuality = value.quality;
}
}  // namespace

// ---------------------------------------------------------------------------
//  ChannelManager
// ---------------------------------------------------------------------------
static void test_channels_are_unique_by_key() {
  ManualClock clock;
  ChannelManager channels(clock);

  const auto first = channels.create(makeDescriptor("mass_01", "g"));
  TEST_ASSERT_TRUE(first.ok());
  const auto duplicate = channels.create(makeDescriptor("mass_01", "g"));
  TEST_ASSERT_FALSE(duplicate.ok());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kAlreadyExists),
                        static_cast<int>(duplicate.code()));
  TEST_ASSERT_EQUAL_UINT(first.value(), channels.findByKey("mass_01"));
  TEST_ASSERT_EQUAL_UINT(kInvalidChannel, channels.findByKey("nope"));
}

static void test_channel_handles_survive_removal_of_other_channels() {
  ManualClock clock;
  ChannelManager channels(clock);

  const ChannelHandle a = channels.create(makeDescriptor("a")).value();
  const ChannelHandle b = channels.create(makeDescriptor("b")).value();
  const ChannelHandle c = channels.create(makeDescriptor("c")).value();

  TEST_ASSERT_TRUE(channels.remove(b).ok());
  TEST_ASSERT_TRUE(channels.exists(a));
  TEST_ASSERT_FALSE(channels.exists(b));
  TEST_ASSERT_TRUE(channels.exists(c));
  TEST_ASSERT_EQUAL_STRING("c", channels.descriptor(c)->key.c_str());
  TEST_ASSERT_EQUAL_UINT(2, channels.activeCount());
}

static void test_pipeline_records_raw_calibrated_and_processed() {
  ManualClock clock;
  ChannelManager channels(clock);
  const ChannelHandle handle = channels.create(makeDescriptor("mass_01", "g")).value();

  // Stage 0: calibration  y = (x - 1000) * 0.5
  // Stage 1: moving average over 2 samples
  modules::CalibrationProcessor calibration;
  MapConfig calibrationConfig;
  calibrationConfig.set("type", "polynomial")
      .set("x_center", "0")
      .set("x_scale", "1")
      .setArray("coefficients", {-500.0f, 0.5f});
  TEST_ASSERT_TRUE(calibration.configure(calibrationConfig).ok());

  modules::MovingAverageProcessor average;
  MapConfig averageConfig;
  averageConfig.set("window", "2");
  TEST_ASSERT_TRUE(average.configure(averageConfig).ok());

  IProcessor* stages[] = {&calibration, &average};
  TEST_ASSERT_TRUE(channels.setPipeline(handle, stages, 2, /*calibrationStage=*/0).ok());

  g_listenerCalls = 0;
  TEST_ASSERT_TRUE(channels.addListener(recordSample, nullptr).ok());

  TEST_ASSERT_TRUE(channels.publishRaw(handle, 1000.0f, 1000));
  const ChannelValue* value = channels.value(handle);
  TEST_ASSERT_EQUAL_FLOAT(1000.0f, value->raw);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, value->calibrated);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, value->processed);

  TEST_ASSERT_TRUE(channels.publishRaw(handle, 1200.0f, 2000));
  value = channels.value(handle);
  TEST_ASSERT_EQUAL_FLOAT(1200.0f, value->raw);
  TEST_ASSERT_EQUAL_FLOAT(100.0f, value->calibrated);   // (1200-1000)*0.5
  TEST_ASSERT_EQUAL_FLOAT(50.0f, value->processed);     // mean(0, 100)
  TEST_ASSERT_EQUAL_UINT(2, value->sequence);

  TEST_ASSERT_EQUAL_INT(2, g_listenerCalls);
  TEST_ASSERT_EQUAL_FLOAT(50.0f, g_lastProcessed);
}

static void test_out_of_range_samples_are_flagged_not_dropped() {
  ManualClock clock;
  ChannelManager channels(clock);
  ChannelDescriptor descriptor = makeDescriptor("temp_01", "degC");
  descriptor.minimum = -40.0f;
  descriptor.maximum = 125.0f;
  const ChannelHandle handle = channels.create(descriptor).value();

  channels.publishRaw(handle, 20.0f, 1000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ChannelQuality::kGood),
                        static_cast<int>(channels.value(handle)->quality));

  channels.publishRaw(handle, 300.0f, 2000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ChannelQuality::kOutOfRange),
                        static_cast<int>(channels.value(handle)->quality));
  // The value itself is preserved: hiding it would hide the fault.
  TEST_ASSERT_EQUAL_FLOAT(300.0f, channels.value(handle)->processed);
}

static void test_stale_channels_are_detected() {
  ManualClock clock;
  ChannelManager channels(clock);
  ChannelDescriptor descriptor = makeDescriptor("temp_01", "degC");
  descriptor.expectedIntervalUs = 100000;  // 10 Hz
  const ChannelHandle handle = channels.create(descriptor).value();

  channels.publishRaw(handle, 20.0f, 1000000);
  channels.tick(1200000);  // 2 periods later — still fine
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ChannelQuality::kGood),
                        static_cast<int>(channels.value(handle)->quality));

  channels.tick(1400000);  // 4 periods later
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ChannelQuality::kStale),
                        static_cast<int>(channels.value(handle)->quality));
}

static void test_going_stale_reaches_the_listeners() {
  // A channel goes stale precisely because samples STOPPED, so there is no
  // next sample to carry the news.  If the transition does not reach the
  // listeners, the telemetry batcher never marks the channel dirty and the
  // browser keeps painting the last value as if it were fresh.
  ManualClock clock;
  ChannelManager channels(clock);
  ChannelDescriptor descriptor = makeDescriptor("temp_01", "degC");
  descriptor.expectedIntervalUs = 100000;
  const ChannelHandle handle = channels.create(descriptor).value();

  channels.publishRaw(handle, 20.0f, 1000000);
  g_listenerCalls = 0;
  TEST_ASSERT_TRUE(channels.addListener(recordSample, nullptr).ok());

  channels.tick(1200000);
  TEST_ASSERT_EQUAL_INT(0, g_listenerCalls);   // still fresh, nothing to say

  channels.tick(1400000);
  TEST_ASSERT_EQUAL_INT(1, g_listenerCalls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ChannelQuality::kStale),
                        static_cast<int>(g_lastQuality));

  // ...and it is announced once, not on every tick for the rest of time.
  channels.tick(1600000);
  channels.tick(1800000);
  TEST_ASSERT_EQUAL_INT(1, g_listenerCalls);

  TEST_ASSERT_TRUE(channels.removeListener(recordSample, nullptr).ok());
}

static void test_a_failed_device_faults_its_channels_audibly() {
  // The device's poll task is disabled the moment it fails, so this is the
  // last chance to tell anyone that its numbers are no longer real.
  ManualClock clock;
  ChannelManager channels(clock);
  ChannelDescriptor descriptor = makeDescriptor("temp_01", "degC");
  descriptor.source = 7;
  const ChannelHandle handle = channels.create(descriptor).value();
  channels.publishRaw(handle, 20.0f, 1000);

  g_listenerCalls = 0;
  TEST_ASSERT_TRUE(channels.addListener(recordSample, nullptr).ok());

  channels.setSourceQuality(7, ChannelQuality::kFaulted);
  TEST_ASSERT_EQUAL_INT(1, g_listenerCalls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ChannelQuality::kFaulted),
                        static_cast<int>(g_lastQuality));
  // The last value is kept: hiding it would hide the fault (§46).
  TEST_ASSERT_EQUAL_FLOAT(20.0f, channels.value(handle)->processed);

  // Repeating the same verdict is not news.
  channels.setSourceQuality(7, ChannelQuality::kFaulted);
  TEST_ASSERT_EQUAL_INT(1, g_listenerCalls);

  // A channel belonging to a different device is untouched.
  channels.setSourceQuality(8, ChannelQuality::kFaulted);
  TEST_ASSERT_EQUAL_INT(1, g_listenerCalls);

  TEST_ASSERT_TRUE(channels.removeListener(recordSample, nullptr).ok());
}

static void test_writing_to_an_input_channel_is_rejected() {
  ManualClock clock;
  ChannelManager channels(clock);
  const ChannelHandle handle = channels.create(makeDescriptor("temp_01")).value();
  const Status status = channels.write(handle, 1.0f);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kChannelTypeMismatch),
                        static_cast<int>(status.code));
}

// ---------------------------------------------------------------------------
//  CalibrationSolver — the §12 worked example
// ---------------------------------------------------------------------------
static void test_linear_fit_of_load_cell_reference_points() {
  const CalibrationPoint points[] = {
      {453211.0f, 0.0f},
      {498322.0f, 100.0f},
      {543419.0f, 200.0f},
  };

  const auto result = CalibrationSolver::fitLinear(points, 3);
  TEST_ASSERT_TRUE(result.ok());
  const PolynomialFit& fit = result.value();

  // The fit must reproduce the reference points to well under a milligram.
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, static_cast<float>(fit.evaluate(453211.0)));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 100.0f, static_cast<float>(fit.evaluate(498322.0)));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 200.0f, static_cast<float>(fit.evaluate(543419.0)));

  // And interpolate sensibly in between.
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 50.0f, static_cast<float>(fit.evaluate(475766.0)));
  TEST_ASSERT_TRUE(fit.rSquared > 0.9999);
  TEST_ASSERT_TRUE(fit.maxResidual < 0.05);
}

static void test_quadratic_fit_is_exact_on_three_points() {
  // y = 2u^2 - 3u + 1 sampled far from the origin, which is precisely the case
  // that destroys a naive normal-equations solve in float.
  CalibrationPoint points[5];
  for (int i = 0; i < 5; ++i) {
    const double x = 500000.0 + i * 1000.0;
    const double u = (x - 502000.0) / 2000.0;
    points[i].raw = static_cast<float>(x);
    points[i].reference = static_cast<float>(2.0 * u * u - 3.0 * u + 1.0);
  }

  const auto result = CalibrationSolver::fitPolynomial(points, 5, 2);
  TEST_ASSERT_TRUE(result.ok());
  TEST_ASSERT_TRUE(result.value().rmsResidual < 1e-3);
  TEST_ASSERT_FLOAT_WITHIN(1e-2f, 1.0f,
                           static_cast<float>(result.value().evaluate(502000.0)));
}

static void test_solver_rejects_underdetermined_and_degenerate_input() {
  const CalibrationPoint two[] = {{0.0f, 0.0f}, {1.0f, 1.0f}};
  const auto tooFew = CalibrationSolver::fitPolynomial(two, 2, 2);
  TEST_ASSERT_FALSE(tooFew.ok());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kCalibrationInsufficientPoints),
                        static_cast<int>(tooFew.code()));

  // Three identical abscissae cannot determine a line.
  const CalibrationPoint same[] = {{5.0f, 1.0f}, {5.0f, 2.0f}, {5.0f, 3.0f}};
  const auto singular = CalibrationSolver::fitPolynomial(same, 3, 1);
  TEST_ASSERT_FALSE(singular.ok());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kCalibrationSingular),
                        static_cast<int>(singular.code()));
}

// ---------------------------------------------------------------------------
//  Processors
// ---------------------------------------------------------------------------
static void test_calibration_table_clamps_instead_of_extrapolating() {
  modules::CalibrationProcessor processor;
  MapConfig config;
  config.set("type", "table")
      .setArray("table_x", {0.0f, 10.0f, 20.0f})
      .setArray("table_y", {0.0f, 5.0f, 30.0f});
  TEST_ASSERT_TRUE(processor.configure(config).ok());

  bool valid = true;
  TEST_ASSERT_EQUAL_FLOAT(2.5f, processor.process(5.0f, 0, valid));
  TEST_ASSERT_EQUAL_FLOAT(17.5f, processor.process(15.0f, 0, valid));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, processor.process(-100.0f, 0, valid));
  TEST_ASSERT_EQUAL_FLOAT(30.0f, processor.process(1000.0f, 0, valid));
}

static void test_calibration_rejects_unsorted_table() {
  modules::CalibrationProcessor processor;
  MapConfig config;
  config.set("type", "table")
      .setArray("table_x", {0.0f, 20.0f, 10.0f})
      .setArray("table_y", {0.0f, 5.0f, 30.0f});
  TEST_ASSERT_FALSE(processor.configure(config).ok());
}

static void test_moving_average_uses_partial_window_while_filling() {
  modules::MovingAverageProcessor processor;
  MapConfig config;
  config.set("window", "4");
  TEST_ASSERT_TRUE(processor.configure(config).ok());

  bool valid = true;
  TEST_ASSERT_EQUAL_FLOAT(10.0f, processor.process(10.0f, 0, valid));
  TEST_ASSERT_EQUAL_FLOAT(15.0f, processor.process(20.0f, 0, valid));
  TEST_ASSERT_EQUAL_FLOAT(20.0f, processor.process(30.0f, 0, valid));
  TEST_ASSERT_EQUAL_FLOAT(25.0f, processor.process(40.0f, 0, valid));
  // Window is full; the first sample now drops out.
  TEST_ASSERT_EQUAL_FLOAT(35.0f, processor.process(50.0f, 0, valid));
  TEST_ASSERT_TRUE(valid);
}

static void test_moving_average_rejects_absurd_windows() {
  modules::MovingAverageProcessor processor;
  MapConfig config;
  config.set("window", "5000");
  TEST_ASSERT_FALSE(processor.configure(config).ok());
}

// ---------------------------------------------------------------------------
//  SignalSimulator — end-to-end driver -> channel path, no hardware
// ---------------------------------------------------------------------------
static void test_simulator_drives_a_channel_end_to_end() {
  ManualClock clock;
  ChannelManager channels(clock);
  EventBus events;
  ResourceManager resources(ChipProfile::esp32());

  const ChannelHandle handle = channels.create(makeDescriptor("sim_01", "degC")).value();
  const ChannelHandle handles[] = {handle};

  MapConfig config;
  config.set("waveform", "sine")
      .set("amplitude", "10")
      .set("offset", "60")
      .set("period_s", "4")
      .set("noise", "0");

  modules::SignalSimulator device;
  DeviceContext context;
  context.self = 1;
  context.manifest = &modules::SignalSimulator::manifest();
  context.config = &config;
  context.clock = &clock;
  context.resources = &resources;
  context.channels = &channels;
  context.events = &events;
  context.channelHandles = handles;
  context.channelCount = 1;

  TEST_ASSERT_TRUE(device.configure(context).ok());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kConfigured),
                        static_cast<int>(device.state()));
  TEST_ASSERT_TRUE(device.begin().ok());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceState::kRunning),
                        static_cast<int>(device.state()));

  // t = 0 -> sin(0) = 0 -> offset
  device.poll(clock.nowMicros());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, channels.value(handle)->processed);

  // Quarter period -> peak
  clock.advanceMicros(1000000);
  device.poll(clock.nowMicros());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 70.0f, channels.value(handle)->processed);

  // Three quarters -> trough
  clock.advanceMicros(2000000);
  device.poll(clock.nowMicros());
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 50.0f, channels.value(handle)->processed);

  // A simulator must claim no hardware.
  TEST_ASSERT_EQUAL_UINT(0, resources.claimCount());
}

static void test_simulator_rejects_invalid_configuration() {
  ManualClock clock;
  ChannelManager channels(clock);
  EventBus events;
  ResourceManager resources(ChipProfile::esp32());
  const ChannelHandle handle = channels.create(makeDescriptor("sim_02")).value();
  const ChannelHandle handles[] = {handle};

  MapConfig config;
  config.set("period_s", "0");

  modules::SignalSimulator device;
  DeviceContext context;
  context.config = &config;
  context.clock = &clock;
  context.resources = &resources;
  context.channels = &channels;
  context.events = &events;
  context.channelHandles = handles;
  context.channelCount = 1;

  const Status status = device.configure(context);
  TEST_ASSERT_FALSE(status.ok());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ErrorCode::kDeviceConfigInvalid),
                        static_cast<int>(status.code));
}


// ---------------------------------------------------------------------------
//  The processing library (Milestone 5)
// ---------------------------------------------------------------------------
namespace {

// Runs one sample through a stage with a fresh `valid` flag, the way
// ChannelManager does.
float run(IProcessor& stage, float input, Micros now, bool& valid) {
  valid = true;
  return stage.process(input, now, valid);
}

}  // namespace

static void test_median_rejects_a_spike_that_an_average_would_smear() {
  modules::MedianProcessor median;
  modules::MovingAverageProcessor average;
  MapConfig config;
  config.set("window", "5");
  TEST_ASSERT_TRUE(median.configure(config).ok());
  TEST_ASSERT_TRUE(average.configure(config).ok());

  const float samples[] = {10.0f, 10.0f, 10.0f, 900.0f, 10.0f};
  bool valid = true;
  float medianOut = 0.0f;
  float averageOut = 0.0f;
  for (std::size_t i = 0; i < 5; ++i) {
    medianOut = run(median, samples[i], (i + 1) * 1000, valid);
    // The window is not full until the fifth sample, and a partial median is
    // not a median.
    if (i < 4) TEST_ASSERT_FALSE(valid);
    bool ignored = true;
    averageOut = average.process(samples[i], (i + 1) * 1000, ignored);
  }
  TEST_ASSERT_TRUE(valid);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, medianOut);
  // The same spike moves the average by 178 units and stays in it for four
  // more samples.  That is the whole reason both filters exist.
  TEST_ASSERT_TRUE(averageOut > 150.0f);

  // Even windows have no middle element and are refused rather than quietly
  // rounded to one side.
  MapConfig even;
  even.set("window", "4");
  TEST_ASSERT_FALSE(median.configure(even).ok());
}

static void test_low_pass_uses_the_real_interval_not_an_assumed_one() {
  modules::LowPassProcessor filter;
  MapConfig config;
  config.set("tau_s", "1.0");
  TEST_ASSERT_TRUE(filter.configure(config).ok());

  bool valid = true;
  // Seeded with the first sample: starting from zero would make every channel
  // ramp up for the first few tau and look like a real transient.
  TEST_ASSERT_EQUAL_FLOAT(100.0f, run(filter, 100.0f, 0, valid));

  // One time constant later, a step to 0 should have fallen to 1/e of 100.
  const float afterOneTau = run(filter, 0.0f, 1000000, valid);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 36.8f, afterOneTau);

  // A stall is not a free pass: with dt = 10 tau the filter simply catches up,
  // which is the honest answer — it knows nothing about the missing interval.
  const float afterStall = run(filter, 50.0f, 11000000, valid);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 50.0f, afterStall);
}

static void test_derivative_reports_a_real_rate_and_nothing_before_it_can() {
  modules::DerivativeProcessor derivative;
  MapConfig config;
  config.set("window", "2").set("per", "min");
  TEST_ASSERT_TRUE(derivative.configure(config).ok());

  // 0.5 units per second == 30 per minute.
  bool valid = true;
  run(derivative, 0.0f, 0, valid);
  TEST_ASSERT_FALSE(valid);          // no second point yet
  run(derivative, 0.5f, 1000000, valid);
  TEST_ASSERT_FALSE(valid);
  const float rate = run(derivative, 1.0f, 2000000, valid);
  TEST_ASSERT_TRUE(valid);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, rate);

  // Reporting 0 while warming up would be a lie that looks like a steady
  // signal, which is why the stage suppresses the sample instead.
  derivative.reset();
  run(derivative, 5.0f, 3000000, valid);
  TEST_ASSERT_FALSE(valid);
}

static void test_integral_uses_trapezoids_and_can_be_bounded() {
  modules::IntegralProcessor integral;
  MapConfig config;
  config.set("per", "s");
  TEST_ASSERT_TRUE(integral.configure(config).ok());

  bool valid = true;
  run(integral, 0.0f, 0, valid);
  // A ramp from 0 to 10 over 10 s integrates to exactly 50 by the trapezoid
  // rule; the rectangle rule would give 55 and drift further every second.
  float total = 0.0f;
  for (int second = 1; second <= 10; ++second) {
    total = run(integral, static_cast<float>(second), second * 1000000ULL, valid);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, total);

  MapConfig bounded;
  bounded.set("per", "s").set("min", "0").set("max", "5");
  modules::IntegralProcessor capped;
  TEST_ASSERT_TRUE(capped.configure(bounded).ok());
  run(capped, 10.0f, 0, valid);
  TEST_ASSERT_EQUAL_FLOAT(5.0f, run(capped, 10.0f, 5000000, valid));
}

static void test_statistics_computes_a_standard_deviation_that_survives_an_offset() {
  modules::StatisticsProcessor stats;
  MapConfig config;
  config.set("statistic", "stddev").set("window", "4");
  TEST_ASSERT_TRUE(stats.configure(config).ok());

  // 20.00, 20.02, 19.98, 20.00 around a large offset: the one-pass
  // E[x^2] - E[x]^2 form subtracts two nearly equal numbers here and returns
  // garbage, sometimes a negative variance.
  bool valid = true;
  const float samples[] = {20.00f, 20.02f, 19.98f, 20.00f};
  float result = 0.0f;
  for (std::size_t i = 0; i < 4; ++i) {
    result = run(stats, samples[i], (i + 1) * 1000, valid);
  }
  TEST_ASSERT_TRUE(result > 0.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.01633f, result);

  MapConfig span;
  span.set("statistic", "peak_to_peak").set("window", "4");
  modules::StatisticsProcessor peak;
  TEST_ASSERT_TRUE(peak.configure(span).ok());
  for (std::size_t i = 0; i < 4; ++i) run(peak, samples[i], (i + 1) * 1000, valid);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.04f, run(peak, 20.0f, 5000, valid));
}

static void test_clamp_can_hide_an_excursion_or_expose_it() {
  MapConfig saturating;
  saturating.set("min", "0").set("max", "100").set("mode", "saturate");
  modules::ClampProcessor saturate;
  TEST_ASSERT_TRUE(saturate.configure(saturating).ok());

  bool valid = true;
  TEST_ASSERT_EQUAL_FLOAT(100.0f, run(saturate, 250.0f, 1000, valid));
  TEST_ASSERT_TRUE(valid);   // the excursion is now invisible — by request

  MapConfig invalidating;
  invalidating.set("min", "0").set("max", "100").set("mode", "invalidate");
  modules::ClampProcessor discard;
  TEST_ASSERT_TRUE(discard.configure(invalidating).ok());
  run(discard, 250.0f, 1000, valid);
  TEST_ASSERT_FALSE(valid);  // ...and here it is a visible gap instead

  MapConfig inverted;
  inverted.set("min", "100").set("max", "0");
  modules::ClampProcessor rejected;
  TEST_ASSERT_FALSE(rejected.configure(inverted).ok());
}

static void test_deadband_holds_until_the_signal_really_moves() {
  modules::DeadbandProcessor deadband;
  MapConfig config;
  config.set("threshold", "0.5");
  TEST_ASSERT_TRUE(deadband.configure(config).ok());

  bool valid = true;
  TEST_ASSERT_EQUAL_FLOAT(20.0f, run(deadband, 20.0f, 1000, valid));
  TEST_ASSERT_EQUAL_FLOAT(20.0f, run(deadband, 20.3f, 2000, valid));
  TEST_ASSERT_EQUAL_FLOAT(20.0f, run(deadband, 19.7f, 3000, valid));
  // Past the threshold it jumps to the new value — and quantises by exactly
  // the threshold in the process, which is why this belongs in a log and not
  // in front of a control loop.
  TEST_ASSERT_EQUAL_FLOAT(20.6f, run(deadband, 20.6f, 4000, valid));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_channels_are_unique_by_key);
  RUN_TEST(test_channel_handles_survive_removal_of_other_channels);
  RUN_TEST(test_pipeline_records_raw_calibrated_and_processed);
  RUN_TEST(test_out_of_range_samples_are_flagged_not_dropped);
  RUN_TEST(test_median_rejects_a_spike_that_an_average_would_smear);
  RUN_TEST(test_low_pass_uses_the_real_interval_not_an_assumed_one);
  RUN_TEST(test_derivative_reports_a_real_rate_and_nothing_before_it_can);
  RUN_TEST(test_integral_uses_trapezoids_and_can_be_bounded);
  RUN_TEST(test_statistics_computes_a_standard_deviation_that_survives_an_offset);
  RUN_TEST(test_clamp_can_hide_an_excursion_or_expose_it);
  RUN_TEST(test_deadband_holds_until_the_signal_really_moves);
  RUN_TEST(test_stale_channels_are_detected);
  RUN_TEST(test_going_stale_reaches_the_listeners);
  RUN_TEST(test_a_failed_device_faults_its_channels_audibly);
  RUN_TEST(test_writing_to_an_input_channel_is_rejected);
  RUN_TEST(test_linear_fit_of_load_cell_reference_points);
  RUN_TEST(test_quadratic_fit_is_exact_on_three_points);
  RUN_TEST(test_solver_rejects_underdetermined_and_degenerate_input);
  RUN_TEST(test_calibration_table_clamps_instead_of_extrapolating);
  RUN_TEST(test_calibration_rejects_unsorted_table);
  RUN_TEST(test_moving_average_uses_partial_window_while_filling);
  RUN_TEST(test_moving_average_rejects_absurd_windows);
  RUN_TEST(test_simulator_drives_a_channel_end_to_end);
  RUN_TEST(test_simulator_rejects_invalid_configuration);
  return UNITY_END();
}
