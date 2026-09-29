// M3/G5 gate: the Stage A risk ladder O3 — the par delta of every trade, every aggregate and
// the book to every quote through the adjoint over the whole tape with the IFT through the
// implicit blocks (fixtures/stage_a.hpp ladder, one adjoint lane per output) — against forward
// mode (the same problem on Dual<n_quotes>: solver::implicit_dual per block and the book on
// Dual, one pass) at 1e-12 relative to the row's scale, and against bump-and-recalibrate
// (Richardson of 10 bp / 5 bp central differences, two recalibrated program runs each) at 1e-6
// for the book and a few trades, above a noise floor that carries the solver's tolerance (every
// bumped run is recalibrated to ‖F‖∞ < solve_tol, so a PV of scale S carries about
// solve_tol × 30 S of solution noise, on top of the rounding of the PV difference). The two AD modes are timed: the per-trade ladder is n_trades outputs
// by n_quotes inputs, the shape where forward mode is the cheaper one (M4 makes it a rule).
//
// Named *_adjoint_test: a gate of scripts/mutation_test.sh (D33).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <vector>

#include "epykos/solver/implicit_program.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;
using epykos::test::lane_knots;
using epykos::test::stage_a;
using epykos::test::stage_a_tape;

namespace {

constexpr int N = 70;   // the Stage A quote count (22 USD-SOFR + 18 EUR-ESTR + 18 EUR-EURIBOR-3M + 12 EUR-EURIBOR-6M)

struct Risk {
  std::vector<double> out;             // the forward outputs at the quotes
  std::vector<double> trade_rows;      // n_trades × N: the adjoint per-trade ladder
  std::vector<double> aggregate_rows;  // (currencies + netting sets + book) × N
  std::vector<std::vector<double>> knots;   // the solved knots of the adjoint's lane
  double seconds_trades = 0.0, seconds_aggregates = 0.0;
};

const Risk& risk() {
  static const Risk r = [] {
    const fixtures::StageA& s = stage_a();
    const fixtures::StageATape& t = stage_a_tape();
    solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(s, 64));
    Risk x;
    epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
    x.aggregate_rows = fixtures::ladder(prog, s.quotes, t.layout.aggregate_ordinals(), &x.out);
    x.seconds_aggregates = epykos::test::seconds_since(t0);
    x.knots = lane_knots(prog, t, 0);
    t0 = epykos::test::clock_type::now();
    x.trade_rows = fixtures::ladder(prog, s.quotes, t.layout.pv_ordinals());
    x.seconds_trades = epykos::test::seconds_since(t0);
    std::cout << "[  reverse ] ladder of " << t.layout.aggregate_ordinals().size() << " aggregates: " << x.seconds_aggregates << " s; of " << s.n_trades()
              << " trades: " << x.seconds_trades << " s (" << x.seconds_trades / s.n_trades() * 1e3 << " ms per lane); " << prog.last_run().to_string() << '\n';
    return x;
  }();
  return r;
}

struct Forward {
  fixtures::StageAForward<N> f;
  double seconds = 0.0;
};

const Forward& forward() {
  static const Forward fw = [] {
    const fixtures::StageA& s = stage_a();
    const fixtures::StageATape& t = stage_a_tape();
    const Risk& r = risk();
    solver::SolveOptions o;
    o.tol = s.options.solve_tol;
    Forward x;
    const epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
    // At the adjoint's own solution: the tangents are evaluated at the same point.
    x.f = fixtures::forward_stage_a<N>(s, *t.set, s.quotes, s.options.mode, o, &r.knots);
    x.seconds = epykos::test::seconds_since(t0);
    std::cout << "[  forward ] Dual<" << N << "> pass (every trade's par delta to every quote): " << x.seconds << " s;";
    for (std::size_t k = 0; k < x.f.reports.size(); ++k) std::cout << " block " << k << ' ' << x.f.reports[k].iterations << " it";
    std::cout << '\n';
    return x;
  }();
  return fw;
}

}  // namespace

TEST(StageARiskAdjoint, PerTradeLadderMatchesForwardMode) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  const Risk& r = risk();
  const Forward& fw = forward();
  ASSERT_EQ(s.n_quotes(), N);
  const fixtures::StageABook<double> book = fixtures::price_stage_a_at(s, *t.set, r.knots);
  const fixtures::StageAScales scales = fixtures::stage_a_scales(s, book);
  const std::vector<double> frows = fw.f.pv_rows();
  std::size_t compared = 0, failures = 0, value_failures = 0;
  double worst = 0.0;
  std::size_t wi = 0, wk = 0;
  for (int i = 0; i < s.n_trades(); ++i) {
    const std::size_t is = static_cast<std::size_t>(i);
    const double value = r.out[static_cast<std::size_t>(t.layout.pv(i))];
    if (std::fabs(value - fw.f.book.pv[is].v) > 1e-12 * std::max(std::fabs(value), scales.trade[is])) {
      if (value_failures < 5) ADD_FAILURE() << "trade " << i << ": program " << value << " vs Dual value " << fw.f.book.pv[is].v;
      ++value_failures;
    }
    double row_scale = scales.trade[is];
    for (int k = 0; k < N; ++k) row_scale = std::max(row_scale, std::fabs(frows[is * N + static_cast<std::size_t>(k)]));
    for (int k = 0; k < N; ++k) {
      const std::size_t ik = is * N + static_cast<std::size_t>(k);
      const double e = std::fabs(r.trade_rows[ik] - frows[ik]) / row_scale;
      ++compared;
      if (e > worst) {
        worst = e;
        wi = is;
        wk = static_cast<std::size_t>(k);
      }
      if (e > 1e-12) {
        if (failures < 5) ADD_FAILURE() << "trade " << i << " (" << s.instruments[is].id << "), quote " << k << " (" << s.quote_keys[static_cast<std::size_t>(k)] << "): adjoint " << r.trade_rows[ik] << " vs forward " << frows[ik] << " (row scale " << row_scale << ")";
        ++failures;
      }
    }
  }
  std::cout << "[  vs dual ] " << compared << " entries: worst |adjoint - forward| / row scale = " << worst << " at trade " << wi << " (" << s.instruments[wi].id
            << "), quote " << wk << " (" << s.quote_keys[wk] << "); gate 1e-12\n";
  EXPECT_EQ(value_failures, 0u);
  EXPECT_EQ(failures, 0u);
  std::cout << "[  ad mode ] reverse: " << s.n_trades() << " adjoint lanes " << r.seconds_trades << " s; forward: one Dual<" << N << "> pass " << fw.seconds
            << " s -> the per-trade ladder (" << s.n_trades() << " outputs x " << N << " quotes) is cheaper in forward mode by " << r.seconds_trades / fw.seconds
            << "x on this host; the book and aggregate ladders (" << t.layout.aggregate_ordinals().size() << " outputs) took " << r.seconds_aggregates
            << " s in reverse\n";
}

TEST(StageARiskAdjoint, AggregateLaddersAreTheSumsOfTheirTrades) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  const Risk& r = risk();
  const Forward& fw = forward();
  const std::vector<int> agg = t.layout.aggregate_ordinals();
  // Forward mode's aggregates: the tangents of the aggregate Duals.
  std::vector<const epykos::Dual<N>*> ref;
  for (int c = 0; c < s.n_currencies(); ++c) ref.push_back(&fw.f.book.currency_total[static_cast<std::size_t>(c)]);
  for (int n = 0; n < s.n_netting_sets(); ++n) ref.push_back(&fw.f.book.netting_total[static_cast<std::size_t>(n)]);
  ref.push_back(&fw.f.book.book_total);
  ASSERT_EQ(ref.size(), agg.size());
  double worst = 0.0;
  std::size_t failures = 0;
  for (std::size_t a = 0; a < agg.size(); ++a) {
    double row_scale = 0.0;
    for (int k = 0; k < N; ++k) row_scale = std::max(row_scale, std::fabs(ref[a]->d[static_cast<std::size_t>(k)]));
    // Rounding-level residues of cancelling trades are judged at the scale of the trades (D26).
    for (std::size_t i = 0; i < static_cast<std::size_t>(s.n_trades()); ++i) {
      for (int k = 0; k < N; ++k) row_scale = std::max(row_scale, std::fabs(fw.f.book.pv[i].d[static_cast<std::size_t>(k)]) * 1e-3);
    }
    for (int k = 0; k < N; ++k) {
      const double e = std::fabs(r.aggregate_rows[a * N + static_cast<std::size_t>(k)] - ref[a]->d[static_cast<std::size_t>(k)]) / row_scale;
      worst = std::max(worst, e);
      if (e > 1e-12) {
        if (failures < 5) ADD_FAILURE() << "aggregate " << a << ", quote " << k << ": adjoint " << r.aggregate_rows[a * N + static_cast<std::size_t>(k)] << " vs forward " << ref[a]->d[static_cast<std::size_t>(k)];
        ++failures;
      }
    }
  }
  std::cout << "[  aggreg  ] " << agg.size() << " aggregate rows: worst |adjoint - forward| / row scale = " << worst << '\n';
  EXPECT_EQ(failures, 0u);
  // The book ladder in bp: reported.
  const std::vector<double> bp = fixtures::ladder_per_bp(s, std::vector<double>(r.aggregate_rows.end() - N, r.aggregate_rows.end()));
  std::cout << "[  book dv01] per 1 bp:";
  for (int k = 0; k < N; k += 7) std::cout << ' ' << s.quote_keys[static_cast<std::size_t>(k)] << ' ' << bp[static_cast<std::size_t>(k)];
  std::cout << " ...\n";
}

TEST(StageARiskAdjoint, LadderMatchesBumpAndRecalibrate) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  const Risk& r = risk();
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(s, 8));
  // The book and five trades of different kinds (the first of each blueprint met in trade order).
  std::vector<int> ordinals = {t.layout.book};
  std::vector<std::size_t> rows_in_ladder;   // index into the risk rows: n_trades + aggregates
  std::vector<std::string> seen;
  for (int i = 0; i < s.n_trades() && ordinals.size() < 6; ++i) {
    const std::string& bp = s.trades[static_cast<std::size_t>(i)].blueprint;
    if (std::find(seen.begin(), seen.end(), bp) != seen.end()) continue;
    seen.push_back(bp);
    ordinals.push_back(t.layout.pv(i));
  }
  auto ladder_row = [&](std::size_t which, int k) {
    const int o = ordinals[which];
    if (o == t.layout.book) return r.aggregate_rows[(r.aggregate_rows.size() / N - 1) * N + static_cast<std::size_t>(k)];
    return r.trade_rows[static_cast<std::size_t>(o - t.layout.pv0) * N + static_cast<std::size_t>(k)];
  };
  constexpr double eps = std::numeric_limits<double>::epsilon();
  const fixtures::StageABook<double> book = fixtures::price_stage_a_at(s, *t.set, r.knots);
  const fixtures::StageAScales scales = fixtures::stage_a_scales(s, book);
  double worst_rich = 0.0, worst_floor = 0.0;
  std::size_t below = 0, fails = 0, compared = 0;
  std::vector<std::vector<double>> pm;   // the four bumped quote vectors per quote: +h, -h, +h/2, -h/2
  const epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
  for (int k = 0; k < N; ++k) {
    const double h = s.quote_is_price[static_cast<std::size_t>(k)] ? 0.1 : 1e-3;   // 10 bp of rate either way
    pm.clear();
    for (double f : {1.0, -1.0, 0.5, -0.5}) {
      std::vector<double> q = s.quotes;
      q[static_cast<std::size_t>(k)] += f * h;
      pm.push_back(q);
    }
    const std::vector<double> out = fixtures::run_lanes(prog, pm);
    for (std::size_t w = 0; w < ordinals.size(); ++w) {
      const int o = ordinals[w];
      auto at = [&](std::size_t lane) { return out[static_cast<std::size_t>(o) * 4 + lane]; };
      const double d1 = (at(0) - at(1)) / (2.0 * h), d2 = (at(2) - at(3)) / h;
      const double rich = (4.0 * d2 - d1) / 3.0;
      const double adj = ladder_row(w, k);
      const double scale = o == t.layout.book ? scales.book : scales.trade[static_cast<std::size_t>(o - t.layout.pv0)];
      // Rounding of a difference of two PVs of that scale, plus the recalibration's own noise
      // (the solved knots move by up to solve_tol × ‖J⁻¹‖ ~ solve_tol per bump, a PV of scale S
      // by ~30 × solve_tol × S over its duration), over the bump.
      const double floor = (64.0 * eps * scale + 30.0 * s.options.solve_tol * scale) / h;
      const double diff = std::fabs(adj - rich);
      ++compared;
      worst_floor = std::max(worst_floor, diff / floor);
      if (std::fabs(rich) <= floor / 1e-6) {
        ++below;
        if (diff > floor) {
          ++fails;
          ADD_FAILURE() << "output " << o << ", quote " << k << ": adjoint " << adj << " vs bump " << rich << " above the floor " << floor;
        }
        continue;
      }
      worst_rich = std::max(worst_rich, diff / std::fabs(rich));
      if (diff > 1e-6 * std::fabs(rich)) {
        ++fails;
        ADD_FAILURE() << "output " << o << " (" << (o == t.layout.book ? "book" : s.instruments[static_cast<std::size_t>(o - t.layout.pv0)].id) << "), quote " << k << " ("
                      << s.quote_keys[static_cast<std::size_t>(k)] << "): adjoint " << adj << " vs Richardson bump-and-recalibrate " << rich << " (1 bp " << d1 << ", 0.5 bp " << d2 << ")";
      }
    }
  }
  std::cout << "[  bump    ] " << ordinals.size() << " outputs x " << N << " quotes, Richardson of 10 / 5 bp (" << compared << " entries, " << 4 * N << " recalibrated runs in "
            << epykos::test::seconds_since(t0) << " s): worst |adj - fd| / |fd| vs Richardson " << worst_rich << " (gate 1e-6), " << below
            << " entries below the FD noise floor (worst |adj - fd| / floor " << worst_floor << "), " << fails << " failures\n";
  EXPECT_EQ(fails, 0u);
}
