// M4/R-b: the shared structural-surgery primitives (rewrite/ir_edit.hpp) R4a, R4b and R5 build on.
// Tested here in isolation, on hand-built programs small enough to check by inspection, before
// any rule relies on them.
#include <gtest/gtest.h>

#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/rewrite/ir_edit.hpp"

namespace ir = epykos::ir;
namespace detail = epykos::rewrite::detail;

namespace {

// input(3 rows) -> A = mul(@0,@1) [2 rows, gathers input rows 0*1 and 1*2] -> output both rows of A.
ir::Program input_then_a_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 3, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2};
  p.input_values = {2.0, 3.0, 5.0};

  p.domains.push_back(ir::Domain{"mul(@0,@1)", 2, 3, 0, false, {0}, false, -1});
  p.gathers.push_back(ir::Gather{1, {0, 1}});  // @0: row0->in0(2.0), row1->in1(3.0)
  p.gathers.push_back(ir::Gather{1, {1, 2}});  // @1: row0->in1(3.0), row1->in2(5.0)
  p.groups.push_back(ir::Group{1, {ir::Step{epykos::Op::Mul, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}}}});
  p.outputs = {3, 4};  // 2*3=6, 3*5=15
  return p;
}

TEST(IrEdit, InsertDomainAfterShiftsEverythingAfterAndKeepsOutputsCorrect) {
  const ir::Program before = input_then_a_program();
  ir::validate(before);
  const std::vector<double> out_before = ir::evaluate(before, before.input_values);
  ASSERT_EQ(out_before, (std::vector<double>{6.0, 15.0}));

  // Insert a 2-row "neg(@identity into input)" domain right after the input domain (id 0), read
  // by nobody: outputs (which point at the OLD domain-1 "mul" rows) must land on the SAME values
  // after every later domain and value id shifts.
  ir::Domain nd;
  nd.name = "neg(@0)";
  nd.rows = 2;
  ir::Group ng;
  std::vector<ir::Gather> new_gathers = {ir::Gather{detail::new_domain_marker(), {0, 1}}};  // negates input rows 0,1
  // insert_domain_after's own contract: new gathers are appended after every existing one, so
  // this program's one new gather (there were none before) lands at index `before.gathers.size()`.
  ng.steps = {ir::Step{epykos::Op::Neg, ir::Slot{ir::SlotKind::Gather, static_cast<std::int32_t>(before.gathers.size())}, {}, {}, {}}};

  const detail::InsertResult r = detail::insert_domain_after(before, /*after=*/0, nd, ng, new_gathers, {});
  ir::validate(r.program);
  EXPECT_EQ(r.new_id, 1);
  EXPECT_EQ(r.new_base, 3);
  ASSERT_EQ(r.program.domains.size(), 3u);
  EXPECT_EQ(r.program.domains[2].value_base, 5);  // shifted by +2 (the inserted domain's rows)

  const std::vector<double> out_after = ir::evaluate(r.program, before.input_values);
  EXPECT_EQ(out_after, out_before);

  // The new domain itself computes what it should: neg(2.0) = -2.0, neg(3.0) = -3.0.
  ir::Evaluator ev(r.program);
  std::vector<double> outs(r.program.outputs.size());
  ev.run(before.input_values.data(), outs.data());
  EXPECT_DOUBLE_EQ(ev.values()[3], -2.0);
  EXPECT_DOUBLE_EQ(ev.values()[4], -3.0);
}

// input(2 rows) -> P = mul(@0,@1) [1 row: in0*in1] -> D = add(gather(P), @in) [1 row: P + in2]
// (D also reads a THIRD input directly). P has exactly one reader (D, via a bijective identity
// gather over its single row) -- the shape merge_producer_into_consumer targets.
ir::Program producer_consumer_program() {
  ir::Program p;
  p.domains.push_back(ir::Domain{"input", 3, 0, 0, false, {}, false, -1});
  p.groups.push_back(ir::Group{0, {ir::Step{epykos::Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}}}});
  p.inputs = {0, 1, 2};
  p.input_values = {2.0, 3.0, 7.0};

  p.domains.push_back(ir::Domain{"mul(@0,@1)", 1, 3, 0, false, {0}, false, -1});
  p.gathers.push_back(ir::Gather{1, {0}});  // @0 -> in0
  p.gathers.push_back(ir::Gather{1, {1}});  // @1 -> in1
  p.groups.push_back(ir::Group{1, {ir::Step{epykos::Op::Mul, ir::Slot{ir::SlotKind::Gather, 0}, ir::Slot{ir::SlotKind::Gather, 1}, {}, {}}}});

  p.domains.push_back(ir::Domain{"add(@0,@1)", 1, 4, 0, false, {0, 1}, false, -1});
  p.gathers.push_back(ir::Gather{2, {3}});   // @2 (index 2): identity gather into P's one row (value id 3)
  p.gathers.push_back(ir::Gather{2, {2}});   // @3 (index 3): in2
  p.groups.push_back(ir::Group{2, {ir::Step{epykos::Op::Add, ir::Slot{ir::SlotKind::Gather, 2}, ir::Slot{ir::SlotKind::Gather, 3}, {}, {}}}});
  p.outputs = {4};  // (in0*in1) + in2 = 2*3 + 7 = 13
  return p;
}

TEST(IrEdit, MergeProducerIntoConsumerAgreesWithTheUnmergedProgram) {
  const ir::Program before = producer_consumer_program();
  ir::validate(before);
  const std::vector<double> out_before = ir::evaluate(before, before.input_values);
  ASSERT_EQ(out_before, (std::vector<double>{13.0}));

  const ir::Program after = detail::merge_producer_into_consumer(before, /*producer=*/1, /*consumer=*/2, /*consumer_gather=*/2);
  ir::validate(after);
  ASSERT_EQ(after.domains.size(), 2u);           // producer absorbed
  EXPECT_EQ(after.groups[1].steps.size(), 2u);   // producer's Mul + consumer's Add, one group
  EXPECT_EQ(after.groups[1].steps[1].op, epykos::Op::Add);
  EXPECT_EQ(after.groups[1].steps[1].a.kind, ir::SlotKind::Step);
  EXPECT_EQ(after.groups[1].steps[1].a.index, 0);  // now reads the Mul step directly, not a gather

  const std::vector<double> out_after = ir::evaluate(after, before.input_values);
  EXPECT_EQ(out_after, out_before);
}

}  // namespace
