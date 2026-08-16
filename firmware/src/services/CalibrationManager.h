// =============================================================================
//  services/CalibrationManager.h — which calibration a channel is running (§12).
//
//  WHAT THIS OWNS AND WHAT IT DOES NOT (ADR-0014)
//
//  A calibration has two very different halves:
//
//    * the ACTIVE one — a live property of a running channel.  The logger has
//      to stamp every dataset with it (§48), the API has to report it, and
//      exactly one may exist per channel.  That is state, and this class owns it.
//
//    * the HISTORY — every fit ever made for that channel, with its reference
//      points and residuals.  It is read when somebody opens the calibration
//      editor and never otherwise.  That is a document; it lives in
//      calibrations.json and is not held in RAM.  Sixteen reference points per
//      record is 128 bytes, and a rig that has been recalibrated weekly for a
//      year would otherwise cost kilobytes to serve a page nobody has open.
//
//  Records are IMMUTABLE.  Recalibrating produces a new version; it never edits
//  an old one.  That is what makes "roll back to Tuesday's calibration" a
//  pointer move instead of an archaeology exercise, and it is what makes a
//  recorded dataset attributable at all.
//
//  This class is Arduino-free and JSON-free, like the rest of services/.
// =============================================================================
#pragma once

#include "core/Error.h"
#include "core/Types.h"
#include "services/CalibrationSolver.h"

namespace lc {

enum class CalibrationKind : std::uint8_t {
  kOffset = 0,   // y = x + b        — a tare
  kLinear,       // y = a0 + a1*u
  kPoly2,
  kPoly3,
  kTable,        // piecewise linear through the points, no extrapolation
};

const char* toString(CalibrationKind kind);
bool parseCalibrationKind(const char* text, CalibrationKind& out);

// Polynomial order implied by the kind; kTable has none and returns 0.
std::size_t polynomialOrderFor(CalibrationKind kind);

// The part of a calibration record that a RUNNING system needs.  Reference
// points are deliberately absent: nothing in the data plane looks at them.
struct ActiveCalibration {
  KeyString channel;
  CalibrationIdString id;
  std::uint16_t version = 0;
  CalibrationKind kind = CalibrationKind::kLinear;
  UnitString unit;
  std::uint8_t pointCount = 0;

  // Shown next to the reading so an operator can judge the fit at a glance.
  // A calibration whose quality nobody can see is a calibration nobody should
  // trust (§12).
  float rmsResidual = 0.0f;
  float maxResidual = 0.0f;
  float rSquared = 0.0f;

  // 0 when the clock had never been synchronised — recorded honestly rather
  // than filled with boot-relative nonsense that looks like a date.
  EpochMs createdEpochMs = 0;
};

class CalibrationManager {
 public:
  static constexpr std::size_t capacity() { return limits::kMaxActiveCalibrations; }

  // Installs (or replaces) the active calibration of a channel.  One per
  // channel is an invariant, not a convention: two active fits on one channel
  // means the number on screen depends on which one the pipeline happened to
  // pick up.
  Status setActive(const ActiveCalibration& record);

  Status clearActive(const char* channelKey);

  const ActiveCalibration* activeFor(const char* channelKey) const;
  const ActiveCalibration* byId(const char* id) const;

  std::size_t count() const { return count_; }
  const ActiveCalibration& at(std::size_t index) const { return records_[index]; }

  void clearAll() { count_ = 0; }

  // Builds "<channelKey>#<version>".  Returns false if the key was too long to
  // fit, which the caller must turn into a validation error rather than
  // silently producing a truncated — and therefore colliding — id.
  static bool makeId(const char* channelKey, std::uint16_t version,
                     CalibrationIdString& out);

 private:
  std::size_t indexOf(const char* channelKey) const;

  ActiveCalibration records_[limits::kMaxActiveCalibrations];
  std::size_t count_ = 0;
};

}  // namespace lc
