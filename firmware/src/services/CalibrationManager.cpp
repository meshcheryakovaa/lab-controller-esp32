#include "services/CalibrationManager.h"

#include <cstdio>
#include <cstring>

namespace lc {

const char* toString(CalibrationKind kind) {
  switch (kind) {
    case CalibrationKind::kOffset: return "offset";
    case CalibrationKind::kLinear: return "linear";
    case CalibrationKind::kPoly2:  return "poly2";
    case CalibrationKind::kPoly3:  return "poly3";
    case CalibrationKind::kTable:  return "table";
  }
  return "linear";
}

bool parseCalibrationKind(const char* text, CalibrationKind& out) {
  if (text == nullptr) return false;
  if (std::strcmp(text, "offset") == 0) { out = CalibrationKind::kOffset; return true; }
  if (std::strcmp(text, "linear") == 0) { out = CalibrationKind::kLinear; return true; }
  if (std::strcmp(text, "poly2") == 0)  { out = CalibrationKind::kPoly2;  return true; }
  if (std::strcmp(text, "poly3") == 0)  { out = CalibrationKind::kPoly3;  return true; }
  if (std::strcmp(text, "table") == 0)  { out = CalibrationKind::kTable;  return true; }
  return false;
}

std::size_t polynomialOrderFor(CalibrationKind kind) {
  switch (kind) {
    case CalibrationKind::kOffset: return 0;
    case CalibrationKind::kLinear: return 1;
    case CalibrationKind::kPoly2:  return 2;
    case CalibrationKind::kPoly3:  return 3;
    case CalibrationKind::kTable:  break;
  }
  return 0;
}

bool CalibrationManager::makeId(const char* channelKey, std::uint16_t version,
                                CalibrationIdString& out) {
  if (channelKey == nullptr || channelKey[0] == '\0') return false;
  char buffer[CalibrationIdString::capacity() + 1];
  const int written =
      std::snprintf(buffer, sizeof(buffer), "%s#%u", channelKey,
                    static_cast<unsigned>(version));
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
    return false;
  }
  return out.assign(buffer);
}

std::size_t CalibrationManager::indexOf(const char* channelKey) const {
  if (channelKey == nullptr) return count_;
  for (std::size_t i = 0; i < count_; ++i) {
    if (records_[i].channel.equals(channelKey)) return i;
  }
  return count_;
}

Status CalibrationManager::setActive(const ActiveCalibration& record) {
  if (record.channel.empty()) {
    return fail(ErrorCode::kInvalidArgument, "calibration has no channel");
  }
  if (record.id.empty()) {
    return fail(ErrorCode::kInvalidArgument, "calibration has no id");
  }

  const std::size_t existing = indexOf(record.channel.c_str());
  if (existing < count_) {
    records_[existing] = record;
    return ok();
  }
  if (count_ >= capacity()) {
    return fail(ErrorCode::kOutOfCapacity, "too many calibrated channels");
  }
  records_[count_] = record;
  ++count_;
  return ok();
}

Status CalibrationManager::clearActive(const char* channelKey) {
  const std::size_t index = indexOf(channelKey);
  if (index >= count_) return fail(ErrorCode::kNotFound, "no active calibration");
  records_[index] = records_[count_ - 1];
  --count_;
  return ok();
}

const ActiveCalibration* CalibrationManager::activeFor(const char* channelKey) const {
  const std::size_t index = indexOf(channelKey);
  return (index < count_) ? &records_[index] : nullptr;
}

const ActiveCalibration* CalibrationManager::byId(const char* id) const {
  if (id == nullptr) return nullptr;
  for (std::size_t i = 0; i < count_; ++i) {
    if (records_[i].id.equals(id)) return &records_[i];
  }
  return nullptr;
}

}  // namespace lc
