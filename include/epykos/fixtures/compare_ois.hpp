// EpykosEngine — the MX head-to-head fixture: a single USD SOFR OIS curve and an OIS book, on a
// curve family and an instrument set that a SECOND engine can be handed verbatim (D21, D71).
//
// Test-only fixture (D28): engine headers never include this. It exists so that the SwapEngine
// comparison of ROADMAP.md §MX can be run on inputs both engines compute the SAME quantities
// from, which `bench/compare/README.md` states clause by clause. Nothing here is derived from
// SwapEngine (D11): the curve, the instruments, the book and the quotes are built from this
// repository's own `blueprints/` and its own conventions layer, and the bundle emitter below
// writes them out in a neutral, purely time-based form.
//
// Why this fixture and not `fixtures/stage_a.hpp`:
//
//   1. ONE curve, ONE currency, ONE instrument type (fixed vs compounded-SOFR OIS, par-rate
//      quoted). Stage A's deposits, SR3 futures and EUR curves have no counterpart in the
//      neutral form, and a futures quote is a price, not a par rate.
//   2. The curve is `linear` in `logdf`, i.e. log DF piecewise-linear on {0, knot_t...}: the
//      instantaneous forward is piecewise constant and the DF at EVERY time — between knots,
//      before the first and beyond the last — is pinned by the knot values alone. That removes
//      the interpolation scheme from the comparison instead of assuming the two engines share
//      one. `docs/DECISIONS.md` D71 records the measurement that established the other engine's
//      bundle curve is this same family.
//   3. SQUARE: one knot per calibration instrument, knots at the instruments' maturities, so the
//      calibration Jacobian is square and non-singular and neither side needs a pseudo-inverse
//      or a regulariser, which the two engines would not share.
//   4. NO seasoned trades and NO realised fixings: every accrual starts at or after the
//      valuation date, so the fixings history is empty and the book is a function of the curve
//      alone.
//
// Outputs, all of `PROBLEM.md` §2's O1-O3 restricted to this problem: O1 the calibrated knots,
// O2 per-trade PV and the book total, O3 the ladder d(PV)/d(quote) through the IFT.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "epykos/conventions/fixings.hpp"
#include "epykos/conventions/registry.hpp"
#include "epykos/maths/instrument/blueprint.hpp"
#include "epykos/maths/instrument/builder.hpp"
#include "epykos/solver/curve_set.hpp"
#include "epykos/solver/implicit.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::fixtures {

struct CompareOisOptions {
  std::string valuation = "2026-09-23";   // the valuation date (ISO)
  // The calibration tenors. Empty: the default annual-to-10Y-then-standard set below. One
  // instrument and one knot per tenor, so the block is square.
  std::vector<std::string> tenors;
  int trades = 64;                        // book size
  double flat_start = 0.03;               // solver start, flat zero rate in the region's variable
  double solve_tol = 1e-13;
  std::uint64_t seed = 20260925;
  double quote_noise_bp = 1.0;            // 0: the generating curve's exact par quotes
  double generating_short = 0.0380;       // the generating zero curve (same shape as Stage A's)
  double generating_long = 0.0430;
  double generating_reversion_years = 3.0;
  double rate_moneyness = 0.15;           // book fixed rates are par * (1 + U(-m, m))
  int max_batch = 64;
};

// The default calibration tenor set: annual to 10Y then the standard long end. Stated in long
// form per D64 at every use site.
const std::vector<std::string>& compare_ois_default_tenors();

struct CompareOisTrade {
  std::string tenor;
  double notional = 0.0;
  int side = +1;              // +1 receives fixed
  double fixed_rate = 0.0;
  conventions::Date effective = 0;
  conventions::Date termination = 0;
};

struct CompareOis {
  CompareOisOptions options;
  conventions::Registry registry;
  instrument::Blueprints blueprints;
  conventions::FixingsHistory fixings;          // EMPTY by construction: no seasoned trades
  instrument::CurveSlots slots;                 // slot 0 = USD-SOFR
  conventions::Date valuation = 0;
  instrument::CurveDefinition definition;       // built in code, not read from blueprints/curves/
  instrument::CalibrationSet set;
  std::vector<double> quotes;                   // the par-rate quotes, one per instrument
  std::vector<double> generating_z;             // the generating curve's knot values (logdf)
  std::vector<instrument::Instrument> book;
  std::vector<CompareOisTrade> trade_spec;

  instrument::BuildContext context() const;
  int n_quotes() const noexcept { return static_cast<int>(quotes.size()); }
  int n_trades() const noexcept { return static_cast<int>(book.size()); }
  double generating_df(int slot, double t) const;
};

CompareOis make_compare_ois(const CompareOisOptions& options = {});

struct CompareOisTape {
  Tape tape;
  solver::ImplicitRegistry registry;
  std::unique_ptr<solver::CurveSet> set;
  std::vector<int> quote_inputs;                // tape input ordinals of the quotes
  std::vector<int> knot_outputs;                // O1
  std::vector<int> pv_outputs;                  // O2, one per trade
  int book_output = -1;                         // O2 total
  std::vector<double> record_knots;
  std::vector<double> record_pv;
  double record_book = 0.0;
  solver::SolveReport record_report;
  std::size_t nodes_raw = 0;
  std::size_t nodes_after_passes = 0;
  double seconds_record = 0.0;
};

CompareOisTape record_compare_ois(const CompareOis& s, bool passes = true);

solver::ProgramOptions compare_ois_program_options(const CompareOis& s);

// rows[r * n_quotes + k] = d out_{ordinals[r]} / d quote_k, through the IFT (O3).
std::vector<double> compare_ois_ladder(solver::ImplicitProgram& program, const std::vector<double>& quotes,
                                       const std::vector<int>& ordinals, std::vector<double>* out = nullptr);

// ---- the neutral exchange form -----------------------------------------------------------------
//
// A description of THIS fixture's curve, calibration instruments and book in year fractions from
// the valuation date and nothing else — no calendars, no day-count names, no index names, no
// scheme name. `bench/compare/README.md` defines every field. It is written by this repository
// FROM this repository's own tables, so that a second engine can be fed the identical problem
// without either engine reading the other (D11, D21).
// `with_obs_days`: also write, per compounded coupon, every projected observation day as
// [t_rate, t_next, tau_rate, weight]. That is the decomposition THIS engine actually
// evaluates; without it a reader sees only the collapsed one-sub-period form, which is the
// same number and a hundredth of the arithmetic (D71 measures both).
std::string compare_ois_exchange_json(const CompareOis& s, bool with_obs_days = false);

}  // namespace epykos::fixtures
