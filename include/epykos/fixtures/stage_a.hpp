// EpykosEngine — the Stage A desk problem as ONE tape (M3/G5; docs/PROBLEM.md sections 2, 4 Stage A,
// 5; D35, D36). A test-only fixture (D28), not engine API.
//
// What is data and what is seeded. blueprints/problems/stage_a.json (StageADefinition) holds the
// DEFINITIONS: the curves (their definitions in blueprints/curves/*.json, in slot order) and the
// SOFR scheme variants, the plausible levels the synthetic quotes and fixings are generated
// around, the book's trade-type mix, tenor and notional distributions, seasoned fraction and
// netting-set count, and the scenario families. Everything numeric of the fixture is generated
// here from the seed and is SYNTHETIC (D16, D36): quotes (the generating curves' par quotes plus
// noise, so O1 is a fit and not an identity), fixings histories, the ~2,000 trades, the 1,000
// scenario lanes. Conventions, instrument blueprints and curve definitions are the repository's
// data (G0, G2).
//
// The one recording (record_stage_a): the quotes are the tape's free inputs, in the order of
// StageA::quotes; solver::CurveSet::calibrate records one implicit block per curve in the
// dependency order it discovers from the maths (€STR → 6M → 3M; SOFR on its own) — its residual
// outputs and O1 diagnostics (‖Jᵀr‖∞, iterations) are registered by solver/implicit.hpp; the
// knot values of every curve (the unknowns, in each region's variable) are registered as outputs
// (O1); the book is priced off the calibrated Recs on the same tape (O2: per-trade PV in the
// trade currency and in the reporting currency, the two leg PVs, aggregates per currency, per
// netting set and the book) — the calibration instruments' discount factors and the book's are
// the same nodes after cse (PROBLEM.md §5, ir/sharing.hpp); then the E0 passes and, when a
// Select exists (the monotone cubic / composite variants), the select exports (O6). Output
// ordinals are in StageALayout. O3 (the risk ladder) and O4 (the scenario grid) are runs of
// solver::ImplicitProgram over that tape: run_lanes / ladder below.
//
// Record-time discount-factor memo (stated): the book's df(slot, t) callable memoises the Rec of
// each (slot, t) it has already recorded, so a discount factor is recorded once per distinct
// time rather than once per coupon that reads it. The tape after cse is the same either way (the
// merged nodes are exactly the ones the memo returns); the raw recording is smaller by the
// duplicates. Nothing of the maths is folded: the compounding loops, the averages, the forwards
// are recorded per coupon as coupon.hpp writes them.
//
// Reporting currency (stated): Stage A has no FX. pv_usd of a USD trade is the trade's pv node;
// of a EUR trade it is pv × the placeholder 1.0 of the blueprint's fx table, a recorded product
// that Stage B replaces with an FX spot input.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "epykos/conventions/date.hpp"
#include "epykos/conventions/fixings.hpp"
#include "epykos/conventions/registry.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/maths/instrument/blueprint.hpp"
#include "epykos/maths/instrument/builder.hpp"
#include "epykos/maths/instrument/calibrate.hpp"
#include "epykos/maths/instrument/instrument.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/solver/curve_set.hpp"
#include "epykos/solver/implicit.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "epykos/solver/tangent.hpp"
#include "epykos/tape/select_export.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::fixtures {

// Philox sub-streams (the seed is the blueprint's): trade i draws from book + i, quote k's noise
// from quotes + k, scenario lane l's size from scenarios + l.
inline constexpr std::uint64_t stage_a_book_substream = 800000;
inline constexpr std::uint64_t stage_a_quote_substream = 810000;
inline constexpr std::uint64_t stage_a_scenario_substream = 820000;

// ---- the definition (blueprints/problems/stage_a.json) -----------------------------------------

struct StageADefinition {
  std::string path;
  std::string name;
  conventions::Date valuation = 0;
  std::uint64_t seed = 0;
  std::vector<std::string> currencies;
  std::string reporting_currency;
  struct Fx {
    std::string currency;
    double placeholder = 1.0;
  };
  std::vector<Fx> fx;
  struct Generating {
    double short_rate = 0.0;
    double long_rate = 0.0;
    double reversion_years = 1.0;
  };
  struct CurveEntry {
    std::string definition;               // the curve definition (blueprints/curves)
    std::vector<std::string> variants;    // other definitions of the same index (the scheme sweep)
    Generating generating;
  };
  std::vector<CurveEntry> curves;         // slot order
  struct Quotes {
    double noise_bp = 0.0;
    double futures_noise_price = 0.0;
  } quotes;
  struct Fixings {
    conventions::Date from = 0;
    double daily_vol = 0.0;
    double reversion = 0.0;
    std::vector<std::pair<std::string, double>> levels;   // index -> level
  } fixings;
  struct Weighted {
    std::string name;
    double weight = 0.0;
  };
  struct Book {
    int trades = 0;
    std::vector<Weighted> mix;            // blueprint -> weight
    std::vector<Weighted> tenors;         // tenor -> weight
    double notional_min = 0.0, notional_max = 0.0;
    std::string notional_distribution;    // "log_uniform"
    double seasoned_fraction = 0.0;
    int seasoned_age_min = 0, seasoned_age_max = 0;
    int netting_sets = 1;
    double rate_moneyness = 0.0;
  } book;
  struct ScenarioFamily {
    std::string name;                     // parallel | twist | butterfly | per_curve
    int count = 0;
    double size_bp = 0.0;
    double pivot_years = 0.0;
  };
  struct Scenarios {
    int count = 0;
    std::vector<ScenarioFamily> families;
    double t_max_years = 30.0;
    std::string jacobian;                 // "chord" | "per_iteration"
    bool warm_start = false;
  } scenarios;

  double fx_placeholder(const std::string& currency) const;   // throws for an unknown currency
  int currency_index(const std::string& currency) const;      // -1 when unknown
};

// <blueprints root>/problems/stage_a.json (instrument::Blueprints::default_root()).
std::string default_stage_a_path();
// Loads and validates the definition (strict schema: an unknown key or a missing field throws
// json::JsonError / instrument::BlueprintError naming file:line:column).
StageADefinition load_stage_a_definition(const std::string& path = default_stage_a_path());

// ---- the filled problem ------------------------------------------------------------------------

struct StageAOptions {
  std::string usd_curve;            // "" = the blueprint's base definition of slot 0; else one of its variants (the scheme sweep)
  int trades = -1;                  // -1 = the blueprint's count; smaller books for quick checks (the first n of the same draws)
  int scenarios = -1;               // -1 = the blueprint's count; else the families scaled to n lanes
  double quote_noise_bp = -1.0;     // -1 = the blueprint's; 0 = the generating curves' par quotes exactly (the O1 recovery gate)
  double flat_start = 0.03;         // the solver's start: this flat zero rate in every knot's variable
  double solve_tol = 1e-13;         // ‖F‖∞ tolerance of every block (rate units); the compounded residuals' rounding floor is ~3e-14 over 30 years of daily steps, above G4's 1e-14 default
  bool export_selects = true;       // O6 exports after the passes (a tape with no Select gets none)
  solver::CurveSet::Mode mode = solver::CurveSet::Mode::sequential;
  std::string path;                 // the definition file ("" = default_stage_a_path())
};

struct StageATrade {
  int index = -1;
  std::string blueprint;
  std::string currency;
  int netting_set = 0;
  bool seasoned = false;
  int age_days = 0;                 // seasoned: days between the effective and the valuation date
  double moneyness = 0.0;           // ε: the rate / spread is par × (1 + ε) at the record point
  instrument::Trade trade;
};

struct StageAScenario {
  int lane = 0;
  std::string family;
  int curve = -1;                   // per_curve: the shocked slot; else −1 (every curve)
  double size = 0.0;                // the shift in rate units (bp / 1e4)
};

struct StageA {
  StageADefinition def;
  StageAOptions options;
  conventions::Date valuation = 0;
  conventions::Registry registry;
  instrument::Blueprints blueprints;
  conventions::FixingsHistory fixings;                 // SYNTHETIC
  instrument::CurveSlots slots;                        // slot s = def.curves[s]'s index
  std::vector<std::string> curve_definitions;          // per slot: the definition in use (the variant for slot 0 when chosen)
  std::vector<instrument::CalibrationSet> sets;        // per slot
  std::vector<std::vector<double>> generating;         // per slot: the generating zero rates on the set's knots (linear zero)
  std::vector<double> quotes;                          // CurveSet instrument order: slot after slot, each in maturity order
  std::vector<std::string> quote_keys;
  std::vector<int> quote_curve;                        // slot of quote k
  std::vector<double> quote_t;                         // maturity time of quote k's instrument
  std::vector<std::uint8_t> quote_is_price;            // a futures price (shocked by −100·s)
  std::vector<StageATrade> trades;
  std::vector<instrument::Instrument> instruments;     // one per trade
  std::vector<StageAScenario> scenarios;
  std::vector<std::vector<double>> scenario_quotes;    // per lane: n_quotes shocked quotes

  int n_curves() const noexcept { return static_cast<int>(sets.size()); }
  int n_quotes() const noexcept { return static_cast<int>(quotes.size()); }
  int n_trades() const noexcept { return static_cast<int>(instruments.size()); }
  int n_scenarios() const noexcept { return static_cast<int>(scenarios.size()); }
  int n_netting_sets() const noexcept { return def.book.netting_sets; }
  int n_currencies() const noexcept { return static_cast<int>(def.currencies.size()); }
  instrument::BuildContext context() const {
    instrument::BuildContext c;
    c.registry = &registry;
    c.fixings = &fixings;
    c.curves = &slots;
    c.valuation = valuation;
    return c;
  }
  // The generating zero curve of a slot at t (linear in the zero rate on the set's knots).
  double generating_zero(int slot, double t) const;
  double generating_df(int slot, double t) const;
};

// Builds the problem: registry, blueprints, fixings, calibration sets, generating curves and
// quotes, the trade population, the scenario lanes. Throws on a definition the data cannot
// resolve (a missing fixing, an unknown blueprint).
StageA make_stage_a(const StageAOptions& options = {});

// The generating curves' par quotes, E0-pinned (D25): the identical `instrument::par_quotes`
// instantiation make_stage_a uses to build StageA::quotes when quote_noise_bp == 0, compiled in
// this TU's -ffp-contract=off object code. A test comparing StageA::quotes against a freshly
// computed par_quotes(cs, df) must call this rather than epykos::instrument::par_quotes directly:
// that template is header-only, so calling it from a non-_e0 test TU compiles a second
// instantiation that GCC's cross-statement FMA contraction (D25) is free to round differently
// from the one make_stage_a used, even though both compute the same maths.
std::vector<double> stage_a_generating_par_quotes(const instrument::CalibrationSet& cs, const std::function<double(int, double)>& df);

// ---- the book on any Scalar --------------------------------------------------------------------

// A memoising df(slot, t): the value of each (slot, t) is computed once through `inner` (one
// table per slot, keyed by the bit pattern of t).
template <class Scalar, class Inner>
class DfMemo {
 public:
  explicit DfMemo(Inner inner) : inner_(std::move(inner)) {}
  Scalar operator()(int slot, double t) {
    std::uint64_t key;
    std::memcpy(&key, &t, sizeof key);
    if (static_cast<std::size_t>(slot) >= memo_.size()) memo_.resize(static_cast<std::size_t>(slot) + 1);
    std::unordered_map<std::uint64_t, Scalar>& table = memo_[static_cast<std::size_t>(slot)];
    auto it = table.find(key);
    if (it != table.end()) return it->second;
    const Scalar v = inner_(slot, t);
    table.emplace(key, v);
    return v;
  }
  std::size_t size() const noexcept {
    std::size_t n = 0;
    for (const auto& table : memo_) n += table.size();
    return n;
  }

 private:
  Inner inner_;
  std::vector<std::unordered_map<std::uint64_t, Scalar>> memo_;
};

template <class Scalar>
struct StageABook {
  std::vector<Scalar> pv;               // per trade, trade currency
  std::vector<Scalar> pv_usd;           // per trade, reporting currency (the placeholder conversion)
  std::vector<Scalar> leg0, leg1;       // per trade: legs[0], legs[1] (a future: its PV and 0)
  std::vector<Scalar> currency_total;   // per currency of the definition (trade currency)
  std::vector<Scalar> netting_total;    // per netting set (reporting currency)
  Scalar book_total = Scalar(0.0);      // reporting currency
  std::vector<double> accrued0, accrued1;  // structure: accrued interest of legs[0] / legs[1] (doubles)
};

// Prices the book off df(slot, t): the instrument maths of maths/instrument, the aggregates as
// left folds in trade order. Instantiated on double it is the oracle, on Rec it records, on
// Dual<N> it is forward mode.
template <class Scalar, class Curves>
StageABook<Scalar> price_stage_a(const StageA& s, Curves&& df) {
  StageABook<Scalar> b;
  const std::size_t n = static_cast<std::size_t>(s.n_trades());
  b.pv.resize(n);
  b.pv_usd.resize(n);
  b.leg0.resize(n);
  b.leg1.resize(n);
  b.accrued0.assign(n, 0.0);
  b.accrued1.assign(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    const instrument::Instrument& in = s.instruments[i];
    b.pv[i] = instrument::pv<Scalar>(in, df);
    if (in.kind == instrument::Kind::Future) {
      b.leg0[i] = b.pv[i];
      b.leg1[i] = Scalar(0.0);
    } else {
      b.leg0[i] = instrument::leg_pv<Scalar>(in.legs[0], df);
      b.leg1[i] = instrument::leg_pv<Scalar>(in.legs[1], df);
    }
    for (std::size_t l = 0; l < in.legs.size() && l < 2; ++l) (l == 0 ? b.accrued0 : b.accrued1)[i] = instrument::leg_accrued(in.legs[l]);
    if (in.currency == s.def.reporting_currency) {
      b.pv_usd[i] = b.pv[i];
    } else {
      b.pv_usd[i] = b.pv[i] * s.def.fx_placeholder(in.currency);
    }
  }
  // Aggregates: left folds in trade order.
  b.currency_total.assign(static_cast<std::size_t>(s.n_currencies()), Scalar(0.0));
  std::vector<bool> ccy_started(static_cast<std::size_t>(s.n_currencies()), false);
  b.netting_total.assign(static_cast<std::size_t>(s.n_netting_sets()), Scalar(0.0));
  std::vector<bool> net_started(static_cast<std::size_t>(s.n_netting_sets()), false);
  bool book_started = false;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t c = static_cast<std::size_t>(s.def.currency_index(s.instruments[i].currency));
    const std::size_t ns = static_cast<std::size_t>(s.trades[i].netting_set);
    if (!ccy_started[c]) {
      b.currency_total[c] = b.pv[i];
      ccy_started[c] = true;
    } else {
      b.currency_total[c] = b.currency_total[c] + b.pv[i];
    }
    if (!net_started[ns]) {
      b.netting_total[ns] = b.pv_usd[i];
      net_started[ns] = true;
    } else {
      b.netting_total[ns] = b.netting_total[ns] + b.pv_usd[i];
    }
    if (!book_started) {
      b.book_total = b.pv_usd[i];
      book_started = true;
    } else {
      b.book_total = b.book_total + b.pv_usd[i];
    }
  }
  return b;
}

// The leg scale of every trade (|leg0| + |leg1| for swaps and deposits, |N| for a future; D26)
// and of the aggregates (Σ |pv| over their trades), on double, at the solved state `states`.
struct StageAScales {
  std::vector<double> trade;      // per trade
  std::vector<double> currency;   // per currency
  std::vector<double> netting;    // per netting set
  double book = 0.0;
};
StageAScales stage_a_scales(const StageA& s, const StageABook<double>& book);

// ---- the recording -----------------------------------------------------------------------------

// Where every output of the tape is (ordinals; out[o·B + b] in the batched layout).
struct StageALayout {
  int n_outputs = 0;
  int n_trades = 0, n_currencies = 0, n_netting_sets = 0;
  std::vector<std::vector<int>> knots;    // O1: per slot, the output ordinal of every knot value
  int pv0 = -1, pv_usd0 = -1, leg0_0 = -1, leg1_0 = -1;   // O2: first ordinal of each per-trade vector (contiguous)
  int currency0 = -1, netting0 = -1, book = -1;             // O2 aggregates
  SelectExports selects;                                    // O6 (n() == 0 when the tape has no Select)

  int pv(int i) const noexcept { return pv0 + i; }
  int pv_usd(int i) const noexcept { return pv_usd0 + i; }
  int leg0(int i) const noexcept { return leg0_0 + i; }
  int leg1(int i) const noexcept { return leg1_0 + i; }
  int currency(int c) const noexcept { return currency0 + c; }
  int netting(int n) const noexcept { return netting0 + n; }
  std::vector<int> pv_ordinals() const;         // all n_trades pv ordinals
  std::vector<int> aggregate_ordinals() const;  // currencies, netting sets, the book
  // The output groups of the sharing gate: {block outputs (residuals + diagnostics)}, {O1 knots},
  // {O2 book outputs}, {O6 exports} (the last two only when non-empty).
};

struct StageARecordStats {
  double seconds_build = 0.0;        // registry, blueprints, tables, quotes, book, scenarios (make_stage_a)
  double seconds_dependencies = 0.0; // CurveSet dependency discovery (the scratch recordings)
  double seconds_calibrate = 0.0;    // recording the residuals + the record-time solves
  double seconds_book = 0.0;         // recording the book
  double seconds_passes = 0.0;       // the E0 passes
  double seconds_export = 0.0;       // select exports
  double seconds_total = 0.0;        // dependencies + calibrate + book + passes + export
  std::size_t nodes_raw = 0;
  std::size_t nodes_after_passes = 0;
  std::size_t df_memo_hits = 0;      // (slot, t) pairs the book's memo served (distinct times recorded)
  std::size_t n_inputs = 0, n_outputs = 0;
  std::string to_string() const;
};

struct StageATape {
  Tape tape;
  solver::ImplicitRegistry registry;
  std::unique_ptr<solver::CurveSet> set;               // owns the CurveSpecs the Calibration's states point at
  std::vector<int> quote_inputs;                       // tape input ordinals of the quotes (== the free inputs), quote order
  std::vector<std::vector<int>> block_curves;          // per block: its slots (calibration order)
  std::vector<solver::SolveReport> record_reports;     // per block: the record-time solve
  std::vector<std::vector<double>> record_knots;       // per slot: the record-point solution (each region's variable)
  StageABook<double> record_book;                      // the book at the record point on double (the same maths, memoised the same way)
  StageALayout layout;
  StageARecordStats stats;

  int n_blocks() const noexcept { return static_cast<int>(registry.blocks.size()); }
  // Output groups: 0 = block outputs (residuals and diagnostics), 1 = the O1 knot outputs, 2 = the
  // O2 book outputs, 3 = the O6 exports (present only when the tape has any).
  std::vector<std::vector<int>> output_groups() const;
  // The two groups of the cross-stage sharing gate (PROBLEM.md §5, ir/sharing.hpp): the residual
  // outputs and the book outputs. The knot outputs are Inputs and the select exports sit
  // upstream of every discount factor, so no DF domain can feed them.
  std::vector<std::vector<int>> sharing_groups() const;
  // The record-point value of every free input (the quotes), ordinal order.
  std::vector<double> record_quotes() const;
};

// Records the whole problem on one tape: quotes as Inputs, CurveSet::calibrate (implicit blocks
// in discovered dependency order), the knot values as outputs, the book, then the E0 passes
// (`passes`) and the select exports (options.export_selects). The record-point book on double
// (record_book) is priced through the same memoised df on the solved knots.
StageATape record_stage_a(const StageA& s, bool passes = true);

// The book on double at knot values z (per slot, in each region's variable) through the same
// CurveSet curves and the same memoised df as the recording: the oracle for a state.
StageABook<double> price_stage_a_at(const StageA& s, const solver::CurveSet& set, const std::vector<std::vector<double>>& z);

// ---- running the program: O2 / O4 lanes and the O3 ladder ------------------------------------

// solver::ProgramOptions for the problem: the blueprint's Jacobian policy and warm start, a batch
// of `max_batch` lanes.
solver::ProgramOptions stage_a_program_options(const StageA& s, int max_batch = 64);

// Runs `lanes` (each n_quotes quotes) through the program in chunks of its max_batch; returns
// out[o·B + b] over every tape output (B = lanes.size()).
std::vector<double> run_lanes(solver::ImplicitProgram& program, const std::vector<std::vector<double>>& lanes);

// The par-delta ladder of the outputs `ordinals` to every quote at `quotes`: the IFT adjoint,
// one lane per output (chunks of max_batch). Returns rows[r·n_quotes + k] = ∂out_{ordinals[r]}/∂q_k,
// PV per unit of the quote (per 1.0 of rate; a futures quote per 1.0 of price). `out` receives
// the forward outputs of the (identical) lanes when given.
std::vector<double> ladder(solver::ImplicitProgram& program, const std::vector<double>& quotes, const std::vector<int>& ordinals,
                           std::vector<double>* out = nullptr);

// The ladder in the units of PROBLEM.md O3, PV per 1 bp of the quote's RATE: a rate quote's
// entry × 1e-4, a futures price's entry × (−0.01) (a 1 bp rate rise lowers the price by 0.01).
std::vector<double> ladder_per_bp(const StageA& s, const std::vector<double>& rows);

// ---- forward mode: the same problem on Dual<N> (N = n_quotes) -------------------------------
//
// The AD-mode comparison of O3 (PROBLEM.md §5, §7: "the forward-versus-reverse choice per
// Jacobian block is the optimiser's decision"): the whole problem instantiated once on
// Dual<N> with quote k seeded as direction k — every block calibrated by solver::implicit_dual
// (the IFT tangent rule on the templated residuals, earlier curves' tangents flowing through
// the block's parameters) and the book priced on Dual — gives every trade's par delta to every
// quote in ONE pass of the maths, against one adjoint lane per trade for the reverse ladder.
// This is the M2 forward mode (Dual on the templated maths), not a tangent interpreter over the
// IR; it is also the oracle of the reverse ladder (adjoint vs forward at 1e-12, WORKLOADS §M2).
template <int N>
struct StageAForward {
  std::vector<std::vector<Dual<N>>> knots;    // per slot: the solved knots with their tangents dz/dq
  std::vector<solver::SolveReport> reports;   // per block
  StageABook<Dual<N>> book;
  // rows[i·N + k] = ∂pv_i/∂q_k (per unit of the quote), the same layout as ladder().
  std::vector<double> pv_rows() const {
    std::vector<double> rows(book.pv.size() * static_cast<std::size_t>(N));
    for (std::size_t i = 0; i < book.pv.size(); ++i) {
      for (std::size_t k = 0; k < static_cast<std::size_t>(N); ++k) rows[i * static_cast<std::size_t>(N) + k] = book.pv[i].d[k];
    }
    return rows;
  }
};

// `start`: per slot, the start of every block's solve (nullptr: the set's starts). Passing a
// solution already at ‖F‖∞ < options.tol evaluates the tangents AT that point (no step is taken).
template <int N>
StageAForward<N> forward_stage_a(const StageA& s, const solver::CurveSet& set, const std::vector<double>& quotes,
                                 solver::CurveSet::Mode mode = solver::CurveSet::Mode::sequential, const solver::SolveOptions& options = {},
                                 const std::vector<std::vector<double>>* start = nullptr) {
  using D = Dual<N>;
  if (N != s.n_quotes() || static_cast<int>(quotes.size()) != N) {
    throw std::invalid_argument("forward_stage_a: N = " + std::to_string(N) + " but the problem has " + std::to_string(s.n_quotes()) + " quotes");
  }
  const int n_curves = s.n_curves();
  std::vector<D> q(static_cast<std::size_t>(N));
  for (int k = 0; k < N; ++k) q[static_cast<std::size_t>(k)] = D::variable(quotes[static_cast<std::size_t>(k)], k);
  // CurveSet instrument i = (slot c, instrument j of the set) with i = first[c] + j.
  std::vector<int> first(static_cast<std::size_t>(n_curves) + 1, 0);
  for (int c = 0; c < n_curves; ++c) first[static_cast<std::size_t>(c) + 1] = first[static_cast<std::size_t>(c)] + s.sets[static_cast<std::size_t>(c)].n_instruments();
  solver::CurveStates<D> states;
  states.specs = &set.curves();
  states.resize(static_cast<std::size_t>(n_curves));
  StageAForward<N> f;
  f.knots.resize(static_cast<std::size_t>(n_curves));
  std::vector<int> solved;   // curves with values, in solve order
  for (const std::vector<int>& block : set.blocks(mode)) {
    std::vector<int> insts, inst_curve, inst_j;
    int n_z = 0;
    for (int c : block) {
      for (int j = 0; j < s.sets[static_cast<std::size_t>(c)].n_instruments(); ++j) {
        insts.push_back(first[static_cast<std::size_t>(c)] + j);
        inst_curve.push_back(c);
        inst_j.push_back(j);
      }
      n_z += static_cast<int>(set.curve(c).knot_t.size());
    }
    const int n_r = static_cast<int>(insts.size());
    std::vector<D> p;
    for (int i : insts) p.push_back(q[static_cast<std::size_t>(i)]);
    for (int c : solved) p.insert(p.end(), states.z[static_cast<std::size_t>(c)].begin(), states.z[static_cast<std::size_t>(c)].end());
    const int n_p = static_cast<int>(p.size());
    auto R = [&](const auto* z, const auto* pp, auto* F) {
      using S = std::decay_t<decltype(*z)>;
      solver::CurveStates<S> st;
      st.specs = &set.curves();
      st.resize(static_cast<std::size_t>(n_curves));
      std::size_t off = static_cast<std::size_t>(n_r);
      for (int c : solved) {
        const std::size_t nk = set.curve(c).knot_t.size();
        st.set(c, pp + off, nk);
        off += nk;
      }
      off = 0;
      for (int c : block) {
        const std::size_t nk = set.curve(c).knot_t.size();
        st.set(c, z + off, nk);
        off += nk;
      }
      auto df = [&st](int slot, double t) { return st.df(slot, t); };
      for (std::size_t m = 0; m < insts.size(); ++m) {
        const instrument::Instrument& in = s.sets[static_cast<std::size_t>(inst_curve[m])].instruments[static_cast<std::size_t>(inst_j[m])].instrument;
        F[m] = instrument::residual<S>(in, df, pp[m]);
      }
    };
    std::vector<D> z;
    for (int c : block) {
      const std::vector<double>& z0 = start != nullptr ? (*start)[static_cast<std::size_t>(c)] : set.curve(c).start;
      for (double v : z0) z.push_back(D(v));
    }
    f.reports.push_back(solver::implicit_dual<N>(R, n_z, n_p, n_r, p.data(), z.data(), options));
    std::size_t off = 0;
    for (int c : block) {
      const std::size_t nk = set.curve(c).knot_t.size();
      states.set(c, z.data() + off, nk);
      f.knots[static_cast<std::size_t>(c)].assign(z.begin() + static_cast<std::ptrdiff_t>(off), z.begin() + static_cast<std::ptrdiff_t>(off + nk));
      off += nk;
      solved.push_back(c);
    }
  }
  DfMemo<D, std::function<D(int, double)>> memo([&states](int slot, double t) { return states.df(slot, t); });
  f.book = price_stage_a<D>(s, memo);
  return f;
}

// Structure statistics of the inferred program (O6).
struct StageAStructure {
  std::size_t domains = 0, scan_domains = 0, chains = 0, scan_rows = 0, values = 0, columns = 0, gathers = 0, segments = 0, literals = 0;
  std::size_t df_domains = 0;        // domains whose last op is Exp (the discount factors)
  std::size_t select_domains = 0;    // domains whose last op is Select
  std::string to_string() const;
};
StageAStructure stage_a_structure(const ir::Program& p);

// A debugging dump (CSV, never an input): <prefix>trades.csv, <prefix>quotes.csv,
// <prefix>curves.csv, <prefix>scenarios.csv and, with a tape, <prefix>record_point.csv (the
// record-point book per trade and the aggregates). Returns the files written.
std::vector<std::string> stage_a_dump(const StageA& s, const StageATape* tape, const std::string& prefix);

}  // namespace epykos::fixtures
