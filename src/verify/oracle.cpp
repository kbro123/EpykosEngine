// EpykosEngine — error against truth (PRINCIPLES.md §4; D72). See include/epykos/verify/oracle.hpp.
//
// Every error is formed IN THE ORACLE'S ARITHMETIC: the difference is Wide(approx) − truth, not
// approx − truth.hi. Taking it in double would discard truth's low word, which is the whole of the
// information the oracle exists to supply, and would report a rounded version of the error as the
// error. The quotient is taken in Wide too and only the final ratio is rounded to double, once,
// for reporting.
#include "epykos/verify/oracle.hpp"

#include "epykos/mutation/mutation.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace epykos::verify {
namespace {

// |a| for a Wide, as a Wide.
Wide wabs(const Wide& a) noexcept { return a.hi < 0.0 ? -a : a; }

// The error ratio |approx − truth| / m, formed at the oracle's precision and rounded once.
double ratio(const Wide& err_abs, double m) noexcept {
  if (m == 0.0) return err_abs.hi == 0.0 ? 0.0 : std::numeric_limits<double>::infinity();
  if (!std::isfinite(m)) return 0.0;  // an infinite scale exempts the value (differential.hpp's rule)
  return (err_abs / Wide(m)).hi;
}

}  // namespace

double oracle_jacobian_step(double z) noexcept {
  // cbrt(3u) with u = 2^-106, scaled by the input's own magnitude (floored at 1 so a zero knot
  // still gets a usable step).
  constexpr double u = 0x1p-106;
  const double base = std::cbrt(3.0 * u);
  const double m = std::fabs(z);
  return base * (m > 1.0 ? m : 1.0);
}

std::vector<Wide> oracle_jacobian(const OracleFn& truth, const double* z, int n_inputs, int n_outputs,
                                  const JacobianOptions& options) {
  if (!truth) throw std::invalid_argument("oracle_jacobian: null oracle");
  if (z == nullptr) throw std::invalid_argument("oracle_jacobian: null state");
  if (n_inputs < 1 || n_outputs < 1) throw std::invalid_argument("oracle_jacobian: n_inputs and n_outputs must be >= 1");

  std::vector<int> take = options.inputs;
  if (take.empty()) {
    take.resize(static_cast<std::size_t>(n_inputs));
    for (int k = 0; k < n_inputs; ++k) take[static_cast<std::size_t>(k)] = k;
  }
  for (int k : take) {
    if (k < 0 || k >= n_inputs) throw std::invalid_argument("oracle_jacobian: input ordinal out of range");
  }

  const std::size_t nt = take.size();
  const std::size_t no = static_cast<std::size_t>(n_outputs);
  std::vector<Wide> jac(no * nt);
  std::vector<Wide> zw(static_cast<std::size_t>(n_inputs));
  for (int k = 0; k < n_inputs; ++k) zw[static_cast<std::size_t>(k)] = Wide(z[k]);
  std::vector<Wide> plus(no), minus(no);

  std::vector<Wide> plus2(no), minus2(no);

  for (std::size_t j = 0; j < nt; ++j) {
    const std::size_t k = static_cast<std::size_t>(take[j]);
    const double h = options.h > 0.0 ? options.h : oracle_jacobian_step(z[k]);
    const Wide centre = zw[k];

    zw[k] = centre + h;
    truth(zw.data(), plus.data());
    zw[k] = centre - h;
    truth(zw.data(), minus.data());
    const Wide two_h = Wide(h) * 2.0;

    if (!options.richardson) {
      zw[k] = centre;
      for (std::size_t o = 0; o < no; ++o) jac[o * nt + j] = (plus[o] - minus[o]) / two_h;
      continue;
    }

    // D(h) and D(2h); (4 D(h) - D(2h)) / 3 cancels the h^2 truncation term, leaving O(h^4).
    zw[k] = centre + 2.0 * h;
    truth(zw.data(), plus2.data());
    zw[k] = centre - 2.0 * h;
    truth(zw.data(), minus2.data());
    zw[k] = centre;
    const Wide four_h = Wide(h) * 4.0;
    for (std::size_t o = 0; o < no; ++o) {
      const Wide d_h = (plus[o] - minus[o]) / two_h;
      const Wide d_2h = (plus2[o] - minus2[o]) / four_h;
      jac[o * nt + j] = (d_h * 4.0 - d_2h) / 3.0;
    }
  }
  return jac;
}

TruthReport error_against_truth(const StateBall& ball, int n_outputs, const BatchFn& approx, const OracleFn& truth,
                                const TruthOptions& options) {
  if (ball.n_draws < 1 || ball.n_inputs < 1) throw std::invalid_argument("error_against_truth: empty ball");
  if (n_outputs < 1) throw std::invalid_argument("error_against_truth: n_outputs must be >= 1");
  if (options.batch < 1) throw std::invalid_argument("error_against_truth: batch must be >= 1");
  if (!approx) throw std::invalid_argument("error_against_truth: null approx callable");
  if (!truth) throw std::invalid_argument("error_against_truth: null oracle callable");

  std::vector<OutputClass> classes = options.classes;
  if (classes.empty()) classes.push_back(OutputClass{"", 0, n_outputs});
  {
    std::vector<char> covered(static_cast<std::size_t>(n_outputs), 0);
    for (const OutputClass& c : classes) {
      if (c.count < 1 || c.first < 0 || c.first + c.count > n_outputs) {
        throw std::invalid_argument("error_against_truth: output class '" + c.name + "' runs past n_outputs");
      }
      for (int o = c.first; o < c.first + c.count; ++o) {
        if (covered[static_cast<std::size_t>(o)] != 0) {
          throw std::invalid_argument("error_against_truth: output classes overlap at ordinal " + std::to_string(o));
        }
        covered[static_cast<std::size_t>(o)] = 1;
      }
    }
  }

  const std::size_t no = static_cast<std::size_t>(n_outputs);
  const std::size_t ni = static_cast<std::size_t>(ball.n_inputs);

  // Mutant oracle.error_in_double: form the error in double (`approx - truth.hi`) instead of at the
  // oracle's precision, throwing away truth's low word. Queried once per call, never per element.
  const bool error_in_double = mutant("oracle.error_in_double");

  TruthReport rep;
  rep.n_inputs = ball.n_inputs;
  rep.n_outputs = n_outputs;
  rep.n_states = ball.n_draws;
  rep.batch = options.batch;
  rep.scaled = static_cast<bool>(options.scale);
  rep.oracle_mantissa_bits = oracle_mantissa_bits_v<Oracle>;
  rep.headroom_bits = oracle_headroom_bits;
  rep.outputs.assign(no, OutputTruth{});

  std::vector<double> sum_sq(no, 0.0);
  std::vector<double> batch_state(ni * static_cast<std::size_t>(options.batch));
  std::vector<double> batch_out(no * static_cast<std::size_t>(options.batch));
  std::vector<Wide> zw(ni);
  std::vector<Wide> tw(no);
  std::vector<double> scale(no, 0.0);

  for (int first = 0; first < ball.n_draws; first += options.batch) {
    const int B = std::min(options.batch, ball.n_draws - first);
    ball.soa(first, B, batch_state.data());
    approx(batch_state.data(), B, batch_out.data());

    for (int b = 0; b < B; ++b) {
      const int r = first + b;
      const double* z = ball.state(r);
      for (std::size_t k = 0; k < ni; ++k) zw[k] = Wide(z[k]);  // exact promotion
      truth(zw.data(), tw.data());
      if (options.scale) {
        options.scale(z, scale.data());
      } else {
        std::fill(scale.begin(), scale.end(), 0.0);
      }

      for (std::size_t o = 0; o < no; ++o) {
        OutputTruth& st = rep.outputs[o];
        ++st.n_states;
        const Wide& t = tw[o];
        if (!t.has_full_precision()) {
          ++st.degraded;
          ++rep.degraded;
        }
        const double a = batch_out[o * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)];
        const Wide err = error_in_double ? Wide(std::fabs(a - t.hi)) : wabs(Wide(a) - t);
        const double abs_err = err.hi;
        if (abs_err > st.max_abs) st.max_abs = abs_err;

        const double t_mag = std::fabs(t.hi);
        const double s = scale[o];
        const double m = std::max(t_mag, std::fabs(s));
        const double rel = ratio(err, m);
        const double rel_self = ratio(err, t_mag);

        sum_sq[o] += rel * rel;
        if (rel > st.max_rel) {
          st.max_rel = rel;
          st.max_rel_state = r;
          st.truth_at_worst = t.hi;
          st.approx_at_worst = a;
          st.scale_at_worst = s;
        }
        if (rel_self > st.max_rel_self) {
          st.max_rel_self = rel_self;
          st.max_rel_self_state = r;
        }
        const double u = std::isfinite(m) && m > 0.0 ? abs_err / ulp(m) : 0.0;
        if (u > st.max_ulps) st.max_ulps = u;

        if (rel > rep.max_rel) {
          rep.max_rel = rel;
          rep.max_rel_output = static_cast<int>(o);
          rep.max_rel_state = r;
        }
      }
    }
  }

  for (std::size_t o = 0; o < no; ++o) {
    OutputTruth& st = rep.outputs[o];
    st.rms_rel = st.n_states > 0 ? std::sqrt(sum_sq[o] / static_cast<double>(st.n_states)) : 0.0;
  }
  if (rep.max_rel_state >= 0) {
    const double* z = ball.state(rep.max_rel_state);
    rep.worst_state.assign(z, z + ni);
  }

  for (const OutputClass& c : classes) {
    ClassTruth ct;
    ct.name = c.name;
    ct.first = c.first;
    ct.count = c.count;
    double sum_sq_class = 0.0;
    std::size_t n_class = 0;
    std::vector<double> per_output;
    per_output.reserve(static_cast<std::size_t>(c.count));
    for (int o = c.first; o < c.first + c.count; ++o) {
      const OutputTruth& st = rep.outputs[static_cast<std::size_t>(o)];
      per_output.push_back(st.max_rel);
      sum_sq_class += st.rms_rel * st.rms_rel * static_cast<double>(st.n_states);
      n_class += static_cast<std::size_t>(st.n_states);
      ct.degraded += st.degraded;
      if (st.exact()) ++ct.exact_outputs;
      if (st.max_rel > ct.max_rel) {
        ct.max_rel = st.max_rel;
        ct.max_rel_output = o;
        ct.max_rel_state = st.max_rel_state;
        ct.truth_at_worst = st.truth_at_worst;
        ct.approx_at_worst = st.approx_at_worst;
        ct.scale_at_worst = st.scale_at_worst;
      }
      if (st.max_rel_self > ct.max_rel_self) {
        ct.max_rel_self = st.max_rel_self;
        ct.max_rel_self_output = o;
      }
      if (st.max_ulps > ct.max_ulps) ct.max_ulps = st.max_ulps;
    }
    ct.rms_rel = n_class > 0 ? std::sqrt(sum_sq_class / static_cast<double>(n_class)) : 0.0;
    if (!per_output.empty()) {
      std::sort(per_output.begin(), per_output.end());
      ct.median_rel = per_output[per_output.size() / 2];
    }
    rep.classes.push_back(std::move(ct));
  }

  return rep;
}

std::string TruthReport::summary() const {
  std::ostringstream os;
  os << std::scientific << std::setprecision(3);
  os << n_outputs << " outputs x " << n_states << " states, " << n_inputs << " inputs, batch " << batch
     << "; oracle " << oracle_mantissa_bits << " significand bits (" << headroom_bits
     << " more than double, " << std::defaultfloat << std::setprecision(6);
  // 2^headroom as a plain multiplier, which is the number a reader wants.
  os << std::scientific << std::setprecision(2) << std::ldexp(1.0, headroom_bits) << "x finer than a double ulp)"
     << std::scientific << std::setprecision(3);
  os << (scaled ? "; D26-scaled" : "; unscaled");
  if (max_rel_output >= 0) {
    os << ". Worst: output " << max_rel_output << " at state " << max_rel_state << ", relative error " << max_rel;
  } else {
    os << ". No error at any output";
  }
  if (degraded > 0) {
    os << ". WARNING: " << degraded << " (output, state) pairs had an oracle value below full precision";
  }
  os << '.';
  return os.str();
}

std::string TruthReport::per_class_table() const {
  std::ostringstream os;
  os << std::left << std::setw(26) << "class" << std::right << std::setw(8) << "outputs" << std::setw(12) << "max rel"
     << std::setw(12) << "median" << std::setw(12) << "rms" << std::setw(12) << "max ulps" << std::setw(12)
     << "max self" << std::setw(9) << "exact" << '\n';
  for (const ClassTruth& c : classes) {
    os << std::left << std::setw(26) << (c.name.empty() ? std::string("(all)") : c.name) << std::right << std::setw(8)
       << c.count << std::scientific << std::setprecision(3) << std::setw(12) << c.max_rel << std::setw(12)
       << c.median_rel << std::setw(12) << c.rms_rel << std::defaultfloat << std::setprecision(4) << std::setw(12)
       << c.max_ulps << std::scientific << std::setprecision(3) << std::setw(12) << c.max_rel_self << std::defaultfloat
       << std::setw(9) << c.exact_outputs << '\n';
  }
  return os.str();
}

}  // namespace epykos::verify
