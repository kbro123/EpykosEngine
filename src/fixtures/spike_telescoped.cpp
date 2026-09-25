// SPIKE, MEASUREMENT ONLY — see the header for what this is and why it must not become
// production maths (`include/epykos/fixtures/spike_telescoped.hpp`; PRINCIPLES.md §2).
//
// Test-only (D28). NOT an E0 TU, deliberately: `CLAUDE.md` and PRINCIPLES.md §4a retire the
// `-ffp-contract=off` pinning and say in terms "do not add new files to that convention", so this
// one is compiled with the preset's own flags although `src/fixtures/compare_ois_e0.cpp`, the file
// it parallels, is pinned. The consequence is stated rather than hidden:
//
//   * every COUNT this file produces is structural — tape nodes, IR domains, literals, columns,
//     gathers, segments, scan rows — and PRINCIPLES.md §4's structural tier is exact because a
//     graph identity does not round. Contraction cannot move any of them. In particular the tape
//     the recording produces does not depend on how many Newton steps the record-time solve takes:
//     an implicit block is ONE node and its residual sub-program is recorded once.
//   * every VALUE it produces may differ in the last bits from the same value computed in the
//     pinned TU, so `tests/spike/telescoping_prize_test.cpp` compares those by tolerance and not
//     bitwise, which is what PRINCIPLES.md §4 asks for anyway.
//   * the ACCURACY figures the test reports are measured with both coupon forms instantiated in
//     one translation unit under one set of flags, so the comparison between the forms is fair;
//     the absolute error of either form is a property of those flags and is quoted with them.
#include "epykos/fixtures/spike_telescoped.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

#include "epykos/ir/program.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "epykos/tape/passes.hpp"

namespace epykos::fixtures::spike {

using namespace instrument;

namespace {

using clock_type = std::chrono::steady_clock;
double seconds_since(clock_type::time_point t0) { return std::chrono::duration<double>(clock_type::now() - t0).count(); }

// The discount-factor memo of `src/fixtures/compare_ois_e0.cpp`, repeated verbatim so that the
// two recordings share every part of the harness except the coupon form.
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

 private:
  Inner inner_;
  std::unordered_map<std::uint64_t, Scalar> table_;
};

std::string fixed2(double v) {
  std::ostringstream s;
  s << std::fixed << std::setprecision(3) << v;
  return s.str();
}

// n as a ratio against d, "--" when d is zero.
std::string ratio(double n, double d) {
  if (d == 0.0) return "--";
  std::ostringstream s;
  s << std::fixed << std::setprecision(3) << (n / d) << "x";
  return s.str();
}

std::string with_commas(std::size_t v) {
  std::string s = std::to_string(v);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
  return s;
}

}  // namespace

const char* to_string(Form f) noexcept { return f == Form::telescoped ? "telescoped" : "naive"; }

bool telescopes(const Leg& leg, const Coupon& c) noexcept {
  if (c.kind != CouponKind::RfrCompounded) return false;
  if (c.obs_end <= c.obs_begin) return false;
  for (int i = c.obs_begin; i < c.obs_end; ++i) {
    const ObsDay& d = leg.obs[static_cast<std::size_t>(i)];
    if (d.weight != d.tau_rate) return false;
    if (i + 1 < c.obs_end && leg.obs[static_cast<std::size_t>(i + 1)].t_rate != d.t_next) return false;
  }
  return true;
}

bool telescopes(const Instrument& in) noexcept {
  bool any = false;
  for (const Leg& leg : in.legs) {
    for (const Coupon& c : leg.coupons) {
      if (c.kind != CouponKind::RfrCompounded) continue;
      any = true;
      if (!telescopes(leg, c)) return false;
    }
  }
  return any;
}

int add_calibration_set_spike(solver::CurveSet& set, const CalibrationSet& cs, std::span<const double> start, Form form) {
  if (form == Form::naive) return add_calibration_set(set, cs, start);
  if (set.n_curves() != cs.slot) {
    throw std::invalid_argument("add_calibration_set_spike: curve '" + cs.curve + "' has slot " + std::to_string(cs.slot) +
                                " but the set's next curve index is " + std::to_string(set.n_curves()));
  }
  if (start.size() != cs.knot_t.size()) {
    throw std::invalid_argument("add_calibration_set_spike: curve '" + cs.curve + "': " + std::to_string(start.size()) +
                                " start values for " + std::to_string(cs.knot_t.size()) + " knots");
  }
  solver::CurveSpec spec;
  spec.name = cs.curve;
  spec.knot_t = cs.knot_t;
  spec.start.assign(start.begin(), start.end());
  spec.regions = cs.regions;
  const int c = set.add_curve(spec);
  for (const CalibrationInstrument& ci : cs.instruments) {
    const Instrument inst = ci.instrument;
    set.add_instrument(c, ci.key, [inst](const auto& states, const auto& q) {
      using Scalar = std::decay_t<decltype(q)>;
      return spike_residual<Scalar>(inst, [&states](int slot, double t) { return states.df(slot, t); }, q);
    });
  }
  return c;
}

CompareOisTape record_compare_ois_spike(const CompareOis& s, Form form, bool passes) {
  const clock_type::time_point t0 = clock_type::now();
  CompareOisTape r;
  r.set = std::make_unique<solver::CurveSet>();
  const std::vector<double> start = start_values(s.set, s.options.flat_start);
  add_calibration_set_spike(*r.set, s.set, start, form);
  {
    Tape::Scope scope(r.tape);
    std::vector<Rec> q;
    for (double v : s.quotes) {
      r.quote_inputs.push_back(static_cast<int>(r.tape.num_inputs()));
      q.push_back(make_input(r.tape, v));
    }
    solver::SolveOptions solve;
    solve.tol = s.options.solve_tol;
    const solver::CurveSet::Calibration cal =
        r.set->calibrate(r.tape, r.registry, q, solver::CurveSet::Mode::sequential, solve);
    if (cal.results.size() != 1u) throw std::logic_error("record_compare_ois_spike: expected one implicit block");
    r.record_report = cal.results[0].report;
    for (const Rec& zk : cal.states.curve(0)) {
      r.knot_outputs.push_back(register_output(r.tape, zk));
      r.record_knots.push_back(zk.v);
    }
    Memo<Rec, std::function<Rec(int, double)>> memo([&cal](int slot, double t) { return cal.states.df(slot, t); });
    Rec total = Rec(0.0);
    for (const Instrument& in : s.book) {
      const Rec p = form == Form::telescoped ? spike_pv<Rec>(in, memo) : pv<Rec>(in, memo);
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

FormCounts count_compare_ois_spike(const CompareOis& s, Form form) {
  FormCounts f;
  f.form = form;
  f.trades = s.n_trades();
  f.quotes = s.n_quotes();
  for (const CalibrationInstrument& ci : s.set.instruments) {
    f.obs_days_calibration += ci.instrument.n_obs_days();
    f.coupons_calibration += ci.instrument.n_coupons();
  }
  for (const Instrument& in : s.book) {
    f.obs_days_book += in.n_obs_days();
    f.coupons_book += in.n_coupons();
  }
  const CompareOisTape t = record_compare_ois_spike(s, form);
  f.tape_nodes_raw = t.nodes_raw;
  f.tape_nodes_after_passes = t.nodes_after_passes;
  f.seconds_record = t.seconds_record;
  f.converged = t.record_report.converged;
  f.jtr_inf = t.record_report.jtr_inf;
  f.book_pv = t.record_book;

  solver::ImplicitProgram prog(t.tape, t.registry, compare_ois_program_options(s));
  const ir::Program& p = prog.program();
  f.ir_values = p.num_values();
  f.ir_domains = p.domains.size();
  f.ir_literals = p.literals.size();
  f.ir_columns = p.columns.size();
  for (const ir::Column& c : p.columns) f.ir_column_rows += c.values.size();
  f.ir_gathers = p.gathers.size();
  for (const ir::Gather& g : p.gathers) f.ir_gather_rows += g.index.size();
  f.ir_segments = p.segments.size();
  for (const ir::Segment& sg : p.segments) f.ir_segment_members += sg.members.size();
  for (const ir::domain_id d : ir::scan_domains(p)) {
    ++f.ir_scan_domains;
    f.ir_scan_rows += static_cast<std::size_t>(p.domains[static_cast<std::size_t>(d)].rows);
  }
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    f.ir_steps += p.groups[d].steps.size() * static_cast<std::size_t>(p.domains[d].rows);
  }
  for (int k = 0; k < prog.n_blocks(); ++k) {
    const ir::Program& rp = prog.residual_program(k).program();
    f.residual_values += rp.num_values();
    f.residual_domains += rp.domains.size();
    for (std::size_t d = 0; d < rp.domains.size(); ++d) {
      f.residual_steps += rp.groups[d].steps.size() * static_cast<std::size_t>(rp.domains[d].rows);
    }
    for (const ir::domain_id d : ir::scan_domains(rp)) {
      f.residual_scan_rows += static_cast<std::size_t>(rp.domains[static_cast<std::size_t>(d)].rows);
    }
  }
  return f;
}

std::string FormCounts::to_string() const {
  std::ostringstream o;
  o << spike::to_string(form) << ": " << trades << " trades, " << quotes << " quotes, "
    << with_commas(static_cast<std::size_t>(obs_days_calibration)) << " calibration observation days, "
    << with_commas(tape_nodes_raw) << " tape nodes recorded, " << with_commas(tape_nodes_after_passes)
    << " after the passes, " << with_commas(ir_values) << " IR values in " << with_commas(ir_domains) << " domains";
  return o.str();
}

std::string compare_forms(const FormCounts& n, const FormCounts& t) {
  std::ostringstream o;
  auto row = [&o](const char* what, double a, double b) {
    o << "  " << std::left << std::setw(34) << what << std::right << std::setw(14) << with_commas(static_cast<std::size_t>(a))
      << std::setw(14) << with_commas(static_cast<std::size_t>(b)) << std::setw(12) << ratio(a, b) << "\n";
  };
  o << "[spike/telescoping] compare_ois, " << n.trades << " book trades against " << n.quotes
    << " calibration instruments (D64)\n";
  o << "  " << std::left << std::setw(34) << "" << std::right << std::setw(14) << "naive" << std::setw(14) << "telescoped"
    << std::setw(12) << "naive/tel" << "\n";
  row("observation days (calibration)", n.obs_days_calibration, t.obs_days_calibration);
  row("observation days (book)", n.obs_days_book, t.obs_days_book);
  row("coupon periods (calibration)", n.coupons_calibration, t.coupons_calibration);
  row("tape nodes recorded", static_cast<double>(n.tape_nodes_raw), static_cast<double>(t.tape_nodes_raw));
  row("tape nodes after passes", static_cast<double>(n.tape_nodes_after_passes),
      static_cast<double>(t.tape_nodes_after_passes));
  row("IR values", static_cast<double>(n.ir_values), static_cast<double>(t.ir_values));
  row("IR domains", static_cast<double>(n.ir_domains), static_cast<double>(t.ir_domains));
  row("IR scan domains", static_cast<double>(n.ir_scan_domains), static_cast<double>(t.ir_scan_domains));
  row("IR scan rows", static_cast<double>(n.ir_scan_rows), static_cast<double>(t.ir_scan_rows));
  row("IR literals", static_cast<double>(n.ir_literals), static_cast<double>(t.ir_literals));
  row("IR columns", static_cast<double>(n.ir_columns), static_cast<double>(t.ir_columns));
  row("IR column rows", static_cast<double>(n.ir_column_rows), static_cast<double>(t.ir_column_rows));
  row("IR gathers", static_cast<double>(n.ir_gathers), static_cast<double>(t.ir_gathers));
  row("IR gather rows", static_cast<double>(n.ir_gather_rows), static_cast<double>(t.ir_gather_rows));
  row("IR segments", static_cast<double>(n.ir_segments), static_cast<double>(t.ir_segments));
  row("IR segment members", static_cast<double>(n.ir_segment_members), static_cast<double>(t.ir_segment_members));
  row("IR steps x rows", static_cast<double>(n.ir_steps), static_cast<double>(t.ir_steps));
  row("residual slice values", static_cast<double>(n.residual_values), static_cast<double>(t.residual_values));
  row("residual slice domains", static_cast<double>(n.residual_domains), static_cast<double>(t.residual_domains));
  row("residual slice steps x rows", static_cast<double>(n.residual_steps), static_cast<double>(t.residual_steps));
  row("residual slice scan rows", static_cast<double>(n.residual_scan_rows), static_cast<double>(t.residual_scan_rows));
  o << "  " << std::left << std::setw(34) << "record seconds" << std::right << std::setw(14) << fixed2(n.seconds_record)
    << std::setw(14) << fixed2(t.seconds_record) << std::setw(12) << ratio(n.seconds_record, t.seconds_record) << "\n";
  return o.str();
}

}  // namespace epykos::fixtures::spike
