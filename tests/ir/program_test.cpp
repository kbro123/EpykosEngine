// M1/P3 unit tests: the Program data structure (validate, serialise) and the signature rules on
// small hand-built tapes — literal vs column, class-insensitive references, the per-computation
// sharing rule, commutative operands, Const members, recurrence detection and level splitting.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/record_m1.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "ir/ir_test_helpers.hpp"

namespace ir = epykos::ir;
namespace m1 = epykos::m1;
using epykos::make_input;
using epykos::Op;
using epykos::Rec;
using epykos::register_output;
using epykos::Tape;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

ir::domain_id find_domain(const ir::Program& p, const std::string& shape) {
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    if (ir::shape_string(p, static_cast<ir::domain_id>(d)) == shape) return static_cast<ir::domain_id>(d);
  }
  return -1;
}

// infer, expand, compare, and check the evaluator against the replay at a few inputs.
ir::Program check(const Tape& tape, const char* what) {
  const ir::Program p = ir::infer(tape);
  EXPECT_NO_THROW(ir::validate(p)) << what;
  std::string diff;
  EXPECT_TRUE(ir::roundtrip_identical(tape, ir::expand(p), &diff)) << what << '\n' << diff;
  EXPECT_TRUE(ir::deserialize(ir::serialize(p)) == p) << what;
  std::vector<double> in = tape.input_values();
  for (int trial = 0; trial < 3; ++trial) {
    for (std::size_t k = 0; k < in.size(); ++k) in[k] = in[k] * (1.0 + 0.1 * trial) + 0.01 * static_cast<double>(k + 1);
    const std::vector<double> a = epykos::replay(tape, in);
    const std::vector<double> b = ir::evaluate(p, in);
    EXPECT_EQ(a.size(), b.size()) << what;
    for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) EXPECT_EQ(bits(a[k]), bits(b[k])) << what << " output " << k;
  }
  return p;
}

}  // namespace

TEST(IrProgram, UniformConstantsAreLiteralsVaryingOnesColumns) {
  Tape t;
  {
    Tape::Scope scope(t);
    for (int k = 0; k < 3; ++k) {
      Rec x = make_input(t, 1.0 + k);
      register_output(t, x * 2.0);
    }
    for (int k = 0; k < 3; ++k) {
      Rec x = make_input(t, 10.0 + k);
      register_output(t, x * (3.0 + k));
    }
  }
  const ir::Program p = check(t, "literal vs column");
  // x*2.0 and x*k have the same shape mul(@0, slot): one class of 6 rows; the constant slot
  // varies across rows (2, 2, 2, 3, 4, 5) so it is a column.
  const ir::domain_id d = find_domain(p, "mul(@0,$0)");
  ASSERT_GE(d, 0);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(d)].rows, 6);
  const ir::Column& c = p.columns[static_cast<std::size_t>(p.groups[static_cast<std::size_t>(d)].steps[0].b.index)];
  EXPECT_EQ(c.values, (std::vector<double>{2.0, 2.0, 2.0, 3.0, 4.0, 5.0}));

  Tape u;
  {
    Tape::Scope scope(u);
    for (int k = 0; k < 3; ++k) register_output(u, make_input(u, 1.0 + k) * 2.0);
  }
  const ir::Program q = check(u, "literal");
  const ir::domain_id e = find_domain(q, "mul(@0,#0)");
  ASSERT_GE(e, 0);
  EXPECT_EQ(q.domains[static_cast<std::size_t>(e)].rows, 3);
  EXPECT_EQ(q.literals.size(), 1u);
  EXPECT_EQ(q.columns.size(), 0u);
}

TEST(IrProgram, ReferencesCarryNoClass) {
  // z1 = exp(a) * 2, z2 = (a * b) * 2: the factors are boundaries of different classes (both are
  // outputs); their consumers have one signature and gather from two domains.
  Tape t;
  {
    Tape::Scope scope(t);
    Rec a = make_input(t, 0.5);
    Rec b = make_input(t, 0.25);
    Rec y1 = exp(a);
    Rec y2 = a * b;
    register_output(t, y1);
    register_output(t, y2);
    register_output(t, y1 * 2.0);
    register_output(t, y2 * 2.0);
  }
  const ir::Program p = check(t, "class-insensitive refs");
  const ir::domain_id d = find_domain(p, "mul(@0,#0)");
  ASSERT_GE(d, 0);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(d)].rows, 2);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(d)].reads.size(), 2u);
  EXPECT_GE(find_domain(p, "exp(@0)"), 0);
  EXPECT_GE(find_domain(p, "mul(@0,@1)"), 0);
}

TEST(IrProgram, SharingRuleMaterialisesEveryInstanceOfASharedComputation) {
  // f(a) = exp(a)*a is shared by two consumers; f(b) = exp(b)*b has one consumer. Per node,
  // f(b) would be inlined and its consumer would be a class of its own; per computation, f(b)
  // is materialised too and the three consumers share one class.
  Tape t;
  {
    Tape::Scope scope(t);
    Rec a = make_input(t, 0.5);
    Rec b = make_input(t, 0.25);
    Rec fa = exp(a) * a;
    Rec fb = exp(b) * b;
    register_output(t, fa + 1.0);
    register_output(t, fa + 2.0);
    register_output(t, fb + 3.0);
  }
  ir::InferStats stats;
  const ir::Program p = ir::infer(t, &stats);
  EXPECT_EQ(stats.class_promoted, 1u);
  std::string diff;
  EXPECT_TRUE(ir::roundtrip_identical(t, ir::expand(p), &diff)) << diff;
  const ir::domain_id df = find_domain(p, "mul(exp(@0),@1)");
  const ir::domain_id dc = find_domain(p, "add(@0,$0)");
  ASSERT_GE(df, 0);
  ASSERT_GE(dc, 0);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(df)].rows, 2);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(dc)].rows, 3);
  EXPECT_EQ(p.domains.size(), 3u);  // input, f, consumers
}

TEST(IrProgram, CommutativeOperandsAreCanonicallyOrdered) {
  // x*2 and 3*y have the same signature; the class emits the first instance's operand order,
  // and the round-trip comparison treats a+b and b+a as one node.
  Tape t;
  {
    Tape::Scope scope(t);
    Rec x = make_input(t, 0.5);
    Rec y = make_input(t, 0.25);
    register_output(t, x * 2.0);
    register_output(t, 3.0 * y);
    register_output(t, x + y);
    register_output(t, y + x);
  }
  const ir::Program p = check(t, "commutative");
  const ir::domain_id dm = find_domain(p, "mul(@0,$0)");
  ASSERT_GE(dm, 0);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(dm)].rows, 2);
  const ir::domain_id da = find_domain(p, "add(@0,@1)");
  ASSERT_GE(da, 0);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(da)].rows, 2);
}

TEST(IrProgram, ConstantMembersAndConstantOutputsAreRowsOfTheConstDomain) {
  Tape t;
  {
    Tape::Scope scope(t);
    Rec x = make_input(t, 0.5);
    Rec y = make_input(t, 0.25);
    const epykos::node_id c = t.constant(7.0);
    const std::vector<epykos::node_id> args = {c, x.node(), y.node()};
    const epykos::node_id s = t.variadic(Op::Sum, args);
    t.output(s);
    t.output(t.constant(-1.0));  // a constant output
  }
  t.validate();
  const ir::Program p = check(t, "const rows");
  const ir::domain_id dc = find_domain(p, "const($0)");
  ASSERT_GE(dc, 0);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(dc)].rows, 2);
  const ir::domain_id ds = find_domain(p, "sum(%0)");
  ASSERT_GE(ds, 0);
  EXPECT_EQ(p.domain_of(p.outputs[1]), dc);
  EXPECT_EQ(p.domain_of(p.outputs[0]), ds);
  const std::vector<double> out = ir::evaluate(p, {0.5, 0.25});
  EXPECT_EQ(bits(out[0]), bits((7.0 + 0.5) + 0.25));
  EXPECT_EQ(bits(out[1]), bits(-1.0));
}

TEST(IrProgram, DirectRecurrenceIsFlaggedAndSplitByLevel) {
  // x_j = x_{j-1} * c_j + d_j, every x_j an output: one class whose rows read their own class.
  Tape t;
  const int n = 5;
  {
    Tape::Scope scope(t);
    Rec x = make_input(t, 1.0);
    for (int j = 1; j <= n; ++j) {
      x = x * (1.0 + 0.1 * j) + (0.01 * j);
      register_output(t, x);
    }
  }
  ir::InferStats stats;
  const ir::Program p = ir::infer(t, &stats);
  const std::vector<ir::domain_id> rec = ir::recurrent_domains(p);
  EXPECT_EQ(rec.size(), static_cast<std::size_t>(n)) << "one level per step";
  for (ir::domain_id d : rec) {
    EXPECT_EQ(p.domains[static_cast<std::size_t>(d)].rows, 1);
    EXPECT_TRUE(p.domains[static_cast<std::size_t>(d)].recurrent);
  }
  EXPECT_EQ(stats.classes, 2u);
  EXPECT_EQ(stats.domains, static_cast<std::size_t>(n) + 1);
  std::string diff;
  EXPECT_TRUE(ir::roundtrip_identical(t, ir::expand(p), &diff)) << diff;
  const std::vector<double> a = epykos::replay(t, {2.0});
  const std::vector<double> b = ir::evaluate(p, {2.0});
  for (std::size_t k = 0; k < a.size(); ++k) EXPECT_EQ(bits(a[k]), bits(b[k])) << k;
}

TEST(IrProgram, IndirectCycleIsSplitByLevelWithoutARecurrence) {
  // legs = Sum(coupons), swap = 2 * (leg_a - leg_b) (an output, hence a boundary), book =
  // Sum(swaps): the Sum class and the Mul class read each other through one another but no
  // class reads itself directly. The Sum class splits into level 0 (legs) and level 2 (book).
  Tape t;
  {
    Tape::Scope scope(t);
    std::vector<Rec> z;
    for (int k = 0; k < 4; ++k) z.push_back(make_input(t, 0.01 * (k + 1)));
    std::vector<Rec> swaps;
    for (int i = 0; i < 3; ++i) {
      Rec leg_a = exp(z[0]) * (1.0 + i) + exp(z[1]) * (2.0 + i) + exp(z[2]) * (3.0 + i);
      Rec leg_b = exp(z[1]) * (4.0 + i) + exp(z[3]) * (5.0 + i);
      Rec pv = 2.0 * (leg_a - leg_b);
      register_output(t, pv);
      swaps.push_back(pv);
    }
    Rec book = swaps[0];
    for (std::size_t i = 1; i < swaps.size(); ++i) book = book + swaps[i];
    register_output(t, book);
  }
  epykos::standard_passes(t);
  const ir::Program p = check(t, "indirect cycle");
  EXPECT_TRUE(ir::recurrent_domains(p).empty());
  int sums = 0, levels_seen = 0;
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    if (p.groups[d].steps.back().op == Op::Sum) {
      ++sums;
      levels_seen += p.domains[d].level;
    }
  }
  EXPECT_EQ(sums, 2) << "legs at level 0 and the book at level 2";
  EXPECT_EQ(levels_seen, 2);
}

TEST(IrProgram, ValidateRejectsCorruptPrograms) {
  const m1::Book full = m1::make_m1_book();
  const ir::Program good = ir::infer(m1::record_m1(epykos::test::sub_book(full, {0, 300})));
  ASSERT_NO_THROW(ir::validate(good));
  {
    ir::Program bad = good;
    for (ir::Gather& g : bad.gathers) {
      if (!g.index.empty()) {
        g.index[0] = static_cast<ir::value_id>(bad.num_values()) - 1;  // a forward read
        break;
      }
    }
    EXPECT_THROW(ir::validate(bad), std::runtime_error);
  }
  {
    ir::Program bad = good;
    bad.segments[0].offsets.back() += 1;
    EXPECT_THROW(ir::validate(bad), std::runtime_error);
  }
  {
    ir::Program bad = good;
    bad.domains[1].value_base += 1;
    EXPECT_THROW(ir::validate(bad), std::runtime_error);
  }
  {
    ir::Program bad = good;
    bad.groups[2].steps[0].a = ir::Slot{ir::SlotKind::Step, 0};
    EXPECT_THROW(ir::validate(bad), std::runtime_error);
  }
  {
    ir::Program bad = good;
    bad.outputs.push_back(static_cast<ir::value_id>(bad.num_values()));
    EXPECT_THROW(ir::validate(bad), std::runtime_error);
  }
  EXPECT_THROW(ir::deserialize("epykos-ir 2\n"), std::runtime_error);
  EXPECT_THROW(ir::deserialize("nonsense"), std::runtime_error);
}

TEST(IrProgram, SerialisationIsExactOnTheM1Book) {
  const ir::Program p = ir::infer(m1::record_m1(m1::make_m1_book()));
  const std::string text = ir::serialize(p);
  const ir::Program q = ir::deserialize(text);
  EXPECT_TRUE(q == p);
  EXPECT_EQ(ir::serialize(q), text);
  EXPECT_EQ(ir::to_string(q), ir::to_string(p));
  // Value-space helpers.
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    const ir::Domain& dom = p.domains[d];
    EXPECT_EQ(p.domain_of(dom.value_base), static_cast<ir::domain_id>(d));
    EXPECT_EQ(p.domain_of(dom.value_base + dom.rows - 1), static_cast<ir::domain_id>(d));
    EXPECT_EQ(p.row_of(dom.value_base + dom.rows - 1), dom.rows - 1);
  }
  EXPECT_EQ(p.num_values(), static_cast<std::size_t>(p.domains.back().value_base + p.domains.back().rows));
}

TEST(IrProgram, ShapeStringsAndSlotKindNames) {
  EXPECT_STREQ(ir::to_string(ir::SlotKind::Gather), "gather");
  EXPECT_STREQ(ir::to_string(ir::SlotKind::Segment), "segment");
  Tape t;
  {
    Tape::Scope scope(t);
    Rec x = make_input(t, 0.5);
    register_output(t, select(x < 1.0, -x, x * 3.0));
  }
  const ir::Program p = ir::infer(t);
  EXPECT_GE(find_domain(p, "select(cmp_lt(@0,#0),neg(@1),mul(@2,#1))"), 0) << ir::to_string(p);
}
