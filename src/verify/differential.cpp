// The differential tester's comparison (verify/differential.hpp). No maths of the program under
// test is evaluated here: the reference and compiled callables are, in their own TUs. What this
// TU computes is |a − b|, |a − b| / m and |a − b| / ulp(m) — a subtraction and a division each,
// nothing a compiler could contract — so it needs no E0 pin.
#include "epykos/verify/differential.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace epykos::verify {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

std::size_t idx(int i) { return static_cast<std::size_t>(i); }

std::uint64_t bits(double v) noexcept {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// |a − b|: +inf when one is NaN or the two are different infinities; 0 for equal infinities.
double abs_diff(double a, double b) noexcept {
  if (std::isnan(a) || std::isnan(b)) return kInf;
  if (std::isinf(a) || std::isinf(b)) return a == b ? 0.0 : kInf;
  return std::fabs(a - b);
}

// m = max(|a|, |b|, |scale|)
double magnitude(double a, double b, double scale) noexcept {
  return std::max({std::fabs(a), std::fabs(b), std::fabs(scale)});
}

}  // namespace

const char* to_string(Exactness cls) noexcept { return cls == Exactness::E0 ? "E0" : "E1"; }

double ulp(double x) noexcept {
  const double m = std::fabs(x);
  if (!std::isfinite(m)) return kInf;
  const double up = std::nextafter(m, kInf);
  if (std::isfinite(up)) return up - m;
  return m - std::nextafter(m, 0.0);  // m == DBL_MAX
}

double ulp_distance(double a, double b, double scale) noexcept {
  if (bits(a) == bits(b)) return 0.0;
  const double d = abs_diff(a, b);
  if (d == 0.0) return 0.0;  // +0 vs −0, or equal infinities
  if (std::isinf(scale)) return 0.0;  // exempt
  if (std::isinf(d)) return kInf;
  return d / ulp(magnitude(a, b, scale));
}

double relative_error(double a, double b, double scale) noexcept {
  if (bits(a) == bits(b)) return 0.0;
  const double d = abs_diff(a, b);
  if (d == 0.0) return 0.0;
  if (std::isinf(scale)) return 0.0;  // exempt
  if (std::isinf(d)) return kInf;
  const double m = magnitude(a, b, scale);
  return m == 0.0 ? 0.0 : d / m;
}

bool within_ulps(double a, double b, double ulps, double scale) noexcept {
  return ulp_distance(a, b, scale) <= ulps;
}

bool within(double a, double b, const Tolerance& tol, double scale) noexcept {
  if (bits(a) == bits(b)) return true;
  if (tol.cls == Exactness::E0) return false;
  if (ulp_distance(a, b, scale) <= tol.ulps) return true;
  return tol.rel > 0.0 && relative_error(a, b, scale) <= tol.rel;
}

BatchFn batch_of(ScalarFn f, int n_inputs, int n_outputs) {
  if (!f) throw std::invalid_argument("batch_of: empty function");
  if (n_inputs < 1 || n_outputs < 1) throw std::invalid_argument("batch_of: n_inputs and n_outputs must be >= 1");
  return [f, n_inputs, n_outputs](const double* state, int B, double* out) {
    std::vector<double> z(idx(n_inputs));
    std::vector<double> o(idx(n_outputs));
    for (int b = 0; b < B; ++b) {
      for (int k = 0; k < n_inputs; ++k) z[idx(k)] = state[idx(k) * idx(B) + idx(b)];
      f(z.data(), o.data());
      for (int i = 0; i < n_outputs; ++i) out[idx(i) * idx(B) + idx(b)] = o[idx(i)];
    }
  };
}

Report differential(const StateBall& ball, int n_outputs, const ScalarFn& reference, const BatchFn& compiled,
                    const DifferentialOptions& options) {
  if (ball.n_draws < 1 || ball.n_inputs < 1) throw std::invalid_argument("differential: empty ball");
  if (ball.states.size() != idx(ball.n_draws) * idx(ball.n_inputs)) {
    throw std::invalid_argument("differential: ball.states has the wrong size");
  }
  if (n_outputs < 1) throw std::invalid_argument("differential: n_outputs < 1");
  if (options.batch < 1) throw std::invalid_argument("differential: batch < 1");
  if (!reference) throw std::invalid_argument("differential: empty reference");
  if (!compiled) throw std::invalid_argument("differential: empty compiled");

  const int R = ball.n_draws;
  const int K = ball.n_inputs;
  const int B_max = std::min(options.batch, R);
  const bool e1 = options.tolerance.cls == Exactness::E1;
  const bool scaled = e1 && static_cast<bool>(options.scale);

  Report rep;
  rep.n_inputs = K;
  rep.n_outputs = n_outputs;
  rep.n_draws = R;
  rep.batch = options.batch;
  rep.scaled = scaled;
  rep.tolerance = options.tolerance;
  rep.outputs.assign(idx(n_outputs), OutputStats{});

  std::vector<double> state(idx(K) * idx(B_max));
  std::vector<double> out(idx(n_outputs) * idx(B_max));
  std::vector<double> ref(idx(n_outputs));
  std::vector<double> scale(scaled ? idx(n_outputs) : 0u, 0.0);

  for (int first = 0; first < R; first += B_max) {
    const int B = std::min(B_max, R - first);
    ball.soa(first, B, state.data());
    compiled(state.data(), B, out.data());
    for (int b = 0; b < B; ++b) {
      const int r = first + b;
      const double* z = ball.state(r);
      reference(z, ref.data());
      if (scaled) options.scale(z, scale.data());
      for (int o = 0; o < n_outputs; ++o) {
        const double a = ref[idx(o)];
        const double c = out[idx(o) * idx(B) + idx(b)];
        if (bits(a) == bits(c)) continue;
        OutputStats& s = rep.outputs[idx(o)];
        ++s.mismatches;
        ++rep.mismatches;
        const double sc = scaled ? scale[idx(o)] : 0.0;
        const double d = abs_diff(a, c);
        const double rel = relative_error(a, c, sc);
        const double u = ulp_distance(a, c, sc);
        if (!within(a, c, options.tolerance, sc)) {
          ++s.violations;
          ++rep.violations;
        }
        if (d > s.max_abs) s.max_abs = d;
        if (s.rel_draw < 0 || rel > s.max_rel) {
          s.max_rel = rel;
          s.rel_draw = r;
        }
        if (s.ulps_draw < 0 || u > s.max_ulps) {
          s.max_ulps = u;
          s.ulps_draw = r;
          s.ref_at_worst = a;
          s.cmp_at_worst = c;
        }
      }
    }
  }

  for (int o = 0; o < n_outputs; ++o) {
    const OutputStats& s = rep.outputs[idx(o)];
    if (s.bitwise()) continue;
    if (s.max_abs > rep.max_abs) rep.max_abs = s.max_abs;
    if (rep.max_rel_output < 0 || s.max_rel > rep.max_rel) {
      rep.max_rel = s.max_rel;
      rep.max_rel_output = o;
      rep.max_rel_draw = s.rel_draw;
    }
    if (rep.worst_output < 0 || s.max_ulps > rep.max_ulps) {
      rep.max_ulps = s.max_ulps;
      rep.worst_output = o;
      rep.worst_draw = s.ulps_draw;
    }
  }
  if (rep.worst_draw >= 0) {
    const double* z = ball.state(rep.worst_draw);
    rep.worst_state.assign(z, z + K);
  }
  rep.bitwise_equal = rep.mismatches == 0;
  rep.passed = e1 ? rep.violations == 0 : rep.bitwise_equal;
  return rep;
}

std::string Report::summary() const {
  std::ostringstream os;
  os << std::setprecision(4);
  os << "differential " << to_string(tolerance.cls);
  if (tolerance.cls == Exactness::E1) {
    os << " (" << tolerance.ulps << " ulps";
    if (tolerance.rel > 0.0) os << " or " << tolerance.rel << " relative";
    os << (scaled ? ", scaled" : "") << ")";
  }
  os << ": " << n_draws << " draws x " << n_outputs << " outputs, batch " << batch << ": ";
  const std::size_t pairs = idx(n_draws) * idx(n_outputs);
  if (bitwise_equal) {
    os << "bitwise equal";
  } else {
    os << mismatches << " of " << pairs << " values differ, " << violations << " violation(s)"
       << "; max rel " << max_rel << " (output " << max_rel_output << ", draw " << max_rel_draw << ")"
       << "; max " << max_ulps << " ulps (output " << worst_output << ", draw " << worst_draw << ": ref "
       << std::setprecision(17) << outputs[idx(worst_output)].ref_at_worst << " vs "
       << outputs[idx(worst_output)].cmp_at_worst << std::setprecision(4) << ")";
  }
  os << ": " << (passed ? "PASS" : "FAIL");
  return os.str();
}

}  // namespace epykos::verify
