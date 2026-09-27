// M1/P3 gate: the inferred domain chain of the M1 book has the structure docs/WORKLOADS.md §M1
// expects — Knots —affine→ Times —exp→ DF —gather→ Coupons —segment_sum→ Legs → Swaps → Book —
// with the seasoned-first coupons outside the float class. The pass is told nothing about the
// book; every count below is derived from the book's row table.
//
// Observed classes (D22): the three non-interpolated times (t = 0, t = 1 on knot 4 and
// t = 10958/365 beyond the last knot) read the Inputs directly, so the DF domain gathers from
// both the affine domain and the Input domain; the seasoned-first coupons N·τ·R·DF(e) have the
// same op tree as the fixed coupons N·τ·K·DF(e) and share their class; the swaps with T = 1 have
// no leg Sum and form a level-0 domain of the swap class; legs, swaps and the book are one
// class cycle split into levels.
//
// RE-ANCHORED 2026-09-27 (PRINCIPLES.md §5.2a case 3 — the algebra changed the structure these
// assertions were about). `epykos::compile` now runs `simplify` before the data-movement passes,
// and ONE of its peepholes, `mul(x, 1) -> x`, reshapes this book. It fires in exactly two places:
//
//   * the discount factor at t = 1 (on knot 4). `exp(-z·t)` with t == 1.0 loses its Mul, so that
//     one row leaves the DF class `exp(mul(neg(@0),$0))` and becomes a one-row class of its own,
//     `exp(neg(@0))`. Every other distinct time keeps its Mul.
//   * the side multiply. A swap is `side·(fixed − float)` with side == ±1; the 515 swaps with
//     side == +1 lose the Mul and are just `sub(@0,@1)`, while the 485 with side == −1 keep one
//     whose factor is now the UNIFORM literal −1, i.e. `mul(#0,@0)` — the same op tree as the
//     constant-rate coupon `mul($0,@0)`, so those two classes MERGE. That merge pulls the coupon
//     domain into the leg/swap/book class cycle and re-levels the leg Sums.
//
// Measured on this fixture, data movement only -> with the algebra: the tape is 75,698 -> 75,182
// nodes (516 Muls removed: 515 sides + the t = 1 time) and the emitted Program is 10 -> 14
// domains, 8 -> 9 classes, 42,314 -> 42,799 values. Values RISE while nodes FALL because the
// side == −1 swaps now need two rows (a `sub` row and a `mul(−1, ·)` row) where the fused
// `mul($0,sub(@0,@1))` needed one: 485 of them, exactly the difference.
//
// What each test below used to assert, and why it moved, is recorded at the test.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/tape/tape.hpp"

namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;
using epykos::Op;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

struct Fixture {
  fixtures::Book book;
  ir::Program program;
  ir::InferStats stats;
  // Book-derived counts.
  std::size_t n_fixed = 0;          // Σ T_i
  std::size_t n_realised = 0;       // seasoned first float coupons
  std::size_t n_float_plain = 0;    // float coupons with a forward
  std::size_t n_two_plus = 0;       // swaps with T >= 2
  std::size_t n_one = 0;            // swaps with T == 1
  std::vector<double> df_times;     // distinct discount times
  std::size_t n_interior = 0;       // interpolated (not on a knot, inside the knot range)
  std::size_t n_forwards = 0;       // distinct (s, e) pairs of plain float coupons
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = fixtures::make_m1_book();
    x.program = ir::infer(fixtures::record_m1(x.book), &x.stats);
    const fixtures::Book& b = x.book;
    for (int i = 0; i < b.n_swaps; ++i) {
      x.n_fixed += static_cast<std::size_t>(b.tenor[static_cast<std::size_t>(i)]);
      x.n_two_plus += (b.tenor[static_cast<std::size_t>(i)] >= 2);
      x.n_one += (b.tenor[static_cast<std::size_t>(i)] == 1);
    }
    std::vector<std::pair<int, int>> pairs;
    for (int r = 0; r < b.n_rows; ++r) {
      const auto s = static_cast<std::size_t>(r);
      x.df_times.push_back(b.row_t_end[s]);
      if (b.row_leg[s] == fixtures::float_leg) {
        if (b.row_is_realised_first[s]) {
          ++x.n_realised;
        } else {
          ++x.n_float_plain;
          x.df_times.push_back(b.row_t_start[s]);
          pairs.emplace_back(b.row_start_day[s], b.row_end_day[s]);
        }
      }
    }
    std::sort(x.df_times.begin(), x.df_times.end());
    x.df_times.erase(std::unique(x.df_times.begin(), x.df_times.end()), x.df_times.end());
    for (double t : x.df_times) {
      if (!(t > b.knot_t.front() && t < b.knot_t.back())) continue;
      if (std::find(b.knot_t.begin(), b.knot_t.end(), t) != b.knot_t.end()) continue;
      ++x.n_interior;
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    x.n_forwards = pairs.size();
    return x;
  }();
  return f;
}

// The domains whose shape (without the level suffix) is `shape`.
std::vector<ir::domain_id> domains_named(const ir::Program& p, const std::string& shape) {
  std::vector<ir::domain_id> out;
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    if (ir::shape_string(p, static_cast<ir::domain_id>(d)) == shape) out.push_back(static_cast<ir::domain_id>(d));
  }
  return out;
}

ir::domain_id the_domain(const ir::Program& p, const std::string& shape) {
  const std::vector<ir::domain_id> ds = domains_named(p, shape);
  EXPECT_EQ(ds.size(), 1u) << "expected exactly one domain " << shape;
  return ds.empty() ? -1 : ds.front();
}

// Sorted bit patterns of a column.
std::vector<std::uint64_t> sorted_bits(const std::vector<double>& v) {
  std::vector<std::uint64_t> out;
  out.reserve(v.size());
  for (double x : v) out.push_back(bits(x));
  std::sort(out.begin(), out.end());
  return out;
}

const ir::Column& column_of(const ir::Program& p, const ir::Slot& s) {
  EXPECT_EQ(s.kind, ir::SlotKind::Column);
  return p.columns[static_cast<std::size_t>(s.index)];
}

const ir::Gather& gather_of(const ir::Program& p, const ir::Slot& s) {
  EXPECT_EQ(s.kind, ir::SlotKind::Gather);
  return p.gathers[static_cast<std::size_t>(s.index)];
}

// Rows of `src` referenced by `g`, per source domain.
std::size_t reads_from(const ir::Program& p, const ir::Gather& g, ir::domain_id src) {
  std::size_t n = 0;
  for (ir::value_id v : g.index) n += (p.domain_of(v) == src);
  return n;
}

}  // namespace

TEST(IrDomainChain, PrintsTheChain) {
  const Fixture& f = fixture();
  std::cout << "[  chain   ] M1 book\n" << ir::to_string(f.program);
  std::cout << "[  counts  ] fixed " << f.n_fixed << ", realised-first " << f.n_realised << ", float " << f.n_float_plain
            << ", T>=2 " << f.n_two_plus << ", T==1 " << f.n_one << ", distinct times " << f.df_times.size()
            << ", interior " << f.n_interior << ", forwards " << f.n_forwards << '\n';
  ::testing::Test::RecordProperty("domains", std::to_string(f.program.domains.size()));
  ::testing::Test::RecordProperty("values", std::to_string(f.program.num_values()));
}

// Was `NoScanDomainsAndTenDomains`, asserting 10 domains and 8 classes. The algebra split the DF
// class in two and the swap class in two, and merged one of those halves with the coupon class:
// 9 classes over 14 domains (the file header has the arithmetic). No scan either way.
TEST(IrDomainChain, NoScanDomainsAndFourteenDomains) {
  const Fixture& f = fixture();
  EXPECT_TRUE(ir::recurrent_domains(f.program).empty());
  EXPECT_TRUE(ir::scan_class_domains(f.program).empty()) << "no class of the M1 book reads itself (D16, D22)";

  EXPECT_EQ(f.program.domains.size(), 14u);
  // input, affine, DF, DF at t == 1, const-rate coupon AND the negated swap (one class),
  // forward, float coupon, sum, the un-negated swap.
  EXPECT_EQ(f.stats.classes, 9u);
  // sum splits into three levels (two leg levels, the book) and the two swap classes into two
  // each (T == 1 / T >= 2).
  EXPECT_EQ(f.stats.domains, 14u);
  EXPECT_EQ(f.program.inputs.size(), 12u);
  EXPECT_EQ(f.program.outputs.size(), 1001u);
  ASSERT_NO_THROW(ir::validate(f.program));
}

TEST(IrDomainChain, KnotsToTimesIsOneAffineDomainOverTheInputs) {
  const Fixture& f = fixture();
  const ir::Program& p = f.program;
  const ir::domain_id din = the_domain(p, "input");
  ASSERT_GE(din, 0);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(din)].rows, 12);
  for (std::size_t k = 0; k < 12; ++k) EXPECT_EQ(p.inputs[k], ir::value_of(p, din, static_cast<ir::row_id>(k)));

  const ir::domain_id daff = the_domain(p, "affine(#0;%0)");
  ASSERT_GE(daff, 0);
  const ir::Domain& dom = p.domains[static_cast<std::size_t>(daff)];
  EXPECT_EQ(static_cast<std::size_t>(dom.rows), f.n_interior) << "one affine row per interpolated time";
  EXPECT_EQ(dom.reads, std::vector<ir::domain_id>{din});
  const ir::Step& step = p.groups[static_cast<std::size_t>(daff)].steps.back();
  ASSERT_EQ(step.op, Op::Affine);
  ASSERT_EQ(step.konst.kind, ir::SlotKind::Literal);
  EXPECT_EQ(bits(p.literals[static_cast<std::size_t>(step.konst.index)]), bits(-0.0));
  const ir::Segment& seg = p.segments[static_cast<std::size_t>(step.a.index)];
  for (std::size_t r = 0; r + 1 < seg.offsets.size(); ++r) EXPECT_EQ(seg.offsets[r + 1] - seg.offsets[r], 2) << r;
  ASSERT_EQ(seg.coefs.size(), seg.members.size());
  for (std::size_t m = 0; m < seg.members.size(); ++m) {
    EXPECT_EQ(p.domain_of(seg.members[m]), din);
    EXPECT_GT(seg.coefs[m], 0.0);
    EXPECT_LT(seg.coefs[m], 1.0);
  }
  // Each row's two weights are (1 - w, w) on neighbouring knots.
  for (std::size_t r = 0; r + 1 < seg.offsets.size(); ++r) {
    const std::size_t m0 = static_cast<std::size_t>(seg.offsets[r]);
    EXPECT_EQ(p.row_of(seg.members[m0 + 1]), p.row_of(seg.members[m0]) + 1) << r;
    EXPECT_EQ(bits(seg.coefs[m0]), bits(1.0 - seg.coefs[m0 + 1])) << r;
  }
}

// Used to assert ONE domain `exp(mul(neg(@0),$0))` with one row per distinct discount time, its
// time column equal to the book's distinct times and three of its rows reading a knot directly.
// `mul(x, 1) -> x` took the t = 1 row out of that class: it is now the one-row domain
// `exp(neg(@0))`, with no time column at all (the time is gone, not folded to a literal). The
// invariant — one Exp per distinct discount time, and the times are the book's — is re-stated
// over the two domains together.
TEST(IrDomainChain, TimesDomainHasOneExpPerDistinctTime) {
  const Fixture& f = fixture();
  const ir::Program& p = f.program;
  const ir::domain_id din = the_domain(p, "input");
  const ir::domain_id daff = the_domain(p, "affine(#0;%0)");
  const ir::domain_id ddf = the_domain(p, "exp(mul(neg(@0),$0))");
  const ir::domain_id ddf1 = the_domain(p, "exp(neg(@0))");
  ASSERT_GE(ddf, 0);
  ASSERT_GE(ddf1, 0);
  const ir::Domain& dom = p.domains[static_cast<std::size_t>(ddf)];
  const ir::Domain& dom1 = p.domains[static_cast<std::size_t>(ddf1)];
  EXPECT_EQ(dom1.rows, 1) << "exactly one discount time is t == 1";
  EXPECT_EQ(static_cast<std::size_t>(dom.rows + dom1.rows), f.df_times.size()) << "one Exp per distinct time";
  EXPECT_EQ(dom.reads, (std::vector<ir::domain_id>{din, daff}));
  EXPECT_EQ(dom1.reads, std::vector<ir::domain_id>{din}) << "t == 1 is on knot 4, so it reads the Input";
  const ir::Group& g = p.groups[static_cast<std::size_t>(ddf)];
  ASSERT_EQ(g.steps.size(), 3u);
  EXPECT_EQ(g.steps[0].op, Op::Neg);
  EXPECT_EQ(g.steps[1].op, Op::Mul);
  EXPECT_EQ(g.steps[2].op, Op::Exp);
  const ir::Group& g1 = p.groups[static_cast<std::size_t>(ddf1)];
  ASSERT_EQ(g1.steps.size(), 2u) << "no Mul: `mul(neg(z), 1.0)` simplified to `neg(z)`";
  EXPECT_EQ(g1.steps[0].op, Op::Neg);
  EXPECT_EQ(g1.steps[1].op, Op::Exp);
  const ir::Gather& zt = gather_of(p, g.steps[0].a);
  const ir::Gather& zt1 = gather_of(p, g1.steps[0].a);
  EXPECT_EQ(reads_from(p, zt, daff), f.n_interior);
  EXPECT_EQ(reads_from(p, zt, din) + reads_from(p, zt1, din), f.df_times.size() - f.n_interior)
      << "the non-interpolated times read a knot";
  EXPECT_EQ(f.df_times.size() - f.n_interior, 3u);
  // The column is the time t: with 1.0 (the time the other domain no longer carries) put back,
  // the same set as the book's distinct discount times.
  std::vector<double> times = column_of(p, g.steps[1].b).values;
  times.push_back(1.0);
  EXPECT_EQ(sorted_bits(times), sorted_bits(f.df_times));
}

// Unchanged in substance. The only assertions that moved are the three `reads` lists: every
// coupon class now also reads the one-row `exp(neg(@0))` domain the algebra split off the DF
// class, because the coupons whose discount time is t = 1 gather their DF from there.
TEST(IrDomainChain, CouponClassesPartitionTheRowsWithSeasonedFirstOutsideFloat) {
  const Fixture& f = fixture();
  const ir::Program& p = f.program;
  const fixtures::Book& b = f.book;
  const ir::domain_id ddf = the_domain(p, "exp(mul(neg(@0),$0))");
  const ir::domain_id ddf1 = the_domain(p, "exp(neg(@0))");
  const ir::domain_id dfix = the_domain(p, "mul($0,@0)");              // N·τ·K·DF(e) and N·τ·R·DF(e)
  const ir::domain_id dfwd = the_domain(p, "div(sub(div(@0,@1),#0),$0)");  // (DF(s)/DF(e) − 1)/τ
  const ir::domain_id dflt = the_domain(p, "mul(mul($0,@0),@1)");      // N·τ·fwd·DF(e)
  ASSERT_GE(dfix, 0);
  ASSERT_GE(dfwd, 0);
  ASSERT_GE(dflt, 0);
  const ir::Domain& fix = p.domains[static_cast<std::size_t>(dfix)];
  const ir::Domain& fwd = p.domains[static_cast<std::size_t>(dfwd)];
  const ir::Domain& flt = p.domains[static_cast<std::size_t>(dflt)];

  // Row counts add up: every coupon row of the book is in exactly one coupon class.
  EXPECT_EQ(static_cast<std::size_t>(fix.rows), f.n_fixed + f.n_realised);
  EXPECT_EQ(static_cast<std::size_t>(flt.rows), f.n_float_plain);
  EXPECT_EQ(static_cast<std::size_t>(fix.rows + flt.rows), static_cast<std::size_t>(b.n_rows));
  EXPECT_EQ(static_cast<std::size_t>(fwd.rows), f.n_forwards) << "one forward per distinct (s, e) pair";
  EXPECT_EQ(fix.reads, (std::vector<ir::domain_id>{ddf, ddf1}));
  EXPECT_EQ(fwd.reads, (std::vector<ir::domain_id>{ddf, ddf1}));
  EXPECT_EQ(flt.reads, (std::vector<ir::domain_id>{ddf, ddf1, dfwd}));

  // The constant-rate class holds exactly the fixed coupons (N·τ·K) and the seasoned-first
  // coupons (N·τ·R): its column is that multiset, computed here exactly as the maths does.
  std::vector<double> expect_fix, expect_flt_ntau, expect_fwd_tau;
  for (int r = 0; r < b.n_rows; ++r) {
    const auto s = static_cast<std::size_t>(r);
    const auto i = static_cast<std::size_t>(b.row_swap[s]);
    const double N = b.notional[i];
    const double tau = b.row_tau[s];
    if (b.row_leg[s] == fixtures::fixed_leg) {
      expect_fix.push_back(N * tau * b.fixed_rate[i]);
    } else if (b.row_is_realised_first[s]) {
      expect_fix.push_back(N * tau * b.realised_rate[i]);
    } else {
      expect_flt_ntau.push_back(N * tau);
    }
  }
  const ir::Group& gfix = p.groups[static_cast<std::size_t>(dfix)];
  EXPECT_EQ(sorted_bits(column_of(p, gfix.steps.back().a).values), sorted_bits(expect_fix));
  const ir::Group& gflt = p.groups[static_cast<std::size_t>(dflt)];
  ASSERT_EQ(gflt.steps.size(), 2u);
  EXPECT_EQ(sorted_bits(column_of(p, gflt.steps[0].a).values), sorted_bits(expect_flt_ntau));
  EXPECT_EQ(p.domain_of(gather_of(p, gflt.steps[0].b).index[0]), dfwd);
  EXPECT_EQ(p.domain_of(gather_of(p, gflt.steps[1].b).index[0]), ddf);
  // The seasoned-first rows are not float rows: no float row carries a realised rate.
  std::size_t realised_in_float = 0;
  for (double v : column_of(p, gflt.steps[0].a).values) {
    for (int i = 0; i < fixtures::n_seasoned; ++i) {
      const auto s = static_cast<std::size_t>(i);
      const int r0 = fixtures::float_row_begin(b, i);
      if (bits(v) == bits(b.notional[s] * b.row_tau[static_cast<std::size_t>(r0)] * b.realised_rate[s])) ++realised_in_float;
    }
  }
  EXPECT_EQ(realised_in_float, 0u);
  // The forward's literal is 1.0 and its column is τ.
  const ir::Group& gfwd = p.groups[static_cast<std::size_t>(dfwd)];
  ASSERT_EQ(gfwd.steps.size(), 3u);
  ASSERT_EQ(gfwd.steps[1].b.kind, ir::SlotKind::Literal);
  EXPECT_EQ(bits(p.literals[static_cast<std::size_t>(gfwd.steps[1].b.index)]), bits(1.0));
  for (double tau : column_of(p, gfwd.steps[2].b).values) EXPECT_TRUE(bits(tau) == bits(365.0 / 360.0) || bits(tau) == bits(366.0 / 360.0));
}

// Used to assert the chain coupons -> ONE leg Sum domain (level 0, 2·n_two_plus rows, the two
// legs of a swap at consecutive rows) -> TWO swap domains `mul($0,sub(@0,@1))` carrying a side
// column of ±1 (T >= 2 at level 1, T == 1 at level 0) -> the book Sum at level 2.
//
// `mul(x, 1) -> x` dissolved the swap class. There is no `mul($0,sub(@0,@1))` domain and no side
// column anywhere: a swap is now `sub(@0,@1)`, and the 485 swaps with side == −1 carry a separate
// `mul(#0,@0)` row on top of it whose literal is −1. That extra class shares its op tree with the
// constant-rate coupon `mul($0,@0)`, so coupons, leg Sums, swaps, negations and the book are ONE
// class cycle, and the leg Sums split across two levels by whether the leg reads a coupon of that
// merged class (all 969 fixed legs and the 194 seasoned float legs) or not (the 775 remaining
// float legs) — which is also why the two legs of one swap are no longer consecutive rows of one
// domain, and why that assertion is not re-stated here.
//
// The chain itself is intact and is asserted below over the domains as they now are, per swap and
// keyed on the IR rather than on domain ids: every swap's output is `fixed − float`, negated iff
// its side is −1; every leg is a Sum over T coupons of the right classes; the book is one Sum over
// the 1,000 swap PVs in output order.
TEST(IrDomainChain, LegsSwapsAndBookAreSegmentsAndLevels) {
  const Fixture& f = fixture();
  const ir::Program& p = f.program;
  const fixtures::Book& b = f.book;
  const ir::domain_id dfix = the_domain(p, "mul($0,@0)");
  const ir::domain_id dflt = the_domain(p, "mul(mul($0,@0),@1)");
  const std::vector<ir::domain_id> sums = domains_named(p, "sum(%0)");
  const std::vector<ir::domain_id> subs = domains_named(p, "sub(@0,@1)");
  const std::vector<ir::domain_id> negs = domains_named(p, "mul(#0,@0)");
  ASSERT_EQ(sums.size(), 3u) << "two leg levels and the book";
  ASSERT_EQ(subs.size(), 2u) << "fixed − float: T == 1 and T >= 2";
  ASSERT_EQ(negs.size(), 2u) << "the side == −1 rows of each";
  EXPECT_TRUE(domains_named(p, "mul($0,sub(@0,@1))").empty()) << "the fused side multiply is gone";

  const auto dom = [&](ir::domain_id d) -> const ir::Domain& { return p.domains[static_cast<std::size_t>(d)]; };
  const auto grp = [&](ir::domain_id d) -> const ir::Group& { return p.groups[static_cast<std::size_t>(d)]; };
  const auto is_one_of = [](const std::vector<ir::domain_id>& v, ir::domain_id d) {
    return std::find(v.begin(), v.end(), d) != v.end();
  };

  // The book is the one-row Sum, the last output; the other two Sums are the legs.
  const ir::domain_id dbook = p.domain_of(p.outputs.back());
  ASSERT_TRUE(is_one_of(sums, dbook));
  std::vector<ir::domain_id> legs;
  for (ir::domain_id d : sums) {
    if (d != dbook) legs.push_back(d);
  }
  ASSERT_EQ(legs.size(), 2u);
  EXPECT_EQ(static_cast<std::size_t>(dom(legs[0]).rows + dom(legs[1]).rows), 2 * f.n_two_plus)
      << "one Sum row per leg of a swap with T >= 2";
  for (ir::domain_id d : legs) EXPECT_EQ(dom(d).reads.empty(), false);

  // Every negation is `(−1) · <a swap difference>`; nothing else lives in those domains.
  for (ir::domain_id d : negs) {
    ASSERT_EQ(grp(d).steps.size(), 1u);
    EXPECT_EQ(grp(d).steps[0].op, Op::Mul);
    ASSERT_EQ(grp(d).steps[0].a.kind, ir::SlotKind::Literal);
    EXPECT_EQ(bits(p.literals[static_cast<std::size_t>(grp(d).steps[0].a.index)]), bits(-1.0));
    ASSERT_EQ(dom(d).reads.size(), 1u);
    EXPECT_TRUE(is_one_of(subs, dom(d).reads[0]));
  }

  // The members of the leg Sum row that holds value `v`.
  const auto leg_members = [&](ir::value_id v) {
    std::vector<ir::value_id> out;
    const ir::domain_id d = p.domain_of(v);
    EXPECT_TRUE(is_one_of(legs, d)) << "a T >= 2 swap reads a leg Sum";
    if (!is_one_of(legs, d)) return out;
    const ir::Segment& seg = p.segments[static_cast<std::size_t>(grp(d).steps[0].a.index)];
    const auto r = static_cast<std::size_t>(p.row_of(v));
    for (std::int32_t m = seg.offsets[r]; m < seg.offsets[r + 1]; ++m) out.push_back(seg.members[static_cast<std::size_t>(m)]);
    return out;
  };

  std::size_t negated = 0, t1_rows = 0, t2_rows = 0;
  for (int i = 0; i < b.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    const int T = b.tenor[s];
    const bool seasoned = b.seasoned[s] != 0;
    const ir::value_id out = p.outputs[s];

    // side == −1: the output is the negation row, and its one gather reads the difference.
    ir::value_id diff = out;
    if (b.side[s] < 0) {
      ASSERT_TRUE(is_one_of(negs, p.domain_of(out))) << "swap " << i << " has side −1";
      const ir::Step& st = grp(p.domain_of(out)).steps[0];
      diff = gather_of(p, st.b).index[static_cast<std::size_t>(p.row_of(out))];
      ++negated;
    }
    const ir::domain_id dsub = p.domain_of(diff);
    ASSERT_TRUE(is_one_of(subs, dsub)) << "swap " << i;
    (T >= 2 ? t2_rows : t1_rows)++;

    const ir::Group& gs = grp(dsub);
    ASSERT_EQ(gs.steps.size(), 1u);
    EXPECT_EQ(gs.steps[0].op, Op::Sub);
    const auto r = static_cast<std::size_t>(p.row_of(diff));
    const ir::value_id vfix = gather_of(p, gs.steps[0].a).index[r];
    const ir::value_id vflt = gather_of(p, gs.steps[0].b).index[r];
    if (T >= 2) {
      const std::vector<ir::value_id> fixed_leg = leg_members(vfix);
      const std::vector<ir::value_id> float_leg = leg_members(vflt);
      ASSERT_EQ(fixed_leg.size(), static_cast<std::size_t>(T)) << "swap " << i;
      ASSERT_EQ(float_leg.size(), static_cast<std::size_t>(T)) << "swap " << i;
      for (ir::value_id m : fixed_leg) EXPECT_EQ(p.domain_of(m), dfix) << "swap " << i;
      EXPECT_EQ(p.domain_of(float_leg[0]), seasoned ? dfix : dflt) << "swap " << i;
      for (std::size_t j = 1; j < float_leg.size(); ++j) EXPECT_EQ(p.domain_of(float_leg[j]), dflt) << "swap " << i;
    } else {
      // T == 1: no leg Sum, the difference reads the two coupons directly.
      EXPECT_EQ(p.domain_of(vfix), dfix) << "swap " << i;
      EXPECT_EQ(p.domain_of(vflt), seasoned ? dfix : dflt) << "swap " << i;
    }
  }
  EXPECT_EQ(negated, static_cast<std::size_t>(dom(negs[0]).rows + dom(negs[1]).rows))
      << "the negation domains hold exactly the side == −1 swaps";
  EXPECT_EQ(t1_rows, f.n_one);
  EXPECT_EQ(t2_rows, f.n_two_plus);
  EXPECT_EQ(static_cast<std::size_t>(dom(subs[0]).rows + dom(subs[1]).rows), f.n_one + f.n_two_plus);

  // Book: one Sum over the 1,000 swap PVs in output order, the highest level, the last output.
  EXPECT_EQ(dom(dbook).rows, 1);
  const ir::Segment& bseg = p.segments[static_cast<std::size_t>(grp(dbook).steps[0].a.index)];
  ASSERT_EQ(bseg.members.size(), static_cast<std::size_t>(b.n_swaps));
  for (int i = 0; i < b.n_swaps; ++i) EXPECT_EQ(bseg.members[static_cast<std::size_t>(i)], p.outputs[static_cast<std::size_t>(i)]) << i;
  EXPECT_EQ(p.outputs.back(), ir::value_of(p, dbook, 0));
  // Evaluation order follows the chain: every domain of the cycle is emitted after the ones it
  // reads, and the book last of all.
  for (ir::domain_id d : {legs[0], legs[1], subs[0], subs[1], negs[0], negs[1], dbook}) {
    for (ir::domain_id src : dom(d).reads) EXPECT_LT(src, d) << "domain " << d << " reads " << src;
    if (d != dbook) EXPECT_LT(d, dbook);
    EXPECT_LE(dom(d).level, dom(dbook).level) << "the book is at the deepest level";
  }
  std::cout << "[  levels  ] legs " << dom(legs[0]).level << "/" << dom(legs[1]).level << " (" << dom(legs[0]).rows << "/"
            << dom(legs[1]).rows << " rows), diffs " << dom(subs[0]).level << "/" << dom(subs[1]).level << ", negations "
            << dom(negs[0]).level << "/" << dom(negs[1]).level << ", book " << dom(dbook).level << '\n';
}
