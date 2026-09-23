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
#include "epykos/maths/m1/book.hpp"
#include "epykos/tape/record_m1.hpp"
#include "epykos/tape/tape.hpp"

namespace m1 = epykos::m1;
namespace ir = epykos::ir;
using epykos::Op;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

struct Fixture {
  m1::Book book;
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
    x.book = m1::make_m1_book();
    x.program = ir::infer(m1::record_m1(x.book), &x.stats);
    const m1::Book& b = x.book;
    for (int i = 0; i < b.n_swaps; ++i) {
      x.n_fixed += static_cast<std::size_t>(b.tenor[static_cast<std::size_t>(i)]);
      x.n_two_plus += (b.tenor[static_cast<std::size_t>(i)] >= 2);
      x.n_one += (b.tenor[static_cast<std::size_t>(i)] == 1);
    }
    std::vector<std::pair<int, int>> pairs;
    for (int r = 0; r < b.n_rows; ++r) {
      const auto s = static_cast<std::size_t>(r);
      x.df_times.push_back(b.row_t_end[s]);
      if (b.row_leg[s] == m1::float_leg) {
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

TEST(IrDomainChain, NoScanDomainsAndTenDomains) {
  const Fixture& f = fixture();
  EXPECT_TRUE(ir::recurrent_domains(f.program).empty());
  EXPECT_EQ(f.program.domains.size(), 10u);
  EXPECT_EQ(f.stats.classes, 8u);   // input, affine, DF, const-rate coupon, forward, float coupon, sum, swap
  EXPECT_EQ(f.stats.domains, 10u);  // sum and swap split by level: legs / book, T==1 / T>=2
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

TEST(IrDomainChain, TimesDomainHasOneExpPerDistinctTime) {
  const Fixture& f = fixture();
  const ir::Program& p = f.program;
  const ir::domain_id din = the_domain(p, "input");
  const ir::domain_id daff = the_domain(p, "affine(#0;%0)");
  const ir::domain_id ddf = the_domain(p, "exp(mul(neg(@0),$0))");
  ASSERT_GE(ddf, 0);
  const ir::Domain& dom = p.domains[static_cast<std::size_t>(ddf)];
  EXPECT_EQ(static_cast<std::size_t>(dom.rows), f.df_times.size());
  EXPECT_EQ(dom.reads, (std::vector<ir::domain_id>{din, daff}));
  const ir::Group& g = p.groups[static_cast<std::size_t>(ddf)];
  ASSERT_EQ(g.steps.size(), 3u);
  EXPECT_EQ(g.steps[0].op, Op::Neg);
  EXPECT_EQ(g.steps[1].op, Op::Mul);
  EXPECT_EQ(g.steps[2].op, Op::Exp);
  const ir::Gather& zt = gather_of(p, g.steps[0].a);
  EXPECT_EQ(reads_from(p, zt, daff), f.n_interior);
  EXPECT_EQ(reads_from(p, zt, din), f.df_times.size() - f.n_interior) << "the non-interpolated times read a knot";
  EXPECT_EQ(f.df_times.size() - f.n_interior, 3u);
  // The column is the time t: the same set as the book's distinct discount times.
  const ir::Column& t_col = column_of(p, g.steps[1].b);
  EXPECT_EQ(sorted_bits(t_col.values), sorted_bits(f.df_times));
}

TEST(IrDomainChain, CouponClassesPartitionTheRowsWithSeasonedFirstOutsideFloat) {
  const Fixture& f = fixture();
  const ir::Program& p = f.program;
  const m1::Book& b = f.book;
  const ir::domain_id ddf = the_domain(p, "exp(mul(neg(@0),$0))");
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
  EXPECT_EQ(fix.reads, std::vector<ir::domain_id>{ddf});
  EXPECT_EQ(fwd.reads, std::vector<ir::domain_id>{ddf});
  EXPECT_EQ(flt.reads, (std::vector<ir::domain_id>{ddf, dfwd}));

  // The constant-rate class holds exactly the fixed coupons (N·τ·K) and the seasoned-first
  // coupons (N·τ·R): its column is that multiset, computed here exactly as the maths does.
  std::vector<double> expect_fix, expect_flt_ntau, expect_fwd_tau;
  for (int r = 0; r < b.n_rows; ++r) {
    const auto s = static_cast<std::size_t>(r);
    const auto i = static_cast<std::size_t>(b.row_swap[s]);
    const double N = b.notional[i];
    const double tau = b.row_tau[s];
    if (b.row_leg[s] == m1::fixed_leg) {
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
    for (int i = 0; i < m1::n_seasoned; ++i) {
      const auto s = static_cast<std::size_t>(i);
      const int r0 = m1::float_row_begin(b, i);
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

TEST(IrDomainChain, LegsSwapsAndBookAreSegmentsAndLevels) {
  const Fixture& f = fixture();
  const ir::Program& p = f.program;
  const m1::Book& b = f.book;
  const ir::domain_id dfix = the_domain(p, "mul($0,@0)");
  const ir::domain_id dflt = the_domain(p, "mul(mul($0,@0),@1)");
  const std::vector<ir::domain_id> sums = domains_named(p, "sum(%0)");
  const std::vector<ir::domain_id> swaps = domains_named(p, "mul($0,sub(@0,@1))");
  ASSERT_EQ(sums.size(), 2u) << "legs and book";
  ASSERT_EQ(swaps.size(), 2u) << "T == 1 and T >= 2";

  // Legs: level 0, one row per leg of a swap with T >= 2, fixed leg then float leg in swap order,
  // segment length T, members from the two coupon domains.
  const ir::domain_id dlegs = p.domains[static_cast<std::size_t>(sums[0])].level == 0 ? sums[0] : sums[1];
  const ir::domain_id dbook = dlegs == sums[0] ? sums[1] : sums[0];
  const ir::Domain& legs = p.domains[static_cast<std::size_t>(dlegs)];
  EXPECT_EQ(static_cast<std::size_t>(legs.rows), 2 * f.n_two_plus);
  EXPECT_EQ(legs.level, 0);
  EXPECT_EQ(legs.reads, (std::vector<ir::domain_id>{dfix, dflt}));
  const ir::Segment& lseg = p.segments[static_cast<std::size_t>(p.groups[static_cast<std::size_t>(dlegs)].steps[0].a.index)];
  {
    std::size_t row = 0;
    for (int i = 0; i < b.n_swaps; ++i) {
      const int T = b.tenor[static_cast<std::size_t>(i)];
      if (T < 2) continue;
      for (int leg = 0; leg < 2; ++leg, ++row) {
        ASSERT_LT(row + 1, lseg.offsets.size());
        EXPECT_EQ(lseg.offsets[row + 1] - lseg.offsets[row], T) << "swap " << i << " leg " << leg;
        const auto m0 = static_cast<std::size_t>(lseg.offsets[row]);
        if (leg == 0) {
          for (int j = 0; j < T; ++j) EXPECT_EQ(p.domain_of(lseg.members[m0 + static_cast<std::size_t>(j)]), dfix);
        } else {
          const bool seasoned = b.seasoned[static_cast<std::size_t>(i)] != 0;
          EXPECT_EQ(p.domain_of(lseg.members[m0]), seasoned ? dfix : dflt) << "swap " << i;
          for (int j = 1; j < T; ++j) EXPECT_EQ(p.domain_of(lseg.members[m0 + static_cast<std::size_t>(j)]), dflt);
        }
      }
    }
    EXPECT_EQ(row, static_cast<std::size_t>(legs.rows));
  }

  // Swaps: side·(fixed − float). T >= 2 rows read the legs (level 1); T == 1 rows read coupons
  // directly (level 0). Output i is the swap's row.
  const ir::domain_id dsw2 = p.domains[static_cast<std::size_t>(swaps[0])].level == 1 ? swaps[0] : swaps[1];
  const ir::domain_id dsw1 = dsw2 == swaps[0] ? swaps[1] : swaps[0];
  const ir::Domain& sw2 = p.domains[static_cast<std::size_t>(dsw2)];
  const ir::Domain& sw1 = p.domains[static_cast<std::size_t>(dsw1)];
  EXPECT_EQ(static_cast<std::size_t>(sw2.rows), f.n_two_plus);
  EXPECT_EQ(static_cast<std::size_t>(sw1.rows), f.n_one);
  EXPECT_EQ(sw2.level, 1);
  EXPECT_EQ(sw1.level, 0);
  EXPECT_EQ(sw2.reads, std::vector<ir::domain_id>{dlegs});
  EXPECT_EQ(sw1.reads, (std::vector<ir::domain_id>{dfix, dflt}));
  const ir::Group& g2 = p.groups[static_cast<std::size_t>(dsw2)];
  ASSERT_EQ(g2.steps.size(), 2u);
  EXPECT_EQ(g2.steps[0].op, Op::Sub);
  EXPECT_EQ(g2.steps[1].op, Op::Mul);
  const ir::Gather& fixed_leg = gather_of(p, g2.steps[0].a);
  const ir::Gather& float_leg = gather_of(p, g2.steps[0].b);
  const ir::Column& side2 = column_of(p, g2.steps[1].a);
  const ir::Column& side1 = column_of(p, p.groups[static_cast<std::size_t>(dsw1)].steps[1].a);
  {
    std::size_t r2 = 0, r1 = 0;
    for (int i = 0; i < b.n_swaps; ++i) {
      const auto s = static_cast<std::size_t>(i);
      const ir::value_id out = p.outputs[s];
      if (b.tenor[s] >= 2) {
        EXPECT_EQ(out, ir::value_of(p, dsw2, static_cast<ir::row_id>(r2))) << i;
        EXPECT_EQ(bits(side2.values[r2]), bits(static_cast<double>(b.side[s]))) << i;
        // The two legs of swap i are consecutive rows of the legs domain.
        EXPECT_EQ(p.row_of(fixed_leg.index[r2]) + 1, p.row_of(float_leg.index[r2])) << i;
        ++r2;
      } else {
        EXPECT_EQ(out, ir::value_of(p, dsw1, static_cast<ir::row_id>(r1))) << i;
        EXPECT_EQ(bits(side1.values[r1]), bits(static_cast<double>(b.side[s]))) << i;
        ++r1;
      }
    }
  }

  // Book: one Sum over the 1,000 swap PVs in output order, level 2, the last output.
  const ir::Domain& book = p.domains[static_cast<std::size_t>(dbook)];
  EXPECT_EQ(book.rows, 1);
  EXPECT_EQ(book.level, 2);
  EXPECT_EQ(book.reads, (std::vector<ir::domain_id>{std::min(dsw1, dsw2), std::max(dsw1, dsw2)}));
  const ir::Segment& bseg = p.segments[static_cast<std::size_t>(p.groups[static_cast<std::size_t>(dbook)].steps[0].a.index)];
  ASSERT_EQ(bseg.members.size(), static_cast<std::size_t>(b.n_swaps));
  for (int i = 0; i < b.n_swaps; ++i) EXPECT_EQ(bseg.members[static_cast<std::size_t>(i)], p.outputs[static_cast<std::size_t>(i)]) << i;
  EXPECT_EQ(p.outputs.back(), ir::value_of(p, dbook, 0));
  // Evaluation order follows the chain.
  EXPECT_LT(dlegs, dsw2);
  EXPECT_LT(dsw2, dbook);
  EXPECT_LT(dsw1, dbook);
}
