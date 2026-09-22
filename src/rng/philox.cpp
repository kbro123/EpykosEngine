// Floating-point draws of the Philox stream and the AS241 inverse normal CDF.
//
// Fixture bits must not depend on the build preset, so this TU is compiled without floating-point
// contraction on clang whatever the preset (GCC in ISO mode, -std=c++20, never contracts unless
// asked). The pragma precedes every include so it also governs anything inlined from headers.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include "epykos/rng/philox.hpp"

#include <cmath>
#include <limits>

namespace epykos::rng {

namespace {

constexpr double two_pow_minus_53 = 1.0 / 9007199254740992.0;  // 2^-53

std::uint32_t lo32(std::uint64_t x) noexcept { return static_cast<std::uint32_t>(x); }
std::uint32_t hi32(std::uint64_t x) noexcept { return static_cast<std::uint32_t>(x >> 32); }

}  // namespace

std::uint64_t Philox::bits53_at(std::uint64_t n) const noexcept {
  const std::uint64_t block = n >> 1;
  const std::array<std::uint32_t, 4> words =
      philox4x32_10({lo32(sub_), hi32(sub_), lo32(block), hi32(block)}, {lo32(seed_), hi32(seed_)});
  const std::size_t w = static_cast<std::size_t>(2 * (n & 1u));
  const std::uint64_t hi = static_cast<std::uint64_t>(words[w]) << 21;  // bits 21..52
  const std::uint64_t lo = static_cast<std::uint64_t>(words[w + 1]) >> 11;  // bits 0..20
  return hi | lo;
}

double Philox::uniform_at(std::uint64_t n) const noexcept {
  return static_cast<double>(bits53_at(n)) * two_pow_minus_53;
}

double Philox::uniform_open_at(std::uint64_t n) const noexcept {
  return (static_cast<double>(bits53_at(n)) + 0.5) * two_pow_minus_53;
}

double Philox::uniform_range(double lo, double hi) noexcept { return lo + (hi - lo) * uniform(); }

double Philox::log_uniform(double lo, double hi) noexcept {
  const double a = std::log(lo);
  const double b = std::log(hi);
  return std::exp(a + (b - a) * uniform());
}

std::int64_t Philox::uniform_int(std::int64_t lo, std::int64_t hi) noexcept {
  const double span = static_cast<double>(hi - lo + 1);
  const double k = std::floor(uniform() * span);
  return lo + static_cast<std::int64_t>(k);
}

double Philox::gaussian() noexcept { return inverse_normal_cdf(uniform_open()); }

// AS241 PPND16 (Wichura 1988). Three rational approximations: central |p − 0.5| <= 0.425, an
// intermediate tail for sqrt(−log(min(p, 1−p))) <= 5, and the far tail beyond.
double inverse_normal_cdf(double p) noexcept {
  if (!(p >= 0.0 && p <= 1.0)) return std::numeric_limits<double>::quiet_NaN();
  if (p == 0.0) return -std::numeric_limits<double>::infinity();
  if (p == 1.0) return std::numeric_limits<double>::infinity();

  const double q = p - 0.5;
  if (std::fabs(q) <= 0.425) {
    const double r = 0.180625 - q * q;
    const double num = (((((((2.5090809287301226727e+3 * r + 3.3430575583588128105e+4) * r +
                             6.7265770927008700853e+4) * r + 4.5921953931549871457e+4) * r +
                           1.3731693765509461125e+4) * r + 1.9715909503065514427e+3) * r +
                         1.3314166789178437745e+2) * r + 3.3871328727963666080e+0);
    const double den = (((((((5.2264952788528545610e+3 * r + 2.8729085735721942674e+4) * r +
                             3.9307895800092710610e+4) * r + 2.1213794301586595867e+4) * r +
                           5.3941960214247511077e+3) * r + 6.8718700749205790830e+2) * r +
                         4.2313330701600911252e+1) * r + 1.0);
    return q * num / den;
  }

  double r = q < 0.0 ? p : 1.0 - p;
  r = std::sqrt(-std::log(r));
  double val;
  if (r <= 5.0) {
    r -= 1.6;
    const double num = (((((((7.74545014278341407640e-4 * r + 2.27238449892691845833e-2) * r +
                             2.41780725177450611770e-1) * r + 1.27045825245236838258e+0) * r +
                           3.64784832476320460504e+0) * r + 5.76949722146069140550e+0) * r +
                         4.63033784615654529590e+0) * r + 1.42343711074968357734e+0);
    const double den = (((((((1.05075007164441684324e-9 * r + 5.47593808499534494600e-4) * r +
                             1.51986665636164571966e-2) * r + 1.48103976427480074590e-1) * r +
                           6.89767334985100004550e-1) * r + 1.67638483018380384940e+0) * r +
                         2.05319162663775882187e+0) * r + 1.0);
    val = num / den;
  } else {
    r -= 5.0;
    const double num = (((((((2.01033439929228813265e-7 * r + 2.71155556874348757815e-5) * r +
                             1.24266094738807843860e-3) * r + 2.65321895265761230930e-2) * r +
                           2.96560571828504891230e-1) * r + 1.78482653991729133580e+0) * r +
                         5.46378491116411436990e+0) * r + 6.65790464350110377720e+0);
    const double den = (((((((2.04426310338993978564e-15 * r + 1.42151175831644588870e-7) * r +
                             1.84631831751005468180e-5) * r + 7.86869131145613259100e-4) * r +
                           1.48753612908506148525e-2) * r + 1.36929880922735805310e-1) * r +
                         5.99832206555887937690e-1) * r + 1.0);
    val = num / den;
  }
  return q < 0.0 ? -val : val;
}

}  // namespace epykos::rng
