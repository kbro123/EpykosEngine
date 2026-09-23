// The Stage A desk problem (test-only fixture, D28; M3/G5). An E0 TU (-ffp-contract=off in every
// preset, D25): the generating curves, quotes, trade rates and the record-point oracle are the
// same bits in every preset. SYNTHETIC: quotes, fixings, trades and scenarios come from the seed;
// the definitions are blueprints/problems/stage_a.json and the data of G0 / G2.
#include "epykos/fixtures/stage_a.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "epykos/maths/curve/linear.hpp"
#include "epykos/rng/philox.hpp"
#include "epykos/tape/passes.hpp"

namespace epykos::fixtures {

using namespace conventions;
using namespace instrument;
using json::Value;

namespace {

using clock_type = std::chrono::steady_clock;
double seconds_since(clock_type::time_point t0) {
  return std::chrono::duration<double>(clock_type::now() - t0).count();
}

[[noreturn]] void fail(const Value& at, const std::string& what) { throw BlueprintError("stage_a: " + at.where() + ": " + what); }
[[noreturn]] void fail(const std::string& what) { throw BlueprintError("stage_a: " + what); }

void check_keys(const Value& obj, std::initializer_list<const char*> allowed, std::initializer_list<const char*> required, const std::string& what) {
  if (!obj.is_object()) fail(obj, what + ": expected an object");
  for (const auto& m : obj.as_object()) {
    bool ok = false;
    for (const char* a : allowed) ok |= m.first == a;
    if (!ok) fail(m.second, what + ": unknown key \"" + m.first + "\"");
  }
  for (const char* r : required) {
    if (!obj.has(r)) fail(obj, what + ": missing \"" + std::string(r) + "\"");
  }
}

double number(const Value& obj, const char* key, const std::string& what) {
  const Value& v = obj.at(key);
  if (!v.is_number()) fail(v, what + ": \"" + key + "\" must be a number");
  return v.as_double();
}
int integer(const Value& obj, const char* key, const std::string& what) {
  const Value& v = obj.at(key);
  if (!v.is_integer()) fail(v, what + ": \"" + key + "\" must be an integer");
  return static_cast<int>(v.as_int());
}
std::string text(const Value& obj, const char* key, const std::string& what) {
  const Value& v = obj.at(key);
  if (!v.is_string()) fail(v, what + ": \"" + key + "\" must be a string");
  return v.as_string();
}
std::vector<StageADefinition::Weighted> weighted(const Value& arr, const char* key, const std::string& what) {
  if (!arr.is_array() || arr.size() == 0) fail(arr, what + ": expected a non-empty array");
  std::vector<StageADefinition::Weighted> out;
  for (const Value& e : arr.as_array()) {
    check_keys(e, {key, "weight"}, {key, "weight"}, what);
    StageADefinition::Weighted w;
    w.name = text(e, key, what);
    w.weight = number(e, "weight", what);
    if (!(w.weight >= 0.0)) fail(e, what + ": a weight must be >= 0");
    out.push_back(w);
  }
  return out;
}

}  // namespace

// ---- the definition ----------------------------------------------------------------------------

double StageADefinition::fx_placeholder(const std::string& currency) const {
  for (const Fx& f : fx) {
    if (f.currency == currency) return f.placeholder;
  }
  fail("no fx placeholder for currency '" + currency + "'");
}

int StageADefinition::currency_index(const std::string& currency) const {
  for (std::size_t i = 0; i < currencies.size(); ++i) {
    if (currencies[i] == currency) return static_cast<int>(i);
  }
  return -1;
}

std::string default_stage_a_path() { return Blueprints::default_root() + "/problems/stage_a.json"; }

StageADefinition load_stage_a_definition(const std::string& path) {
  const Value doc = json::parse_file(path);
  check_keys(doc, {"meta", "problem"}, {"meta", "problem"}, "document");
  check_keys(doc.at("meta"), {"title", "description", "sources", "notes"}, {"title"}, "meta");
  const Value& p = doc.at("problem");
  check_keys(p, {"name", "valuation", "seed", "currencies", "reporting_currency", "fx", "curves", "quotes", "fixings", "book", "scenarios"},
             {"name", "valuation", "seed", "currencies", "reporting_currency", "fx", "curves", "quotes", "fixings", "book", "scenarios"}, "problem");
  StageADefinition d;
  d.path = path;
  d.name = text(p, "name", "problem");
  d.valuation = parse_iso(text(p, "valuation", "problem"));
  if (!p.at("seed").is_integer()) fail(p.at("seed"), "problem: \"seed\" must be an integer");
  d.seed = static_cast<std::uint64_t>(p.at("seed").as_int());
  for (const Value& c : p.at("currencies").as_array()) {
    if (!c.is_string()) fail(c, "problem: a currency must be a string");
    d.currencies.push_back(c.as_string());
  }
  d.reporting_currency = text(p, "reporting_currency", "problem");
  if (d.currency_index(d.reporting_currency) < 0) fail(p.at("reporting_currency"), "problem: the reporting currency is not one of the currencies");
  for (const Value& f : p.at("fx").as_array()) {
    check_keys(f, {"currency", "placeholder"}, {"currency", "placeholder"}, "fx");
    StageADefinition::Fx x;
    x.currency = text(f, "currency", "fx");
    x.placeholder = number(f, "placeholder", "fx");
    d.fx.push_back(x);
  }
  for (const std::string& c : d.currencies) (void)d.fx_placeholder(c);
  for (const Value& c : p.at("curves").as_array()) {
    check_keys(c, {"definition", "variants", "generating"}, {"definition", "variants", "generating"}, "curve");
    StageADefinition::CurveEntry e;
    e.definition = text(c, "definition", "curve");
    for (const Value& v : c.at("variants").as_array()) {
      if (!v.is_string()) fail(v, "curve: a variant must be a string");
      e.variants.push_back(v.as_string());
    }
    const Value& g = c.at("generating");
    check_keys(g, {"short", "long", "reversion_years"}, {"short", "long", "reversion_years"}, "generating");
    e.generating.short_rate = number(g, "short", "generating");
    e.generating.long_rate = number(g, "long", "generating");
    e.generating.reversion_years = number(g, "reversion_years", "generating");
    if (!(e.generating.reversion_years > 0.0)) fail(g, "generating: reversion_years must be > 0");
    d.curves.push_back(e);
  }
  if (d.curves.empty()) fail(p.at("curves"), "problem: at least one curve");
  {
    const Value& q = p.at("quotes");
    check_keys(q, {"description", "noise_bp", "futures_noise_price"}, {"noise_bp", "futures_noise_price"}, "quotes");
    d.quotes.noise_bp = number(q, "noise_bp", "quotes");
    d.quotes.futures_noise_price = number(q, "futures_noise_price", "quotes");
  }
  {
    const Value& f = p.at("fixings");
    check_keys(f, {"description", "from", "daily_vol", "reversion", "levels"}, {"from", "daily_vol", "reversion", "levels"}, "fixings");
    d.fixings.from = parse_iso(text(f, "from", "fixings"));
    d.fixings.daily_vol = number(f, "daily_vol", "fixings");
    d.fixings.reversion = number(f, "reversion", "fixings");
    for (const Value& l : f.at("levels").as_array()) {
      check_keys(l, {"index", "level"}, {"index", "level"}, "fixings level");
      d.fixings.levels.emplace_back(text(l, "index", "fixings level"), number(l, "level", "fixings level"));
    }
  }
  {
    const Value& b = p.at("book");
    check_keys(b, {"description", "trades", "mix", "tenors", "notional", "seasoned_fraction", "seasoned_age_days", "netting_sets", "rate_moneyness"},
               {"trades", "mix", "tenors", "notional", "seasoned_fraction", "seasoned_age_days", "netting_sets", "rate_moneyness"}, "book");
    d.book.trades = integer(b, "trades", "book");
    d.book.mix = weighted(b.at("mix"), "blueprint", "book mix");
    d.book.tenors = weighted(b.at("tenors"), "tenor", "book tenors");
    const Value& n = b.at("notional");
    check_keys(n, {"min", "max", "distribution"}, {"min", "max", "distribution"}, "book notional");
    d.book.notional_min = number(n, "min", "book notional");
    d.book.notional_max = number(n, "max", "book notional");
    d.book.notional_distribution = text(n, "distribution", "book notional");
    if (d.book.notional_distribution != "log_uniform") fail(n.at("distribution"), "book notional: only \"log_uniform\" is implemented");
    if (!(d.book.notional_min > 0.0 && d.book.notional_max >= d.book.notional_min)) fail(n, "book notional: need 0 < min <= max");
    d.book.seasoned_fraction = number(b, "seasoned_fraction", "book");
    const Value& a = b.at("seasoned_age_days");
    check_keys(a, {"min", "max"}, {"min", "max"}, "book seasoned_age_days");
    d.book.seasoned_age_min = integer(a, "min", "book seasoned_age_days");
    d.book.seasoned_age_max = integer(a, "max", "book seasoned_age_days");
    d.book.netting_sets = integer(b, "netting_sets", "book");
    if (d.book.netting_sets < 1) fail(b.at("netting_sets"), "book: netting_sets must be >= 1");
    d.book.rate_moneyness = number(b, "rate_moneyness", "book");
  }
  {
    const Value& s = p.at("scenarios");
    check_keys(s, {"description", "count", "families", "t_max_years", "solver"}, {"count", "families", "t_max_years", "solver"}, "scenarios");
    d.scenarios.count = integer(s, "count", "scenarios");
    int total = 0;
    for (const Value& f : s.at("families").as_array()) {
      check_keys(f, {"name", "count", "size_bp", "pivot_years"}, {"name", "count", "size_bp", "pivot_years"}, "scenario family");
      StageADefinition::ScenarioFamily fam;
      fam.name = text(f, "name", "scenario family");
      if (fam.name != "parallel" && fam.name != "twist" && fam.name != "butterfly" && fam.name != "per_curve") {
        fail(f.at("name"), "scenario family: unknown family \"" + fam.name + "\"");
      }
      fam.count = integer(f, "count", "scenario family");
      fam.size_bp = number(f, "size_bp", "scenario family");
      fam.pivot_years = number(f, "pivot_years", "scenario family");
      total += fam.count;
      d.scenarios.families.push_back(fam);
    }
    if (total != d.scenarios.count) fail(s.at("count"), "scenarios: the families' counts sum to " + std::to_string(total) + ", not count");
    d.scenarios.t_max_years = number(s, "t_max_years", "scenarios");
    const Value& sol = s.at("solver");
    check_keys(sol, {"jacobian", "warm_start"}, {"jacobian", "warm_start"}, "scenarios solver");
    d.scenarios.jacobian = text(sol, "jacobian", "scenarios solver");
    if (d.scenarios.jacobian != "chord" && d.scenarios.jacobian != "per_iteration") fail(sol.at("jacobian"), "scenarios solver: jacobian must be \"chord\" or \"per_iteration\"");
    if (!sol.at("warm_start").is_bool()) fail(sol.at("warm_start"), "scenarios solver: warm_start must be a boolean");
    d.scenarios.warm_start = sol.at("warm_start").as_bool();
  }
  return d;
}

// ---- the filled problem ------------------------------------------------------------------------

double StageA::generating_zero(int slot, double t) const {
  const CalibrationSet& cs = sets[static_cast<std::size_t>(slot)];
  return curve::linear::zero_rate(cs.knot_t.data(), generating[static_cast<std::size_t>(slot)].data(), static_cast<int>(cs.knot_t.size()), t);
}
double StageA::generating_df(int slot, double t) const {
  const CalibrationSet& cs = sets[static_cast<std::size_t>(slot)];
  return curve::linear::df(cs.knot_t.data(), generating[static_cast<std::size_t>(slot)].data(), static_cast<int>(cs.knot_t.size()), t);
}

namespace {

// z(t) = long + (short − long)·(1 − e^{−x})/x, x = t/τ (the Nelson–Siegel level + slope shape).
double generating_shape(const StageADefinition::Generating& g, double t) {
  const double x = t / g.reversion_years;
  const double f = x > 1e-12 ? (1.0 - std::exp(-x)) / x : 1.0;
  return g.long_rate + (g.short_rate - g.long_rate) * f;
}

std::size_t pick(const std::vector<StageADefinition::Weighted>& w, double u) {
  double total = 0.0;
  for (const auto& x : w) total += x.weight;
  double acc = 0.0;
  for (std::size_t i = 0; i < w.size(); ++i) {
    acc += w[i].weight;
    if (u * total < acc) return i;
  }
  return w.size() - 1;
}

double clip(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

}  // namespace

std::vector<double> stage_a_generating_par_quotes(const CalibrationSet& cs, const std::function<double(int, double)>& df) { return par_quotes(cs, df); }

StageA make_stage_a(const StageAOptions& options) {
  StageA s;
  s.options = options;
  s.def = load_stage_a_definition(options.path.empty() ? default_stage_a_path() : options.path);
  s.valuation = s.def.valuation;
  s.registry = Registry::load();
  s.blueprints = Blueprints::load();
  s.blueprints.validate(s.registry);
  // Slots: the definitions' indices, in the blueprint's order; slot 0 may be recorded on a variant.
  for (std::size_t c = 0; c < s.def.curves.size(); ++c) {
    const StageADefinition::CurveEntry& e = s.def.curves[c];
    std::string name = e.definition;
    if (c == 0 && !options.usd_curve.empty() && options.usd_curve != e.definition) {
      if (std::find(e.variants.begin(), e.variants.end(), options.usd_curve) == e.variants.end()) {
        fail("'" + options.usd_curve + "' is not a variant of '" + e.definition + "' in " + s.def.path);
      }
      name = options.usd_curve;
    }
    const CurveDefinition& def = s.blueprints.curve(name);
    if (def.index != s.blueprints.curve(e.definition).index) fail("variant '" + name + "' projects another index than '" + e.definition + "'");
    s.slots.add(def.index);
    s.curve_definitions.push_back(name);
  }
  // Fixings: SYNTHETIC, per index of the definition's levels.
  for (std::size_t i = 0; i < s.def.fixings.levels.size(); ++i) {
    const auto& [index, level] = s.def.fixings.levels[i];
    SyntheticFixingsSpec spec;
    spec.seed = s.def.seed + i;
    spec.level = level;
    spec.daily_vol = s.def.fixings.daily_vol;
    spec.reversion = s.def.fixings.reversion;
    synthetic_fixings(s.fixings, index, s.registry.calendar(s.registry.index(index).fixing_calendar), s.def.fixings.from, s.valuation - 1, spec);
  }
  const BuildContext ctx = s.context();
  // Calibration sets and the generating curves.
  for (std::size_t c = 0; c < s.def.curves.size(); ++c) {
    const CalibrationSet cs = build_calibration_set(ctx, s.blueprints, s.blueprints.curve(s.curve_definitions[c]));
    if (cs.slot != static_cast<int>(c)) fail("curve '" + cs.curve + "' resolved to slot " + std::to_string(cs.slot) + ", expected " + std::to_string(c));
    std::vector<double> z;
    for (double t : cs.knot_t) z.push_back(generating_shape(s.def.curves[c].generating, t));
    s.sets.push_back(cs);
    s.generating.push_back(z);
  }
  // Quotes: the generating curves' par quotes plus noise.
  auto df_gen = [&](int slot, double t) { return s.generating_df(slot, t); };
  const double noise_bp = options.quote_noise_bp >= 0.0 ? options.quote_noise_bp : s.def.quotes.noise_bp;
  const double noise_price = options.quote_noise_bp >= 0.0 ? s.def.quotes.futures_noise_price * (options.quote_noise_bp / (s.def.quotes.noise_bp > 0.0 ? s.def.quotes.noise_bp : 1.0))
                                                           : s.def.quotes.futures_noise_price;
  for (std::size_t c = 0; c < s.sets.size(); ++c) {
    const CalibrationSet& cs = s.sets[c];
    const std::vector<double> par = par_quotes(cs, df_gen);
    for (std::size_t i = 0; i < cs.instruments.size(); ++i) {
      const std::size_t k = s.quotes.size();
      rng::Philox g(s.def.seed, stage_a_quote_substream + k);
      const bool price = cs.instruments[i].instrument.kind == Kind::Future;
      const double u = g.uniform_range(-1.0, 1.0);
      s.quotes.push_back(par[i] + (price ? noise_price * u : noise_bp * 1e-4 * u));
      s.quote_keys.push_back(cs.instruments[i].key);
      s.quote_curve.push_back(static_cast<int>(c));
      s.quote_t.push_back(cs.instruments[i].instrument.t_maturity);
      s.quote_is_price.push_back(price ? 1 : 0);
    }
  }
  // The book.
  const int n_trades = options.trades >= 0 ? options.trades : s.def.book.trades;
  for (int i = 0; i < n_trades; ++i) {
    rng::Philox g(s.def.seed, stage_a_book_substream + static_cast<std::uint64_t>(i));
    StageATrade t;
    t.index = i;
    t.blueprint = s.def.book.mix[pick(s.def.book.mix, g.uniform())].name;
    const Blueprint& bp = s.blueprints.blueprint(t.blueprint);
    const std::string tenor = s.def.book.tenors[pick(s.def.book.tenors, g.uniform())].name;
    const double notional = g.log_uniform(s.def.book.notional_min, s.def.book.notional_max);
    const int side = g.uniform() < 0.5 ? +1 : -1;
    const bool seasoned = g.uniform() < s.def.book.seasoned_fraction;
    const Period period = Period::parse(tenor);
    const int tenor_days = add_period(s.valuation, period) - s.valuation;
    const int age_cap = std::min(s.def.book.seasoned_age_max, tenor_days - 30);
    const int age = static_cast<int>(g.uniform_int(s.def.book.seasoned_age_min, std::max(s.def.book.seasoned_age_min, age_cap)));
    t.netting_set = static_cast<int>(g.uniform_int(0, s.def.book.netting_sets - 1));
    t.moneyness = g.uniform_range(-s.def.book.rate_moneyness, s.def.book.rate_moneyness);
    t.seasoned = seasoned && age_cap >= s.def.book.seasoned_age_min;
    t.age_days = t.seasoned ? age : 0;
    Trade& tr = t.trade;
    tr.id = t.blueprint + " #" + std::to_string(i);
    tr.blueprint = t.blueprint;
    tr.tenor = period;
    tr.notional = notional;
    tr.side = side;
    if (t.seasoned) {
      tr.effective = s.valuation - t.age_days;
      tr.trade_date = tr.effective;
    }
    Trade probe = tr;
    probe.fixed_rate = 0.0;
    probe.spread = 0.0;
    const Instrument p = build_instrument(ctx, bp, probe);
    const double par0 = instrument::par<double>(p, df_gen);
    if (p.kind == Kind::Basis) {
      tr.spread = par0 * (1.0 + t.moneyness);
    } else {
      tr.fixed_rate = par0 * (1.0 + t.moneyness);
    }
    Instrument in = build_instrument(ctx, bp, tr);
    t.currency = in.currency;
    if (s.def.currency_index(t.currency) < 0) fail("trade '" + tr.id + "' is in currency '" + t.currency + "', not one of the problem's");
    s.trades.push_back(std::move(t));
    s.instruments.push_back(std::move(in));
  }
  // Scenarios: the families scaled to the requested lane count.
  const int n_scen = options.scenarios >= 0 ? options.scenarios : s.def.scenarios.count;
  std::vector<int> counts;
  {
    int assigned = 0;
    for (std::size_t f = 0; f < s.def.scenarios.families.size(); ++f) {
      const bool last = f + 1 == s.def.scenarios.families.size();
      int c = last ? n_scen - assigned
                   : static_cast<int>(std::lround(static_cast<double>(s.def.scenarios.families[f].count) * static_cast<double>(n_scen) / static_cast<double>(s.def.scenarios.count)));
      c = std::max(0, std::min(c, n_scen - assigned));
      counts.push_back(c);
      assigned += c;
    }
  }
  const double t_max = s.def.scenarios.t_max_years;
  for (std::size_t f = 0; f < s.def.scenarios.families.size(); ++f) {
    const StageADefinition::ScenarioFamily& fam = s.def.scenarios.families[f];
    for (int j = 0; j < counts[f]; ++j) {
      StageAScenario sc;
      sc.lane = s.n_scenarios();
      sc.family = fam.name;
      rng::Philox g(s.def.seed, stage_a_scenario_substream + static_cast<std::uint64_t>(sc.lane));
      sc.size = (fam.name == "parallel" && j == 0) ? 0.0 : g.uniform_range(-fam.size_bp, fam.size_bp) * 1e-4;
      if (fam.name == "per_curve") sc.curve = static_cast<int>(g.uniform_int(0, s.n_curves() - 1));
      std::vector<double> q(s.quotes.size());
      for (std::size_t k = 0; k < q.size(); ++k) {
        const double t = s.quote_t[k];
        double shape = 1.0;
        if (fam.name == "twist") {
          shape = (t - fam.pivot_years) / (t_max - fam.pivot_years);
        } else if (fam.name == "butterfly") {
          const double x = clip(std::log(std::max(t, 1e-6) / fam.pivot_years) / std::log(t_max / fam.pivot_years), -1.0, 1.0);
          shape = 2.0 * x * x - 1.0;
        } else if (fam.name == "per_curve") {
          shape = s.quote_curve[k] == sc.curve ? 1.0 : 0.0;
        }
        const double shift = sc.size * shape;
        q[k] = s.quote_is_price[k] ? s.quotes[k] - 100.0 * shift : s.quotes[k] + shift;
      }
      s.scenarios.push_back(sc);
      s.scenario_quotes.push_back(std::move(q));
    }
  }
  return s;
}

// ---- scales, layout, stats ---------------------------------------------------------------------

StageAScales stage_a_scales(const StageA& s, const StageABook<double>& book) {
  StageAScales sc;
  const std::size_t n = static_cast<std::size_t>(s.n_trades());
  sc.trade.resize(n);
  sc.currency.assign(static_cast<std::size_t>(s.n_currencies()), 0.0);
  sc.netting.assign(static_cast<std::size_t>(s.n_netting_sets()), 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    const Instrument& in = s.instruments[i];
    sc.trade[i] = in.kind == Kind::Future ? std::fabs(in.notional) : std::fabs(book.leg0[i]) + std::fabs(book.leg1[i]);
    sc.currency[static_cast<std::size_t>(s.def.currency_index(in.currency))] += std::fabs(book.pv[i]);
    sc.netting[static_cast<std::size_t>(s.trades[i].netting_set)] += std::fabs(book.pv_usd[i]);
    sc.book += std::fabs(book.pv_usd[i]);
  }
  return sc;
}

std::vector<int> StageALayout::pv_ordinals() const {
  std::vector<int> v;
  for (int i = 0; i < n_trades; ++i) v.push_back(pv(i));
  return v;
}

std::vector<int> StageALayout::aggregate_ordinals() const {
  std::vector<int> v;
  for (int c = 0; c < n_currencies; ++c) v.push_back(currency(c));
  for (int n = 0; n < n_netting_sets; ++n) v.push_back(netting(n));
  v.push_back(book);
  return v;
}

std::string StageARecordStats::to_string() const {
  std::ostringstream os;
  os << std::fixed << std::setprecision(3) << "build " << seconds_build << " s; dependencies " << seconds_dependencies << " s; calibrate " << seconds_calibrate
     << " s; book " << seconds_book << " s; passes " << seconds_passes << " s; export " << seconds_export << " s; record total " << seconds_total
     << " s; nodes raw " << nodes_raw << " -> after passes " << nodes_after_passes << "; df memo entries " << df_memo_hits << "; inputs " << n_inputs
     << ", outputs " << n_outputs;
  return os.str();
}

std::vector<std::vector<int>> StageATape::output_groups() const {
  std::vector<std::vector<int>> g(3);
  for (const solver::ImplicitBlock& b : registry.blocks) {
    g[0].insert(g[0].end(), b.residuals.begin(), b.residuals.end());
    g[0].push_back(b.diag_jtr_output);
    g[0].push_back(b.diag_iterations_output);
  }
  for (const std::vector<int>& k : layout.knots) g[1].insert(g[1].end(), k.begin(), k.end());
  for (int i = 0; i < layout.n_trades; ++i) {
    g[2].push_back(layout.pv(i));
    g[2].push_back(layout.pv_usd(i));
    g[2].push_back(layout.leg0(i));
    g[2].push_back(layout.leg1(i));
  }
  for (int o : layout.aggregate_ordinals()) g[2].push_back(o);
  if (layout.selects.n() > 0) {
    std::vector<int> e;
    for (const SelectExport& x : layout.selects.selects) {
      e.push_back(x.mask);
      e.push_back(x.margin);
      e.push_back(x.gap);
    }
    g.push_back(std::move(e));
  }
  return g;
}

std::vector<std::vector<int>> StageATape::sharing_groups() const {
  std::vector<std::vector<int>> g;
  std::vector<int> residuals;
  for (const solver::ImplicitBlock& b : registry.blocks) residuals.insert(residuals.end(), b.residuals.begin(), b.residuals.end());
  g.push_back(std::move(residuals));
  g.push_back(output_groups()[2]);
  return g;
}

std::vector<double> StageATape::record_quotes() const {
  const std::vector<double> all = tape.input_values();
  std::vector<double> q;
  for (int o : quote_inputs) q.push_back(all[static_cast<std::size_t>(o)]);
  return q;
}

// ---- the recording -----------------------------------------------------------------------------

namespace {

std::unique_ptr<solver::CurveSet> make_curve_set(const StageA& s) {
  auto set = std::make_unique<solver::CurveSet>();
  for (const CalibrationSet& cs : s.sets) add_calibration_set(*set, cs, start_values(cs, s.options.flat_start));
  return set;
}

}  // namespace

StageABook<double> price_stage_a_at(const StageA& s, const solver::CurveSet& set, const std::vector<std::vector<double>>& z) {
  std::vector<double> all;
  for (const std::vector<double>& v : z) all.insert(all.end(), v.begin(), v.end());
  const solver::CurveStates<double> st = set.states_at(all);
  DfMemo<double, std::function<double(int, double)>> memo([&st](int slot, double t) { return st.df(slot, t); });
  return price_stage_a<double>(s, memo);
}

StageATape record_stage_a(const StageA& s, bool passes) {
  StageATape r;
  const clock_type::time_point t_start = clock_type::now();
  r.set = make_curve_set(s);
  const std::vector<std::vector<int>> blocks = r.set->blocks(s.options.mode);   // dependency discovery (scratch recordings)
  r.stats.seconds_dependencies = seconds_since(t_start);
  solver::CurveSet::Calibration cal;
  std::size_t memo_entries = 0;
  {
    Tape::Scope scope(r.tape);
    std::vector<Rec> q;
    for (double v : s.quotes) {
      r.quote_inputs.push_back(static_cast<int>(r.tape.num_inputs()));
      q.push_back(make_input(r.tape, v));
    }
    const clock_type::time_point t_cal = clock_type::now();
    solver::SolveOptions solve;
    solve.tol = s.options.solve_tol;
    cal = r.set->calibrate(r.tape, r.registry, q, s.options.mode, solve);
    r.stats.seconds_calibrate = seconds_since(t_cal);
    r.block_curves = cal.block_curves;
    for (const solver::ImplicitResult& res : cal.results) r.record_reports.push_back(res.report);
    // O1: the knot values of every curve, in its regions' variables.
    const clock_type::time_point t_book = clock_type::now();
    r.layout.knots.resize(s.sets.size());
    r.record_knots.resize(s.sets.size());
    for (int c = 0; c < s.n_curves(); ++c) {
      for (const Rec& zk : cal.states.curve(c)) {
        r.layout.knots[static_cast<std::size_t>(c)].push_back(register_output(r.tape, zk));
        r.record_knots[static_cast<std::size_t>(c)].push_back(zk.v);
      }
    }
    // O2: the book off the calibrated Recs, discount factors memoised per (slot, t).
    DfMemo<Rec, std::function<Rec(int, double)>> memo([&cal](int slot, double t) { return cal.states.df(slot, t); });
    const StageABook<Rec> b = price_stage_a<Rec>(s, memo);
    memo_entries = memo.size();
    r.layout.n_trades = s.n_trades();
    r.layout.n_currencies = s.n_currencies();
    r.layout.n_netting_sets = s.n_netting_sets();
    r.layout.pv0 = static_cast<int>(r.tape.num_outputs());
    for (const Rec& x : b.pv) register_output(r.tape, x);
    r.layout.pv_usd0 = static_cast<int>(r.tape.num_outputs());
    for (const Rec& x : b.pv_usd) register_output(r.tape, x);
    r.layout.leg0_0 = static_cast<int>(r.tape.num_outputs());
    for (const Rec& x : b.leg0) register_output(r.tape, x);
    r.layout.leg1_0 = static_cast<int>(r.tape.num_outputs());
    for (const Rec& x : b.leg1) register_output(r.tape, x);
    r.layout.currency0 = static_cast<int>(r.tape.num_outputs());
    for (const Rec& x : b.currency_total) register_output(r.tape, x);
    r.layout.netting0 = static_cast<int>(r.tape.num_outputs());
    for (const Rec& x : b.netting_total) register_output(r.tape, x);
    r.layout.book = register_output(r.tape, b.book_total);
    r.stats.seconds_book = seconds_since(t_book);
  }
  r.tape.validate();
  r.stats.nodes_raw = r.tape.size();
  r.stats.df_memo_hits = memo_entries;
  if (passes) {
    const clock_type::time_point t_p = clock_type::now();
    standard_passes(r.tape);
    r.tape.validate();
    r.stats.seconds_passes = seconds_since(t_p);
  }
  r.stats.nodes_after_passes = r.tape.size();
  if (s.options.export_selects) {
    const clock_type::time_point t_e = clock_type::now();
    r.layout.selects = export_selects(r.tape);
    r.stats.seconds_export = seconds_since(t_e);
  }
  r.layout.n_outputs = static_cast<int>(r.tape.num_outputs());
  r.stats.n_inputs = r.tape.num_inputs();
  r.stats.n_outputs = r.tape.num_outputs();
  r.stats.seconds_total = seconds_since(t_start);
  r.record_book = price_stage_a_at(s, *r.set, r.record_knots);
  return r;
}

// ---- running -----------------------------------------------------------------------------------

solver::ProgramOptions stage_a_program_options(const StageA& s, int max_batch) {
  solver::ProgramOptions o;
  o.max_batch = max_batch;
  o.interpreter.max_batch = max_batch;
  o.adjoint.max_batch = max_batch;
  o.jacobian = static_cast<int>(s.def.scenarios.jacobian == "chord" ? solver::JacobianPolicy::chord : solver::JacobianPolicy::per_iteration);
  o.warm_start = s.def.scenarios.warm_start;
  return o;
}

std::vector<double> run_lanes(solver::ImplicitProgram& program, const std::vector<std::vector<double>>& lanes) {
  const int n_q = program.n_state(), n_out = program.n_outputs();
  const std::size_t B_all = lanes.size();
  std::vector<double> out(static_cast<std::size_t>(n_out) * B_all);
  const int chunk = program.max_batch();
  std::vector<double> st, o;
  for (std::size_t b0 = 0; b0 < B_all; b0 += static_cast<std::size_t>(chunk)) {
    const std::size_t B = std::min(static_cast<std::size_t>(chunk), B_all - b0);
    st.assign(static_cast<std::size_t>(n_q) * B, 0.0);
    o.assign(static_cast<std::size_t>(n_out) * B, 0.0);
    for (std::size_t b = 0; b < B; ++b) {
      const std::vector<double>& q = lanes[b0 + b];
      if (static_cast<int>(q.size()) != n_q) throw std::invalid_argument("run_lanes: a lane has " + std::to_string(q.size()) + " quotes for " + std::to_string(n_q));
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_q); ++k) st[k * B + b] = q[k];
    }
    program.run(st.data(), static_cast<int>(B), o.data());
    for (std::size_t oo = 0; oo < static_cast<std::size_t>(n_out); ++oo) {
      for (std::size_t b = 0; b < B; ++b) out[oo * B_all + b0 + b] = o[oo * B + b];
    }
  }
  return out;
}

std::vector<double> ladder(solver::ImplicitProgram& program, const std::vector<double>& quotes, const std::vector<int>& ordinals, std::vector<double>* out) {
  const int n_q = program.n_state(), n_out = program.n_outputs();
  if (static_cast<int>(quotes.size()) != n_q) throw std::invalid_argument("ladder: " + std::to_string(quotes.size()) + " quotes for " + std::to_string(n_q));
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
    sb.assign(static_cast<std::size_t>(n_q) * B, 0.0);
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_q); ++k) {
      for (std::size_t b = 0; b < B; ++b) st[k * B + b] = quotes[k];
    }
    for (std::size_t b = 0; b < B; ++b) ob[static_cast<std::size_t>(ordinals[r0 + b]) * B + b] = 1.0;
    program.adjoint(st.data(), static_cast<int>(B), ob.data(), o.data(), sb.data());
    for (std::size_t b = 0; b < B; ++b) {
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_q); ++k) rows[(r0 + b) * static_cast<std::size_t>(n_q) + k] = sb[k * B + b];
    }
    if (out != nullptr && r0 == 0) {
      for (std::size_t oo = 0; oo < static_cast<std::size_t>(n_out); ++oo) (*out)[oo] = o[oo * B];
    }
  }
  return rows;
}

std::vector<double> ladder_per_bp(const StageA& s, const std::vector<double>& rows) {
  const std::size_t n_q = static_cast<std::size_t>(s.n_quotes());
  if (n_q == 0 || rows.size() % n_q != 0) throw std::invalid_argument("ladder_per_bp: rows are not a multiple of n_quotes");
  std::vector<double> out(rows.size());
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const std::size_t k = i % n_q;
    out[i] = rows[i] * (s.quote_is_price[k] ? -0.01 : 1e-4);
  }
  return out;
}

// ---- structure ---------------------------------------------------------------------------------

StageAStructure stage_a_structure(const ir::Program& p) {
  StageAStructure s;
  s.domains = p.domains.size();
  s.values = p.num_values();
  s.literals = p.literals.size();
  for (const ir::Column& c : p.columns) s.columns += c.values.size();
  for (const ir::Gather& g : p.gathers) s.gathers += g.index.size();
  for (const ir::Segment& g : p.segments) s.segments += g.members.size();
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    const ir::Domain& dom = p.domains[d];
    if (dom.scan >= 0) {
      ++s.scan_domains;
      s.chains += static_cast<std::size_t>(p.scans[static_cast<std::size_t>(dom.scan)].chains());
      s.scan_rows += static_cast<std::size_t>(dom.rows);
    }
    const Op last = p.groups[d].steps.back().op;
    if (last == Op::Exp) ++s.df_domains;
    if (last == Op::Select) ++s.select_domains;
  }
  return s;
}

std::string StageAStructure::to_string() const {
  std::ostringstream os;
  os << "domains " << domains << " (exp-ended " << df_domains << ", select-ended " << select_domains << "), scan domains " << scan_domains << " with " << chains
     << " chains over " << scan_rows << " rows; values " << values << "; columns " << columns << ", gathers " << gathers << ", segment members " << segments
     << ", literals " << literals;
  return os.str();
}

// ---- dump --------------------------------------------------------------------------------------

std::vector<std::string> stage_a_dump(const StageA& s, const StageATape* tape, const std::string& prefix) {
  std::vector<std::string> files;
  auto open = [&](const std::string& name) {
    const std::string path = prefix + name;
    std::ofstream f(path);
    if (!f) throw std::runtime_error("stage_a_dump: cannot write " + path);
    f << std::setprecision(17);
    files.push_back(path);
    return f;
  };
  {
    std::ofstream f = open("trades.csv");
    f << "index,id,blueprint,kind,currency,side,notional,effective,termination,tenor,fixed_rate,spread,seasoned,age_days,moneyness,netting_set,coupons,obs_days,accrued_leg0,accrued_leg1\n";
    for (std::size_t i = 0; i < s.trades.size(); ++i) {
      const StageATrade& t = s.trades[i];
      const Instrument& in = s.instruments[i];
      f << t.index << ',' << in.id << ',' << t.blueprint << ',' << to_string(in.kind) << ',' << in.currency << ',' << in.side << ',' << in.notional << ','
        << to_iso(in.effective) << ',' << to_iso(in.termination) << ',' << (t.trade.tenor ? t.trade.tenor->to_string() : "") << ',' << in.fixed_rate << ','
        << in.spread << ',' << (t.seasoned ? 1 : 0) << ',' << t.age_days << ',' << t.moneyness << ',' << t.netting_set << ',' << in.n_coupons() << ','
        << in.n_obs_days() << ',' << (in.legs.size() > 0 ? leg_accrued(in.legs[0]) : 0.0) << ',' << (in.legs.size() > 1 ? leg_accrued(in.legs[1]) : 0.0) << '\n';
    }
  }
  {
    std::ofstream f = open("quotes.csv");
    f << "k,curve,key,t_maturity,is_price,quote,par_generating\n";
    auto df_gen = [&](int slot, double t) { return s.generating_df(slot, t); };
    std::vector<double> par;
    for (const CalibrationSet& cs : s.sets) {
      for (double q : par_quotes(cs, df_gen)) par.push_back(q);
    }
    for (std::size_t k = 0; k < s.quotes.size(); ++k) {
      f << k << ',' << s.sets[static_cast<std::size_t>(s.quote_curve[k])].curve << ',' << s.quote_keys[k] << ',' << s.quote_t[k] << ','
        << static_cast<int>(s.quote_is_price[k]) << ',' << s.quotes[k] << ',' << par[k] << '\n';
    }
  }
  {
    std::ofstream f = open("curves.csv");
    f << "slot,curve,definition,knot,t,region_scheme,region_variable,generating_zero,record_value\n";
    for (std::size_t c = 0; c < s.sets.size(); ++c) {
      const CalibrationSet& cs = s.sets[c];
      const curve::Composite comp = cs.composite();
      for (std::size_t k = 0; k < cs.knot_t.size(); ++k) {
        const double t = cs.knot_t[k];
        const curve::RegionSpec& rs = comp.region(comp.region_of(t)).spec;
        f << c << ',' << cs.curve << ',' << s.curve_definitions[c] << ',' << k << ',' << t << ',' << curve::to_string(rs.scheme) << ',' << curve::to_string(rs.variable)
          << ',' << s.generating[c][k] << ',' << (tape != nullptr ? tape->record_knots[c][k] : 0.0) << '\n';
      }
    }
  }
  {
    std::ofstream f = open("scenarios.csv");
    f << "lane,family,curve,size";
    for (std::size_t k = 0; k < s.quotes.size(); ++k) f << ",q" << k;
    f << '\n';
    for (std::size_t l = 0; l < s.scenarios.size(); ++l) {
      const StageAScenario& sc = s.scenarios[l];
      f << sc.lane << ',' << sc.family << ',' << sc.curve << ',' << sc.size;
      for (double q : s.scenario_quotes[l]) f << ',' << q;
      f << '\n';
    }
  }
  if (tape != nullptr) {
    std::ofstream f = open("record_point.csv");
    f << "row,kind,name,currency,value\n";
    const StageABook<double>& b = tape->record_book;
    for (std::size_t i = 0; i < s.trades.size(); ++i) {
      f << i << ",pv," << s.instruments[i].id << ',' << s.instruments[i].currency << ',' << b.pv[i] << '\n';
      f << i << ",pv_usd," << s.instruments[i].id << ',' << s.def.reporting_currency << ',' << b.pv_usd[i] << '\n';
      f << i << ",leg0," << s.instruments[i].id << ',' << s.instruments[i].currency << ',' << b.leg0[i] << '\n';
      f << i << ",leg1," << s.instruments[i].id << ',' << s.instruments[i].currency << ',' << b.leg1[i] << '\n';
    }
    for (std::size_t c = 0; c < b.currency_total.size(); ++c) f << c << ",currency_total," << s.def.currencies[c] << ',' << s.def.currencies[c] << ',' << b.currency_total[c] << '\n';
    for (std::size_t n = 0; n < b.netting_total.size(); ++n) f << n << ",netting_total,set " << n << ',' << s.def.reporting_currency << ',' << b.netting_total[n] << '\n';
    f << "0,book_total,book," << s.def.reporting_currency << ',' << b.book_total << '\n';
    for (std::size_t k = 0; k < tape->record_reports.size(); ++k) {
      f << k << ",block_report,block " << k << ",," << '"' << solver::to_string(tape->record_reports[k]) << '"' << '\n';
    }
  }
  return files;
}

}  // namespace epykos::fixtures
