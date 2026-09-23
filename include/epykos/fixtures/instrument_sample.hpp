// EpykosEngine — the instrument sample (M3/G2; a test-only fixture, D28): five seeded trades of
// every Stage A blueprint (SOFR OIS plain / shifted / shifted + locked out, SOFR averaging
// swaps, €STR OIS, EURIBOR 3M and 6M swaps, 3s6s basis swaps, the three deposits, SR3, SR1 and
// FEU3 futures), two of each seasoned with realised fixings, priced off four linear-zero curves
// (USD-SOFR, EUR-ESTR, EUR-EURIBOR-3M, EUR-EURIBOR-6M) on a common 11-knot grid. SYNTHETIC:
// the trades, quotes and fixings are generated from the seed (D16, D36); the conventions and
// blueprints are the repository's data.
//
// Per blueprint b (sub-stream sample_substream_base + b, draws in this order per trade i):
// tenor from {1Y, 2Y, 3Y, 5Y, 7Y} (deposits: the blueprint's; futures: contract i), notional
// log-uniform in [1e6, 1e8], side ±1, a start offset U{30..300} days back when seasoned (trades
// 0 and 1 of every blueprint), ε ~ U(−0.05, 0.05) on the par rate / spread, a futures traded
// price par ± U(−0.1, 0.1). Fixings: synthetic_fixings from 2025-01-01 to the day before the
// valuation date (levels 4% USD, 2% €STR, 2.2% / 2.5% EURIBOR).
//
// price_sample<Scalar> prices every trade (the engine's instrument maths off the curves) and the
// book total; instantiated on double it is the oracle, on Rec it records (record_sample), on
// Dual<n_inputs> it is the forward-mode oracle of the adjoint.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "epykos/conventions/fixings.hpp"
#include "epykos/conventions/registry.hpp"
#include "epykos/maths/curve/linear.hpp"
#include "epykos/maths/instrument/blueprint.hpp"
#include "epykos/maths/instrument/builder.hpp"
#include "epykos/maths/instrument/instrument.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::fixtures {

inline constexpr std::uint64_t sample_seed = 20260922;
inline constexpr std::uint64_t sample_substream_base = 600000;
inline constexpr int sample_per_blueprint = 5;
inline constexpr int sample_seasoned_per_blueprint = 2;
inline constexpr int sample_curves = 4;
inline constexpr int sample_knots = 11;

struct InstrumentSample {
  conventions::Date valuation = 0;
  conventions::Registry registry;
  instrument::Blueprints blueprints;
  conventions::FixingsHistory fixings;   // SYNTHETIC
  instrument::CurveSlots slots;          // 0 USD-SOFR, 1 EUR-ESTR, 2 EUR-EURIBOR-3M, 3 EUR-EURIBOR-6M
  std::vector<double> knot_t;            // sample_knots, shared by the four curves
  std::vector<double> z0;                // record point: sample_curves × sample_knots, slot-major
  std::vector<std::string> blueprint_names;
  std::vector<instrument::Trade> trades;
  std::vector<instrument::Instrument> instruments;   // one per trade, in trade order

  int n_trades() const noexcept { return static_cast<int>(instruments.size()); }
  int n_inputs() const noexcept { return sample_curves * sample_knots; }
  int n_outputs() const noexcept { return n_trades() + 1; }   // PV per trade, then the book
  instrument::BuildContext context() const {
    instrument::BuildContext c;
    c.registry = &registry;
    c.fixings = &fixings;
    c.curves = &slots;
    c.valuation = valuation;
    return c;
  }
};

// The sample for a seed and valuation date (default 2026-09-23).
InstrumentSample make_instrument_sample(std::uint64_t seed = sample_seed, conventions::Date valuation = 20719 /* 2026-09-23 */);

// DF of curve `slot` at t on the state z (slot-major), the linear-zero scheme.
template <class Scalar>
Scalar sample_df(const InstrumentSample& s, const Scalar* z, int slot, double t) {
  return curve::linear::df(s.knot_t.data(), z + static_cast<std::size_t>(slot) * sample_knots, sample_knots, t);
}

// out[0..n_trades) = pv of trade i; out[n_trades] = Σ_i pv_i (left fold).
template <class Scalar>
void price_sample(const InstrumentSample& s, const Scalar* z, Scalar* out) {
  auto df = [&](int slot, double t) { return sample_df<Scalar>(s, z, slot, t); };
  for (int i = 0; i < s.n_trades(); ++i) out[i] = instrument::pv<Scalar>(s.instruments[static_cast<std::size_t>(i)], df);
  Scalar acc = out[0];
  for (int i = 1; i < s.n_trades(); ++i) acc = acc + out[i];
  out[s.n_trades()] = acc;
}

// The leg scale of trade i (|leg 0| + |leg 1| on the swaps and deposits, |N| for a future; D26)
// and the book scale (Σ |pv_i|) on double.
std::vector<double> sample_scales(const InstrumentSample& s, const double* z);

// The recording: n_inputs Inputs at z0, price_sample<Rec>, n_outputs outputs; the standard
// passes unless run_passes is false.
inline Tape record_sample(const InstrumentSample& s, bool run_passes = true) {
  Tape tape;
  {
    Tape::Scope scope(tape);
    std::vector<Rec> z(static_cast<std::size_t>(s.n_inputs()));
    for (int k = 0; k < s.n_inputs(); ++k) z[static_cast<std::size_t>(k)] = make_input(tape, s.z0[static_cast<std::size_t>(k)]);
    std::vector<Rec> out(static_cast<std::size_t>(s.n_outputs()));
    price_sample<Rec>(s, z.data(), out.data());
    for (const Rec& o : out) register_output(tape, o);
  }
  tape.validate();
  if (run_passes) {
    standard_passes(tape);
    tape.validate();
  }
  return tape;
}

// R states in a ball of radius rho (absolute, on the zero rates) around z0, Philox sub-stream
// sample_substream_base + 1000 + r.
std::vector<std::vector<double>> sample_states(const InstrumentSample& s, int R, double rho = 0.005);

}  // namespace epykos::fixtures
