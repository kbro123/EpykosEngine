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
// D78 relaxes D11/D21 for this file alone: it is the only place in this repository that includes a
// header of the other engine. `bench/compare/README.md` §0 lists every file read to write it. No
// logic of that engine is copied here -- this file calls its public facade and nothing else -- and
// nothing under `include/epykos/` or `src/` is touched by the relaxation.
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
//   risk            ours: the O3 ladder d(book PV)/d(quote) through the adjoint and the IFT.
//                   theirs: BundleSession::price_portfolio_risk(book).ladder.
//
// Every phase is wall-clocked from the OUTSIDE on both sides. The other engine also self-reports
// `solve_micros` / `price_us` / `risk_us`, and those are printed beside our clock because they are
// not the same quantity: its `risk_us` deliberately excludes forming the risk operator M (the
// calibration Jacobian solve), where our ladder builds its Jacobian inside the timed region. Both
// numbers are reported so that gap is visible instead of argued about.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "swaps/api/bundle_api.hpp"
#include "swaps/api/codec.hpp"

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/spike_telescoped.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "epykos/version.hpp"

namespace {

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;
namespace cal = swaps::calibration;

using clock_type = std::chrono::steady_clock;

double us_since(clock_type::time_point t0) {
  return std::chrono::duration<double, std::micro>(clock_type::now() - t0).count();
}

[[noreturn]] void die(const std::string& m) {
  std::cerr << "h2h: " << m << "\n";
  std::exit(2);
}

boost::json::value read_json(const std::string& path) {
  std::ifstream f(path);
  if (!f) die("cannot read " + path);
  std::ostringstream s;
  s << f.rdbuf();
  return boost::json::parse(s.str());
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

void row3(const char* name, const Samples& naive, const Samples& tele, const Samples& theirs,
          const char* note) {
  const double a = naive.median(), b = tele.median(), c = theirs.median();
  char rn[24], rt[24];
  if (a > 0.0 && c > 0.0) std::snprintf(rn, sizeof rn, "%8.2fx", c / a); else std::snprintf(rn, sizeof rn, "%9s", "-");
  if (b > 0.0 && c > 0.0) std::snprintf(rt, sizeof rt, "%8.2fx", c / b); else std::snprintf(rt, sizeof rt, "%9s", "-");
  std::printf("%-16s %11.1f %11.1f %11.1f | %9s %9s | %s\n", name, a, b, c, rn, rt, note);
}

}  // namespace

int main(int argc, char** argv) {
  std::string bundle_path, book_path, cal_bundle_path;
  int trades = 1000, reps = 20, start_years = 0;
  std::vector<std::string> tenors;
  std::uint64_t seed = 20260925;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) die("missing value for " + a);
      return argv[++i];
    };
    if (a == "--bundle") bundle_path = next();
    else if (a == "--book") book_path = next();
    else if (a == "--cal-bundle") cal_bundle_path = next();
    else if (a == "--trades") trades = std::stoi(next());
    else if (a == "--reps") reps = std::stoi(next());
    else if (a == "--seed") seed = std::stoull(next());
    else if (a == "--start-years") start_years = std::stoi(next());
    else if (a == "--tenors") {
      const std::string list = next();
      for (std::size_t b = 0; b <= list.size();) {
        const std::size_t e = std::min(list.find(',', b), list.size());
        if (e > b) tenors.push_back(list.substr(b, e - b));
        b = e + 1;
      }
    } else die("unknown argument " + a);
  }
  if (bundle_path.empty() || book_path.empty()) die("--bundle and --book are required");
  if (cal_bundle_path.empty()) cal_bundle_path = bundle_path;

  // ---- this engine: the same fixture the exchange file was written from -------------------------
  fixtures::CompareOisOptions o;
  o.trades = trades;
  o.seed = seed;
  o.tenors = tenors;
  o.max_start_offset_years = start_years;
  fixtures::CompareOisOptions oc = o;
  oc.trades = 0;                      // the calibration-only twin, for the two calibration phases

  std::cerr << "[h2h] building this engine's programs (" << trades << " trades, "
            << (tenors.empty() ? 16 : static_cast<int>(tenors.size())) << " knots)\n";
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  const fixtures::CompareOis sc = fixtures::make_compare_ois(oc);

  // BOTH coupon forms, from ONE fixture (D74). `naive` is the engine's own maths -- the product
  // loop of maths/instrument/coupon.hpp, what the engine actually records today. `telescoped` is
  // the hand-written spike, which is the SAME ARITHMETIC the other engine is given: one endpoint
  // discount-factor ratio per accrual. The naive row is what this engine is; the telescoped row is
  // what it would be if the optimiser could derive the collapse, and is the only like-for-like
  // comparison of the two engines' machinery rather than of their coupon representations.
  struct Side {
    fixtures::CompareOisTape full, cal;
    std::unique_ptr<solver::ImplicitProgram> full_warm, cal_hot, cal_cold;
    std::vector<double> full_state, cal_state;
  };
  Side side[2];
  for (int fi = 0; fi < 2; ++fi) {
    const fixtures::spike::Form form = fi == 0 ? fixtures::spike::Form::naive : fixtures::spike::Form::telescoped;
    std::cerr << "[h2h]   recording " << fixtures::spike::to_string(form) << "\n";
    Side& sd = side[fi];
    sd.full = fixtures::spike::record_compare_ois_spike(s, form);
    sd.cal = fixtures::spike::record_compare_ois_spike(sc, form);
    solver::ProgramOptions po = fixtures::compare_ois_program_options(s);
    po.warm_start = true;
    po.jacobian = static_cast<int>(solver::JacobianPolicy::chord);
    sd.full_warm = std::make_unique<solver::ImplicitProgram>(sd.full.tape, sd.full.registry, po);
    solver::ProgramOptions pc = fixtures::compare_ois_program_options(sc);
    pc.warm_start = true;
    pc.jacobian = static_cast<int>(solver::JacobianPolicy::chord);
    sd.cal_hot = std::make_unique<solver::ImplicitProgram>(sd.cal.tape, sd.cal.registry, pc);
    pc.warm_start = false;
    pc.jacobian = -1;
    sd.cal_cold = std::make_unique<solver::ImplicitProgram>(sd.cal.tape, sd.cal.registry, pc);
  }

  // ---- the other engine: the same problem, through its public facade ----------------------------
  std::cerr << "[h2h] compiling the other engine's sessions\n";
  const cal::BundleProblem prob = swaps::api::bundle_from_json(read_json(bundle_path));
  const cal::BundleProblem prob_cal = swaps::api::bundle_from_json(read_json(cal_bundle_path));
  const swaps::portfolio::MultiCurveBook book = swaps::api::book_from_json(read_json(book_path));
  swaps::api::BundleSession sess(prob);
  swaps::api::BundleSession sess_cal(prob_cal);
  swaps::api::BundleSession sess_cal_hot(prob_cal);

  const Eigen::VectorXd x0 = cal::flat_x0(prob_cal);
  const Eigen::VectorXd market0 = prob_cal.market();
  if (static_cast<std::size_t>(market0.size()) != s.quotes.size())
    die("the two sides disagree on the quote count: " + std::to_string(market0.size()) + " vs " +
        std::to_string(s.quotes.size()));

  // Both engines must be at the same calibrated state before anything is timed.
  sess.calibrate(cal::flat_x0(prob));
  sess_cal.calibrate(x0);
  sess_cal_hot.calibrate(x0);

  const int n_q = static_cast<int>(s.quotes.size());
  std::vector<double> st(static_cast<std::size_t>(n_q));

  // The market both sides are moved to on rep k. One basis point on one rotating quote: enough
  // that a frozen-Newton tick and a warm solve each do real work, small enough that neither falls
  // back to a cold LM. IDENTICAL on every side by construction.
  auto moved_quotes = [&](int k) {
    std::vector<double> q = s.quotes;
    q[static_cast<std::size_t>(k % n_q)] += ((k % 2) ? 1.0 : -1.0) * 1e-4;
    return q;
  };
  auto set_state = [&](const std::vector<double>& q) {
    for (int j = 0; j < n_q; ++j) st[static_cast<std::size_t>(j)] = q[static_cast<std::size_t>(j)];
  };

  std::vector<double> out_full[2], out_cal[2];
  for (int fi = 0; fi < 2; ++fi) {
    out_full[fi].assign(static_cast<std::size_t>(side[fi].full_warm->n_outputs()), 0.0);
    out_cal[fi].assign(static_cast<std::size_t>(side[fi].cal_hot->n_outputs()), 0.0);
  }

  // ---- warm-up: every path taken, and the streamer started so its tick is a tick ----------------
  std::cerr << "[h2h] warm-up\n";
  for (int k = 0; k < 3; ++k) {
    const std::vector<double> q = moved_quotes(k);
    set_state(q);
    for (int fi = 0; fi < 2; ++fi) {
      side[fi].cal_cold->run(st.data(), 1, out_cal[fi].data());
      side[fi].cal_hot->run(st.data(), 1, out_cal[fi].data());
      side[fi].full_warm->run(st.data(), 1, out_full[fi].data());
    }
    Eigen::VectorXd m(n_q);
    for (int j = 0; j < n_q; ++j) m[j] = q[static_cast<std::size_t>(j)];
    sess_cal.calibrate(x0);
    sess_cal_hot.recalibrate(m);
    sess.price_portfolio(book);
    sess.price_portfolio_risk(book);
  }
  set_state(s.quotes);
  for (int fi = 0; fi < 2; ++fi) {
    Side& sd = side[fi];
    sd.full_warm->run(st.data(), 1, out_full[fi].data());
    sd.cal_hot->run(st.data(), 1, out_cal[fi].data());
    sd.full_state.assign(static_cast<std::size_t>(sd.full_warm->n_inputs()), 0.0);
    for (int k = 0; k < sd.full_warm->n_inputs(); ++k) sd.full_state[static_cast<std::size_t>(k)] = sd.full_warm->full_state(0)[k];
    sd.cal_state.assign(static_cast<std::size_t>(sd.cal_hot->n_inputs()), 0.0);
    for (int k = 0; k < sd.cal_hot->n_inputs(); ++k) sd.cal_state[static_cast<std::size_t>(k)] = sd.cal_hot->full_state(0)[k];
  }

  // The two forms must AGREE before either is timed against anything (PRINCIPLES.md §4): the
  // telescoped recording is a different sequence of roundings, not a different price.
  {
    const double a = out_full[0].at(static_cast<std::size_t>(side[0].full.book_output));
    const double b = out_full[1].at(static_cast<std::size_t>(side[1].full.book_output));
    const double scale = std::max(std::abs(a), 1.0);
    std::printf("[h2h] the two coupon forms agree to %.3e relative on this engine's book NPV\n",
                std::abs(a - b) / scale);
  }

  // ---- the gate: the two ENGINES must agree before either is timed ------------------------------
  //
  // D9 and PRINCIPLES.md §4: a timing comparison between two engines that are not computing the
  // same quantity is meaningless, so the comparison is refused rather than reported with a
  // caveat. Checked here, in this process, on the very objects about to be timed -- not inherited
  // from a separate run of scripts/compare_swapengine.py against a separate exchange file.
  {
    set_state(s.quotes);
    const swaps::api::PortfolioRisk prk = sess.price_portfolio_risk(book);
    bool ok = true;
    for (int fi = 0; fi < 2; ++fi) {
      Side& sd = side[fi];
      const std::vector<int> ordinals = {sd.full.book_output};
      std::vector<double> npv_out;
      const std::vector<double> rows = fixtures::compare_ois_ladder(*sd.full_warm, s.quotes, ordinals, &npv_out);
      if (static_cast<int>(rows.size()) != n_q || static_cast<int>(prk.ladder.size()) != n_q)
        die("ladder length mismatch: ours " + std::to_string(rows.size()) + ", theirs " +
            std::to_string(prk.ladder.size()) + ", quotes " + std::to_string(n_q));
      double lad_scale = 0.0;
      for (int j = 0; j < n_q; ++j) lad_scale = std::max(lad_scale, std::abs(rows[static_cast<std::size_t>(j)]));
      if (lad_scale <= 0.0) lad_scale = 1.0;
      double worst_lad = 0.0;
      for (int j = 0; j < n_q; ++j)
        worst_lad = std::max(worst_lad, std::abs(rows[static_cast<std::size_t>(j)] - prk.ladder[j]) / lad_scale);
      const double our_npv = npv_out.at(static_cast<std::size_t>(sd.full.book_output));
      const double npv_scale = std::max(std::abs(prk.npv), 1.0);
      const double worst_npv = std::abs(our_npv - prk.npv) / npv_scale;
      // The same tolerances scripts/compare_swapengine.py gates on (D71): the ladder relative to
      // its own infinity norm, the NPV relative to the book's own scale.
      const bool row_ok = worst_lad <= 1e-9 && worst_npv <= 1e-10;
      std::printf("[h2h] agreement, ours %-10s vs theirs: book NPV %.3e, ladder %.3e  %s\n",
                  fi == 0 ? "naive" : "telescoped", worst_npv, worst_lad, row_ok ? "ok" : "FAIL");
      ok = ok && row_ok;
    }
    if (!ok) die("the two engines disagree: no timing from this fixture means anything");
  }

  // ---- the measurement: interleaved, one clock --------------------------------------------------
  std::cerr << "[h2h] measuring, " << reps << " repetitions, interleaved\n";
  Samples o_cold[2], o_hot[2], o_eval_full[2], o_eval_cal[2], o_risk[2];
  Samples t_cold, t_hot, t_price, t_risk;
  Samples t_cold_self, t_hot_self, t_price_self, t_risk_self;
  std::vector<double> ladder_out;

  for (int k = 0; k < reps; ++k) {
    const std::vector<double> q = moved_quotes(k);
    set_state(q);
    Eigen::VectorXd m(n_q);
    for (int j = 0; j < n_q; ++j) m[j] = q[static_cast<std::size_t>(j)];

    for (int fi = 0; fi < 2; ++fi) {
      Side& sd = side[fi];
      clock_type::time_point t = clock_type::now();
      sd.cal_cold->run(st.data(), 1, out_cal[fi].data());
      o_cold[fi].add(us_since(t));
      t = clock_type::now();
      sd.cal_hot->run(st.data(), 1, out_cal[fi].data());
      o_hot[fi].add(us_since(t));
      t = clock_type::now();
      sd.full_warm->interpreter().run(sd.full_state.data(), 1, out_full[fi].data());
      o_eval_full[fi].add(us_since(t));
      t = clock_type::now();
      sd.cal_hot->interpreter().run(sd.cal_state.data(), 1, out_cal[fi].data());
      o_eval_cal[fi].add(us_since(t));
      const std::vector<int> ordinals = {sd.full.book_output};
      t = clock_type::now();
      const std::vector<double> rows = fixtures::compare_ois_ladder(*sd.full_warm, q, ordinals, &ladder_out);
      o_risk[fi].add(us_since(t));
      if (rows.empty()) die("our ladder returned nothing");
    }

    clock_type::time_point t = clock_type::now();
    const cal::CalibrationResult& rc = sess_cal.calibrate(x0);
    t_cold.add(us_since(t));
    t_cold_self.add(rc.solve_micros);
    t = clock_type::now();
    const cal::CalibrationResult& rh = sess_cal_hot.recalibrate(m);
    t_hot.add(us_since(t));
    t_hot_self.add(rh.solve_micros);
    t = clock_type::now();
    const swaps::api::PortfolioReprice pr = sess.price_portfolio(book);
    t_price.add(us_since(t));
    t_price_self.add(pr.price_us);
    t = clock_type::now();
    const swaps::api::PortfolioRisk prk = sess.price_portfolio_risk(book);
    t_risk.add(us_since(t));
    t_risk_self.add(prk.risk_us);
  }

  // ---- report -----------------------------------------------------------------------------------
  std::printf("\nEpykosEngine %s [%s] vs the other engine -- one process, one steady_clock, interleaved\n",
              epykos::version(), epykos::build_flags());
  std::printf("%d calibration instruments on %d knots, %d book trades, %d repetitions\n\n",
              n_q, static_cast<int>(s.set.knot_t.size()), trades, reps);
  std::printf("%-16s %11s %11s %11s | %9s %9s | %s\n", "phase, median us", "ours naive",
              "ours telesc", "theirs", "naive x", "telesc x", "note");
  row3("calibrate_cold", o_cold[0], o_cold[1], t_cold, "ours O1+O2 in one pass; theirs O1 only");
  row3("calibrate_hot", o_hot[0], o_hot[1], t_hot, "ours warm+chord; theirs frozen-Newton tick");
  row3("price_all", o_eval_full[0], o_eval_full[1], t_price, "ours book AND instruments; theirs book only");
  row3("price_cal_only", o_eval_cal[0], o_eval_cal[1], Samples{}, "ours only: the same pass, no book");
  row3("risk_ladder", o_risk[0], o_risk[1], t_risk, "d(book PV)/d(quote), both analytic");
  std::printf("\nbook-attributable forward pass = price_all - price_cal_only:"
              " naive %.1f us, telescoped %.1f us"
              "  (arithmetic on two measurements, not a measurement)\n",
              o_eval_full[0].median() - o_eval_cal[0].median(),
              o_eval_full[1].median() - o_eval_cal[1].median());
  std::printf("\nthe other engine's OWN reported times (medians, us): cold %.1f  hot %.1f  price %.1f  risk %.1f\n",
              t_cold_self.median(), t_hot_self.median(), t_price_self.median(), t_risk_self.median());
  std::printf("  its risk_us EXCLUDES forming the risk operator M; our risk_ladder includes its Jacobian,\n"
              "  so the outer-clock column is the honest one and its own column is the floor.\n");
  for (int fi = 0; fi < 2; ++fi) {
    const solver::ImplicitProgram::RunStats& rs = side[fi].full_warm->last_run();
    std::printf("  ours %-11s tape %zu raw -> %zu after passes; ladder solves=%lld residual_evals=%lld jacobians=%lld\n",
                fi == 0 ? "naive" : "telescoped", side[fi].full.nodes_raw, side[fi].full.nodes_after_passes,
                static_cast<long long>(rs.solves), static_cast<long long>(rs.residual_evaluations),
                static_cast<long long>(rs.jacobians));
  }
  return 0;
}
