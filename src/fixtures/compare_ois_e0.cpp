// The MX head-to-head fixture (test-only, D28; D71). An E0 TU (-ffp-contract=off in every preset,
// D25): the generating curve, the quotes and the book's rates are the same bits in every preset,
// so the numbers this fixture hands a second engine do not depend on the build.
//
// SYNTHETIC throughout: quotes and trades come from the seed of `CompareOisOptions`, exactly as
// `docs/WORKLOADS.md` requires. Nothing here is derived from any other engine (D11).
#include "epykos/fixtures/compare_ois.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "epykos/maths/instrument/calibrate.hpp"
#include "epykos/maths/instrument/instrument.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"

namespace epykos::fixtures {

using namespace conventions;
using namespace instrument;

namespace {

using clock_type = std::chrono::steady_clock;
double seconds_since(clock_type::time_point t0) { return std::chrono::duration<double>(clock_type::now() - t0).count(); }

// The generating zero curve of the Stage A problem definition, reused so that the level and shape
// of this fixture's quotes are the same plausible-but-synthetic USD curve:
//   z(t) = long + (short - long) * (1 - exp(-t/tau)) / (t/tau)
double generating_zero(const CompareOisOptions& o, double t) {
  const double tau = o.generating_reversion_years;
  if (t <= 0.0) return o.generating_short;
  const double x = t / tau;
  return o.generating_long + (o.generating_short - o.generating_long) * (1.0 - std::exp(-x)) / x;
}

std::string iso(Date d) { return conventions::to_iso(d); }

// A discount factor memo per (slot, t), so that the coupons of two trades that share a payment
// time record ONE df sub-graph. `fixtures/stage_a.hpp` has the same thing; it is repeated here
// rather than included so that this fixture does not pull the whole Stage A fixture in.
template <class Scalar, class Inner>
class Memo {
 public:
  explicit Memo(Inner inner) : inner_(std::move(inner)) {}
  Scalar operator()(int slot, double t) {
    std::uint64_t key = 0;
    std::memcpy(&key, &t, sizeof key);
    auto it = table_.find(key);
    if (it != table_.end()) return it->second;
    const Scalar v = inner_(slot, t);
    table_.emplace(key, v);
    return v;
  }
  std::size_t size() const noexcept { return table_.size(); }

 private:
  Inner inner_;
  std::unordered_map<std::uint64_t, Scalar> table_;
};

// A double written so that a reader recovers the same bits (17 significant digits).
std::string num(double v) {
  std::ostringstream s;
  s << std::setprecision(17) << v;
  return s.str();
}

void emit_coupons(std::ostringstream& out, const Leg& leg, const char* indent, bool with_obs_days) {
  bool first = true;
  for (const Coupon& c : leg.coupons) {
    if (!first) out << ",\n";
    first = false;
    out << indent << "{\"t_start\": " << num(c.t_start) << ", \"t_end\": " << num(c.t_end) << ", \"t_pay\": " << num(c.t_pay)
        << ", \"tau_accrual\": " << num(c.accrual);
    if (c.kind == CouponKind::RfrCompounded) {
      out << ", \"tau_obs\": " << num(c.obs_tau);
      out << ", \"n_obs_days\": " << (c.obs_end - c.obs_begin);
      if (with_obs_days) {
        // Every projected observation day of the coupon, as the sub-period [t_rate, t_next) the
        // overnight forward spans and the accrual `weight` it is applied for. For the plain
        // observation method the spans tile the observation period exactly and the product
        // telescopes; emitting them is what lets a reader reproduce THIS engine's own arithmetic
        // instead of the collapsed one-sub-period form (D71: the two are compared both ways).
        out << ",\n" << indent << " \"obs_days\": [";
        for (int i = c.obs_begin; i < c.obs_end; ++i) {
          const ObsDay& d = leg.obs[static_cast<std::size_t>(i)];
          out << (i > c.obs_begin ? ", " : "") << "[" << num(d.t_rate) << ", " << num(d.t_next) << ", " << num(d.tau_rate)
              << ", " << num(d.weight) << "]";
        }
        out << "]";
      }
    }
    out << ", \"kind\": \"" << to_string(c.kind) << "\"}";
  }
  out << "\n";
}

}  // namespace

const std::vector<std::string>& compare_ois_default_tenors() {
  // Sixteen calibration tenors: annual one year through ten years, then twelve, fifteen, twenty,
  // twenty-five, thirty and forty years, every one of them in the USD-SOFR-OIS blueprint's own
  // cited standard tenor set. One instrument and one knot each, so the `compare_ois` fixture
  // calibrates sixteen instruments onto sixteen knots (D64) and the block is square.
  static const std::vector<std::string> t = {"1Y", "2Y", "3Y", "4Y",  "5Y",  "6Y",  "7Y",  "8Y",
                                             "9Y", "10Y", "12Y", "15Y", "20Y", "25Y", "30Y", "40Y"};
  return t;
}

BuildContext CompareOis::context() const {
  BuildContext c;
  c.registry = &registry;
  c.fixings = &fixings;
  c.curves = &slots;
  c.valuation = valuation;
  return c;
}

double CompareOis::generating_df(int, double t) const {
  const curve::Composite comp = set.composite();
  return comp.df(generating_z.data(), t);
}

CompareOis make_compare_ois(const CompareOisOptions& options) {
  CompareOis s;
  s.options = options;
  s.registry = Registry::load();
  s.blueprints = Blueprints::load();
  s.blueprints.validate(s.registry);
  s.valuation = parse_iso(options.valuation);
  s.slots.add("USD-SOFR");
  // The fixings history stays EMPTY: every trade of this fixture starts at or after the
  // valuation date, so no coupon has a realised day (compare_ois.hpp, reason 4).

  // The curve definition, built in code rather than read from blueprints/curves/*.json: this
  // fixture's curve is a comparison artefact, not a curve the desk problem calibrates, so it
  // does not belong in the committed curve definitions (D36 keeps those as the problem's data).
  CurveDefinition& def = s.definition;
  def.name = "USD-SOFR-COMPARE";
  def.index = "USD-SOFR";
  def.currency = "USD";
  def.description =
      "MX head-to-head: USD SOFR OIS, par-rate quoted, log-linear discount factors (piecewise-constant "
      "instantaneous forward), one knot per instrument at its maturity.";
  CurveRegionDef region;
  region.from = "";                              // the single region starts at t = 0
  region.scheme = curve::SchemeKind::linear;
  region.variable = curve::Variable::logdf;      // log DF piecewise linear on {0, knot_t...}
  def.regions.push_back(region);
  CurveInstrumentEntry entry;
  entry.blueprint = "USD-SOFR-OIS";
  entry.tenors = options.tenors.empty() ? compare_ois_default_tenors() : options.tenors;
  def.instruments.push_back(entry);
  // def.knot_tenors stays empty: the knots are the instruments' maturities, so the block is square.

  const BuildContext ctx = s.context();
  s.set = build_calibration_set(ctx, s.blueprints, def);

  // The generating curve in the region's variable (logdf): y(t) = -z(t) * t.
  s.generating_z.reserve(s.set.knot_t.size());
  for (double t : s.set.knot_t) s.generating_z.push_back(-generating_zero(options, t) * t);

  // The quotes: the generating curve's par quotes plus uniform noise of +/- quote_noise_bp, so
  // that the calibration is a fit and not an identity (noise 0 recovers the generating curve).
  auto df_gen = [&](int slot, double t) { return s.generating_df(slot, t); };
  s.quotes = par_quotes(s.set, df_gen);
  if (options.quote_noise_bp != 0.0) {
    rng::Philox p(options.seed, 1);
    const double n = options.quote_noise_bp * 1e-4;
    for (double& q : s.quotes) q += p.uniform_range(-n, n);
  }

  // The book: `trades` OIS trades. Trade i draws a calibration instrument to MATURE WITH, a
  // forward-start offset, a notional log-uniform between one million and two hundred million, a
  // side, and a fixed rate that is that instrument's par quote moved by uniform
  // +/- rate_moneyness, so the book is genuinely off-market.
  //
  // Two properties of that draw are deliberate and load-bearing, and neither is cosmetic:
  //
  //   * the TERMINATION is a calibration instrument's own termination, so the trade's last
  //     payment lands exactly on a curve knot and nothing is ever priced beyond the last one,
  //     where the two engines' extrapolations differ (bench/compare/README.md section 4 item 5,
  //     gated by tests/compare/ois_test.cpp);
  //   * the EFFECTIVE date offset is drawn but `max_start_offset_days` DEFAULTS TO 0, so every
  //     trade spot-starts and its schedule is whole annual periods with NO STUB. That is forced,
  //     not preferred: at an offset of 360 days the trades become structurally distinct and the
  //     two engines stop agreeing -- book NPV and ladder off by 1e-3 to 1e-4 relative while the
  //     curve still matches to 5.8e-13, which localises the disagreement to the front stub and
  //     nothing else. A stub-free book is necessarily built on ONE annual grid, so its trades
  //     share coupon structure and the E0 passes collapse them: measured, a book of two hundred
  //     and fifty-six trades is 92,534 nodes against 80,581 for no book at all, about 47 nodes
  //     per trade, where the distinct-start book cost about 125. This engine's book-side cost is
  //     therefore understated relative to a real desk book, and that caveat runs in ITS favour;
  //     it is recorded in bench/compare/README.md section 4 rather than left implicit.
  const std::vector<std::string>& tenors = def.instruments[0].tenors;
  rng::Philox pb(options.seed, 2);
  for (int i = 0; i < options.trades; ++i) {
    const std::size_t ti = static_cast<std::size_t>(pb.uniform() * static_cast<double>(tenors.size()));
    const std::size_t tix = std::min(ti, tenors.size() - 1);
    const Instrument& anchor = s.set.instruments[tix].instrument;
    Trade t;
    t.id = "CMP-" + std::to_string(i);
    t.blueprint = "USD-SOFR-OIS";
    const int term_days = static_cast<int>(anchor.termination - anchor.effective);
    const int max_offset = std::min(options.max_start_offset_days, term_days / 2);
    const int offset = max_offset > 0 ? static_cast<int>(pb.uniform() * static_cast<double>(max_offset)) : 0;
    t.effective = anchor.effective + offset;
    t.termination = anchor.termination;   // a knot, by construction
    const double lo = std::log(1.0e6), hi = std::log(2.0e8);
    t.notional = std::exp(pb.uniform_range(lo, hi));
    t.side = pb.uniform() < 0.5 ? +1 : -1;
    t.fixed_rate = s.quotes[tix] * (1.0 + pb.uniform_range(-options.rate_moneyness, options.rate_moneyness));
    const Instrument in = build_instrument(ctx, s.blueprints, t);
    CompareOisTrade spec;
    spec.tenor = tenors[tix];
    spec.notional = *t.notional;
    spec.side = *t.side;
    spec.fixed_rate = *t.fixed_rate;
    spec.effective = in.effective;
    spec.termination = in.termination;
    s.trade_spec.push_back(spec);
    s.book.push_back(in);
  }
  return s;
}

CompareOisTape record_compare_ois(const CompareOis& s, bool passes) {
  const clock_type::time_point t0 = clock_type::now();
  CompareOisTape r;
  r.set = std::make_unique<solver::CurveSet>();
  const std::vector<double> start = start_values(s.set, s.options.flat_start);
  add_calibration_set(*r.set, s.set, start);
  {
    Tape::Scope scope(r.tape);
    std::vector<Rec> q;
    for (double v : s.quotes) {
      r.quote_inputs.push_back(static_cast<int>(r.tape.num_inputs()));
      q.push_back(make_input(r.tape, v));
    }
    solver::SolveOptions solve;
    solve.tol = s.options.solve_tol;
    const solver::CurveSet::Calibration cal = r.set->calibrate(r.tape, r.registry, q, solver::CurveSet::Mode::sequential, solve);
    if (cal.results.size() != 1u) throw std::logic_error("compare_ois: expected one implicit block");
    r.record_report = cal.results[0].report;
    // O1: the calibrated knots, in the region's variable (logdf).
    for (const Rec& zk : cal.states.curve(0)) {
      r.knot_outputs.push_back(register_output(r.tape, zk));
      r.record_knots.push_back(zk.v);
    }
    // O2: the book off the calibrated Recs, discount factors memoised per (slot, t).
    Memo<Rec, std::function<Rec(int, double)>> memo([&cal](int slot, double t) { return cal.states.df(slot, t); });
    Rec total = Rec(0.0);
    for (const Instrument& in : s.book) {
      const Rec p = pv<Rec>(in, memo);
      r.pv_outputs.push_back(register_output(r.tape, p));
      r.record_pv.push_back(p.v);
      total = total + p;
    }
    r.book_output = register_output(r.tape, total);
    r.record_book = total.v;
  }
  r.tape.validate();
  r.nodes_raw = r.tape.size();
  if (passes) {
    standard_passes(r.tape);
    r.tape.validate();
  }
  r.nodes_after_passes = r.tape.size();
  r.seconds_record = seconds_since(t0);
  return r;
}

solver::ProgramOptions compare_ois_program_options(const CompareOis& s) {
  solver::ProgramOptions o;
  o.max_batch = s.options.max_batch;
  o.interpreter.max_batch = s.options.max_batch;
  o.adjoint.max_batch = s.options.max_batch;
  return o;
}

std::vector<double> compare_ois_ladder(solver::ImplicitProgram& program, const std::vector<double>& quotes,
                                       const std::vector<int>& ordinals, std::vector<double>* out) {
  const int n_q = program.n_state(), n_out = program.n_outputs();
  if (static_cast<int>(quotes.size()) != n_q) {
    throw std::invalid_argument("compare_ois_ladder: " + std::to_string(quotes.size()) + " quotes for " + std::to_string(n_q));
  }
  const std::size_t R = ordinals.size();
  std::vector<double> rows(R * static_cast<std::size_t>(n_q), 0.0);
  if (out != nullptr) out->assign(static_cast<std::size_t>(n_out), 0.0);
  const int chunk = program.max_batch();
  std::vector<double> st, ob, o, sb;
  for (std::size_t r0 = 0; r0 < R; r0 += static_cast<std::size_t>(chunk)) {
    const std::size_t B = std::min(static_cast<std::size_t>(chunk), R - r0);
    st.assign(static_cast<std::size_t>(n_q) * B, 0.0);
    ob.assign(static_cast<std::size_t>(n_out) * B, 0.0);
    o.assign(static_cast<std::size_t>(n_out) * B, 0.0);
    // D65: state_bar is sized over EVERY tape input, not over the free inputs.
    sb.assign(static_cast<std::size_t>(program.n_inputs()) * B, 0.0);
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_q); ++k) {
      for (std::size_t b = 0; b < B; ++b) st[k * B + b] = quotes[k];
    }
    for (std::size_t b = 0; b < B; ++b) ob[static_cast<std::size_t>(ordinals[r0 + b]) * B + b] = 1.0;
    program.adjoint(st.data(), static_cast<int>(B), ob.data(), o.data(), sb.data());
    for (std::size_t b = 0; b < B; ++b) {
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_q); ++k) {
        rows[(r0 + b) * static_cast<std::size_t>(n_q) + k] = sb[k * B + b];
      }
    }
    if (out != nullptr && r0 == 0) {
      for (std::size_t oo = 0; oo < static_cast<std::size_t>(n_out); ++oo) (*out)[oo] = o[oo * B];
    }
  }
  return rows;
}

std::string compare_ois_exchange_json(const CompareOis& s, bool with_obs_days) {
  std::ostringstream o;
  o << "{\n";
  o << "  \"format\": \"epykos-compare-ois 1\",\n";
  o << "  \"meta\": {\n";
  o << "    \"produced_by\": \"EpykosEngine fixtures::compare_ois_exchange_json\",\n";
  o << "    \"description\": \"One USD SOFR OIS curve and an OIS book, as year fractions from the valuation "
       "date on ACT/365F curve time. Every accrual, observation and payment time a price depends on is "
       "given explicitly; no calendar, day-count or index name is needed to reprice it. The curve is "
       "log DF piecewise linear on {0, knots...}, i.e. the instantaneous forward is piecewise constant, "
       "so the discount factor at EVERY time is pinned by the knot values alone.\",\n";
  o << "    \"curve_family\": \"logdf_piecewise_linear\",\n";
  o << "    \"quote_kind\": \"par_rate\",\n";
  o << "    \"obs_days_included\": " << (with_obs_days ? "true" : "false") << ",\n";
  o << "    \"synthetic\": true,\n";
  o << "    \"seed\": " << s.options.seed << "\n";
  o << "  },\n";
  o << "  \"valuation\": \"" << iso(s.valuation) << "\",\n";
  o << "  \"index\": \"USD-SOFR\",\n";
  o << "  \"knots\": [";
  for (std::size_t k = 0; k < s.set.knot_t.size(); ++k) o << (k ? ", " : "") << num(s.set.knot_t[k]);
  o << "],\n";
  o << "  \"instruments\": [\n";
  for (std::size_t i = 0; i < s.set.instruments.size(); ++i) {
    const CalibrationInstrument& ci = s.set.instruments[i];
    const Instrument& in = ci.instrument;
    o << "    {\n";
    o << "      \"key\": \"" << ci.key << "\", \"tenor\": \"" << ci.tenor << "\",\n";
    o << "      \"market\": " << num(s.quotes[i]) << ",\n";
    o << "      \"effective\": \"" << iso(in.effective) << "\", \"termination\": \"" << iso(in.termination) << "\",\n";
    o << "      \"t_maturity\": " << num(in.t_maturity) << ",\n";
    o << "      \"fixed\": [\n";
    emit_coupons(o, in.legs[0], "        ", false);
    o << "      ],\n";
    o << "      \"float\": [\n";
    emit_coupons(o, in.legs[1], "        ", with_obs_days);
    o << "      ]\n";
    o << "    }" << (i + 1 < s.set.instruments.size() ? "," : "") << "\n";
  }
  o << "  ],\n";
  o << "  \"book\": [\n";
  for (std::size_t i = 0; i < s.book.size(); ++i) {
    const Instrument& in = s.book[i];
    const CompareOisTrade& sp = s.trade_spec[i];
    o << "    {\n";
    o << "      \"id\": \"" << in.id << "\", \"tenor\": \"" << sp.tenor << "\",\n";
    o << "      \"notional\": " << num(sp.notional) << ", \"side\": " << sp.side
      << ", \"fixed_rate\": " << num(sp.fixed_rate) << ",\n";
    o << "      \"effective\": \"" << iso(in.effective) << "\", \"termination\": \"" << iso(in.termination) << "\",\n";
    o << "      \"fixed\": [\n";
    emit_coupons(o, in.legs[0], "        ", false);
    o << "      ],\n";
    o << "      \"float\": [\n";
    emit_coupons(o, in.legs[1], "        ", with_obs_days);
    o << "      ]\n";
    o << "    }" << (i + 1 < s.book.size() ? "," : "") << "\n";
  }
  o << "  ]\n";
  o << "}\n";
  return o.str();
}

}  // namespace epykos::fixtures
