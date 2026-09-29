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
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "ir/ir_test_helpers.hpp"

namespace ir = epykos::ir;
namespace fixtures = epykos::fixtures;
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

TEST(IrProgram, DirectRecurrenceIsAScanDomain) {
  // x_j = x_{j-1} * c_j + d_j, every x_j an output: a chain of five identical steps, detected
  // without hints (D41): one recurrent scan domain of five rows whose carry gather reads the
  // input for the first row and the previous row after that.
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
  EXPECT_EQ(stats.chains, 1u);
  EXPECT_EQ(stats.chain_nodes, static_cast<std::size_t>(n));
  EXPECT_EQ(stats.scan_classes, 1u);
  EXPECT_EQ(stats.scan_rounds, 1u);
  const std::vector<ir::domain_id> scan = ir::scan_domains(p);
  ASSERT_EQ(scan.size(), 1u) << ir::to_string(p);
  EXPECT_EQ(ir::recurrent_domains(p), scan);
  EXPECT_EQ(ir::scan_class_domains(p), scan);
  const ir::Domain& dom = p.domains[static_cast<std::size_t>(scan[0])];
  EXPECT_EQ(dom.rows, n);
  EXPECT_TRUE(dom.recurrent);
  EXPECT_TRUE(dom.scan_class);
  EXPECT_EQ(dom.name, "add(mul(^,$0),$1)@scan");
  ASSERT_EQ(p.scans.size(), 1u);
  const ir::Scan& sc = p.scans[0];
  EXPECT_EQ(sc.domain, scan[0]);
  EXPECT_EQ(sc.chains(), 1);
  EXPECT_EQ(sc.chain_offsets, (std::vector<std::int32_t>{0, n}));
  const ir::Gather& carry = p.gathers[static_cast<std::size_t>(sc.carry_gather)];
  EXPECT_EQ(carry.index[0], p.inputs[0]);
  for (int r = 1; r < n; ++r) EXPECT_EQ(carry.index[static_cast<std::size_t>(r)], dom.value_base + r - 1);
  EXPECT_EQ(stats.classes, 2u);
  EXPECT_EQ(stats.domains, 2u);
  std::string diff;
  EXPECT_TRUE(ir::roundtrip_identical(t, ir::expand(p), &diff)) << diff;
  const std::vector<double> a = epykos::replay(t, {2.0});
  const std::vector<double> b = ir::evaluate(p, {2.0});
  for (std::size_t k = 0; k < a.size(); ++k) EXPECT_EQ(bits(a[k]), bits(b[k])) << k;
  // The serialised form carries the scan.
  const ir::Program q = ir::deserialize(ir::serialize(p));
  EXPECT_TRUE(q == p);
  EXPECT_EQ(q.scans, p.scans);
}

TEST(IrProgram, TwoStepChainsAreNotScans) {
  // Two identical steps in a row are straight-line code (D41: a chain needs three): x·a·b with
  // a, b references stays one class of inner nodes, exp(exp(x)) with the inner exp an output
  // stays a level-split class (D23).
  Tape t;
  {
    Tape::Scope scope(t);
    for (int r = 0; r < 3; ++r) {
      const Rec x = make_input(t, 0.5 + r);
      const Rec a = make_input(t, 1.5 + r);
      const Rec b = make_input(t, 2.5 + r);
      register_output(t, x * a * b);
      const Rec inner = exp(x);
      register_output(t, inner);
      register_output(t, exp(inner));
    }
  }
  ir::InferStats stats;
  const ir::Program p = check(t, "two-step chains");
  ir::infer(t, &stats);
  EXPECT_EQ(stats.chains, 0u);
  EXPECT_TRUE(ir::scan_domains(p).empty()) << ir::to_string(p);
  EXPECT_TRUE(ir::recurrent_domains(p).empty());
  EXPECT_EQ(ir::scan_class_domains(p).size(), 2u) << "exp@L0 and exp@L1";
  EXPECT_GE(find_domain(p, "mul(mul(@0,@1),@2)"), 0) << ir::to_string(p);
}

TEST(IrProgram, ARunningSumOfInnerValuesIsNotAScan) {
  // acc = acc + term_k with every partial sum used once is a reduction (fold_sum's Sum), never a
  // scan: on the raw recording the chain is one add tree, after fold_sum one Sum row.
  Tape t;
  {
    Tape::Scope scope(t);
    for (int r = 0; r < 2; ++r) {
      Rec acc = make_input(t, 0.1 + r);
      for (int k = 1; k <= 6; ++k) acc = acc + exp(make_input(t, 0.01 * k + r));
      register_output(t, acc);
    }
  }
  ir::InferStats stats;
  const ir::Program raw = ir::infer(t, &stats);
  EXPECT_EQ(stats.chains, 0u);
  EXPECT_TRUE(ir::scan_domains(raw).empty()) << ir::to_string(raw);
  epykos::standard_passes(t);
  const ir::Program passed = ir::infer(t, &stats);
  EXPECT_EQ(stats.chains, 0u);
  EXPECT_TRUE(ir::scan_domains(passed).empty()) << ir::to_string(passed);
  EXPECT_GE(find_domain(passed, "sum(%0)"), 0) << ir::to_string(passed);
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
  EXPECT_TRUE(ir::scan_class_domains(p).empty());
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
  const fixtures::Book full = fixtures::make_m1_book();
  const ir::Program good = ir::infer(fixtures::record_m1(epykos::test::sub_book(full, {0, 300})));
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
  EXPECT_THROW(ir::deserialize("epykos-ir 1\n"), std::runtime_error);  // an older format version
  EXPECT_THROW(ir::deserialize("epykos-ir 2\n"), std::runtime_error);  // truncated
  EXPECT_THROW(ir::deserialize("nonsense"), std::runtime_error);

}

TEST(IrProgram, SerialisationIsExactOnTheM1Book) {
  const ir::Program p = ir::infer(fixtures::record_m1(fixtures::make_m1_book()));
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

// A group of more than 24 steps is named "<op>[<steps>]" with no whitespace, so that serialize
// (one token per name) round-trips: five different two-node steps cycled fifteen times is one
// 30-step group (the cycle is ten nodes long, beyond the depth the scan detection looks for a
// carry, so it is straight-line code, not a chain: D41; two alternating steps would be a
// period-two scan).
TEST(IrProgram, LongGroupNamesSerialiseAndRoundTrip) {
  Tape t;
  {
    Tape::Scope scope(t);
    for (int row = 0; row < 3; ++row) {
      Rec y = make_input(t, 1.0 + row);
      for (int k = 0; k < 15; ++k) {
        switch (k % 5) {
          case 0: y = sqrt(y * 1.5); break;
          case 1: y = log(y / 0.5); break;
          case 2: y = exp(y * 0.1); break;
          case 3: y = -(y - 2.0); break;
          default: y = recip(y * 0.25); break;
        }
      }
      register_output(t, y);
    }
  }
  const ir::Program p = check(t, "long group");  // includes deserialize(serialize(p)) == p
  const ir::domain_id d = find_domain(p, "recip[30]");
  ASSERT_GE(d, 0) << ir::to_string(p);
  EXPECT_EQ(p.groups[static_cast<std::size_t>(d)].steps.size(), 30u);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(d)].rows, 3);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(d)].name, "recip[30]");
  EXPECT_TRUE(ir::scan_domains(p).empty());
  // A name with whitespace is refused by serialize rather than written as two tokens.
  ir::Program q = p;
  q.domains[static_cast<std::size_t>(d)].name = "recip[30 steps]";
  EXPECT_THROW((void)ir::serialize(q), std::runtime_error);
}

// The same fifteen steps of one kind form a chain: three chains of fifteen sqrt(y * 1.5) steps
// (their intermediates used once) are one scan domain of 45 rows, sequential along each chain.
TEST(IrProgram, RepeatedInnerStepsAreAScan) {
  Tape t;
  {
    Tape::Scope scope(t);
    for (int row = 0; row < 3; ++row) {
      Rec y = make_input(t, 1.0 + row);
      for (int k = 0; k < 15; ++k) y = sqrt(y * 1.5);
      register_output(t, y);
    }
  }
  ir::InferStats stats;
  const ir::Program p = check(t, "sqrt chains");
  ir::infer(t, &stats);
  EXPECT_EQ(stats.chains, 3u);
  EXPECT_EQ(stats.chain_nodes, 45u);
  const ir::domain_id d = find_domain(p, "sqrt(mul(^,#0))");
  ASSERT_GE(d, 0) << ir::to_string(p);
  EXPECT_EQ(p.domains[static_cast<std::size_t>(d)].name, "sqrt(mul(^,#0))@scan");
  EXPECT_EQ(p.domains[static_cast<std::size_t>(d)].rows, 45);
  ASSERT_EQ(p.scans.size(), 1u);
  EXPECT_EQ(p.scans[0].chain_offsets, (std::vector<std::int32_t>{0, 15, 30, 45}));
  const std::vector<double> a = epykos::replay(t, {1.0, 2.0, 3.0});
  const std::vector<double> b = ir::evaluate(p, {1.0, 2.0, 3.0});
  for (std::size_t k = 0; k < a.size(); ++k) EXPECT_EQ(bits(a[k]), bits(b[k])) << k;
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
