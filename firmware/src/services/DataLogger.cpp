#include "services/DataLogger.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/Format.h"

namespace lc {
namespace {

// Roughly what one row costs: two timestamps, one or two numbers per channel,
// and the quality mask.  Used only for "will this fit" — deliberately generous,
// because the failure mode of underestimating is the one this whole file exists
// to prevent.
std::size_t estimateRowBytes(std::size_t channels, bool includeRaw) {
  return 28 + channels * (includeRaw ? 26u : 14u) + 8;
}

}  // namespace

const char* toString(LogStopReason reason) {
  switch (reason) {
    case LogStopReason::kNone:        return "none";
    case LogStopReason::kOperator:    return "operator";
    case LogStopReason::kFull:        return "medium_full";
    case LogStopReason::kWriteFailed: return "write_failed";
    case LogStopReason::kShutdown:    return "shutdown";
    case LogStopReason::kOffloadBacklogFull: return "offload_backlog_full";
  }
  return "none";
}

const char* toString(LogStorageMode mode) {
  switch (mode) {
    case LogStorageMode::kSingle:            return "single";
    case LogStorageMode::kContinuousOffload: return "continuous_offload";
  }
  return "single";
}

bool parseStorageMode(const char* text, LogStorageMode& out) {
  if (text == nullptr || text[0] == '\0') {
    // An old client that never heard of modes gets the old behaviour.  That is
    // the whole compatibility story, and it belongs in one place.
    out = LogStorageMode::kSingle;
    return true;
  }
  if (std::strcmp(text, "single") == 0) {
    out = LogStorageMode::kSingle;
    return true;
  }
  if (std::strcmp(text, "continuous_offload") == 0) {
    out = LogStorageMode::kContinuousOffload;
    return true;
  }
  return false;
}

Status DataLogger::begin() {
  if (sampleTask_ != kInvalidTask) return ok();

  // kTelemetry: after safety, control and acquisition.  A dataset is a record
  // of what the rig did, and it must never be the reason the rig did it late.
  const Result<TaskId> sampler = scheduler_.addPeriodic(
      "log.sample", 20000, TaskPriority::kTelemetry, sampleTrampoline, this);
  if (!sampler.ok()) return sampler.error();
  sampleTask_ = sampler.value();

  // kBackground, and this is where the filesystem is touched.  See the header:
  // in a cooperative scheduler a flash write blocks the safety pass behind it.
  const Result<TaskId> flusher = scheduler_.addPeriodic(
      "log.flush", 500000, TaskPriority::kBackground, flushTrampoline, this);
  if (!flusher.ok()) return flusher.error();
  flushTask_ = flusher.value();
  return ok();
}

void DataLogger::sampleTrampoline(void* context) {
  DataLogger* self = static_cast<DataLogger*>(context);
  self->sampleTick(self->clock_.nowMicros());
}

void DataLogger::flushTrampoline(void* context) {
  static_cast<DataLogger*>(context)->flushTick();
}

void DataLogger::publish(std::uint8_t severity, const char* detail,
                         ErrorCode code) {
  Event event;
  event.type = (severity >= 3) ? EventType::kLoggingStopped
                               : EventType::kLoggingStarted;
  event.severity = severity;
  event.code = code;
  event.detail = detail;  // static lifetime only
  event.timestamp = clock_.nowMicros();
  events_.publish(event);
}

// ---------------------------------------------------------------------------
//  Starting and stopping
// ---------------------------------------------------------------------------
Status DataLogger::start(const LogSpec& spec, double expectedSeconds) {
  if (status_.recording) return fail(ErrorCode::kResourceBusy, "already recording");
  if (sink_ == nullptr) return fail(ErrorCode::kNotSupported, "no log storage");
  if (spec.channelCount == 0) {
    return fail(ErrorCode::kInvalidArgument, "a dataset needs at least one channel");
  }
  if (spec.channelCount > limits::kMaxLoggedChannels) {
    return fail(ErrorCode::kOutOfCapacity, "too many channels for one dataset");
  }
  if (!(spec.rateHz > 0.0f) || spec.rateHz > kMaxRateHz) {
    return fail(ErrorCode::kInvalidArgument, "rate must be 0…50 Hz");
  }

  // Columns are named from the channels as they are NOW.  A dataset whose
  // header says "mass_01.g" and whose rows came from a channel that has since
  // been redefined is worse than no header at all.
  const char* columns[limits::kMaxLoggedChannels * 2];
  static char names[limits::kMaxLoggedChannels * 2][limits::kKeyLength + limits::kUnitLength + 8];
  std::size_t columnCount = 0;
  for (std::size_t i = 0; i < spec.channelCount; ++i) {
    const ChannelDescriptor* descriptor = channels_.descriptor(spec.channels[i]);
    if (descriptor == nullptr) {
      return fail(ErrorCode::kChannelNotFound, "a channel in the list is gone");
    }
    if (spec.includeRaw) {
      std::snprintf(names[columnCount], sizeof(names[0]), "%s.raw",
                    descriptor->key.c_str());
      columns[columnCount] = names[columnCount];
      ++columnCount;
    }
    std::snprintf(names[columnCount], sizeof(names[0]), "%s.%s",
                  descriptor->key.c_str(),
                  descriptor->unit.empty() ? "value" : descriptor->unit.c_str());
    columns[columnCount] = names[columnCount];
    ++columnCount;
  }

  // Will it fit?  Arithmetic anybody can do, done here so that nobody has to.
  const std::size_t writable = sink_->writableBytes();
  const std::size_t perRow = estimateRowBytes(spec.channelCount, spec.includeRaw);
  if (writable < perRow * 16) {
    return fail(ErrorCode::kFilesystemFull,
                "not enough space left to record anything");
  }
  if (expectedSeconds > 0.0) {
    const double needed =
        static_cast<double>(perRow) * spec.rateHz * expectedSeconds;
    if (needed > static_cast<double>(writable)) {
      // Refused with both numbers, at the start, instead of discovered eight
      // hours in.
      char detail[limits::kDetailLength];
      std::snprintf(detail, sizeof(detail), "needs ~%.0f KB, %.0f KB free",
                    needed / 1024.0, static_cast<double>(writable) / 1024.0);
      return fail(ErrorCode::kFilesystemFull, detail);
    }
  }

  spec_ = spec;
  status_ = LogStatus{};
  status_.name = spec.name;
  status_.channelCount = spec.channelCount;
  status_.rateHz = spec.rateHz;
  status_.storageMode = spec.storageMode;
  status_.nextGlobalRow = 1;

  const Status opened = sink_->openSession(spec_, columns, columnCount, status_.id);
  if (!opened.ok()) return opened;

  periodUs_ = static_cast<Micros>(1e6 / spec.rateHz);
  lastRowUs_ = 0;
  buffered_ = 0;
  nextGlobalRow_ = 1;
  bufferedRows_ = 0;
  firstBufferedGlobalRow_ = 0;
  status_.recording = true;
  status_.startedUs = clock_.nowMicros();
  status_.startedEpochMs = clock_.epochMillis();
  publish(1, "logging started", ErrorCode::kOk);
  return ok();
}

void DataLogger::endSession(LogStopReason reason, const Error& detail) {
  if (!status_.recording) return;

  // Everything still in RAM belongs to this dataset — but not at the cost of
  // the reserve.  Rows that no longer fit above it are counted as dropped
  // rather than written into the space the instrument needs to stay
  // configurable: the dataset says it lost them, which is the whole point.
  if (buffered_ > 0 && sink_ != nullptr) {
    if (sink_->writableBytes() >= buffered_) {
      const Status written = sink_->appendRows(pendingBatch());
      if (written.ok()) {
        status_.bytesWritten += buffered_;
      } else {
        status_.droppedRows += 1;
      }
    } else {
      // One buffer's worth of rows; the count is approximate by exactly the
      // amount the buffer holds, and saying so beats pretending otherwise.
      status_.droppedRows += 1;
    }
    buffered_ = 0;
    bufferedRows_ = 0;
  }

  status_.recording = false;
  status_.stopReason = reason;
  status_.lastError = detail;
  status_.truncated =
      (reason == LogStopReason::kFull || reason == LogStopReason::kWriteFailed);
  if (status_.truncated) truncationNotice_ = true;

  if (sink_ != nullptr) sink_->closeSession(status_);

  publish(status_.truncated ? 4 : 1,
          status_.truncated ? "the dataset was truncated: the medium is full"
                            : "logging stopped",
          detail.code);
}

Status DataLogger::stop(LogStopReason reason) {
  if (!status_.recording) return fail(ErrorCode::kInvalidState, "not recording");
  endSession(reason, ok());
  return ok();
}

bool DataLogger::takeTruncationNotice() {
  const bool notice = truncationNotice_;
  truncationNotice_ = false;
  return notice;
}

// ---------------------------------------------------------------------------
//  Sampling
// ---------------------------------------------------------------------------
void DataLogger::appendRow(Micros now) {
  std::size_t used = 0;
  // Every append is checked.  Before 0.15.1-m15 they were summed blind, and a
  // single wide column — a saturated sensor printed as ",%.3f" of 3.4e38 is 44
  // characters on its own — pushed the offset past the end of the buffer, after
  // which the "remaining capacity" was an unsigned subtraction that had wrapped.
  // See core/Format.h.
  bool complete = true;

  // t_ms is measured from the start of the LOGICAL session and is not reset by
  // rotation: a segment boundary is a fact about files, not about the
  // experiment (M15 §3.1).
  const Micros elapsedMs = (now - status_.startedUs) / 1000ULL;
  const EpochMs epoch = clock_.epochMillis();
  const std::uint32_t globalRow = nextGlobalRow_;
  complete &= appendFormat(row_, sizeof(row_), used, "%llu,%llu",
                           static_cast<unsigned long long>(elapsedMs),
                           static_cast<unsigned long long>(epoch));
  if (spec_.storageMode == LogStorageMode::kContinuousOffload) {
    complete &= appendFormat(row_, sizeof(row_), used, ",%u",
                             static_cast<unsigned>(globalRow));
  }

  // One bit per channel: set when that channel's value was anything other than
  // GOOD at this instant.  Cheaper than a quality column per channel and, more
  // importantly, impossible to leave out of a row.
  std::uint32_t qualityMask = 0;

  // Room kept back for the quality mask, which is not optional: a row that ran
  // out of space before it would be missing its last column, and a CSV with a
  // short row is a CSV that silently changes shape halfway down.
  const std::size_t bodyCapacity = sizeof(row_) - kRowSuffixBytes;

  for (std::size_t i = 0; i < spec_.channelCount; ++i) {
    const ChannelValue* value = channels_.value(spec_.channels[i]);
    const ChannelDescriptor* descriptor = channels_.descriptor(spec_.channels[i]);
    std::uint8_t precision = (descriptor != nullptr) ? descriptor->precision : 3;
    // Clamped, not trusted: precision arrives from a configuration file, and
    // "%.*f" with a precision of 200 is a 240-character column.
    if (precision > kMaxPrecision) precision = kMaxPrecision;

    if (value == nullptr) {
      // The channel disappeared mid-session (its device was removed).  The
      // column stays — a dataset whose column count changes halfway through is
      // not a dataset — and the quality bit says why it is empty.
      qualityMask |= (1u << i);
      complete &= appendFormat(row_, bodyCapacity, used,
                               spec_.includeRaw ? ",," : ",");
      continue;
    }
    if (value->quality != ChannelQuality::kGood) qualityMask |= (1u << i);

    if (spec_.includeRaw) {
      complete &= appendFormat(row_, bodyCapacity, used, ",%.6g",
                               static_cast<double>(value->raw));
    }
    complete &= appendFormat(row_, bodyCapacity, used, ",%.*f",
                             static_cast<int>(precision),
                             static_cast<double>(value->processed));
  }

  complete &= appendFormat(row_, sizeof(row_), used, ",%u\n", qualityMask);

  // The number is consumed whether or not the row survives.  A dropped row that
  // renumbered the ones after it would close the gap in the CSV and make a loss
  // undetectable by reading the file — which is the only way most people will
  // ever read it.
  ++nextGlobalRow_;
  status_.nextGlobalRow = nextGlobalRow_;

  if (!complete) {
    // kRowBytes is derived from the same limits the logger enforces at start(),
    // so this is unreachable by configuration.  If it ever fires, the row is
    // malformed and is counted as lost rather than written: a truncated line in
    // the middle of a dataset is worse than a visible gap, because the gap is
    // the thing global_row and dropped_rows exist to make visible.
    ++status_.droppedRows;
    return;
  }

  if (buffered_ + used > kBufferBytes) {
    // The medium is not keeping up.  Counted, reported in the footer and in the
    // index, and never quietly skipped.
    ++status_.droppedRows;
    return;
  }
  if (bufferedRows_ == 0) firstBufferedGlobalRow_ = globalRow;
  std::memcpy(buffer_ + buffered_, row_, used);
  buffered_ += used;
  ++bufferedRows_;
  lastBufferedGlobalRow_ = globalRow;
  ++status_.rows;
}

/** The pending buffer, described for the store.  Empty when nothing is held. */
LogBatch DataLogger::pendingBatch() const {
  LogBatch batch;
  batch.text = buffer_;
  batch.bytes = buffered_;
  batch.rows = bufferedRows_;
  batch.firstGlobalRow = firstBufferedGlobalRow_;
  batch.lastGlobalRow = lastBufferedGlobalRow_;
  batch.droppedRowsTotal = status_.droppedRows;
  return batch;
}

void DataLogger::sampleTick(Micros now) {
  if (!status_.recording) return;
  if (lastRowUs_ != 0 && now - lastRowUs_ < periodUs_) return;
  lastRowUs_ = now;
  appendRow(now);
}

void DataLogger::flushTick() {
  if (sink_ == nullptr) return;
  if (!status_.recording || buffered_ == 0) return;

  // The reserve check happens BEFORE the write, not after a failure: the point
  // is that the instrument keeps enough room to serve its own interface and
  // save its configuration, whatever the dataset is doing.
  if (sink_->writableBytes() < buffered_) {
    endSession(LogStopReason::kFull,
               fail(ErrorCode::kFilesystemFull,
                    "the medium reached its reserve; the run continues"));
    return;
  }

  const Status written = sink_->appendRows(pendingBatch());
  if (!written.ok()) {
    // A store that has run out of room for the OFFLOAD QUEUE says so
    // specifically, so the operator is told to reconnect the collector rather
    // than to go and delete datasets.
    endSession(written.code == ErrorCode::kOutOfCapacity
                   ? LogStopReason::kOffloadBacklogFull
                   : LogStopReason::kWriteFailed,
               written);
    return;
  }
  status_.bytesWritten += buffered_;
  buffered_ = 0;
  bufferedRows_ = 0;

  // A part closed during that write.  Told to whoever is listening so the
  // collector starts at once instead of at its next poll.
  const std::uint32_t ready = sink_->takeSegmentReadyNotice();
  if (ready != 0) {
    Event event;
    event.type = EventType::kLogSegmentReady;
    event.severity = 1;
    event.code = ErrorCode::kOk;
    event.source = ready;
    event.detail = "a log segment is ready to collect";
    event.timestamp = clock_.nowMicros();
    events_.publish(event);
  }
}

}  // namespace lc
