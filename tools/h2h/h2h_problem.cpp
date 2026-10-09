// The named problems, built. Read h2h_problem.hpp's header comment first.
#include "h2h_problem.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

#include "epykos/maths/instrument/blueprint.hpp"
#include "epykos/solver/curve_set.hpp"

namespace h2h {

namespace {

namespace ei = epykos::instrument;

[[noreturn]] void bad(const std::string& what) { throw std::runtime_error("h2h problem: " + what); }

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void apply_lanes(solver::ProgramOptions& o, int max_batch, int lane_tile) {
  o.max_batch = max_batch;
  o.interpreter.max_batch = max_batch;
  o.interpreter.lane_tile = lane_tile;
  o.adjoint.max_batch = max_batch;
  o.adjoint.lane_tile = lane_tile;
}

}  // namespace

OurProblem::~OurProblem() = default;

OurProblem::OurProblem(const ProblemOptions& options) : options_(options) {
  if (options_.name == "compare_ois") {
    build_compare_ois();
  } else {
    build_stage_a();
  }
}

void OurProblem::build_compare_ois() {
  fixtures::CompareOisOptions o;
  o.trades = options_.book ? (options_.trades < 0 ? 1000 : options_.trades) : 0;
  o.seed = options_.seed;
  o.tenors = options_.tenors;
  o.max_start_offset_years = options_.start_years;
  o.max_batch = options_.max_batch;
  if (!options_.family.empty() && options_.family != "USD-SOFR-OIS")
    bad("compare_ois has one product family, 'USD-SOFR-OIS'; asked for '" + options_.family + "'");

  const double t0 = now_s();
  ois_ = std::make_unique<fixtures::CompareOis>(fixtures::make_compare_ois(o));
  ois_tape_ = std::make_unique<fixtures::CompareOisTape>(fixtures::record_compare_ois(*ois_));
  seconds_record_ = now_s() - t0;
  nodes_raw_ = ois_tape_->nodes_raw;
  nodes_after_ = ois_tape_->nodes_after_passes;
  set_ = ois_tape_->set.get();
  knot_outputs_.push_back(ois_tape_->knot_outputs);
  pv_ordinals_ = ois_tape_->pv_outputs;
  book_output_ = ois_tape_->book_output;

  view_.label = "compare_ois";
  CurveView cv;
  cv.definition = ois_->definition.name;
  cv.index = ois_->definition.index;
  cv.currency = ois_->definition.currency;
  cv.knot_t = ois_->set.knot_t;
  view_.curves.push_back(std::move(cv));
  for (const ei::CalibrationInstrument& ci : ois_->set.instruments) {
    view_.cal.push_back(&ci.instrument);
    view_.cal_slot.push_back(0);
  }
  view_.quotes = ois_->quotes;
  view_.quote_is_price.assign(view_.quotes.size(), 0);
  for (const ei::Instrument& in : ois_->book) {
    view_.book.push_back(&in);
    view_.family.push_back("USD-SOFR-OIS");
  }

  solver::ProgramOptions po = fixtures::compare_ois_program_options(*ois_);
  apply_lanes(po, options_.max_batch, options_.lane_tile);
  po.warm_start = true;
  po.jacobian = static_cast<int>(solver::JacobianPolicy::chord);
  full_ = std::make_unique<solver::ImplicitProgram>(ois_tape_->tape, ois_tape_->registry, po);
  if (!options_.book) {
    cal_hot_ = std::make_unique<solver::ImplicitProgram>(ois_tape_->tape, ois_tape_->registry, po);
    po.warm_start = false;
    po.jacobian = -1;
    cal_cold_ = std::make_unique<solver::ImplicitProgram>(ois_tape_->tape, ois_tape_->registry, po);
  }
}

void OurProblem::build_stage_a() {
  fixtures::StageAOptions o;
  o.path = options_.problem_path.empty()
               ? ei::Blueprints::default_root() + "/problems/" + options_.name + ".json"
               : options_.problem_path;
  o.trades = options_.trades;
  o.scenarios = 0;   // O4 is not one of the four compared phases; do not draw 1,000 lanes for nothing

  const double t0 = now_s();
  stage_ = std::make_unique<fixtures::StageA>(fixtures::make_stage_a(o));
  fixtures::StageA& s = *stage_;

  // The book, restricted. A per-family run is its OWN problem -- own tape, own gate, own timings --
  // so the restriction happens HERE, before anything is recorded, and never as a slice of an
  // aggregate afterwards. The draws are untouched: trade i keeps the blueprint, tenor, notional,
  // side, seasoning, netting set and moneyness it drew from Philox sub-stream book + i, so a
  // family's trades are bit-for-bit the ones the full book carries.
  const bool all = options_.family.empty();
  if (!options_.book || !all) {
    std::vector<ei::Instrument> keep_in;
    std::vector<fixtures::StageATrade> keep_tr;
    if (options_.book) {
      for (std::size_t i = 0; i < s.trades.size(); ++i) {
        if (s.trades[i].blueprint != options_.family) continue;
        keep_in.push_back(s.instruments[i]);
        keep_tr.push_back(s.trades[i]);
      }
      if (keep_in.empty()) {
        std::string known;
        for (const fixtures::StageADefinition::Weighted& w : s.def.book.mix) known += (known.empty() ? "" : ", ") + w.name;
        bad("no trade of '" + o.path + "' has blueprint '" + options_.family + "' (the mix is: " + known + ")");
      }
    }
    s.instruments = std::move(keep_in);
    s.trades = std::move(keep_tr);
  }

  stage_tape_ = std::make_unique<fixtures::StageATape>(fixtures::record_stage_a(s));
  seconds_record_ = now_s() - t0;
  nodes_raw_ = stage_tape_->stats.nodes_raw;
  nodes_after_ = stage_tape_->stats.nodes_after_passes;
  set_ = stage_tape_->set.get();
  knot_outputs_ = stage_tape_->layout.knots;
  // pv_usd: the per-trade PV in the reporting currency, which is what the book total sums and so
  // what their book NPV is comparable with. Stage A has no FX, so for a USD trade this IS pv and
  // for a EUR trade it is pv x the placeholder 1.0 (a recorded product).
  for (int i = 0; i < stage_tape_->layout.n_trades; ++i) pv_ordinals_.push_back(stage_tape_->layout.pv_usd(i));
  book_output_ = stage_tape_->layout.book;

  view_.label = s.def.name + (all ? std::string() : "/" + options_.family);
  for (int c = 0; c < s.n_curves(); ++c) {
    const ei::CalibrationSet& cs = s.sets[static_cast<std::size_t>(c)];
    CurveView cv;
    cv.definition = cs.curve;
    cv.index = cs.index;
    cv.currency = s.blueprints.curve(cs.curve).currency;
    cv.knot_t = cs.knot_t;
    view_.curves.push_back(std::move(cv));
    for (const ei::CalibrationInstrument& ci : cs.instruments) {
      view_.cal.push_back(&ci.instrument);
      view_.cal_slot.push_back(c);
    }
  }
  view_.quotes = s.quotes;
  view_.quote_is_price = s.quote_is_price;
  if (view_.cal.size() != view_.quotes.size())
    bad("the calibration instruments (" + std::to_string(view_.cal.size()) + ") and the quotes (" +
        std::to_string(view_.quotes.size()) + ") are not in step -- the ladder columns would not align");
  for (std::size_t i = 0; i < s.instruments.size(); ++i) {
    view_.book.push_back(&s.instruments[i]);
    view_.family.push_back(s.trades[i].blueprint);
  }

  solver::ProgramOptions po = fixtures::stage_a_program_options(s, options_.max_batch);
  apply_lanes(po, options_.max_batch, options_.lane_tile);
  po.warm_start = true;
  po.jacobian = static_cast<int>(solver::JacobianPolicy::chord);
  full_ = std::make_unique<solver::ImplicitProgram>(stage_tape_->tape, stage_tape_->registry, po);
  if (!options_.book) {
    cal_hot_ = std::make_unique<solver::ImplicitProgram>(stage_tape_->tape, stage_tape_->registry, po);
    po.warm_start = false;
    po.jacobian = -1;
    cal_cold_ = std::make_unique<solver::ImplicitProgram>(stage_tape_->tape, stage_tape_->registry, po);
  }
}

std::vector<Family> families_of(const ProblemView& v) {
  std::vector<Family> out;
  for (const std::string& f : v.family) {
    auto it = std::find_if(out.begin(), out.end(), [&](const Family& x) { return x.name == f; });
    if (it == out.end())
      out.push_back(Family{f, 1});
    else
      ++it->trades;
  }
  return out;
}

int OurProblem::n_knots() const noexcept {
  int n = 0;
  for (const CurveView& c : view_.curves) n += static_cast<int>(c.knot_t.size());
  return n;
}

std::vector<double> OurProblem::ladder(const std::vector<double>& quotes, const std::vector<int>& ordinals,
                                       std::vector<double>* out) const {
  if (ois_tape_ != nullptr) return fixtures::compare_ois_ladder(*full_, quotes, ordinals, out);
  return fixtures::ladder(*full_, quotes, ordinals, out);
}

std::vector<std::vector<double>> OurProblem::discount_factors(
    const double* out, const std::vector<std::vector<double>>& times) const {
  if (times.size() != knot_outputs_.size())
    bad("discount_factors: " + std::to_string(times.size()) + " time grids for " +
        std::to_string(knot_outputs_.size()) + " curves");
  solver::CurveStates<double> st;
  st.specs = &set_->curves();
  st.resize(knot_outputs_.size());
  for (std::size_t c = 0; c < knot_outputs_.size(); ++c) {
    std::vector<double> z;
    for (int ord : knot_outputs_[c]) z.push_back(out[ord]);
    st.set(static_cast<int>(c), z);
  }
  std::vector<std::vector<double>> df(knot_outputs_.size());
  for (std::size_t c = 0; c < knot_outputs_.size(); ++c)
    for (double t : times[c]) df[c].push_back(st.df(static_cast<int>(c), t));
  return df;
}

}  // namespace h2h
