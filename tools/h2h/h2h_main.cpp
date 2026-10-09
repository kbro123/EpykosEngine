// tools/h2h/ — the REPRESENTATIVE head-to-head: both engines, one process, one clock (D78).
//
// What this is and why it exists
// ------------------------------
// `bench/compare/` measures this engine through Google Benchmark and the other engine through its
// JSON CLI, one fresh process per sample. Two clocks, two processes, one of them cold: D77 could
// therefore only bound its own answer ("2.82x is an UPPER bound"). This binary removes that whole
// class of doubt. It links BOTH engines, builds the SAME problem for each, and times four phases
// with ONE `std::chrono::steady_clock`, interleaved round-robin so any drift lands on both sides.
//
// D78 relaxed D11/D21 for this directory alone; the owner widened that on 2026-10-09 to permit
// READING that checkout, so the exchange schema below is read off its own codec instead of probed
// from its CLI's error messages. Nothing is copied, ported or adapted into `include/epykos/` or
// `src/`, which stay clean-room -- that property is what makes this benchmark worth running at all.
// `bench/compare/README.md` §0 lists every file read.
//
// WHAT CHANGED ON 2026-10-09, and why
// -----------------------------------
// The harness compared ONE USD SOFR OIS curve and a book of plain spot-starting OIS, while
// `blueprints/problems/stage_a.json` is a problem this engine already prices with four curves, two
// currencies, deposits, futures, tenor basis, averaging swaps, lockouts and ~20% seasoned trades.
// So every number quoted against the other engine covered about a quarter of what we can price.
// Three things are now different:
//
//   1. The problem SHAPE is a parameter (`--problem`, `h2h_problem.hpp`), with Stage A as the
//      headline case and `compare_ois` retained verbatim so D90's numbers stay reproducible.
//   2. The exchange is TYPED and per coupon (`h2h_bridge.hpp`): our row tables become
//      `cal::BundleProblem` / `pf::MultiCurveBook` as C++ objects, so the compiler checks the
//      schema and a coupon the schema cannot carry is a loud failure, never a different price.
//      `--exchange` still writes a file, now through their own `bundle_to_json`.
//   3. The RISK LADDER'S ROW COUNT is a parameter and is printed with every ladder number. D103 §1
//      found this harness passed `ordinals = {book_output}` -- so D90's flagship 531.5 us is a
//      ONE-ROW ladder, which D92 had independently shown is the unrepresentative case (the exit
//      Jacobian is 15.8% of it and 0.5% of a 256-row one). A ladder figure without its row count
//      and its book size is not a measurement, and this binary will not print one.
//
// The four phases, and what makes each one like-for-like
// ------------------------------------------------------
//   calibrate_cold  ours: the calibration-only program (no book) solved from its flat start.
//                   theirs: BundleSession::calibrate(flat_x0) -- the LM cold solve.
//   calibrate_hot   ours: the same program, warm_start + the chord Jacobian policy, re-solved from
//                   the previous solution to a moved market.
//                   theirs: BundleSession::recalibrate(market) -- one frozen-Newton tick on the
//                   streaming engine (started, and its Jacobian paid for, in the warm-up).
//   price           ours: the whole-program forward pass at the solved state, MINUS the same pass
//                   on the calibration-only program. Both are measured; the difference is the
//                   book-attributable cost and is labelled as arithmetic on two measurements, not
//                   a measurement. Our tape prices the calibration instruments and the book in one
//                   program and cannot be asked for the book alone.
//                   theirs: BundleSession::price_portfolio(book) -- the book alone, so the
//                   subtraction is on OUR side and the asymmetry is stated, not hidden.
//   risk            ours: the O3 ladder d(out)/d(quote) through the adjoint and the IFT, over
//                   `--ladder-rows` rows. theirs: price_portfolio_risk(book).ladder is ONE
//                   book-level row per call, so R rows cost R calls on single-trade books and that
//                   is what is timed and said.
//
// Every phase is wall-clocked from the OUTSIDE on both sides. The other engine also self-reports
// `solve_micros` / `price_us` / `risk_us`, and those are printed beside our clock because they are
// not the same quantity: its `risk_us` deliberately excludes forming the risk operator M (the
// calibration Jacobian solve), where our ladder builds its Jacobian inside the timed region.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "swaps/api/bundle_api.hpp"
#include "swaps/api/codec.hpp"

#include "h2h_bridge.hpp"
#include "h2h_problem.hpp"

#include "epykos/fixtures/spike_telescoped.hpp"
#include "epykos/version.hpp"

namespace {

namespace cal = swaps::calibration;
namespace fixtures = epykos::fixtures;
namespace pf = swaps::portfolio;

using clock_type = std::chrono::steady_clock;

double us_since(clock_type::time_point t0) {
  return std::chrono::duration<double, std::micro>(clock_type::now() - t0).count();
}

[[noreturn]] void die(const std::string& m) {
  std::cerr << "h2h: " << m << "\n";
  std::exit(2);
}

double load_1min() {
  double la[3] = {0.0, 0.0, 0.0};
  if (getloadavg(la, 3) < 1) return -1.0;
  return la[0];
}

// One phase's samples, in microseconds, reduced the way `bench/run.sh` reduces a benchmark: the
// median is the headline and min / p90 say how tight the distribution is (D9 -- a single sample is
// what made D76 report 21x for a 42x gap).
struct Samples {
  std::vector<double> us;
  void add(double v) { us.push_back(v); }
  double q(double p) const {
    if (us.empty()) return 0.0;
    std::vector<double> v = us;
    std::sort(v.begin(), v.end());
    const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size())));
    return v[i];
  }
  double median() const { return q(0.5); }
  double min() const { return q(0.0); }
  double p90() const { return q(0.9); }
};

void row(const char* name, const Samples& ours, const Samples& theirs, const char* note) {
  const double a = ours.median(), c = theirs.median();
  char r[24];
  if (a > 0.0 && c > 0.0)
    std::snprintf(r, sizeof r, "%8.2fx", c / a);
  else
    std::snprintf(r, sizeof r, "%9s", "-");
  std::printf("  %-22s %11.1f %11.1f | %9s | %s\n", name, a, c, r, note);
}

// The worst relative disagreement of two vectors, scaled by `scale` (0 => by each pair's own size).
struct Worst {
  double rel = 0.0;
  int at = -1;
};
Worst worst_of(const std::vector<double>& a, const std::vector<double>& b, double scale) {
  if (a.size() != b.size()) die("comparing " + std::to_string(a.size()) + " values against " + std::to_string(b.size()));
  Worst w;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const double s = scale > 0.0 ? scale : std::max(std::max(std::abs(a[i]), std::abs(b[i])), 1.0);
    const double e = std::abs(a[i] - b[i]) / s;
    if (e > w.rel) {
      w.rel = e;
      w.at = static_cast<int>(i);
    }
  }
  return w;
}

double inf_norm(const std::vector<double>& v) {
  double m = 0.0;
  for (double x : v) m = std::max(m, std::abs(x));
  return m > 0.0 ? m : 1.0;
}

// The agreement tolerances. The SAME ones scripts/compare_swapengine.py gates on (D71), and they
// are never relaxed: a failure stops the timing of whatever failed.
struct Tolerance {
  double discount = 1e-12;
  double npv = 1e-10;
  double ladder = 1e-9;
};

// 200 sample times PER CURVE, each spanning that curve alone, up to and including its own last
// knot. Not one shared grid: the four Stage A curves' last knots are different adjusted dates, and
// a grid reaching the longest of them would sample the others past their last knot, where the two
// engines extrapolate differently by construction (README §5).
std::vector<std::vector<double>> sample_times(const h2h::ProblemView& v) {
  std::vector<std::vector<double>> out;
  for (const h2h::CurveView& c : v.curves) {
    const double t_max = c.knot_t.empty() ? 0.0 : c.knot_t.back();
    std::vector<double> ts;
    for (int i = 1; i <= 200; ++i) ts.push_back(t_max * static_cast<double>(i) / 200.0);
    out.push_back(std::move(ts));
  }
  return out;
}

struct FamilyResult {
  std::string name;
  int trades = 0, positions = 0;
  double agree_npv = 0.0, agree_ladder = 0.0;
  bool ok = false;
  double ours_price = 0.0, theirs_price = 0.0, theirs_bound = 0.0, ours_price_book = 0.0;
  double ours_risk = 0.0, theirs_risk = 0.0;
  int rows = 0;
  std::size_t nodes_raw = 0, nodes_after = 0;
  std::string note;
};

}  // namespace

int main(int argc, char** argv) {
  h2h::ProblemOptions po;
  int reps = 20, ladder_rows = 1;
  bool per_family = false, load_check = true, verify_spike = false, per_trade_on_failure = true;
  double max_load = -1.0;
  std::string exchange_path;
  Tolerance tol;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) die("missing value for " + a);
      return argv[++i];
    };
    if (a == "--problem") po.name = next();
    else if (a == "--problem-path") po.problem_path = next();
    else if (a == "--family") po.family = next();
    else if (a == "--per-family") per_family = true;
    else if (a == "--trades") po.trades = std::stoi(next());
    else if (a == "--reps") reps = std::stoi(next());
    else if (a == "--seed") po.seed = std::stoull(next());
    else if (a == "--start-years") po.start_years = std::stoi(next());
    else if (a == "--max-batch") po.max_batch = std::stoi(next());
    else if (a == "--lane-tile") po.lane_tile = std::stoi(next());
    else if (a == "--ladder-rows") ladder_rows = std::stoi(next());
    else if (a == "--exchange") exchange_path = next();
    else if (a == "--max-load") max_load = std::stod(next());
    else if (a == "--no-load-check") load_check = false;
    else if (a == "--verify-spike") verify_spike = true;
    else if (a == "--no-per-trade-bisect") per_trade_on_failure = false;
    else if (a == "--tenors") {
      const std::string list = next();
      for (std::size_t b = 0; b <= list.size();) {
        const std::size_t e = std::min(list.find(',', b), list.size());
        if (e > b) po.tenors.push_back(list.substr(b, e - b));
        b = e + 1;
      }
    } else if (a == "--help" || a == "-h") {
      std::printf(
          "h2h --problem <compare_ois|stage_a|stage_a_h2h|...> [--family <blueprint>] [--per-family]\n"
          "    [--trades N] [--tenors T,...] [--seed S] [--start-years Y] [--reps R]\n"
          "    [--ladder-rows R] [--max-batch B] [--lane-tile L] [--exchange <file>]\n"
          "    [--max-load X] [--no-load-check] [--verify-spike]\n");
      return 0;
    } else {
      die("unknown argument " + a);
    }
  }
  if (ladder_rows < 1) die("--ladder-rows must be >= 1");
  if (max_load < 0.0) max_load = static_cast<double>(std::max(1L, sysconf(_SC_NPROCESSORS_ONLN))) / 2.0;

  // ---- the box must be reserved (D9, bench/run.sh's rule, tools/ladder's self-check) -----------
  const double load_before = load_1min();
  if (load_check && load_before > max_load)
    die("the 1-minute load is " + std::to_string(load_before) + ", above cores/2 = " + std::to_string(max_load) +
        ". Nothing else may run during this measurement (D9). Wait, or pass --no-load-check and "
        "state the load beside every number you quote.");

  std::printf("EpykosEngine %s [%s] vs the other engine -- one process, one steady_clock, interleaved\n",
              epykos::version(), epykos::build_flags());
  std::printf("problem %s, max_batch %d, lane_tile %d, ladder rows %d, %d repetitions; 1-minute load before %.2f (bar %.2f)\n",
              po.name.c_str(), po.max_batch, po.lane_tile, ladder_rows, reps, load_before, max_load);

  // ---- the calibration-only twin: ONE recording, shared by every family -------------------------
  // The bundle never carries the book, so the calibration problem and both calibration phases are
  // family-independent and are built, gated and timed exactly once.
  std::cerr << "[h2h] recording the calibration-only program\n";
  h2h::ProblemOptions pc = po;
  pc.book = false;
  pc.family.clear();
  const h2h::OurProblem cal_only(pc);
  if (!cal_only.has_cal_twin()) die("the calibration twin was not built");

  h2h::Mapping bundle_map;
  const cal::BundleProblem prob = h2h::bundle_of(cal_only.view(), &bundle_map);
  const int n_q = cal_only.n_quotes();
  if (prob.n_residuals() != n_q)
    die("the two sides disagree on the quote count: theirs " + std::to_string(prob.n_residuals()) + ", ours " +
        std::to_string(n_q));
  if (prob.n_knots() != cal_only.n_knots())
    die("the two sides disagree on the knot count: theirs " + std::to_string(prob.n_knots()) + ", ours " +
        std::to_string(cal_only.n_knots()));
  std::printf("\n%d calibration instruments on %d knots over %d curves (%s)\n", n_q, prob.n_knots(),
              static_cast<int>(cal_only.view().curves.size()),
              [&] {
                std::string s;
                for (const h2h::CurveView& c : cal_only.view().curves) s += (s.empty() ? "" : ", ") + c.definition;
                return s;
              }()
                  .c_str());
  std::printf("exchange mapping: %s\n", bundle_map.to_string().c_str());

  const std::vector<double>& quotes = cal_only.view().quotes;
  const std::vector<double> dmarket = h2h::market_jacobian(cal_only.view());

  swaps::api::BundleSession sess_cal(prob);
  swaps::api::BundleSession sess_cal_hot(prob);
  const Eigen::VectorXd x0 = cal::flat_x0(prob);
  sess_cal.calibrate(x0);
  sess_cal_hot.calibrate(x0);

  // ---- gate 1: the CALIBRATED CURVE -------------------------------------------------------------
  // Before a single microsecond. D9 and PRINCIPLES.md §4: a timing comparison between two engines
  // that are not computing the same quantity is refused, not reported with a caveat.
  const std::vector<std::vector<double>> ts = sample_times(cal_only.view());
  double agree_curve = 0.0;
  {
    std::vector<double> st(static_cast<std::size_t>(n_q));
    for (int k = 0; k < n_q; ++k) st[static_cast<std::size_t>(k)] = quotes[static_cast<std::size_t>(k)];
    std::vector<double> out(static_cast<std::size_t>(cal_only.cal_cold().n_outputs()), 0.0);
    cal_only.cal_cold().run(st.data(), 1, out.data());
    const std::vector<std::vector<double>> ours = cal_only.discount_factors(out.data(), ts);
    int worst_curve = -1;
    std::size_t n_points = 0;
    for (std::size_t c = 0; c < ours.size(); ++c) {
      // Their sample() takes ONE grid for every curve, so ask it once per curve on that curve's own
      // grid and read only that curve's row.
      const std::vector<swaps::api::CurveSample> theirs = sess_cal.sample(ts[c]);
      if (theirs.size() != ours.size()) die("curve count mismatch on the sample");
      const Worst w = worst_of(ours[c], theirs[c].discount, 1.0);
      n_points += ts[c].size();
      if (w.rel > agree_curve) {
        agree_curve = w.rel;
        worst_curve = static_cast<int>(c);
      }
    }
    const bool ok = agree_curve <= tol.discount;
    std::printf("\nagreement, the calibrated curve: worst discount factor %.3e over %d curves / %zu sample"
                " times (worst on curve %d, tolerance %.0e)  %s\n",
                agree_curve, static_cast<int>(ours.size()), n_points, worst_curve, tol.discount,
                ok ? "ok" : "FAIL");
    if (!ok)
      die("the two engines do not agree on the calibrated curve: no timing from this problem means anything");
  }

  // ---- the two coupon forms, as evidence rather than as a timing column ------------------------
  // D90's table had an `ours telescoped` column beside `ours naive`: the hand-written spike of D74
  // against the engine's own product-loop recording. Since D81 the engine DERIVES the collapse, so
  // the two recordings compile to the same program and the column measured run-to-run noise. This
  // checks that claim instead of asserting it, and nothing is timed from it.
  if (verify_spike) {
    if (po.name != "compare_ois") die("--verify-spike is a compare_ois diagnostic (the spike fixture is that fixture's)");
    fixtures::CompareOisOptions so;
    so.trades = po.trades < 0 ? 1000 : po.trades;
    so.seed = po.seed;
    so.tenors = po.tenors;
    so.max_start_offset_years = po.start_years;
    const fixtures::CompareOis s = fixtures::make_compare_ois(so);
    const fixtures::CompareOisTape a = fixtures::spike::record_compare_ois_spike(s, fixtures::spike::Form::naive);
    const fixtures::CompareOisTape b = fixtures::spike::record_compare_ois_spike(s, fixtures::spike::Form::telescoped);
    std::printf("spike check: the engine's own recording is %zu raw -> %zu after the passes; the hand-telescoped\n"
                "  spike is %zu raw -> %zu. Same program after the passes: %s. D90's second timing column is\n"
                "  retired on that basis (it measured the same program twice).\n",
                a.nodes_raw, a.nodes_after_passes, b.nodes_raw, b.nodes_after_passes,
                a.nodes_after_passes == b.nodes_after_passes ? "YES" : "NO");
  }

  // ---- the market both sides are moved to on rep k ---------------------------------------------
  // One basis point on one rotating quote: enough that a frozen-Newton tick and a warm solve each
  // do real work, small enough that neither falls back to a cold LM. IDENTICAL on both sides by
  // construction -- and on a FUTURES row the move is applied in their rate units and converted to
  // our price units through the same dmarket, so the two markets are the same market.
  auto moved = [&](int k) {
    std::vector<double> q = quotes;
    const std::size_t j = static_cast<std::size_t>(k % n_q);
    const double bump = ((k % 2) ? 1.0 : -1.0) * 1e-4;   // in RATE units on every row
    q[j] += bump * (dmarket[j] != 0.0 ? 1.0 / dmarket[j] : 1.0);
    return q;
  };
  auto their_market = [&](const std::vector<double>& q) {
    Eigen::VectorXd m(n_q);
    for (int j = 0; j < n_q; ++j) {
      const std::size_t u = static_cast<std::size_t>(j);
      m[j] = cal_only.view().quote_is_price[u] ? (1.0 - q[u] / 100.0) : q[u];
    }
    return m;
  };

  // ---- the two calibration phases, interleaved -------------------------------------------------
  std::vector<double> st(static_cast<std::size_t>(n_q));
  std::vector<double> out_cal(static_cast<std::size_t>(cal_only.cal_hot().n_outputs()), 0.0);
  auto set_state = [&](const std::vector<double>& q) {
    for (int j = 0; j < n_q; ++j) st[static_cast<std::size_t>(j)] = q[static_cast<std::size_t>(j)];
  };

  std::cerr << "[h2h] warm-up (calibration)\n";
  for (int k = 0; k < 3; ++k) {
    const std::vector<double> q = moved(k);
    set_state(q);
    cal_only.cal_cold().run(st.data(), 1, out_cal.data());
    cal_only.cal_hot().run(st.data(), 1, out_cal.data());
    sess_cal.set_market(their_market(q));
    sess_cal.calibrate(x0);
    sess_cal_hot.recalibrate(their_market(q));
  }
  // The solved state of the calibration-only program, for the forward-pass-only "price" phase.
  set_state(quotes);
  cal_only.cal_hot().run(st.data(), 1, out_cal.data());
  std::vector<double> cal_state(static_cast<std::size_t>(cal_only.cal_hot().n_inputs()), 0.0);
  for (int k = 0; k < cal_only.cal_hot().n_inputs(); ++k)
    cal_state[static_cast<std::size_t>(k)] = cal_only.cal_hot().full_state(0)[k];

  Samples o_cold, o_hot, o_eval_cal, t_cold, t_hot, t_cold_self, t_hot_self;
  std::cerr << "[h2h] measuring the calibration, " << reps << " repetitions, interleaved\n";
  for (int k = 0; k < reps; ++k) {
    const std::vector<double> q = moved(k);
    set_state(q);
    const Eigen::VectorXd m = their_market(q);

    // Both cold solves are at the SAME market. D78's harness re-ran `calibrate(x0)` at the BASE
    // market while ours ran at the moved one, so `calibrate_cold` compared two different inputs --
    // immaterially (1 bp on one of n quotes), but it was not the same question asked twice.
    // `set_market` writes the quote RHS with no solve and is outside the clock.
    sess_cal.set_market(m);
    clock_type::time_point t = clock_type::now();
    cal_only.cal_cold().run(st.data(), 1, out_cal.data());
    o_cold.add(us_since(t));
    t = clock_type::now();
    cal_only.cal_hot().run(st.data(), 1, out_cal.data());
    o_hot.add(us_since(t));
    t = clock_type::now();
    cal_only.cal_hot().interpreter().run(cal_state.data(), 1, out_cal.data());
    o_eval_cal.add(us_since(t));

    t = clock_type::now();
    const cal::CalibrationResult& rc = sess_cal.calibrate(x0);
    t_cold.add(us_since(t));
    t_cold_self.add(rc.solve_micros);
    t = clock_type::now();
    const cal::CalibrationResult& rh = sess_cal_hot.recalibrate(m);
    t_hot.add(us_since(t));
    t_hot_self.add(rh.solve_micros);
  }

  std::printf("\n--- calibration (family-independent: the bundle never carries the book) ---\n");
  std::printf("  %-22s %11s %11s | %9s | %s\n", "phase, median us", "ours", "theirs", "theirs/ours", "note");
  row("calibrate_cold", o_cold, t_cold, "ours from the flat start; theirs LM from flat_x0");
  row("calibrate_hot", o_hot, t_hot, "ours warm+chord; theirs frozen-Newton tick");
  std::printf("  %-22s %11.1f %11s | %9s | %s\n", "price_cal_only", o_eval_cal.median(), "-", "-",
              "ours only: the forward pass with NO book");
  std::printf("  their own reported solve times (medians, us): cold %.1f  hot %.1f\n", t_cold_self.median(),
              t_hot_self.median());
  {
    const epykos::solver::ImplicitProgram::RunStats& rs = cal_only.cal_hot().last_run();
    std::printf("  ours: tape %zu raw -> %zu after the passes; hot solve solves=%lld residual_evals=%lld jacobians=%lld\n",
                cal_only.nodes_raw(), cal_only.nodes_after_passes(), static_cast<long long>(rs.solves),
                static_cast<long long>(rs.residual_evaluations), static_cast<long long>(rs.jacobians));
  }

  // ---- the book: once for the whole book, then once per product family -------------------------
  // `--family X` runs that family alone; otherwise the whole book first, and then -- under
  // --per-family -- each product family as its own problem, discovered from the full book's view.
  std::vector<std::string> wanted{po.family};
  std::vector<FamilyResult> results;
  bool any_failure = false;

  for (std::size_t wi = 0; wi < wanted.size(); ++wi) {
    h2h::ProblemOptions pb = po;
    pb.family = wanted[wi];
    const std::string shown = pb.family.empty() ? "ALL" : pb.family;
    std::cerr << "[h2h] recording the book: " << shown << "\n";
    const h2h::OurProblem ours(pb);

    if (wi == 0 && per_family && po.family.empty())
      for (const h2h::Family& f : h2h::families_of(ours.view())) wanted.push_back(f.name);

    h2h::Mapping bm;
    const pf::MultiCurveBook book = h2h::book_of(ours.view(), &bm);
    if (book.positions.empty()) die("the book for '" + shown + "' is empty");

    swaps::api::BundleSession sess(prob);
    sess.calibrate(cal::flat_x0(prob));

    FamilyResult fr;
    fr.name = shown;
    fr.trades = ours.n_trades();
    fr.positions = static_cast<int>(book.positions.size());
    fr.nodes_raw = ours.nodes_raw();
    fr.nodes_after = ours.nodes_after_passes();
    fr.rows = std::min(ladder_rows, std::max(1, ours.n_trades()));

    std::printf("\n--- book: %s --- %d trades -> %d of their positions, tape %zu raw -> %zu after the passes,"
                " recorded in %.2f s\n",
                shown.c_str(), fr.trades, fr.positions, fr.nodes_raw, fr.nodes_after, ours.seconds_record());
    std::printf("  %s\n", bm.to_string().c_str());

    // README §5, asserted rather than assumed: inside [0, last knot] the two curves are the same
    // function of the knot values; beyond it they are not (we hold the variable flat, they continue
    // the last forward). Extend the tenor set or the book without re-checking this and the
    // agreement below stops being valid, so it is checked for every problem, every family.
    {
      const std::vector<double> t_max = h2h::max_time_read(ours.view());
      bool past = false;
      for (std::size_t c = 0; c < t_max.size(); ++c) {
        const double last = ours.view().curves[c].knot_t.empty() ? 0.0 : ours.view().curves[c].knot_t.back();
        if (t_max[c] > last) {
          past = true;
          std::printf("  EXTRAPOLATION: curve %zu (%s) is read at t = %.9f, past its last knot %.9f -- the two\n"
                      "    engines extrapolate DIFFERENTLY there (README §5) and any agreement below is luck\n",
                      c, ours.view().curves[c].definition.c_str(), t_max[c], last);
        }
      }
      if (!past) {
        std::string s;
        for (std::size_t c = 0; c < t_max.size(); ++c) {
          char b[64];
          std::snprintf(b, sizeof b, "%.4f/%.4f", t_max[c],
                        ours.view().curves[c].knot_t.empty() ? 0.0 : ours.view().curves[c].knot_t.back());
          s += (s.empty() ? "" : " ") + std::string(b);
        }
        std::printf("  nothing is priced past the last knot (max t read / last knot, per curve): %s\n", s.c_str());
      }
    }

    // ---- gate 2: the book NPV and the book-level risk ladder -----------------------------------
    std::vector<double> npv_out;
    std::vector<double> our_book_ladder;
    {
      const std::vector<int> ordinals = {ours.book_output()};
      our_book_ladder = ours.ladder(quotes, ordinals, &npv_out);
      if (static_cast<int>(our_book_ladder.size()) != n_q)
        die("our book ladder has " + std::to_string(our_book_ladder.size()) + " columns for " + std::to_string(n_q) +
            " quotes");
      // The ONE unit conversion in this comparison, applied to OUR side: a futures row's quote is a
      // PRICE here and a RATE there, and d(rate)/d(price) = -1/100.
      std::vector<double> our_in_their_units(our_book_ladder.size());
      for (std::size_t k = 0; k < our_book_ladder.size(); ++k) our_in_their_units[k] = our_book_ladder[k] / dmarket[k];

      const swaps::api::PortfolioRisk prk = sess.price_portfolio_risk(book);
      if (static_cast<int>(prk.ladder.size()) != n_q)
        die("their ladder has " + std::to_string(prk.ladder.size()) + " columns for " + std::to_string(n_q) + " quotes");
      std::vector<double> theirs(prk.ladder.data(), prk.ladder.data() + prk.ladder.size());

      const double our_npv = npv_out.at(static_cast<std::size_t>(ours.book_output()));
      double gross = 0.0;
      for (int o : ours.pv_ordinals()) gross += std::abs(npv_out.at(static_cast<std::size_t>(o)));
      if (gross <= 0.0) gross = std::max(std::abs(prk.npv), 1.0);
      fr.agree_npv = std::abs(our_npv - prk.npv) / gross;
      fr.agree_ladder = worst_of(our_in_their_units, theirs, inf_norm(theirs)).rel;
      fr.ok = fr.agree_npv <= tol.npv && fr.agree_ladder <= tol.ladder;
      std::printf("  agreement: book NPV %.3e (tol %.0e; ours %.17e, theirs %.17e, gross %.6e),\n"
                  "             ladder %.3e (tol %.0e, %d buckets, inf-norm %.6e)  %s\n",
                  fr.agree_npv, tol.npv, our_npv, prk.npv, gross, fr.agree_ladder, tol.ladder, n_q,
                  inf_norm(theirs), fr.ok ? "ok" : "FAIL");
      if (!fr.ok) {
        any_failure = true;
        fr.note = "DISAGREE -- not timed";
        if (per_trade_on_failure) {
          // Localise it: price each trade of this book on its own and name the worst.
          double worst = 0.0;
          int at = -1;
          for (int i = 0; i < ours.n_trades(); ++i) {
            const pf::MultiCurveBook one = h2h::book_of_trade(ours.view(), static_cast<std::size_t>(i));
            const double t1 = sess.price_portfolio(one).npv;
            const double o1 = npv_out.at(static_cast<std::size_t>(ours.pv_ordinals()[static_cast<std::size_t>(i)]));
            const double e = std::abs(o1 - t1) / std::max(std::abs(o1), 1.0);
            if (e > worst) {
              worst = e;
              at = i;
            }
          }
          std::printf("  localised: the worst single trade is index %d at %.3e relative (family '%s')\n", at, worst,
                      at >= 0 ? ours.view().family[static_cast<std::size_t>(at)].c_str() : "?");
        }
        results.push_back(fr);
        continue;   // a disagreement means no timing from this product (the brief's rule)
      }
    }

    // ---- the timed phases, interleaved --------------------------------------------------------
    std::vector<double> out_full(static_cast<std::size_t>(ours.full().n_outputs()), 0.0);
    set_state(quotes);
    ours.full().run(st.data(), 1, out_full.data());
    std::vector<double> full_state(static_cast<std::size_t>(ours.full().n_inputs()), 0.0);
    for (int k = 0; k < ours.full().n_inputs(); ++k)
      full_state[static_cast<std::size_t>(k)] = ours.full().full_state(0)[k];

    // The R ladder rows and, on their side, the R single-trade books that are the only way their
    // API yields R rows (price_portfolio_risk returns ONE book-level ladder per call).
    std::vector<int> ordinals;
    std::vector<pf::MultiCurveBook> their_rows;
    if (fr.rows == 1) {
      ordinals.push_back(ours.book_output());
    } else {
      for (int i = 0; i < fr.rows; ++i) {
        ordinals.push_back(ours.pv_ordinals()[static_cast<std::size_t>(i)]);
        their_rows.push_back(h2h::book_of_trade(ours.view(), static_cast<std::size_t>(i)));
      }
    }

    // THEIR AMORTISED REPRICE PATH, which D78/D90 did not use and should have.
    // `price_portfolio` is documented in their own header as the ONE-SHOT path -- "the one-shot
    // price_portfolio() deliberately stays on the templated path ... so the compiled twin is
    // reserved for THIS cached path" -- and `bind_portfolio` + `reprice_bound` is the amortised one,
    // "the AMORTIZED path for a live book repriced every streaming tick". This harness reprices the
    // same book `reps` times, so the bound path is what it should be timing: anything else measures
    // their cold entry point against our warm one. Both are timed and both are reported; the bound
    // column is the headline ratio. There is NO bound risk path in that facade -- risk has one entry
    // point -- so the ladder comparison was already on their best route.
    sess.bind_portfolio(book);

    std::cerr << "[h2h] warm-up (book " << shown << ")\n";
    for (int k = 0; k < 3; ++k) {
      const std::vector<double> q = moved(k);
      set_state(q);
      ours.full().run(st.data(), 1, out_full.data());
      sess.price_portfolio(book);
      sess.reprice_bound();
      sess.price_portfolio_risk(book);
    }

    Samples o_eval_full, o_risk, t_price, t_bound, t_risk, t_price_self, t_bound_self, t_risk_self;
    std::vector<double> ladder_out;
    std::cerr << "[h2h] measuring the book " << shown << ", " << reps << " repetitions, interleaved\n";
    for (int k = 0; k < reps; ++k) {
      const std::vector<double> q = moved(k);
      set_state(q);

      clock_type::time_point t = clock_type::now();
      ours.full().interpreter().run(full_state.data(), 1, out_full.data());
      o_eval_full.add(us_since(t));
      t = clock_type::now();
      const std::vector<double> rows = ours.ladder(q, ordinals, &ladder_out);
      o_risk.add(us_since(t));
      if (rows.empty()) die("our ladder returned nothing");

      t = clock_type::now();
      const swaps::api::PortfolioReprice pr = sess.price_portfolio(book);
      t_price.add(us_since(t));
      t_price_self.add(pr.price_us);
      t = clock_type::now();
      const swaps::api::PortfolioReprice pb2 = sess.reprice_bound();
      t_bound.add(us_since(t));
      t_bound_self.add(pb2.price_us);
      if (their_rows.empty()) {
        t = clock_type::now();
        const swaps::api::PortfolioRisk prk = sess.price_portfolio_risk(book);
        t_risk.add(us_since(t));
        t_risk_self.add(prk.risk_us);
      } else {
        t = clock_type::now();
        double self = 0.0;
        for (const pf::MultiCurveBook& one : their_rows) self += sess.price_portfolio_risk(one).risk_us;
        t_risk.add(us_since(t));
        t_risk_self.add(self);
      }
    }

    fr.ours_price = o_eval_full.median();
    fr.theirs_price = t_price.median();
    fr.theirs_bound = t_bound.median();
    // The BOOK-ATTRIBUTABLE forward pass: our whole-program pass minus the same pass on the
    // calibration-only twin. Their `price_portfolio` prices the book and nothing else, so this is
    // the comparable quantity -- and it matters most on a small family book, where the 70
    // calibration instruments are the majority of our raw `price` number. It is arithmetic on two
    // measurements and is labelled as such everywhere it appears.
    fr.ours_price_book = o_eval_full.median() - o_eval_cal.median();
    fr.ours_risk = o_risk.median();
    fr.theirs_risk = t_risk.median();

    std::printf("  %-22s %11s %11s | %9s | %s\n", "phase, median us", "ours", "theirs", "theirs/ours", "note");
    row("price", o_eval_full, t_price, "ours warm pass; THEIRS ONE-SHOT (price_portfolio) -- D90's column");
    row("price_bound", o_eval_full, t_bound,
        "ours warm pass; theirs AMORTISED (bind_portfolio + reprice_bound) -- the headline");
    char rn[64];
    std::snprintf(rn, sizeof rn, "risk_ladder(R=%d)", fr.rows);
    row(rn, o_risk, t_risk,
        their_rows.empty() ? "d(book PV)/d(quote), ONE book-level row -- the clean comparable"
                           : "R per-trade rows; the asymmetry is stated below, not divided away");
    if (!their_rows.empty()) {
      // The R > 1 comparison is NOT symmetric, and the asymmetry runs in THEIR favour, so it is
      // stated rather than hidden in a ratio. Our R rows come out of ONE batched adjoint over the
      // whole book's program, whose forward half walks every trade in the book and not just the R
      // rows asked for -- our tape prices the book in one program and cannot be asked for a subset.
      // Their R rows come from R `price_portfolio_risk` calls on SINGLE-TRADE books, which is the
      // only way their API yields per-trade rows (it returns one book-level ladder per call) and
      // which touches R trades and no more. So R = 1 above is the like-for-like; this row is the
      // per-row cost of each engine's own best route to a per-trade ladder.
      std::printf("  %-22s %11.1f %11.1f | %8.2fx | %s\n", "  ... per row",
                  o_risk.median() / static_cast<double>(fr.rows), t_risk.median() / static_cast<double>(fr.rows),
                  o_risk.median() > 0.0 ? t_risk.median() / o_risk.median() : 0.0,
                  "ours carries the WHOLE book in every call; theirs carries R trades");
    }
    std::printf("  book-attributable forward pass = price - price_cal_only: %.1f us"
                "  (arithmetic on two measurements, not a measurement)\n",
                o_eval_full.median() - o_eval_cal.median());
    std::printf("  their own reported times (medians, us): price %.1f  bound %.1f  risk %.1f  (its risk_us EXCLUDES forming the\n"
                "    risk operator M; our ladder builds its Jacobian inside the timed region, so the outer clock is\n"
                "    the honest column and its own is the floor)\n",
                t_price_self.median(), t_bound_self.median(), t_risk_self.median());
    {
      const epykos::solver::ImplicitProgram::RunStats& rs = ours.full().last_run();
      std::printf("  ours: ladder solves=%lld residual_evals=%lld jacobians=%lld\n",
                  static_cast<long long>(rs.solves), static_cast<long long>(rs.residual_evaluations),
                  static_cast<long long>(rs.jacobians));
    }
    results.push_back(fr);

    if (!exchange_path.empty() && wi == 0) {
      std::ofstream f(exchange_path);
      if (!f) die("cannot write " + exchange_path);
      f << h2h::exchange_json(ours.view());
      std::printf("  wrote the exchange file (their schema, through their own bundle_to_json) to %s\n",
                  exchange_path.c_str());
    }
  }

  // ---- the summary: ratios are the headline, absolutes are not comparable across sessions ------
  const double load_after = load_1min();
  std::printf("\n=== %s: theirs/ours, by product family (within-run ratios; D90: the ABSOLUTES are not\n"
              "    comparable across sessions, the ratios are) ===\n",
              po.name.c_str());
  std::printf("%-32s %7s %7s %5s %9s %9s %9s %9s %11s %11s\n", "family", "trades", "posns", "rows", "price x",
              "bound x", "bound x*", "risk x", "agree npv", "agree ladder");
  for (const FamilyResult& r : results) {
    if (!r.ok) {
      std::printf("%-32s %7d %7d %5d %9s %9s %9s %9s %11.3e %11.3e   %s\n", r.name.c_str(), r.trades, r.positions,
                  r.rows, "-", "-", "-", "-", r.agree_npv, r.agree_ladder, r.note.c_str());
      continue;
    }
    char pb[16];
    if (r.ours_price_book > 0.0)
      std::snprintf(pb, sizeof pb, "%8.2fx", r.theirs_bound / r.ours_price_book);
    else
      std::snprintf(pb, sizeof pb, "%9s", "-");
    std::printf("%-32s %7d %7d %5d %8.2fx %8.2fx %9s %8.2fx %11.3e %11.3e\n", r.name.c_str(), r.trades, r.positions,
                r.rows, r.ours_price > 0.0 ? r.theirs_price / r.ours_price : 0.0,
                r.ours_price > 0.0 ? r.theirs_bound / r.ours_price : 0.0, pb,
                r.ours_risk > 0.0 ? r.theirs_risk / r.ours_risk : 0.0, r.agree_npv, r.agree_ladder);
  }
  std::printf("\ncalibrate_cold %.2fx, calibrate_hot %.2fx (theirs/ours; < 1 means we are slower)\n",
              o_cold.median() > 0.0 ? t_cold.median() / o_cold.median() : 0.0,
              o_hot.median() > 0.0 ? t_hot.median() / o_hot.median() : 0.0);
  std::printf("  price x  = theirs/ours with THEIR ONE-SHOT price_portfolio -- D90's column, and their own\n"
              "             header calls it the cold entry point. Kept only for continuity with D90.\n"
              "  bound x  = theirs/ours with their AMORTISED bind_portfolio + reprice_bound, which is what a\n"
              "             harness repricing the same book %d times should time. THE HEADLINE.\n"
              "  bound x* = the same against our BOOK-ATTRIBUTABLE pass (ours minus the calibration-only twin,\n"
              "             %.1f us -- ours prices the book AND all %d calibration instruments in one program and\n"
              "             cannot be asked for the book alone). Arithmetic on two measurements, not a measurement.\n"
              "  risk x   = at R = 1 a like-for-like book-level ladder; at R > 1 see the per-row rows above,\n"
              "             where the asymmetry (ours carries the whole book, theirs R trades) is stated. Their\n"
              "             facade has NO bound risk path, so this was already on their best route.\n",
              reps, o_eval_cal.median(), n_q);
  std::printf("worst curve agreement %.3e; max_batch %d, lane_tile %d, ladder rows %d, %d repetitions\n",
              agree_curve, po.max_batch, po.lane_tile, ladder_rows, reps);
  std::printf("1-minute load: %.2f before, %.2f after (bar %.2f)%s\n", load_before, load_after, max_load,
              load_check ? "" : "  [--no-load-check: the bar was NOT enforced]");
  return any_failure ? 1 : 0;
}
