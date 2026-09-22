// M1/P2 integration E0 gate: the UNMODIFIED P1 maths (maths/m1/price.hpp) records on Rec, and the
// tape replays bit-identically to price_book<double> at the record point and at every one of the
// 64 batch states — before any pass and after each of cse, dce, fold_sum, affine_collapse, dce.
//
// This TU is compiled with -ffp-contract=off in every preset (the _e0_test.cpp convention), so the
// double oracle (header-only, computed here), the Rec record-point values and the header-only
// replay all run without fused multiply-adds. The book and the batch come from libepykos and are
// the same data for every TU of a build.
//
// Node counts before/after each pass are printed and recorded as test properties (informational).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/maths/m1/book.hpp"
#include "epykos/maths/m1/price.hpp"
#include "epykos/maths/m1/reference.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/op.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/record_m1.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "tape/tape_test_helpers.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace m1 = epykos::m1;
using epykos::Op;
using epykos::PassResult;
using epykos::Rec;
using epykos::Replayer;
using epykos::Tape;
using epykos::test::bits;
using epykos::test::count_op;

namespace {

// The seeded fixtures and the double oracle of this TU, built once.
struct Fixture {
  m1::Book book;
  m1::Batch batch;
  m1::ReferenceTable oracle;  // record + 64 states, computed in this (contraction-free) TU
};

const Fixture& fixture() {
  static const Fixture f = [] {
    Fixture x;
    x.book = m1::make_m1_book();
    x.batch = m1::make_m1_batch();
    x.oracle = m1::m1_reference_values(x.book, x.batch);
    return x;
  }();
  return f;
}

// Replays `tape` at the record point and at every batch state; every output must equal the
// oracle bitwise. Returns the number of mismatching (state, output) pairs.
std::size_t replay_mismatches(const Tape& tape, const char* what) {
  const Fixture& f = fixture();
  EXPECT_EQ(tape.num_inputs(), static_cast<std::size_t>(m1::n_knots)) << what;
  EXPECT_EQ(tape.num_outputs(), static_cast<std::size_t>(f.book.n_swaps + 1)) << what;
  Replayer rp(tape);
  std::vector<double> out(tape.num_outputs(), 0.0);
  std::vector<double> z(static_cast<std::size_t>(m1::n_knots), 0.0);
  std::size_t mismatches = 0;
  auto check = [&](const double* expect, const std::string& where) {
    for (std::size_t k = 0; k < out.size(); ++k) {
      if (bits(out[k]) != bits(expect[k])) {
        if (mismatches < 5) {
          ADD_FAILURE() << what << ", " << where << ", output " << k << ": replay " << out[k]
                        << " vs double " << expect[k];
        }
        ++mismatches;
      }
    }
  };
  // Record point: the tape's own input values (== book.z0).
  const std::vector<double> z_rec = tape.input_values();
  rp.run(z_rec.data(), out.data());
  check(f.oracle.record.data(), "record point");
  for (int b = 0; b < f.batch.n_states; ++b) {
    f.batch.state(b, z.data());
    rp.run(z.data(), out.data());
    check(f.oracle.state(b), "state " + std::to_string(b));
  }
  EXPECT_EQ(mismatches, 0u) << what;
  return mismatches;
}

std::string histogram_string(const Tape& t) {
  const std::vector<std::size_t> h = t.op_histogram();
  std::string s;
  for (int op = 0; op < epykos::op_count; ++op) {
    if (h[static_cast<std::size_t>(op)] == 0) continue;
    if (!s.empty()) s += ' ';
    s += epykos::to_string(static_cast<Op>(op));
    s += '=';
    s += std::to_string(h[static_cast<std::size_t>(op)]);
  }
  return s;
}

void log_counts(const char* stage, const Tape& t) {
  std::cout << "[  counts  ] " << stage << ": " << t.size() << " nodes (" << histogram_string(t) << ")\n";
  ::testing::Test::RecordProperty(std::string("nodes_") + stage, std::to_string(t.size()));
}

// The distinct discount times the maths evaluates: DF(e_j) on every row, DF(s_j) on float rows
// that are not the realised first coupon (price.hpp). After cse there is one Exp per time.
std::vector<double> distinct_df_times(const m1::Book& b) {
  std::vector<double> ts;
  for (int r = 0; r < b.n_rows; ++r) {
    const auto s = static_cast<std::size_t>(r);
    ts.push_back(b.row_t_end[s]);
    if (b.row_leg[s] == m1::float_leg && !b.row_is_realised_first[s]) ts.push_back(b.row_t_start[s]);
  }
  std::sort(ts.begin(), ts.end());
  ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
  return ts;
}

// Times strictly inside the knot range and not on a knot: the ones whose zero rate is a two-term
// interpolation ((1 − w)·z_k + w·z_{k+1}) and therefore one Sum after fold_sum / one Affine after
// affine_collapse.
std::size_t count_interior_times(const m1::Book& b, const std::vector<double>& ts) {
  std::size_t n = 0;
  for (double t : ts) {
    if (!(t > b.knot_t.front() && t < b.knot_t.back())) continue;
    if (std::find(b.knot_t.begin(), b.knot_t.end(), t) != b.knot_t.end()) continue;
    ++n;
  }
  return n;
}

std::size_t count_swaps_with_two_or_more_periods(const m1::Book& b) {
  std::size_t n = 0;
  for (int t : b.tenor) n += (t >= 2);
  return n;
}

}  // namespace

// ---- recording ---------------------------------------------------------------------------------------

TEST(M1RecordE0, UnmodifiedMathsRecordsWithoutThrowing) {
  const Fixture& f = fixture();
  Tape t;
  std::vector<Rec> swap_pv(static_cast<std::size_t>(f.book.n_swaps));
  Rec book_pv;
  // Recording discipline: no .value() on a tainted value, no structural_if on a tainted predicate,
  // no Rec used outside its scope. Any of these throws RecordError.
  ASSERT_NO_THROW({
    Tape::Scope scope(t);
    std::vector<Rec> z(static_cast<std::size_t>(m1::n_knots));
    for (int k = 0; k < m1::n_knots; ++k) {
      z[static_cast<std::size_t>(k)] = epykos::make_input(t, f.book.z0[static_cast<std::size_t>(k)]);
    }
    m1::price_book<Rec>(f.book, z.data(), swap_pv.data(), &book_pv);
    for (const Rec& pv : swap_pv) epykos::register_output(t, pv);
    epykos::register_output(t, book_pv);
  });
  ASSERT_NO_THROW(t.validate());
  log_counts("recorded", t);

  ASSERT_EQ(t.num_inputs(), 12u);
  ASSERT_EQ(t.num_outputs(), 1001u);
  // Input ordinal k is knot k with the record-point value z0[k].
  for (int k = 0; k < m1::n_knots; ++k) {
    const epykos::Node& n = t[t.inputs()[static_cast<std::size_t>(k)]];
    EXPECT_EQ(n.op, Op::Input) << k;
    EXPECT_EQ(n.a, k) << k;
    EXPECT_EQ(bits(n.konst), bits(f.book.z0[static_cast<std::size_t>(k)])) << k;
  }
  EXPECT_EQ(t.input_values(), std::vector<double>(f.book.z0.begin(), f.book.z0.end()));
  // Every output depends on the inputs, is a distinct node, and is registered in swap order then
  // the book PV (the oracle's ordering).
  for (std::size_t i = 0; i < t.num_outputs(); ++i) {
    EXPECT_TRUE(t.tainted(t.outputs()[i])) << "output " << i;
  }
  for (int i = 0; i < f.book.n_swaps; ++i) {
    EXPECT_EQ(t.outputs()[static_cast<std::size_t>(i)], swap_pv[static_cast<std::size_t>(i)].node()) << i;
  }
  EXPECT_EQ(t.outputs()[static_cast<std::size_t>(f.book.n_swaps)], book_pv.node());

  // The M1 maths branches only on structure: no comparison, no select, and only the arithmetic the
  // Scalar contract promises (+ − × ÷, unary minus, exp) plus leaves.
  const std::vector<std::size_t> h = t.op_histogram();
  for (int op = 0; op < epykos::op_count; ++op) {
    const Op o = static_cast<Op>(op);
    const bool allowed = o == Op::Const || o == Op::Input || o == Op::Add || o == Op::Sub || o == Op::Mul ||
                         o == Op::Div || o == Op::Neg || o == Op::Exp;
    if (!allowed) EXPECT_EQ(h[static_cast<std::size_t>(op)], 0u) << epykos::to_string(o);
  }
  EXPECT_GT(count_op(t, Op::Exp), 0u);
  EXPECT_GT(count_op(t, Op::Div), 0u);
  // One Exp per DF evaluation before cse (fixed rows: DF(e); float rows: DF(e) and, unless
  // realised, DF(s)).
  std::size_t expected_exps = 0;
  for (int r = 0; r < f.book.n_rows; ++r) {
    const auto s = static_cast<std::size_t>(r);
    expected_exps += 1 + (f.book.row_leg[s] == m1::float_leg && !f.book.row_is_realised_first[s]);
  }
  EXPECT_EQ(count_op(t, Op::Exp), expected_exps);

  // The Rec record-point values are the double values bitwise (same arithmetic, same TU flags).
  for (int i = 0; i < f.book.n_swaps; ++i) {
    const auto s = static_cast<std::size_t>(i);
    EXPECT_EQ(bits(swap_pv[s].unchecked_value()), bits(f.oracle.record_swap_pv(i))) << i;
  }
  EXPECT_EQ(bits(book_pv.unchecked_value()), bits(f.oracle.record_book_pv()));
}

// ---- E0 replay before and after every pass -------------------------------------------------------------

TEST(M1RecordE0, ReplayIsBitIdenticalToDoubleAfterEveryPass) {
  const Fixture& f = fixture();
  Tape t = m1::record_m1_raw(f.book);
  log_counts("recorded", t);
  const std::size_t n_recorded = t.size();
  ASSERT_EQ(replay_mismatches(t, "raw recording"), 0u);

  const std::vector<double> df_times = distinct_df_times(f.book);
  const std::size_t n_interior = count_interior_times(f.book, df_times);
  const std::size_t n_two_plus = count_swaps_with_two_or_more_periods(f.book);
  std::cout << "[  counts  ] distinct DF times " << df_times.size() << ", interior " << n_interior
            << ", swaps with T>=2: " << n_two_plus << '\n';

  struct Step {
    const char* name;
    PassResult (*run)(Tape&);
  };
  const Step steps[] = {
      {"cse", [](Tape& x) { return epykos::cse(x); }},
      {"dce", [](Tape& x) { return epykos::dce(x); }},
      {"fold_sum", [](Tape& x) { return epykos::fold_sum(x, {}); }},
      {"affine_collapse", [](Tape& x) { return epykos::affine_collapse(x); }},
      {"final_dce", [](Tape& x) { return epykos::dce(x); }},
  };
  std::size_t previous = t.size();
  for (const Step& step : steps) {
    const PassResult r = step.run(t);
    ASSERT_NO_THROW(t.validate()) << step.name;
    EXPECT_EQ(r.nodes_before, previous) << step.name;
    EXPECT_EQ(r.nodes_after, t.size()) << step.name;
    EXPECT_EQ(r.remap.size(), previous) << step.name;
    log_counts(step.name, t);
    EXPECT_EQ(t.num_inputs(), 12u) << step.name;
    EXPECT_EQ(t.num_outputs(), 1001u) << step.name;
    EXPECT_EQ(replay_mismatches(t, step.name), 0u) << step.name;
    previous = t.size();

    // Structural expectations per stage (the gate is the bit-identity above; these say the
    // passes did what they claim on this book).
    const std::string name = step.name;
    if (name == "cse") {
      EXPECT_EQ(count_op(t, Op::Exp), df_times.size()) << "one Exp per distinct time after cse";
      EXPECT_EQ(count_op(t, Op::Input), 12u);
    } else if (name == "dce") {
      EXPECT_EQ(count_op(t, Op::Exp), df_times.size());
    } else if (name == "fold_sum") {
      EXPECT_EQ(count_op(t, Op::Add), 0u) << "every Add folded into a Sum";
      // One Sum per leg with two or more coupons, one for the book, one per interpolated time.
      EXPECT_EQ(count_op(t, Op::Sum), 1u + 2u * n_two_plus + n_interior);
    } else if (name == "affine_collapse") {
      EXPECT_GT(count_op(t, Op::Affine), 0u) << "the interpolation collapsed";
      EXPECT_EQ(count_op(t, Op::Sum) + count_op(t, Op::Affine), 1u + 2u * n_two_plus + n_interior);
      EXPECT_GE(count_op(t, Op::Sum), 1u + 2u * n_two_plus) << "coupon and book sums survive";
    }
  }
  EXPECT_LT(t.size(), n_recorded);
  EXPECT_EQ(count_op(t, Op::Add), 0u);
  EXPECT_EQ(count_op(t, Op::Exp), df_times.size());
  // Every Affine node reads only Inputs (the interpolation is on the knots themselves).
  for (const epykos::Node& n : t.nodes()) {
    if (n.op != Op::Affine) continue;
    for (epykos::node_id a : t.args(n)) EXPECT_EQ(t[a].op, Op::Input);
    EXPECT_EQ(t.args(n).size(), 2u);
    EXPECT_EQ(bits(n.konst), bits(-0.0)) << "no leading constant in an interpolation";
  }
  // Outputs are still 1,001 distinct tainted nodes in the same order (swap PVs then book PV):
  // the book PV is the Sum of the 1,000 swap PVs in swap order.
  const epykos::Node& book_node = t[t.outputs().back()];
  ASSERT_EQ(book_node.op, Op::Sum);
  ASSERT_EQ(book_node.nargs, f.book.n_swaps);
  for (int i = 0; i < f.book.n_swaps; ++i) {
    EXPECT_EQ(t.args(book_node)[static_cast<std::size_t>(i)], t.outputs()[static_cast<std::size_t>(i)]) << i;
  }
}

// Batch state 0 is the record point bitwise, on the tape as on the oracle.
TEST(M1RecordE0, BatchStateZeroReplaysAsTheRecordPoint) {
  const Fixture& f = fixture();
  Tape t = m1::record_m1(f.book);
  Replayer rp(t);
  std::vector<double> out_rec(t.num_outputs()), out_0(t.num_outputs());
  const std::vector<double> z_rec = t.input_values();
  rp.run(z_rec.data(), out_rec.data());
  std::vector<double> z0(static_cast<std::size_t>(m1::n_knots));
  f.batch.state(0, z0.data());
  EXPECT_EQ(z0, z_rec);
  rp.run(z0.data(), out_0.data());
  for (std::size_t k = 0; k < out_rec.size(); ++k) EXPECT_EQ(bits(out_rec[k]), bits(out_0[k])) << k;
  // And the other states are genuinely different states.
  std::vector<double> z(static_cast<std::size_t>(m1::n_knots)), out(t.num_outputs());
  for (int b = 1; b < f.batch.n_states; ++b) {
    f.batch.state(b, z.data());
    rp.run(z.data(), out.data());
    EXPECT_NE(out.back(), out_rec.back()) << b;
  }
}

// ---- the record_m1 helper -------------------------------------------------------------------------------

TEST(M1RecordE0, HelperEqualsTheManualPipelineAndStandardPasses) {
  const Fixture& f = fixture();
  // Manual: raw recording then the five passes.
  Tape manual = m1::record_m1_raw(f.book);
  const std::size_t n_raw = manual.size();
  epykos::cse(manual);
  const std::size_t n_cse = manual.size();
  epykos::dce(manual);
  const std::size_t n_dce = manual.size();
  epykos::fold_sum(manual);
  const std::size_t n_fold = manual.size();
  epykos::affine_collapse(manual);
  const std::size_t n_affine = manual.size();
  epykos::dce(manual);
  const std::size_t n_final = manual.size();

  m1::RecordM1Stats stats;
  const Tape helper = m1::record_m1(f.book, &stats);
  EXPECT_EQ(stats.recorded, n_raw);
  EXPECT_EQ(stats.after_cse, n_cse);
  EXPECT_EQ(stats.after_dce, n_dce);
  EXPECT_EQ(stats.after_fold_sum, n_fold);
  EXPECT_EQ(stats.after_affine, n_affine);
  EXPECT_EQ(stats.after_final_dce, n_final);
  EXPECT_EQ(stats.after_final_dce, helper.size());
  EXPECT_EQ(stats.num_inputs, 12u);
  EXPECT_EQ(stats.num_outputs, 1001u);
  EXPECT_EQ(epykos::to_string(helper), epykos::to_string(manual));

  // standard_passes on a fresh raw recording is the same tape.
  Tape standard = m1::record_m1_raw(f.book);
  epykos::standard_passes(standard);
  EXPECT_EQ(epykos::to_string(standard), epykos::to_string(helper));

  // run_passes = false is the raw recording.
  m1::RecordM1Options no_passes;
  no_passes.run_passes = false;
  m1::RecordM1Stats raw_stats;
  const Tape raw = m1::record_m1(f.book, &raw_stats, no_passes);
  EXPECT_EQ(raw.size(), n_raw);
  EXPECT_EQ(raw_stats.recorded, n_raw);
  EXPECT_EQ(raw_stats.after_final_dce, n_raw);
  EXPECT_EQ(epykos::to_string(raw), epykos::to_string(m1::record_m1_raw(f.book)));

  // Recording is deterministic: two recordings of the same book are the same tape.
  EXPECT_EQ(epykos::to_string(m1::record_m1(f.book)), epykos::to_string(helper));

  // And the helper's tape is the E0 tape.
  EXPECT_EQ(replay_mismatches(helper, "record_m1"), 0u);
  std::cout << "[  counts  ] record_m1: recorded " << stats.recorded << ", cse " << stats.after_cse << ", dce "
            << stats.after_dce << ", fold_sum " << stats.after_fold_sum << ", affine " << stats.after_affine
            << ", final dce " << stats.after_final_dce << '\n';
}
