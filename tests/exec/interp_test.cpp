// M1/P4 unit tests: op coverage beyond the M1 book (select / comparisons / constants through
// P2's mini book; fma, log, sqrt, recip through a small recording; the per-row Sum fallback and
// the Const domain through a hand-built program), option validation, describe(), and the E1
// exp_poly mode against std::exp. The bit-identity gate on the M1 book is m1_interp_e0_test.cpp.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/exec/interpreter.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/record_m1.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "tape/mini_book.hpp"

namespace ir = epykos::ir;
namespace exec = epykos::exec;
namespace m1 = epykos::m1;
using epykos::Op;
using epykos::Rec;
using epykos::Replayer;
using epykos::Tape;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// Runs the interpreter at B = states.size() and at B = 1 per state; both must equal the tape
// replay (and the IR evaluator) bitwise. Returns the mismatch count.
std::size_t check_against_replay(const Tape& tape, const ir::Program& program, const std::vector<std::vector<double>>& states,
                                 exec::Options o = {}) {
  exec::Interpreter in(program, o);
  Replayer rp(tape);
  ir::Evaluator ev(program);
  const int B = static_cast<int>(states.size());
  const std::size_t K = static_cast<std::size_t>(in.n_inputs());
  const std::size_t n_out = static_cast<std::size_t>(in.n_outputs());
  std::vector<double> state(K * static_cast<std::size_t>(B));
  for (std::size_t k = 0; k < K; ++k) {
    for (int b = 0; b < B; ++b) state[k * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)] = states[static_cast<std::size_t>(b)][k];
  }
  std::vector<double> batched(n_out * static_cast<std::size_t>(B));
  in.run(state.data(), B, batched.data());
  std::size_t bad = 0;
  std::vector<double> expect(n_out), evaluated(n_out), single(n_out);
  for (int b = 0; b < B; ++b) {
    const std::vector<double>& z = states[static_cast<std::size_t>(b)];
    rp.run(z.data(), expect.data());
    ev.run(z.data(), evaluated.data());
    in.run(z.data(), 1, single.data());
    for (std::size_t o = 0; o < n_out; ++o) {
      const double lane = batched[o * static_cast<std::size_t>(B) + static_cast<std::size_t>(b)];
      if (bits(lane) != bits(expect[o]) || bits(single[o]) != bits(expect[o]) || bits(evaluated[o]) != bits(expect[o])) {
        if (bad < 5) ADD_FAILURE() << "state " << b << " output " << o << ": batched " << lane << ", single " << single[o]
                                   << ", replay " << expect[o] << ", evaluator " << evaluated[o];
        ++bad;
      }
    }
  }
  return bad;
}

}  // namespace

// P2's mini book prices with a value branch (select on a comparison against a literal) on every
// swap, so the Select / CmpLt / Const-domain paths are exercised.
TEST(Interp, MiniBookWithSelectMatchesReplay) {
  const std::vector<epykos::test::MiniSwap> book = epykos::test::mini_book(40);
  Tape tape = epykos::test::record_mini_book(book);
  epykos::standard_passes(tape);
  const ir::Program program = ir::infer(tape);
  bool has_select = false, has_cmp = false;
  for (const ir::Group& g : program.groups) {
    for (const ir::Step& s : g.steps) {
      has_select |= s.op == Op::Select;
      has_cmp |= epykos::op_is_comparison(s.op);
    }
  }
  EXPECT_TRUE(has_select);
  EXPECT_TRUE(has_cmp);
  std::vector<std::vector<double>> states;
  for (const std::array<double, epykos::test::kMiniKnots>& s : epykos::test::mini_states(9)) states.emplace_back(s.begin(), s.end());
  for (int tile : {1, 5, 256}) {
    for (int lane_tile : {1, 2, 4, 9}) {
      exec::Options o;
      o.tile = tile;
      o.lane_tile = lane_tile;
      EXPECT_EQ(check_against_replay(tape, program, states, o), 0u) << "tile " << tile << " lane_tile " << lane_tile;
    }
  }
}

// Every remaining supported op: fma, log, sqrt, recip, the other comparisons, max / min / abs
// (selects), sub / neg / div, with constants in every operand position.
TEST(Interp, EveryArithmeticOpMatchesReplay) {
  Tape tape;
  {
    Tape::Scope scope(tape);
    std::vector<Rec> x;
    for (int k = 0; k < 6; ++k) x.push_back(epykos::make_input(tape, 0.5 + 0.25 * k));
    std::vector<Rec> outs;
    for (int k = 0; k < 5; ++k) {
      const Rec a = x[static_cast<std::size_t>(k)];
      const Rec b = x[static_cast<std::size_t>(k) + 1];
      const Rec f = fma(a, b, 0.125);
      const Rec g = log(a + 1.0) + sqrt(b * 2.0) + recip(a) + fma(f, a, b);
      const Rec h = select(a <= b, g, -g) + select(a >= b, 1.0, a) + select(a == b, b, 2.0) + select(a > b, a / b, b - a);
      const Rec m = max(a, b) - min(a, 0.75) + abs(a - b) + select(0.6 < a, a, 3.0) + select(a < 0.9, 4.0, 5.0);
      outs.push_back(h * m + (f - 3.0 * b) / (2.0 - a) + exp(-a));
    }
    // The total scales each term so that it folds into an Affine over the Sum nodes (a Sum whose
    // members are Sums of the same class directly is a scan candidate for the signature pass).
    Rec total = outs[0] * 1.5;
    for (std::size_t k = 1; k < outs.size(); ++k) total = total + outs[k] * (1.5 + static_cast<double>(k));
    for (const Rec& r : outs) epykos::register_output(tape, r);
    epykos::register_output(tape, total);
  }
  tape.validate();
  epykos::standard_passes(tape);
  const ir::Program program = ir::infer(tape);
  std::vector<Op> seen;
  for (const ir::Group& g : program.groups) {
    for (const ir::Step& s : g.steps) seen.push_back(s.op);
  }
  for (Op op : {Op::Fma, Op::Log, Op::Sqrt, Op::Recip, Op::Select, Op::CmpLe, Op::CmpGe, Op::CmpEq, Op::CmpGt, Op::CmpLt,
                Op::Div, Op::Sub, Op::Neg, Op::Exp}) {
    EXPECT_NE(std::find(seen.begin(), seen.end(), op), seen.end()) << "op " << epykos::to_string(op) << " not on the program";
  }
  std::vector<std::vector<double>> states;
  epykos::test::SplitMix64 rng(7);
  for (int b = 0; b < 11; ++b) {
    std::vector<double> z(6);
    for (double& v : z) v = rng.uniform(0.3, 2.0);
    states.push_back(z);
  }
  states[3][1] = states[3][2];  // an equal pair for CmpEq
  for (int lane_tile : {1, 4, 11}) {
    exec::Options o;
    o.lane_tile = lane_tile;
    EXPECT_EQ(check_against_replay(tape, program, states, o), 0u) << "lane_tile " << lane_tile;
  }
}

// A hand-built program: a Sum step that shares its group with a Mul (the per-row fallback), a
// Const domain, an Affine with a column c_0 and zero members, and a 4-way Sum. Checked against
// the IR evaluator.
TEST(Interp, HandBuiltProgramSumFallbackConstAndAffine) {
  ir::Program p;
  // d0: inputs (3 rows).
  p.domains.push_back({"input", 3, 0, 0, false, {}});
  p.groups.push_back({0, {ir::Step{Op::Input, {ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2};
  p.input_values = {1.0, 2.0, 3.0};
  // d1: const domain (2 rows) with a column.
  p.domains.push_back({"const($0)", 2, 3, 0, false, {}});
  p.columns.push_back({1, {0.5, -0.25}});
  p.groups.push_back({1, {ir::Step{Op::Const, {}, {}, {}, {ir::SlotKind::Column, 0}}}});
  // d2: affine with column c_0 over {input0, input1} for row 0, {input2} for row 1, {input0} for
  // row 2 (every segment row has at least one member; ir::validate requires it).
  p.domains.push_back({"affine($1;%0)", 3, 5, 0, false, {0}});
  p.columns.push_back({2, {10.0, 20.0, 30.0}});
  p.segments.push_back({2, {0, 2, 3, 4}, {0, 1, 2, 0}, {0.5, 0.25, 2.0, -1.0}});
  p.groups.push_back({2, {ir::Step{Op::Affine, {ir::SlotKind::Segment, 0}, {}, {}, {ir::SlotKind::Column, 1}}}});
  // d3: mul(sum(%0), #0): a Sum that is not its group's only step (per-row path), 2 rows reading
  // d0..d2 with different lengths.
  p.domains.push_back({"mul(sum(%1),#0)", 2, 8, 0, false, {0, 1, 2}});
  p.literals.push_back(3.0);
  p.segments.push_back({3, {0, 4, 6}, {0, 3, 5, 6, 4, 7}, {}});
  p.groups.push_back({3, {ir::Step{Op::Sum, {ir::SlotKind::Segment, 1}, {}, {}, {}},
                          ir::Step{Op::Mul, {ir::SlotKind::Step, 0}, {ir::SlotKind::Literal, 0}, {}, {}}}});
  // d4: sum over everything (1 row, 10 members).
  p.domains.push_back({"sum(%2)", 1, 10, 1, false, {0, 1, 2, 3}});
  p.segments.push_back({4, {0, 10}, {9, 8, 7, 6, 5, 4, 3, 2, 1, 0}, {}});
  p.groups.push_back({4, {ir::Step{Op::Sum, {ir::SlotKind::Segment, 2}, {}, {}, {}}}});
  p.outputs = {8, 9, 10, 3, 5, 7};
  ASSERT_NO_THROW(ir::validate(p));

  ir::Evaluator ev(p);
  std::vector<double> expect(p.outputs.size());
  for (int lane_tile : {1, 4, 5}) {
    for (int tile : {1, 2, 256}) {
      exec::Options o;
      o.tile = tile;
      o.lane_tile = lane_tile;
      exec::Interpreter in(p, o);
      const int B = 5;
      std::vector<double> state(3 * B), out(p.outputs.size() * B);
      for (int b = 0; b < B; ++b) {
        for (int k = 0; k < 3; ++k) state[static_cast<std::size_t>(k * B + b)] = 1.0 + k + 0.1 * b;
      }
      in.run(state.data(), B, out.data());
      for (int b = 0; b < B; ++b) {
        const double z[3] = {1.0 + 0.1 * b, 2.0 + 0.1 * b, 3.0 + 0.1 * b};
        ev.run(z, expect.data());
        for (std::size_t o2 = 0; o2 < expect.size(); ++o2) {
          EXPECT_EQ(bits(out[o2 * B + static_cast<std::size_t>(b)]), bits(expect[o2]))
              << "tile " << tile << " lane_tile " << lane_tile << " lane " << b << " output " << o2;
        }
      }
    }
  }
}

TEST(Interp, OptionsAndBatchWidthAreValidated) {
  const m1::Book book = m1::make_m1_book();
  const Tape tape = m1::record_m1(book);
  const ir::Program program = ir::infer(tape);
  EXPECT_THROW(exec::Interpreter(program, exec::Options{0, 64, 8, exec::ExpMode::std_exp}), std::invalid_argument);
  EXPECT_THROW(exec::Interpreter(program, exec::Options{256, 0, 8, exec::ExpMode::std_exp}), std::invalid_argument);
  EXPECT_THROW(exec::Interpreter(program, exec::Options{256, 64, 0, exec::ExpMode::std_exp}), std::invalid_argument);
  exec::Interpreter in(program, exec::Options{256, 4, 8, exec::ExpMode::std_exp});
  EXPECT_EQ(in.max_batch(), 4);
  std::vector<double> state(12 * 5, 0.04), out(1001 * 5);
  EXPECT_THROW(in.run(state.data(), 5, out.data()), std::invalid_argument);
  EXPECT_THROW(in.run(state.data(), 0, out.data()), std::invalid_argument);
  EXPECT_NO_THROW(in.run(state.data(), 4, out.data()));
  // A program with a reserved op is rejected (ir::validate runs first and throws runtime_error).
  ir::Program bad = program;
  bad.groups[2].steps[0].op = Op::Gather;
  EXPECT_THROW(exec::Interpreter{bad}, std::runtime_error);
  // A program that fails validation is rejected.
  ir::Program broken = program;
  broken.outputs.push_back(static_cast<ir::value_id>(program.num_values()) + 5);
  EXPECT_THROW(exec::Interpreter{broken}, std::runtime_error);
}

TEST(Interp, DescribeListsTheM1Plan) {
  const m1::Book book = m1::make_m1_book();
  const Tape tape = m1::record_m1(book);
  const ir::Program program = ir::infer(tape);
  exec::Interpreter in(program);
  const std::string d = in.describe();
  EXPECT_NE(d.find("tile 256"), std::string::npos);
  EXPECT_NE(d.find("whole-domain affine"), std::string::npos);
  EXPECT_NE(d.find("whole-domain sum"), std::string::npos);
  EXPECT_NE(d.find("exp(vec)"), std::string::npos);
  EXPECT_NE(d.find("mul(col,gat)"), std::string::npos);
  EXPECT_NE(d.find("values: " + std::to_string(program.num_values())), std::string::npos);
  EXPECT_EQ(in.num_values(), program.num_values());
  EXPECT_EQ(in.value_bytes(), program.num_values() * 8u * 8u);  // default lane_tile 8
  EXPECT_GT(in.table_bytes(), 0u);
  EXPECT_GT(in.scratch_bytes(), 0u);
  std::cout << d;
}

// E1: exp_poly instead of std::exp. Within 1e-12 relative of the E0 result on the M1 book.
TEST(Interp, ExpPolyModeIsWithinE1OfStdExp) {
  const m1::Book book = m1::make_m1_book();
  const m1::Batch batch = m1::make_m1_batch();
  const Tape tape = m1::record_m1(book);
  const ir::Program program = ir::infer(tape);
  exec::Options poly;
  poly.exp = exec::ExpMode::poly;
  exec::Interpreter e0(program), e1(program, poly);
  EXPECT_NE(e1.describe().find("exp_poly(vec)"), std::string::npos);
  std::vector<double> a(1001 * 64), b(1001 * 64);
  e0.run(batch.z.data(), 64, a.data());
  e1.run(batch.z.data(), 64, b.data());
  double max_rel = 0.0;
  for (std::size_t k = 0; k < a.size(); ++k) {
    const double scale = std::max(std::abs(a[k]), 1e6);  // PVs are O(1e4..1e7); 1e6 floors the tiny ones
    max_rel = std::max(max_rel, std::abs(a[k] - b[k]) / scale);
  }
  EXPECT_LT(max_rel, 1e-12);
}
