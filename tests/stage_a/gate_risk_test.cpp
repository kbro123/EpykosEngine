// M3/G6 gate: the risk gates of PROBLEM.md section 6 O3 on the Stage A tape, each an explicit
// boolean with its worst number ("[  GATE    ]" lines):
//
//   adjoint_o2_vs_fd / adjoint_o2_vs_dual   the mechanical adjoint of the pricing graph (O2):
//        d out / d (every tape input) through adjoint::Adjoint over the whole program at the
//        record point — the knots are Inputs, so this is the book's sensitivity to the knots,
//        no IFT — for a seeded sample of trades and the aggregates, one lane per output, against
//        central finite differences of the interpreter over the knots (h = 1e-6 in the knot's
//        variable; gate 1e-6 of the row scale) and against forward mode (the book on Dual<70>
//        with the 70 knots as directions; gate 1e-12 of the row scale). The quote and
//        diagnostic entries are asserted zero (the book reads no quote directly).
//   ift_vs_dual                             the IFT ladder (O3) of the same sample against
//        forward_stage_a<70> at the adjoint's own solution (gate 1e-12 of the row scale).
//   ift_vs_bump                             the IFT ladder against bump-and-recalibrate
//        (Richardson of 10 / 5 bp central differences, four recalibrated lanes per quote) on
//        50 seeded (trade, quote) pairs — 50 seeded trades, each paired with a seeded quote
//        among the ones its ladder row is significantly sensitive to (>= 1e-3 of the row's
//        largest entry) — and on the book's whole row (70 quotes); gate 1e-6 relative, above
//        the noise floor of a recalibrated difference (stated as in
//        tests/stage_a/risk_adjoint_test.cpp: the solver's tolerance × 30 × the leg scale, plus
//        64 ulps of the leg scale, over the bump).
//
// Not a gate of scripts/mutation_test.sh (stage_a_risk_adjoint_test is; this one adds the
// whole-program adjoint and the sampled bump grid on top of it).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "epykos/rng/philox.hpp"
#include "epykos/scalar/dual.hpp"
#include "epykos/solver/curve_set.hpp"
#include "epykos/solver/implicit_program.hpp"
#include "stage_a/stage_a_test_helpers.hpp"

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;
using epykos::test::lane_knots;
using epykos::test::stage_a;
using epykos::test::stage_a_tape;

namespace {

constexpr int N = 70;                 // quotes == knots on the Stage A problem
constexpr std::uint64_t gate_seed = 20260923;
constexpr int n_sample_trades = 24;   // trades in the whole-program adjoint sample
constexpr int n_pairs = 50;           // (trade, quote) pairs of the bump gate

// Knot (slot c, k) -> tape input ordinal, and the flat knot index (slot order) of every ordinal.
struct KnotMap {
  std::vector<std::vector<int>> ordinal;   // [slot][k]
  std::vector<int> flat_of_ordinal;        // tape input ordinal -> flat knot index, -1 otherwise
  std::vector<int> slot_of_flat, k_of_flat;
};

KnotMap knot_map() {
  const fixtures::StageATape& t = stage_a_tape();
  KnotMap m;
  m.ordinal.resize(t.record_knots.size());
  m.flat_of_ordinal.assign(t.tape.num_inputs(), -1);
  for (std::size_t b = 0; b < t.registry.blocks.size(); ++b) {
    std::size_t off = 0;
    for (int c : t.block_curves[b]) {
      const std::size_t nk = t.record_knots[static_cast<std::size_t>(c)].size();
      for (std::size_t k = 0; k < nk; ++k) m.ordinal[static_cast<std::size_t>(c)].push_back(t.registry.blocks[b].unknowns[off + k]);
      off += nk;
    }
  }
  int flat = 0;
  for (std::size_t c = 0; c < m.ordinal.size(); ++c) {
    for (std::size_t k = 0; k < m.ordinal[c].size(); ++k, ++flat) {
      m.flat_of_ordinal[static_cast<std::size_t>(m.ordinal[c][k])] = flat;
      m.slot_of_flat.push_back(static_cast<int>(c));
      m.k_of_flat.push_back(static_cast<int>(k));
    }
  }
  return m;
}

std::vector<int> sample_trades(int n, std::uint64_t substream) {
  const fixtures::StageA& s = stage_a();
  epykos::rng::Philox g(gate_seed, substream);
  std::set<int> picked;
  while (static_cast<int>(picked.size()) < n) picked.insert(static_cast<int>(g.uniform_range(0.0, static_cast<double>(s.n_trades()))) % s.n_trades());
  return std::vector<int>(picked.begin(), picked.end());
}

// The sampled outputs of the O2 adjoint gates: n_sample_trades trade PVs, the currency totals,
// two netting sets and the book.
std::vector<int> o2_sample() {
  const fixtures::StageATape& t = stage_a_tape();
  std::vector<int> ordinals;
  for (int i : sample_trades(n_sample_trades, 1)) ordinals.push_back(t.layout.pv(i));
  for (int c = 0; c < t.layout.n_currencies; ++c) ordinals.push_back(t.layout.currency(c));
  ordinals.push_back(t.layout.netting(0));
  ordinals.push_back(t.layout.netting(t.layout.n_netting_sets - 1));
  ordinals.push_back(t.layout.book);
  return ordinals;
}

const char* name_of(int ordinal) {
  const fixtures::StageATape& t = stage_a_tape();
  if (ordinal == t.layout.book) return "book";
  if (ordinal >= t.layout.currency0 && ordinal < t.layout.currency0 + t.layout.n_currencies) return "currency";
  if (ordinal >= t.layout.netting0 && ordinal < t.layout.netting0 + t.layout.n_netting_sets) return "netting";
  return "pv";
}

}  // namespace

TEST(StageAGateRisk, WholeProgramAdjointOfO2MatchesFiniteDifferencesAndForwardMode) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  const KnotMap km = knot_map();
  ASSERT_EQ(km.slot_of_flat.size(), static_cast<std::size_t>(N));
  const std::vector<int> ordinals = o2_sample();
  const int B = static_cast<int>(ordinals.size());
  ASSERT_LE(B, 64);
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(s, 64));
  const int n_in = prog.n_inputs(), n_out = prog.n_outputs();
  // The record point: the full input vector (quotes, solved knots, diagnostics).
  const std::vector<double> x = t.tape.input_values();
  ASSERT_EQ(static_cast<int>(x.size()), n_in);
  // The whole-program adjoint, one lane per sampled output.
  std::vector<double> state(static_cast<std::size_t>(n_in) * static_cast<std::size_t>(B)), out_bar(static_cast<std::size_t>(n_out) * static_cast<std::size_t>(B), 0.0),
      out(static_cast<std::size_t>(n_out) * static_cast<std::size_t>(B)), state_bar(static_cast<std::size_t>(n_in) * static_cast<std::size_t>(B));
  for (int k = 0; k < n_in; ++k) {
    for (int b = 0; b < B; ++b) state[static_cast<std::size_t>(k) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = x[static_cast<std::size_t>(k)];
  }
  for (int b = 0; b < B; ++b) out_bar[static_cast<std::size_t>(ordinals[static_cast<std::size_t>(b)]) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = 1.0;
  epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
  prog.whole_adjoint().run(state.data(), B, out_bar.data(), out.data(), state_bar.data());
  const double t_adj = epykos::test::seconds_since(t0);
  auto adj = [&](int b, int ordinal) { return state_bar[static_cast<std::size_t>(ordinal) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)]; };
  // Every non-knot input has a zero adjoint for a book output (the quotes and the diagnostics).
  std::size_t nonzero_free = 0;
  for (int b = 0; b < B; ++b) {
    for (int k = 0; k < n_in; ++k) {
      if (km.flat_of_ordinal[static_cast<std::size_t>(k)] < 0 && adj(b, k) != 0.0) ++nonzero_free;
    }
  }
  EXPECT_EQ(nonzero_free, 0u) << "a book output has a non-zero adjoint to a quote or a diagnostic";
  // Forward mode: the book on Dual<N> with the knots as directions (flat knot index = direction).
  t0 = epykos::test::clock_type::now();
  using D = epykos::Dual<N>;
  solver::CurveStates<D> st;
  st.specs = &t.set->curves();
  st.resize(static_cast<std::size_t>(s.n_curves()));
  {
    int flat = 0;
    for (int c = 0; c < s.n_curves(); ++c) {
      std::vector<D> z;
      for (std::size_t k = 0; k < km.ordinal[static_cast<std::size_t>(c)].size(); ++k, ++flat) z.push_back(D::variable(x[static_cast<std::size_t>(km.ordinal[static_cast<std::size_t>(c)][k])], flat));
      st.set(c, z);
    }
  }
  fixtures::DfMemo<D, std::function<D(int, double)>> memo([&st](int slot, double t_) { return st.df(slot, t_); });
  const fixtures::StageABook<D> fw = fixtures::price_stage_a<D>(s, memo);
  const double t_fwd = epykos::test::seconds_since(t0);
  auto fwd_of = [&](int ordinal) -> const D& {
    if (ordinal == t.layout.book) return fw.book_total;
    if (ordinal >= t.layout.currency0 && ordinal < t.layout.currency0 + t.layout.n_currencies) return fw.currency_total[static_cast<std::size_t>(ordinal - t.layout.currency0)];
    if (ordinal >= t.layout.netting0 && ordinal < t.layout.netting0 + t.layout.n_netting_sets) return fw.netting_total[static_cast<std::size_t>(ordinal - t.layout.netting0)];
    return fw.pv[static_cast<std::size_t>(ordinal - t.layout.pv0)];
  };
  // Central finite differences over the knots through the interpreter: 2N lanes of the full state.
  t0 = epykos::test::clock_type::now();
  const double h = 1e-6;
  std::vector<double> fd(static_cast<std::size_t>(B) * static_cast<std::size_t>(N), 0.0);
  {
    const int chunk = prog.interpreter().max_batch();
    std::vector<double> st2, o2;
    for (int j0 = 0; j0 < 2 * N; j0 += chunk) {
      const int L = std::min(chunk, 2 * N - j0);
      st2.assign(static_cast<std::size_t>(n_in) * static_cast<std::size_t>(L), 0.0);
      o2.assign(static_cast<std::size_t>(n_out) * static_cast<std::size_t>(L), 0.0);
      for (int l = 0; l < L; ++l) {
        const int j = (j0 + l) / 2;
        const double sign = (j0 + l) % 2 == 0 ? 1.0 : -1.0;
        const int ord = km.ordinal[static_cast<std::size_t>(km.slot_of_flat[static_cast<std::size_t>(j)])][static_cast<std::size_t>(km.k_of_flat[static_cast<std::size_t>(j)])];
        for (int k = 0; k < n_in; ++k) st2[static_cast<std::size_t>(k) * static_cast<std::size_t>(L) + static_cast<std::size_t>(l)] = x[static_cast<std::size_t>(k)];
        st2[static_cast<std::size_t>(ord) * static_cast<std::size_t>(L) + static_cast<std::size_t>(l)] += sign * h;
      }
      prog.interpreter().run(st2.data(), L, o2.data());
      for (int l = 0; l + 1 < L; l += 2) {
        const int j = (j0 + l) / 2;
        for (int b = 0; b < B; ++b) {
          const std::size_t o = static_cast<std::size_t>(ordinals[static_cast<std::size_t>(b)]);
          const double plus = o2[o * static_cast<std::size_t>(L) + static_cast<std::size_t>(l)], minus = o2[o * static_cast<std::size_t>(L) + static_cast<std::size_t>(l + 1)];
          fd[static_cast<std::size_t>(b) * N + static_cast<std::size_t>(j)] = (plus - minus) / (2.0 * h);
        }
      }
    }
  }
  const double t_fd = epykos::test::seconds_since(t0);
  // Compare, per sampled output, against the row scale (the largest forward-mode entry of the row).
  double worst_fd = 0.0, worst_dual = 0.0, worst_value = 0.0;
  int wfd_b = -1, wfd_j = -1, wd_b = -1, wd_j = -1;
  std::size_t compared = 0, fails_fd = 0, fails_dual = 0;
  for (int b = 0; b < B; ++b) {
    const int o = ordinals[static_cast<std::size_t>(b)];
    const D& f = fwd_of(o);
    double row_scale = 0.0;
    for (int j = 0; j < N; ++j) row_scale = std::max(row_scale, std::fabs(f.d[static_cast<std::size_t>(j)]));
    ASSERT_GT(row_scale, 0.0) << "output " << o << " has no knot sensitivity";
    worst_value = std::max(worst_value, std::fabs(out[static_cast<std::size_t>(o) * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] - f.v) / std::max(std::fabs(f.v), row_scale * 1e-3));
    for (int j = 0; j < N; ++j) {
      const int ord = km.ordinal[static_cast<std::size_t>(km.slot_of_flat[static_cast<std::size_t>(j)])][static_cast<std::size_t>(km.k_of_flat[static_cast<std::size_t>(j)])];
      const double a = adj(b, ord);
      const double efd = std::fabs(a - fd[static_cast<std::size_t>(b) * N + static_cast<std::size_t>(j)]) / row_scale;
      const double edual = std::fabs(a - f.d[static_cast<std::size_t>(j)]) / row_scale;
      ++compared;
      if (efd > worst_fd) { worst_fd = efd; wfd_b = b; wfd_j = j; }
      if (edual > worst_dual) { worst_dual = edual; wd_b = b; wd_j = j; }
      if (efd > 1e-6) {
        if (fails_fd < 5) ADD_FAILURE() << name_of(o) << " output " << o << ", knot " << j << ": adjoint " << a << " vs FD " << fd[static_cast<std::size_t>(b) * N + static_cast<std::size_t>(j)] << " (row scale " << row_scale << ")";
        ++fails_fd;
      }
      if (edual > 1e-12) {
        if (fails_dual < 5) ADD_FAILURE() << name_of(o) << " output " << o << ", knot " << j << ": adjoint " << a << " vs Dual " << f.d[static_cast<std::size_t>(j)] << " (row scale " << row_scale << ")";
        ++fails_dual;
      }
    }
  }
  std::cout << "[  o2 adj  ] " << B << " outputs (" << n_sample_trades << " trades, " << t.layout.n_currencies << " currencies, 2 netting sets, the book) x " << N
            << " knots: whole-program adjoint " << t_adj << " s (" << B << " lanes), Dual<" << N << "> book " << t_fwd << " s, FD " << 2 * N << " interpreter lanes " << t_fd
            << " s; worst forward value mismatch " << worst_value << '\n';
  std::cout << "[  GATE    ] adjoint_o2_vs_fd ok=" << (fails_fd == 0 ? 1 : 0) << " worst=" << worst_fd << " at_output=" << (wfd_b >= 0 ? ordinals[static_cast<std::size_t>(wfd_b)] : -1)
            << " knot=" << wfd_j << " h=" << h << " gate=1e-6 entries=" << compared << '\n';
  std::cout << "[  GATE    ] adjoint_o2_vs_dual ok=" << (fails_dual == 0 ? 1 : 0) << " worst=" << worst_dual << " at_output=" << (wd_b >= 0 ? ordinals[static_cast<std::size_t>(wd_b)] : -1)
            << " knot=" << wd_j << " gate=1e-12 entries=" << compared << '\n';
  EXPECT_EQ(fails_fd, 0u);
  EXPECT_EQ(fails_dual, 0u);
  EXPECT_LT(worst_value, 1e-12) << "the forward pass of the adjoint differs from the Dual value";
}

TEST(StageAGateRisk, IftLadderOnSampledPairsMatchesBumpAndRecalibrateAndForwardMode) {
  const fixtures::StageA& s = stage_a();
  const fixtures::StageATape& t = stage_a_tape();
  // 50 seeded trades; the book's whole row. Each trade's quote is drawn below among the quotes
  // it is significantly sensitive to (a EUR trade against a USD quote is zero on both sides and
  // would test nothing).
  const std::vector<int> trades = sample_trades(n_pairs, 2);
  std::vector<int> ordinals = {t.layout.book};
  for (int i : trades) ordinals.push_back(t.layout.pv(i));
  auto row_of = [&](int ordinal) {
    for (std::size_t r = 0; r < ordinals.size(); ++r) {
      if (ordinals[r] == ordinal) return r;
    }
    return std::size_t(-1);
  };
  // The IFT ladder (one adjoint lane per output) at the quotes.
  solver::ImplicitProgram prog(t.tape, t.registry, fixtures::stage_a_program_options(s, 64));
  std::vector<double> out;
  epykos::test::clock_type::time_point t0 = epykos::test::clock_type::now();
  const std::vector<double> rows = fixtures::ladder(prog, s.quotes, ordinals, &out);
  const double t_ladder = epykos::test::seconds_since(t0);
  epykos::rng::Philox g(gate_seed, 4);
  std::vector<std::pair<int, int>> pairs;   // (trade, quote)
  for (std::size_t r = 1; r < ordinals.size(); ++r) {
    double row_max = 0.0;
    for (int k = 0; k < N; ++k) row_max = std::max(row_max, std::fabs(rows[r * N + static_cast<std::size_t>(k)]));
    std::vector<int> significant;
    for (int k = 0; k < N; ++k) {
      if (std::fabs(rows[r * N + static_cast<std::size_t>(k)]) >= 1e-3 * row_max) significant.push_back(k);
    }
    ASSERT_FALSE(significant.empty()) << "trade " << trades[r - 1] << " has no quote sensitivity";
    const int pick = static_cast<int>(g.uniform_range(0.0, static_cast<double>(significant.size()))) % static_cast<int>(significant.size());
    pairs.emplace_back(trades[r - 1], significant[static_cast<std::size_t>(pick)]);
  }
  const std::vector<std::vector<double>> knots = lane_knots(prog, t, 0);
  const fixtures::StageABook<double> book = fixtures::price_stage_a_at(s, *t.set, knots);
  const fixtures::StageAScales scales = fixtures::stage_a_scales(s, book);
  // Forward mode at the adjoint's own solution.
  solver::SolveOptions so;
  so.tol = s.options.solve_tol;
  t0 = epykos::test::clock_type::now();
  const fixtures::StageAForward<N> fw = fixtures::forward_stage_a<N>(s, *t.set, s.quotes, s.options.mode, so, &knots);
  const double t_fwd = epykos::test::seconds_since(t0);
  double worst_dual = 0.0;
  std::size_t fails_dual = 0, compared_dual = 0;
  int wd_o = -1, wd_k = -1;
  for (std::size_t r = 0; r < ordinals.size(); ++r) {
    const int o = ordinals[r];
    const epykos::Dual<N>& f = o == t.layout.book ? fw.book.book_total : fw.book.pv[static_cast<std::size_t>(o - t.layout.pv0)];
    double row_scale = o == t.layout.book ? scales.book : scales.trade[static_cast<std::size_t>(o - t.layout.pv0)];
    for (int k = 0; k < N; ++k) row_scale = std::max(row_scale, std::fabs(f.d[static_cast<std::size_t>(k)]));
    for (int k = 0; k < N; ++k) {
      const double e = std::fabs(rows[r * N + static_cast<std::size_t>(k)] - f.d[static_cast<std::size_t>(k)]) / row_scale;
      ++compared_dual;
      if (e > worst_dual) { worst_dual = e; wd_o = o; wd_k = k; }
      if (e > 1e-12) {
        if (fails_dual < 5) ADD_FAILURE() << name_of(o) << " output " << o << ", quote " << k << ": IFT adjoint " << rows[r * N + static_cast<std::size_t>(k)] << " vs Dual " << f.d[static_cast<std::size_t>(k)];
        ++fails_dual;
      }
    }
  }
  std::cout << "[  GATE    ] ift_vs_dual ok=" << (fails_dual == 0 ? 1 : 0) << " worst=" << worst_dual << " at_output=" << wd_o << " quote=" << wd_k << " gate=1e-12 entries=" << compared_dual
            << " rows=" << ordinals.size() << '\n';
  EXPECT_EQ(fails_dual, 0u);
  // Bump-and-recalibrate: every quote for the book's row, the pairs' quotes for the trades;
  // four recalibrated lanes per quote (+h, -h, +h/2, -h/2), Richardson of the two central differences.
  solver::ImplicitProgram bump(t.tape, t.registry, fixtures::stage_a_program_options(s, 8));
  constexpr double eps = std::numeric_limits<double>::epsilon();
  double worst_rel = 0.0, worst_floor = 0.0;
  std::size_t compared = 0, below = 0, fails = 0, runs = 0;
  int wr_o = -1, wr_k = -1;
  t0 = epykos::test::clock_type::now();
  for (int k = 0; k < N; ++k) {
    std::vector<std::size_t> want = {0};   // the book's row, every quote
    for (const auto& p : pairs) {
      if (p.second == k) want.push_back(row_of(t.layout.pv(p.first)));
    }
    const double h = s.quote_is_price[static_cast<std::size_t>(k)] ? 0.1 : 1e-3;   // 10 bp of rate either way
    std::vector<std::vector<double>> pm;
    for (double f : {1.0, -1.0, 0.5, -0.5}) {
      std::vector<double> q = s.quotes;
      q[static_cast<std::size_t>(k)] += f * h;
      pm.push_back(q);
    }
    const std::vector<double> o4 = fixtures::run_lanes(bump, pm);
    runs += 4;
    for (int b = 0; b < 4; ++b) {
      for (int blk = 0; blk < bump.n_blocks(); ++blk) EXPECT_TRUE(bump.report(blk, b).converged) << "quote " << k << " bump lane " << b << " block " << blk;
    }
    for (std::size_t r : want) {
      const int o = ordinals[r];
      auto at = [&](std::size_t lane) { return o4[static_cast<std::size_t>(o) * 4 + lane]; };
      const double d1 = (at(0) - at(1)) / (2.0 * h), d2 = (at(2) - at(3)) / h;
      const double rich = (4.0 * d2 - d1) / 3.0;
      const double a = rows[r * N + static_cast<std::size_t>(k)];
      const double scale = o == t.layout.book ? scales.book : scales.trade[static_cast<std::size_t>(o - t.layout.pv0)];
      const double floor = (64.0 * eps * scale + 30.0 * s.options.solve_tol * scale) / h;
      const double diff = std::fabs(a - rich);
      ++compared;
      if (std::fabs(rich) <= floor / 1e-6) {
        ++below;
        worst_floor = std::max(worst_floor, diff / floor);
        if (diff > floor) {
          ++fails;
          ADD_FAILURE() << name_of(o) << " output " << o << ", quote " << k << ": adjoint " << a << " vs bump " << rich << " above the noise floor " << floor;
        }
        continue;
      }
      const double rel = diff / std::fabs(rich);
      if (rel > worst_rel) { worst_rel = rel; wr_o = o; wr_k = k; }
      if (rel > 1e-6) {
        ++fails;
        ADD_FAILURE() << name_of(o) << " output " << o << " (" << (o == t.layout.book ? "book" : s.instruments[static_cast<std::size_t>(o - t.layout.pv0)].id) << "), quote " << k << " ("
                      << s.quote_keys[static_cast<std::size_t>(k)] << "): IFT adjoint " << a << " vs Richardson bump-and-recalibrate " << rich << " (10 bp " << d1 << ", 5 bp " << d2 << ")";
      }
    }
  }
  const double t_bump = epykos::test::seconds_since(t0);
  std::cout << "[  bump    ] " << n_pairs << " (trade, quote) pairs over " << trades.size() << " trades + the book's " << N << " quotes: " << compared << " entries, " << runs
            << " recalibrated lanes in " << t_bump << " s; " << below << " entries below the FD noise floor (worst |adj - fd| / floor " << worst_floor << "); ladder " << t_ladder
            << " s (" << ordinals.size() << " adjoint lanes), Dual<" << N << "> pass " << t_fwd << " s\n";
  std::cout << "[  GATE    ] ift_vs_bump ok=" << (fails == 0 ? 1 : 0) << " worst=" << worst_rel << " at_output=" << wr_o << " quote=" << wr_k << " gate=1e-6 entries=" << compared << " below_floor=" << below
            << " pairs=" << n_pairs << '\n';
  EXPECT_EQ(fails, 0u);
}
