#include "services/CalibrationSolver.h"

#include <cmath>

namespace lc {
namespace {

// Gaussian elimination with partial pivoting on a small dense system.
// n <= kMaxPolynomialOrder + 1 == 5, so an O(n^3) direct solve is free.
bool solveLinearSystem(double matrix[][kMaxPolynomialOrder + 2], std::size_t n) {
  for (std::size_t col = 0; col < n; ++col) {
    std::size_t pivot = col;
    for (std::size_t row = col + 1; row < n; ++row) {
      if (std::fabs(matrix[row][col]) > std::fabs(matrix[pivot][col])) {
        pivot = row;
      }
    }
    if (std::fabs(matrix[pivot][col]) < 1e-12) return false;  // singular

    if (pivot != col) {
      for (std::size_t k = col; k <= n; ++k) {
        const double tmp = matrix[col][k];
        matrix[col][k] = matrix[pivot][k];
        matrix[pivot][k] = tmp;
      }
    }

    for (std::size_t row = col + 1; row < n; ++row) {
      const double factor = matrix[row][col] / matrix[col][col];
      for (std::size_t k = col; k <= n; ++k) {
        matrix[row][k] -= factor * matrix[col][k];
      }
    }
  }

  for (std::size_t i = n; i-- > 0;) {
    double sum = matrix[i][n];
    for (std::size_t j = i + 1; j < n; ++j) sum -= matrix[i][j] * matrix[j][n];
    matrix[i][n] = sum / matrix[i][i];
  }
  return true;
}

}  // namespace

double PolynomialFit::evaluate(double x) const {
  const double u = (x - xCenter) / xScale;
  // Horner, from the highest term down.
  double result = coefficients[order];
  for (std::size_t i = order; i-- > 0;) {
    result = result * u + coefficients[i];
  }
  return result;
}

Result<PolynomialFit> CalibrationSolver::fitPolynomial(
    const CalibrationPoint* points, std::size_t count, std::size_t order) {
  if (points == nullptr) return fail(ErrorCode::kInvalidArgument, "no points");
  if (order > kMaxPolynomialOrder) {
    return fail(ErrorCode::kInvalidArgument, "polynomial order too high");
  }
  if (count < order + 1) {
    return fail(ErrorCode::kCalibrationInsufficientPoints);
  }
  if (count > kMaxCalibrationPoints) count = kMaxCalibrationPoints;

  PolynomialFit fit;
  fit.order = order;

  // --- centre and scale the abscissa --------------------------------------
  double sum = 0.0;
  double minimum = points[0].raw;
  double maximum = points[0].raw;
  for (std::size_t i = 0; i < count; ++i) {
    sum += points[i].raw;
    if (points[i].raw < minimum) minimum = points[i].raw;
    if (points[i].raw > maximum) maximum = points[i].raw;
  }
  fit.xCenter = sum / static_cast<double>(count);
  const double span = maximum - minimum;
  fit.xScale = (span > 1e-9) ? (span * 0.5) : 1.0;

  // --- normal equations ----------------------------------------------------
  const std::size_t n = order + 1;
  double system[kMaxPolynomialOrder + 1][kMaxPolynomialOrder + 2] = {{0.0}};

  for (std::size_t i = 0; i < count; ++i) {
    const double u = (points[i].raw - fit.xCenter) / fit.xScale;
    const double y = points[i].reference;

    double powers[2 * kMaxPolynomialOrder + 1];
    powers[0] = 1.0;
    for (std::size_t p = 1; p <= 2 * order; ++p) powers[p] = powers[p - 1] * u;

    for (std::size_t row = 0; row < n; ++row) {
      for (std::size_t col = 0; col < n; ++col) {
        system[row][col] += powers[row + col];
      }
      system[row][n] += powers[row] * y;
    }
  }

  if (!solveLinearSystem(system, n)) {
    // Typically means duplicated abscissae, or asking for a cubic through
    // four collinear points.
    return fail(ErrorCode::kCalibrationSingular,
                "points do not determine a unique fit");
  }
  for (std::size_t i = 0; i < n; ++i) fit.coefficients[i] = system[i][n];

  // --- residual statistics -------------------------------------------------
  double sumSquaredError = 0.0;
  double sumReference = 0.0;
  for (std::size_t i = 0; i < count; ++i) sumReference += points[i].reference;
  const double meanReference = sumReference / static_cast<double>(count);

  double totalVariance = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    const double predicted = fit.evaluate(points[i].raw);
    const double residual = points[i].reference - predicted;
    sumSquaredError += residual * residual;
    const double deviation = points[i].reference - meanReference;
    totalVariance += deviation * deviation;
    const double absolute = std::fabs(residual);
    if (absolute > fit.maxResidual) fit.maxResidual = absolute;
  }
  fit.rmsResidual = std::sqrt(sumSquaredError / static_cast<double>(count));
  fit.rSquared = (totalVariance > 1e-12)
                     ? (1.0 - sumSquaredError / totalVariance)
                     : 1.0;

  return fit;
}

Result<PolynomialFit> CalibrationSolver::fitOffset(
    const CalibrationPoint* points, std::size_t count) {
  if (points == nullptr || count == 0) {
    return fail(ErrorCode::kCalibrationInsufficientPoints);
  }
  // y = x + b, with b the mean difference between reference and raw.
  double sum = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    sum += static_cast<double>(points[i].reference) - points[i].raw;
  }

  PolynomialFit fit;
  fit.order = 1;
  fit.xCenter = 0.0;
  fit.xScale = 1.0;
  fit.coefficients[0] = sum / static_cast<double>(count);
  fit.coefficients[1] = 1.0;

  double sumSquaredError = 0.0;
  for (std::size_t i = 0; i < count; ++i) {
    const double residual = points[i].reference - fit.evaluate(points[i].raw);
    sumSquaredError += residual * residual;
    const double absolute = std::fabs(residual);
    if (absolute > fit.maxResidual) fit.maxResidual = absolute;
  }
  fit.rmsResidual = std::sqrt(sumSquaredError / static_cast<double>(count));
  fit.rSquared = 1.0;
  return fit;
}

}  // namespace lc
