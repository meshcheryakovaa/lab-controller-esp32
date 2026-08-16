// =============================================================================
//  services/CalibrationSolver.h — turns reference points into coefficients (§12).
//
//  The user enters a table:
//        RAW      REFERENCE
//      453211           0 g
//      498322         100 g
//      543419         200 g
//  and the firmware computes the fit.  No spreadsheets, no manual algebra.
//
//  NUMERICAL NOTE — this is the part that quietly goes wrong otherwise.
//  Fitting y = a0 + a1*x + a2*x^2 directly on raw HX711 counts (~5e5) means
//  building a normal-equations matrix with entries around 1e24 and solving it
//  in float.  The result is garbage.  So the solver always centres and scales
//  the abscissa first:
//        u = (x - xCenter) / xScale
//  fits in u, and returns xCenter/xScale alongside the coefficients.  The
//  processor applies the same transform at run time.  Coefficients are
//  therefore only meaningful together with their transform — store all of it.
// =============================================================================
#pragma once

#include <cstddef>

#include "core/Error.h"

namespace lc {

inline constexpr std::size_t kMaxPolynomialOrder = 4;
inline constexpr std::size_t kMaxCalibrationPoints = 16;

struct CalibrationPoint {
  float raw = 0.0f;
  float reference = 0.0f;
};

struct PolynomialFit {
  double coefficients[kMaxPolynomialOrder + 1] = {0.0};
  std::size_t order = 1;
  double xCenter = 0.0;
  double xScale = 1.0;

  // Quality indicators the UI should show next to the fit — a calibration the
  // operator cannot judge is a calibration nobody should trust.
  double rmsResidual = 0.0;
  double maxResidual = 0.0;
  double rSquared = 0.0;

  double evaluate(double x) const;
};

class CalibrationSolver {
 public:
  // Least-squares polynomial fit.  Requires at least (order + 1) points.
  static Result<PolynomialFit> fitPolynomial(const CalibrationPoint* points,
                                             std::size_t count,
                                             std::size_t order);

  // Convenience wrappers with the semantics of §12.
  static Result<PolynomialFit> fitOffset(const CalibrationPoint* points,
                                         std::size_t count);
  static Result<PolynomialFit> fitLinear(const CalibrationPoint* points,
                                         std::size_t count) {
    return fitPolynomial(points, count, 1);
  }
};

}  // namespace lc
