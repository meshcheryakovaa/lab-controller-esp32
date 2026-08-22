#include "storage/LogStore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "core/Format.h"
#include "core/MemoryDiagnostics.h"
#include "storage/JsonUtils.h"

namespace lc {

Status LogStore::begin() {
  const LockGuard held(mutex_);
  const Status directory = backend_.ensureDirectory(kDirectory);
  if (!directory.ok()) return directory;
  // Sort out what a power cut left behind before anything can read the index
  // and believe it (§13).
  return recoverSessions();
}

std::uint32_t LogStore::takeSegmentReadyNotice() {
  const LockGuard held(mutex_);
  const std::uint32_t ready = segmentReadyNotice_;
  segmentReadyNotice_ = 0;
  return ready;
}

std::size_t LogStore::writableBytes() const {
  const std::size_t free = backend_.freeBytes();
  return (free > kReserveBytes) ? (free - kReserveBytes) : 0;
}

Status LogStore::loadIndex(JsonDocument& out) const {
  const LockGuard held(mutex_);
  out.clear();
  if (!backend_.exists(kIndexPath)) {
    JsonObject root = out.to<JsonObject>();
    root["schemaVersion"] = LC_CONFIG_SCHEMA_VERSION;
    root["next"] = 1;
    root["logs"].to<JsonArray>();
    return ok();
  }

  const Result<std::size_t> fileSize = backend_.size(kIndexPath);
  if (!fileSize.ok()) return fileSize.error();
  if (fileSize.value() >= kMaxIndexBytes) {
    return fail(ErrorCode::kPayloadTooLarge, kIndexPath);
  }
  const std::size_t capacity = fileSize.value() + 1;
  char* buffer = new (std::nothrow) char[capacity];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");
  const Result<std::size_t> read = backend_.read(kIndexPath, buffer, capacity);
  if (!read.ok()) {
    delete[] buffer;
    return read.error();
  }
  // const char*, so ArduinoJson copies instead of pointing into a buffer that
  // is about to be freed — the trap ConfigStorage documents and RunLog fell
  // into once already.
  const DeserializationError parsed =
      deserializeJson(out, static_cast<const char*>(buffer), read.value());
  delete[] buffer;
  if (parsed) return fail(ErrorCode::kConfigCorrupt, parsed.c_str());
  return ok();
}

Status LogStore::saveIndex(JsonDocument& document) {
  const std::size_t needed = measureJson(document) + 1;
  if (needed >= kMaxIndexBytes) return fail(ErrorCode::kPayloadTooLarge, kIndexPath);
  char* buffer = new (std::nothrow) char[needed];
  if (buffer == nullptr) return fail(ErrorCode::kOutOfCapacity, "no heap");
  const std::size_t written = serializeJson(document, buffer, needed);
  const Status saved = backend_.writeAtomic(kIndexPath, buffer, written);
  delete[] buffer;
  return saved;
}

std::size_t LogStore::sessionCount() const {
  const LockGuard held(mutex_);
  JsonDocument index;
  if (!loadIndex(index).ok()) return 0;
  return index["logs"].as<JsonArrayConst>().size();
}

Status LogStore::writeHeader(const LogSpec& spec, const char* const* columns,
                             std::size_t columnCount) {
  // The column line is kept because every SEGMENT needs it: each part of a
  // continuous session is a valid CSV on its own, and one that inherited its
  // column names from a file the reader may never have received would not be.
  //
  // A truncated column line is not a cosmetic problem.  It is a CSV whose
  // header names fewer columns than the rows contain, or names one of them
  // half-way through — a file that every parser reads differently and none
  // rejects.  So this REFUSES rather than truncates (0.15.1-m15).
  std::size_t line = 0;
  bool complete =
      appendFormat(columnLine_, sizeof(columnLine_), line, "t_ms,epoch_ms");
  if (spec.storageMode == LogStorageMode::kContinuousOffload) {
    complete &= appendFormat(columnLine_, sizeof(columnLine_), line, ",global_row");
  }
  for (std::size_t i = 0; i < columnCount; ++i) {
    complete &=
        appendFormat(columnLine_, sizeof(columnLine_), line, ",%s", columns[i]);
  }
  complete &= appendFormat(columnLine_, sizeof(columnLine_), line, ",quality_mask");

  if (!complete) {
    // kColumnLineBytes is derived from kMaxLoggedChannels and the key/unit
    // lengths, so a rig the logger accepts always fits.  Reaching this means
    // one of those limits moved without the arithmetic moving with it.
    columnLine_[0] = '\0';
    columnLineLength_ = 0;
    return fail(ErrorCode::kOutOfCapacity,
                "the column names do not fit in one header line");
  }
  columnLineLength_ = line;
  return ok();
}

/**
 * The header of one segment.  Deferred until the first batch arrives, because
 * `first_global_row` is a fact about the rows and is not known before them —
 * and because a segment that never receives a row should not exist as a file.
 */
Status LogStore::ensureSegmentHeader(std::uint32_t firstGlobalRow) {
  if (headerWritten_) return ok();
  if (columnLineLength_ == 0) {
    return fail(ErrorCode::kInvalidState, "the column line was never built");
  }

  char header[kHeaderBytes];
  std::size_t used = 0;
  // Every fragment is checked (core/Format.h).  The version of this function
  // that summed snprintf return values could leave `used` well past
  // sizeof(header) — and then handed that number to backend_.append() as the
  // length to write, so the filesystem read several hundred bytes of whatever
  // followed the buffer and put them in the dataset.
  bool complete = appendFormat(header, sizeof(header), used,
                               "# dataset: %s\n", currentId_.c_str());
  if (mode_ == LogStorageMode::kContinuousOffload) {
    complete &= appendFormat(header, sizeof(header), used,
                             "# segment: %u\n# mode: %s\n",
                             static_cast<unsigned>(sequence_), toString(mode_));
  }
  complete &= appendFormat(
      header, sizeof(header), used,
      "# name: %s\n# experiment: %s\n# operator: %s\n# sample: %s\n",
      spec_.name.c_str(),
      spec_.experiment.empty() ? "(manual)" : spec_.experiment.c_str(),
      spec_.operatorName.empty() ? "(unknown)" : spec_.operatorName.c_str(),
      spec_.sample.empty() ? "(unspecified)" : spec_.sample.c_str());

  complete &= appendFormat(
      header, sizeof(header), used,
      "# firmware: %s\n# config_fingerprint: %08x\n# config_revision: %u\n"
      "# rate_hz: %.4g\n",
      LC_FIRMWARE_VERSION, static_cast<unsigned>(storage_.fingerprint()),
      static_cast<unsigned>(storage_.revision()),
      static_cast<double>(spec_.rateHz));

  if (mode_ == LogStorageMode::kContinuousOffload) {
    // Enough to detect a missing part by reading two files, without an index.
    complete &= appendFormat(header, sizeof(header), used,
                             "# first_global_row: %u\n# previous_segment: %u\n",
                             static_cast<unsigned>(firstGlobalRow),
                             static_cast<unsigned>(previousSegment_));
  }
  if (!complete) {
    // The fixed part of the header did not fit, which no configuration should
    // be able to arrange — every field in it is a bounded FixedString.
    return fail(ErrorCode::kOutOfCapacity, "the dataset header does not fit");
  }

  // Which calibrations produced the units in the column names.  Without this
  // line the numbers are grams only by assertion.
  //
  // This list is the one genuinely unbounded part: a rig may have far more
  // calibrations than a 1 KiB header can name.  So it is cut DELIBERATELY and
  // the header says how many it left out, rather than stopping at whatever
  // happened to fit and reading as a complete list.
  appendFormat(header, sizeof(header), used, "# calibrations:");
  const std::size_t reserve = 48;  // room for " (+NN more)\n" and a margin
  std::size_t listed = 0;
  std::size_t omitted = 0;
  if (calibrations_ != nullptr) {
    for (std::size_t i = 0; i < calibrations_->count(); ++i) {
      const ActiveCalibration& record = calibrations_->at(i);
      std::size_t attempt = used;
      const bool fitted =
          appendFormat(header, sizeof(header) - reserve, attempt, "%s %s=%s",
                       listed == 0 ? "" : ",", record.channel.c_str(),
                       record.id.c_str());
      if (!fitted) {
        // Rewound: a half-written entry would name a calibration that is not
        // the one that produced the column.
        header[used] = '\0';
        omitted = calibrations_->count() - i;
        break;
      }
      used = attempt;
      ++listed;
    }
  }
  if (listed == 0 && omitted == 0) {
    appendFormat(header, sizeof(header), used, " none");
  }
  if (omitted > 0) {
    appendFormat(header, sizeof(header), used, " (+%u more)",
                 static_cast<unsigned>(omitted));
  }
  appendFormat(header, sizeof(header), used, "\n");

  const Status written = backend_.append(currentPath_.c_str(), header, used);
  if (!written.ok()) return written;
  segmentBytes_ += used;

  // The column line goes to the file as its own append, and never through the
  // header buffer.  It is up to kColumnLineBytes long on a full rig, which no
  // sane header buffer could also hold — trying to make it was what turned a
  // truncation into an over-read.
  const Status columns =
      backend_.append(currentPath_.c_str(), columnLine_, columnLineLength_);
  if (!columns.ok()) return columns;
  const Status newline = backend_.append(currentPath_.c_str(), "\n", 1);
  if (!newline.ok()) return newline;
  segmentBytes_ += columnLineLength_ + 1;

  headerWritten_ = true;
  return ok();
}

void LogStore::segmentPath(std::uint32_t sequence, FixedString<64>& out) const {
  char path[64];
  std::snprintf(path, sizeof(path), "%s/%s_p%06u.csv", kDirectory,
                currentId_.c_str(), static_cast<unsigned>(sequence));
  out.assign(path);
}

Status LogStore::openSegment(std::uint32_t sequence) {
  sequence_ = sequence;
  segmentBytes_ = 0;
  payloadBytes_ = 0;
  segmentRows_ = 0;
  segmentFirstRow_ = 0;
  segmentLastRow_ = 0;
  payloadCrc_.reset();
  headerWritten_ = false;
  lastActiveSaveBytes_ = 0;
  segmentPath(sequence, currentPath_);
  // A leftover from an interrupted run with the same number would otherwise be
  // appended to, producing a file with two headers.
  backend_.remove(currentPath_.c_str());
  return ok();
}

Status LogStore::openSession(const LogSpec& spec, const char* const* columns,
                             std::size_t columnCount, KeyString& id) {
  const LockGuard held(mutex_);
  if (open_) return fail(ErrorCode::kResourceBusy, "a dataset is already open");

  JsonDocument index;
  const Status loaded = loadIndex(index);
  if (!loaded.ok()) return loaded;

  JsonArray logs = index["logs"].as<JsonArray>();
  if (logs.size() >= limits::kMaxLogSessions) {
    // Refused, not made room for.  Which dataset is expendable is not a
    // decision firmware gets to make (§33).  A continuous session counts once
    // however many segments it goes on to produce (M15 §11).
    return fail(ErrorCode::kOutOfCapacity,
                "the log index is full; delete a dataset first");
  }

  mode_ = spec.storageMode;
  segmentLimit_ = kDefaultSegmentBytes;
  if (mode_ == LogStorageMode::kContinuousOffload) {
    if (spec.segmentBytes != 0) {
      segmentLimit_ = spec.segmentBytes;
      if (segmentLimit_ < kMinSegmentBytes) segmentLimit_ = kMinSegmentBytes;
      if (segmentLimit_ > kMaxSegmentBytes) segmentLimit_ = kMaxSegmentBytes;
    }
    // Continuous mode turns the filesystem into a transfer buffer, and a buffer
    // that cannot hold the part being written plus one waiting to be collected
    // is not a buffer.  Refused here, with the numbers, rather than three
    // minutes in (§21).
    if (writableBytes() < segmentLimit_ * 2) {
      char detail[limits::kDetailLength];
      std::snprintf(detail, sizeof(detail),
                    "continuous logging needs ~%u KB free, %u KB available",
                    static_cast<unsigned>(segmentLimit_ * 2 / 1024),
                    static_cast<unsigned>(writableBytes() / 1024));
      return fail(ErrorCode::kFilesystemFull, detail);
    }
  }

  const std::uint32_t next = index["next"] | 1;
  char buffer[limits::kKeyLength];
  std::snprintf(buffer, sizeof(buffer), "log_%04u", static_cast<unsigned>(next));
  currentId_.assign(buffer);
  id = currentId_;

  spec_ = spec;
  collectorId_ = spec.collectorId;
  previousSegment_ = 0;
  // Checked: this builds columnLine_, and a dataset whose column line did not
  // fit must never reach the disk (0.15.1-m15).
  const Status columnsBuilt = writeHeader(spec, columns, columnCount);
  if (!columnsBuilt.ok()) return columnsBuilt;

  if (mode_ == LogStorageMode::kContinuousOffload) {
    openSegment(1);
  } else {
    char path[64];
    std::snprintf(path, sizeof(path), "%s/%s.csv", kDirectory, currentId_.c_str());
    currentPath_.assign(path);
    sequence_ = 0;
    segmentBytes_ = 0;
    headerWritten_ = false;
    backend_.remove(currentPath_.c_str());
    // Single mode keeps writing its header at open, as it always did: there is
    // no first_global_row to wait for, and an empty dataset with a header is
    // exactly what the Logs page has always shown while recording.
    const Status header = ensureSegmentHeader(1);
    if (!header.ok()) {
      backend_.remove(currentPath_.c_str());
      return header;
    }
  }

  // The index entry is written at OPEN, not at close: a controller that loses
  // power mid-session must still leave a trace that the dataset exists, and the
  // file on disk is not self-describing to a page that lists datasets.
  JsonObject entry = logs.add<JsonObject>();
  entry["id"] = jsonCopy(currentId_.c_str());
  entry["name"] = jsonCopy(spec.name.c_str());
  entry["experiment"] = jsonCopy(spec.experiment.c_str());
  entry["operator"] = jsonCopy(spec.operatorName.c_str());
  entry["sample"] = jsonCopy(spec.sample.c_str());
  entry["path"] = jsonCopy(currentPath_.c_str());
  entry["rate_hz"] = spec.rateHz;
  entry["channels"] = spec.channelCount;
  entry["config_revision"] = storage_.revision();
  entry["config_fingerprint"] = storage_.fingerprint();
  entry["firmware"] = LC_FIRMWARE_VERSION;
  entry["state"] = "RECORDING";
  entry["rows"] = 0;
  entry["dropped"] = 0;
  entry["bytes"] = 0;
  entry["truncated"] = false;
  entry["mode"] = toString(mode_);
  if (mode_ == LogStorageMode::kContinuousOffload) {
    entry["collector_id"] = jsonCopy(collectorId_.c_str());
    entry["segment_bytes"] = segmentLimit_;
    entry["segments_completed"] = 0;
    entry["segments_acked"] = 0;
    entry["acked_through"] = 0;
    entry["next_segment"] = 1;
    entry["pending"].to<JsonArray>();
    JsonObject active = entry["active"].to<JsonObject>();
    active["sequence"] = 1;
    active["path"] = jsonCopy(currentPath_.c_str());
    active["bytes"] = 0;
    active["rows"] = 0;
  }
  JsonArray columnNames = entry["columns"].to<JsonArray>();
  for (std::size_t i = 0; i < columnCount; ++i) columnNames.add(jsonCopy(columns[i]));

  index["next"] = next + 1;
  const Status saved = saveIndex(index);
  if (!saved.ok()) {
    backend_.remove(currentPath_.c_str());
    return saved;
  }

  open_ = true;
  return ok();
}

/**
 * Move the finished segment into the queue and record it.
 *
 * Order matters and is the point of §12: the footer and the index are written
 * BEFORE anything says READY.  A segment announced before its footer exists is
 * one a collector may download, checksum against a total that was never
 * written, and fail — or worse, one whose ACK deletes a file that was still
 * being closed.
 */
Status LogStore::recordSegmentInIndex(bool truncated, std::uint32_t nextSequence) {
  JsonDocument index;
  const Status loaded = loadIndex(index);
  if (!loaded.ok()) return loaded;

  for (JsonObject entry : index["logs"].as<JsonArray>()) {
    if (std::strcmp(entry["id"] | "", currentId_.c_str()) != 0) continue;
    JsonArray pending = entry["pending"].as<JsonArray>();
    if (pending.isNull()) pending = entry["pending"].to<JsonArray>();
    if (pending.size() >= kMaxPendingSegments) {
      return fail(ErrorCode::kOutOfCapacity,
                  "the offload queue is full; nothing is collecting segments");
    }
    JsonObject item = pending.add<JsonObject>();
    item["sequence"] = sequence_;
    item["path"] = jsonCopy(currentPath_.c_str());
    item["bytes"] = segmentBytes_;
    item["rows"] = segmentRows_;
    item["first_row"] = segmentFirstRow_;
    item["last_row"] = segmentLastRow_;
    item["payload_bytes"] = payloadBytes_;
    char crc[16];
    std::snprintf(crc, sizeof(crc), "%08x",
                  static_cast<unsigned>(payloadCrc_.value()));
    item["payload_crc32"] = jsonCopy(crc);
    // A part that a power cut interrupted may be downloaded, but it is never
    // offered as a complete one.
    item["state"] = truncated ? "RECOVERED_TRUNCATED" : "READY";

    entry["segments_completed"] = (entry["segments_completed"] | 0u) + 1u;
    entry["next_segment"] = sequence_ + 1;

    // The part that is about to be opened is recorded in the SAME write.  Two
    // writes would be two chances to lose power in between, and the window
    // between them is exactly the one where the index would describe no active
    // file at all — so a cut there would leave the new part unclaimed by
    // anything, which is how measurements become an orphan instead of a
    // recovered segment.  One write also keeps the flash cost at §22's "one
    // index save per rotation".
    if (nextSequence != 0) {
      FixedString<64> nextPath;
      char path[64];
      std::snprintf(path, sizeof(path), "%s/%s_p%06u.csv", kDirectory,
                    currentId_.c_str(), static_cast<unsigned>(nextSequence));
      nextPath.assign(path);
      JsonObject active = entry["active"].to<JsonObject>();
      active["sequence"] = nextSequence;
      active["path"] = jsonCopy(nextPath.c_str());
      active["bytes"] = 0;
      active["rows"] = 0;
    } else {
      entry.remove("active");
    }
    return saveIndex(index);
  }
  return fail(ErrorCode::kNotFound, currentId_.c_str());
}

Status LogStore::finalizeSegment(bool truncated, std::uint32_t nextSequence) {
  if (!headerWritten_) {
    // Nothing was ever written into this part; there is no file to close and
    // nothing to hand over.
    return ok();
  }
  char footer[320];
  std::size_t used = 0;
  // The footer carries the checksum that authorises deleting this file, so a
  // truncated one is not a cosmetic loss: it is a segment that can never be
  // acknowledged and therefore sits in the queue for ever.  kSegmentFooterReserve
  // is what appendRows keeps free for exactly these bytes.
  const bool complete = appendFormat(
      footer, sizeof(footer), used,
      "# segment_complete\n# segment_rows: %u\n# first_global_row: %u\n"
      "# last_global_row: %u\n# dropped_rows_total: %u\n"
      "# payload_bytes: %u\n# payload_crc32: %08x\n%s",
      static_cast<unsigned>(segmentRows_),
      static_cast<unsigned>(segmentFirstRow_),
      static_cast<unsigned>(segmentLastRow_),
      static_cast<unsigned>(droppedAtSegmentEnd_),
      static_cast<unsigned>(payloadBytes_),
      static_cast<unsigned>(payloadCrc_.value()),
      truncated
          ? "# RECOVERED_TRUNCATED: power was lost while this part was open\n"
          : "");
  if (!complete) return fail(ErrorCode::kInternal, "the segment footer does not fit");
  if (used > 0) {
    const Status written = backend_.append(currentPath_.c_str(), footer, used);
    if (!written.ok()) return written;
    segmentBytes_ += used;
  }
  const Status recorded = recordSegmentInIndex(truncated, nextSequence);
  if (!recorded.ok()) return recorded;
  previousSegment_ = sequence_;
  // Only now: the footer is on disk and the index names the part.  Announcing
  // READY any earlier would invite a collector to download a file that is still
  // being closed (§12).
  segmentReadyNotice_ = sequence_;
  return ok();
}

/** The active segment's size, written rarely.  See kActiveSaveEveryBytes. */
Status LogStore::refreshActiveInIndex() {
  JsonDocument index;
  if (!loadIndex(index).ok()) return ok();
  for (JsonObject entry : index["logs"].as<JsonArray>()) {
    if (std::strcmp(entry["id"] | "", currentId_.c_str()) != 0) continue;
    JsonObject active = entry["active"].as<JsonObject>();
    if (active.isNull()) active = entry["active"].to<JsonObject>();
    active["sequence"] = sequence_;
    active["path"] = jsonCopy(currentPath_.c_str());
    active["bytes"] = segmentBytes_;
    active["rows"] = segmentRows_;
    return saveIndex(index);
  }
  return ok();
}

Status LogStore::appendRows(const LogBatch& batch) {
  const LockGuard held(mutex_);
  if (!open_) return fail(ErrorCode::kInvalidState, "no dataset is open");
  if (batch.bytes == 0) return ok();
  droppedAtSegmentEnd_ = batch.droppedRowsTotal;

  if (mode_ == LogStorageMode::kContinuousOffload) {
    // Rotate BEFORE writing, so the limit is a promise about the finished file
    // rather than a place it happened to stop.  `segmentRows_ > 0` keeps a
    // single oversized batch from closing an empty part behind it.
    const std::size_t projected = segmentBytes_ + batch.bytes + kSegmentFooterReserve;
    if (segmentRows_ > 0 && projected > segmentLimit_) {
      LC_MEM_REPORT("before segment rotate");
      LC_MEM_CHECK("before segment rotate");
      const std::uint32_t next = sequence_ + 1;
      const Status closed = finalizeSegment(false, next);
      if (!closed.ok()) return closed;
      const Status opened = openSegment(next);
      if (!opened.ok()) return opened;
      LC_MEM_REPORT("after segment rotate");
      LC_MEM_CHECK("after segment rotate");
    }
    // Room for this batch AND for the footer that will close the part.  A file
    // whose footer did not fit cannot be verified, and an unverifiable file can
    // never be acknowledged — so it would sit in the queue for ever.
    if (writableBytes() < batch.bytes + kSegmentFooterReserve) {
      return fail(ErrorCode::kOutOfCapacity,
                  "no room for the next segment: the queue is not being collected");
    }
  }

  const Status header = ensureSegmentHeader(batch.firstGlobalRow);
  if (!header.ok()) return header;

  const Status written = backend_.append(currentPath_.c_str(), batch.text, batch.bytes);
  if (!written.ok()) return written;

  // The checksum covers the DATA ROWS only — not the header, and not the footer
  // that will carry it.  Otherwise the number would have to be part of what it
  // is computed over (§9).
  payloadCrc_.update(batch.text, batch.bytes);
  payloadBytes_ += batch.bytes;
  segmentBytes_ += batch.bytes;
  if (segmentRows_ == 0) segmentFirstRow_ = batch.firstGlobalRow;
  segmentLastRow_ = batch.lastGlobalRow;
  segmentRows_ += batch.rows;

  if (mode_ == LogStorageMode::kContinuousOffload
      && segmentBytes_ - lastActiveSaveBytes_ >= kActiveSaveEveryBytes) {
    lastActiveSaveBytes_ = segmentBytes_;
    refreshActiveInIndex();
  }
  return ok();
}

void LogStore::closeSession(const LogStatus& status) {
  const LockGuard held(mutex_);
  if (!open_) return;
  droppedAtSegmentEnd_ = status.droppedRows;

  if (mode_ == LogStorageMode::kContinuousOffload) {
    // The last part is usually short, and that is not a defect: it is where the
    // operator pressed stop.  No next segment: the session is over.
    finalizeSegment(false, 0);
  } else {
    // The footer.  A dataset that simply stops is a dataset somebody will assume
    // is complete, so every one of them ends with a line that says how.
    char footer[256];
    std::size_t used = 0;
    appendFormat(footer, sizeof(footer), used,
                 "# ended: %s\n# rows: %u\n# dropped_rows: %u\n%s",
                 toString(status.stopReason), static_cast<unsigned>(status.rows),
                 static_cast<unsigned>(status.droppedRows),
                 status.truncated
                     ? "# TRUNCATED: the medium reached its reserve; this dataset"
                       " is incomplete\n"
                     : "# complete\n");
    if (used > 0) {
      backend_.append(currentPath_.c_str(), footer, used);
    }
  }

  JsonDocument index;
  if (loadIndex(index).ok()) {
    for (JsonObject entry : index["logs"].as<JsonArray>()) {
      if (std::strcmp(entry["id"] | "", currentId_.c_str()) != 0) continue;
      if (mode_ == LogStorageMode::kContinuousOffload) {
        // Not COMPLETE: the measurements are not all on the tablet yet.  A
        // session that claimed to be finished while parts were still queued on
        // the controller is exactly the false success M15 exists to avoid.
        const std::size_t waiting = entry["pending"].as<JsonArrayConst>().size();
        entry["state"] = status.truncated
            ? "TRUNCATED"
            : (waiting > 0 ? "FINALIZING" : "COMPLETE_OFFLOADED");
      } else {
        entry["state"] = status.truncated ? "TRUNCATED" : "COMPLETE";
      }
      entry["reason"] = toString(status.stopReason);
      entry["rows"] = status.rows;
      entry["dropped"] = status.droppedRows;
      entry["bytes"] = status.bytesWritten;
      entry["truncated"] = status.truncated;
      entry["started_epoch_ms"] = status.startedEpochMs;
      entry["duration_s"] =
          static_cast<double>(status.rows) /
          (status.rateHz > 0.0f ? static_cast<double>(status.rateHz) : 1.0);
      entry.remove("active");
      if (!status.lastError.ok()) {
        JsonObject error = entry["error"].to<JsonObject>();
        error["code"] = status.lastError.symbol();
        error["detail"] = jsonCopy(status.lastError.detail.c_str());
      }
      break;
    }
    saveIndex(index);
  }

  open_ = false;
  mode_ = LogStorageMode::kSingle;
  currentId_.assign("");
  currentPath_.assign("");
}

// ---------------------------------------------------------------------------
//  M15: the offload queue
// ---------------------------------------------------------------------------

void LogStore::describeSegment(JsonObjectConst entry, SegmentInfo& out) {
  out.sequence = entry["sequence"] | 0u;
  out.bytes = entry["bytes"] | 0u;
  out.rows = entry["rows"] | 0u;
  out.firstRow = entry["first_row"] | 0u;
  out.lastRow = entry["last_row"] | 0u;
  out.payloadCrc32 = static_cast<std::uint32_t>(
      std::strtoul(entry["payload_crc32"] | "0", nullptr, 16));
  out.state.assign(entry["state"] | "READY");
  out.path.assign(entry["path"] | "");
}

std::size_t LogStore::listSegments(const char* id, SegmentInfo* out,
                                   std::size_t capacity) const {
  const LockGuard held(mutex_);
  JsonDocument index;
  if (!loadIndex(index).ok()) return 0;
  std::size_t count = 0;
  for (JsonObjectConst entry : index["logs"].as<JsonArrayConst>()) {
    if (std::strcmp(entry["id"] | "", id) != 0) continue;
    for (JsonObjectConst item : entry["pending"].as<JsonArrayConst>()) {
      if (count >= capacity) break;
      describeSegment(item, out[count++]);
    }
    break;
  }
  return count;
}

bool LogStore::segmentInfo(const char* id, std::uint32_t sequence,
                           SegmentInfo& out) const {
  const LockGuard held(mutex_);
  JsonDocument index;
  if (!loadIndex(index).ok()) return false;
  for (JsonObjectConst entry : index["logs"].as<JsonArrayConst>()) {
    if (std::strcmp(entry["id"] | "", id) != 0) continue;
    for (JsonObjectConst item : entry["pending"].as<JsonArrayConst>()) {
      if ((item["sequence"] | 0u) != sequence) continue;
      describeSegment(item, out);
      // A part still being written has no footer and no final checksum.  It is
      // deliberately invisible to the export route: serving it would hand out a
      // file whose contents change after the read (§26).
      return !out.path.empty();
    }
    break;
  }
  return false;
}

Status LogStore::acknowledgeSegment(const char* id, std::uint32_t sequence,
                                    const char* collectorId, std::size_t bytes,
                                    std::uint32_t payloadCrc32,
                                    bool& alreadyAcknowledged) {
  const LockGuard held(mutex_);
  alreadyAcknowledged = false;
  LC_MEM_REPORT("before segment ack");
  LC_MEM_CHECK("before segment ack");

  JsonDocument index;
  const Status loaded = loadIndex(index);
  if (!loaded.ok()) return loaded;

  for (JsonObject entry : index["logs"].as<JsonArray>()) {
    if (std::strcmp(entry["id"] | "", id) != 0) continue;

    // Whoever started the session owns the deletions.  Another tab of the same
    // browser is a different collector: it may not have the file, and an ACK
    // from it would delete measurements nobody holds (§18).
    const char* owner = entry["collector_id"] | "";
    if (owner[0] != '\0' && collectorId != nullptr && collectorId[0] != '\0'
        && std::strcmp(owner, collectorId) != 0) {
      return fail(ErrorCode::kForbidden,
                  "this session is being collected by another client");
    }

    // The response to a successful ACK can be lost on the way back.  Repeating
    // it must not look like an error, or a collector would retry for ever
    // against a file that is already gone (§23).
    if (sequence <= (entry["acked_through"] | 0u)) {
      alreadyAcknowledged = true;
      return ok();
    }

    JsonArray pending = entry["pending"].as<JsonArray>();
    for (std::size_t i = 0; i < pending.size(); ++i) {
      JsonObject item = pending[i];
      if ((item["sequence"] | 0u) != sequence) continue;

      const std::size_t storedBytes = item["bytes"] | 0u;
      const std::uint32_t storedCrc = static_cast<std::uint32_t>(
          std::strtoul(item["payload_crc32"] | "0", nullptr, 16));
      // Both numbers, both exact.  This comparison is the entire authorisation
      // to delete a file that holds measurements.
      if (bytes != storedBytes) {
        return fail(ErrorCode::kInvalidArgument, "the size does not match");
      }
      if (payloadCrc32 != storedCrc) {
        return fail(ErrorCode::kInvalidArgument, "the checksum does not match");
      }

      const char* path = item["path"] | "";
      if (path[0] != '\0') backend_.remove(path);
      pending.remove(i);
      LC_MEM_REPORT("after segment ack");
      LC_MEM_CHECK("after segment ack");
      entry["segments_acked"] = (entry["segments_acked"] | 0u) + 1u;
      // Monotonic and contiguous: what makes a repeat answerable without
      // keeping every number ever acknowledged.
      if (sequence == (entry["acked_through"] | 0u) + 1u) {
        entry["acked_through"] = sequence;
      }
      // A stopped session whose last part has now arrived is finally finished.
      if (pending.size() == 0
          && std::strcmp(entry["state"] | "", "FINALIZING") == 0) {
        entry["state"] = "COMPLETE_OFFLOADED";
      }
      return saveIndex(index);
    }

    // In the index, not in the queue, and not below the acknowledged mark: the
    // segment is either still being written or never existed.
    if (sequence >= (entry["next_segment"] | 1u)) {
      return fail(ErrorCode::kInvalidState,
                  "that segment is still being written");
    }
    return fail(ErrorCode::kNotFound, "no such segment");
  }
  return fail(ErrorCode::kNotFound, id);
}

/**
 * Segment files that no index entry claims (§13).
 *
 * The backend cannot enumerate a directory, so this probes the name space the
 * store itself creates rather than scanning: for each continuous session, the
 * sequence numbers near the tail are checked against the queue.  A file that
 * exists there and is claimed by nothing is either a part whose index entry was
 * lost, or one whose deletion failed after its ACK.
 *
 * The window is bounded (kOrphanProbeDepth) because probing thousands of paths
 * at boot would be a slow start for a case that only ever happens near the end
 * of a run.  It is a diagnostic, not a guarantee — and nothing here deletes
 * anything: an unexplained file holding measurements is a question for the
 * operator.
 */
std::size_t LogStore::listOrphans(FixedString<64>* out,
                                  std::size_t capacity) const {
  const LockGuard held(mutex_);
  JsonDocument index;
  if (!loadIndex(index).ok()) return 0;

  std::size_t found = 0;
  for (JsonObjectConst entry : index["logs"].as<JsonArrayConst>()) {
    if (std::strcmp(entry["mode"] | "single", "continuous_offload") != 0) continue;
    const std::uint32_t next = entry["next_segment"] | 1u;
    const std::uint32_t from =
        (next > kOrphanProbeDepth) ? (next - kOrphanProbeDepth) : 1u;
    JsonObjectConst active = entry["active"].as<JsonObjectConst>();

    for (std::uint32_t sequence = from; sequence <= next && found < capacity; ++sequence) {
      char path[64];
      std::snprintf(path, sizeof(path), "%s/%s_p%06u.csv", kDirectory,
                    entry["id"] | "", static_cast<unsigned>(sequence));
      if (!backend_.exists(path)) continue;

      bool claimed = false;
      if (!active.isNull() && (active["sequence"] | 0u) == sequence) claimed = true;
      for (JsonObjectConst item : entry["pending"].as<JsonArrayConst>()) {
        if ((item["sequence"] | 0u) == sequence) { claimed = true; break; }
      }
      if (!claimed) out[found++].assign(path);
    }
  }
  return found;
}

/**
 * What a power cut leaves behind, sorted out once at boot (§13).
 *
 * The rule throughout is that nothing is deleted and nothing is upgraded: an
 * interrupted part becomes a part that SAYS it was interrupted, a session whose
 * file has vanished becomes an ERROR rather than a quiet success, and a file
 * nobody claims is left exactly where it is for the operator to look at.
 */
Status LogStore::recoverSessions() {
  JsonDocument index;
  if (!loadIndex(index).ok()) return ok();
  bool changed = false;

  for (JsonObject entry : index["logs"].as<JsonArray>()) {
    const char* state = entry["state"] | "";
    JsonArray pending = entry["pending"].as<JsonArray>();

    // A queued segment whose file is gone must never be reported as available,
    // and must never be quietly forgotten: the measurements really are lost.
    for (std::size_t i = 0; i < pending.size();) {
      const char* path = pending[i]["path"] | "";
      if (path[0] != '\0' && !backend_.exists(path)) {
        entry["state"] = "ERROR";
        JsonObject error = entry["error"].to<JsonObject>();
        error["code"] = "NOT_FOUND";
        error["detail"] = "a queued segment file is missing";
        error["sequence"] = pending[i]["sequence"];
        pending.remove(i);
        changed = true;
        continue;
      }
      ++i;
    }

    if (std::strcmp(state, "RECORDING") != 0) continue;

    // The session was open when the power went.  Its active part has no footer,
    // so it is closed as what it is — incomplete — and offered for collection
    // rather than presented as a whole segment.
    JsonObject active = entry["active"].as<JsonObject>();
    if (!active.isNull()) {
      const char* path = active["path"] | "";
      if (path[0] != '\0' && backend_.exists(path)) {
        char footer[192];
        std::size_t used = 0;
        appendFormat(footer, sizeof(footer), used,
                     "# segment_complete\n# segment_rows: %u\n"
                     "# RECOVERED_TRUNCATED: power was lost while this part was"
                     " open; its checksum is unknown\n",
                     static_cast<unsigned>(active["rows"] | 0u));
        if (used > 0) {
          backend_.append(path, footer, used);
        }
        const Result<std::size_t> size = backend_.size(path);
        if (pending.isNull()) pending = entry["pending"].to<JsonArray>();
        if (pending.size() < kMaxPendingSegments) {
          JsonObject item = pending.add<JsonObject>();
          item["sequence"] = active["sequence"];
          item["path"] = jsonCopy(path);
          item["bytes"] = size.ok() ? size.value() : (active["bytes"] | 0u);
          item["rows"] = active["rows"] | 0u;
          item["first_row"] = 0;
          item["last_row"] = 0;
          // No checksum: the bytes were never all written, so there is nothing
          // honest to compare against.  Such a part is downloadable and NOT
          // acknowledgeable by checksum — the operator saves it deliberately.
          item["payload_crc32"] = "";
          item["state"] = "RECOVERED_TRUNCATED";
        }
      }
      entry.remove("active");
    }
    entry["state"] = "INTERRUPTED";
    entry["reason"] = "power was lost while recording";
    entry["truncated"] = true;
    changed = true;
  }

  if (!changed) return ok();
  return saveIndex(index);
}

bool LogStore::pathFor(const char* id, FixedString<64>& out) const {
  const LockGuard held(mutex_);
  JsonDocument index;
  if (!loadIndex(index).ok()) return false;
  for (JsonObjectConst entry : index["logs"].as<JsonArrayConst>()) {
    if (std::strcmp(entry["id"] | "", id) != 0) continue;
    out.assign(entry["path"] | "");
    return !out.empty();
  }
  return false;
}

Status LogStore::removeSession(const char* id) {
  const LockGuard held(mutex_);
  if (open_ && currentId_.equals(id)) {
    return fail(ErrorCode::kResourceBusy, "this dataset is being recorded");
  }

  JsonDocument index;
  const Status loaded = loadIndex(index);
  if (!loaded.ok()) return loaded;

  JsonArray logs = index["logs"].as<JsonArray>();
  for (std::size_t i = 0; i < logs.size(); ++i) {
    if (std::strcmp(logs[i]["id"] | "", id) != 0) continue;
    const char* path = logs[i]["path"] | "";
    if (path[0] != '\0') backend_.remove(path);
    logs.remove(i);
    return saveIndex(index);
  }
  return fail(ErrorCode::kNotFound, id);
}

}  // namespace lc
