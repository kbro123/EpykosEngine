// M3/G4: the cross-stage sharing helpers (ir/sharing.hpp) on a small recorded program: reach
// masks, reader counts, the report and the assertion, including a domain that is NOT shared.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/ir/sharing.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

using epykos::Op;
using epykos::Rec;
using epykos::Tape;
namespace ir = epykos::ir;

namespace {

// Group A: y0 = exp(2a) + 1; y1 = exp(exp(2a)). Group B: y2 = exp(3b) · a; y3 = exp(2a) · b.
// After the passes: one `exp(mul(@,$))` domain with rows exp(2a), exp(3b) — read by both groups
// — and one `exp(@)` domain (the outer exp of y1) read by group A only.
struct Small {
  Tape tape;
  ir::Program program;
  std::vector<std::vector<int>> groups;
};

Small make_small() {
  Small s;
  {
    Tape::Scope scope(s.tape);
    const Rec a = epykos::make_input(s.tape, 0.1);
    const Rec b = epykos::make_input(s.tape, 0.2);
    const Rec e2a = exp(a * 2.0);
    const Rec e3b = exp(b * 3.0);
    epykos::register_output(s.tape, e2a + 1.0);
    epykos::register_output(s.tape, exp(exp(a * 2.0)));
    epykos::register_output(s.tape, e3b * a);
    epykos::register_output(s.tape, exp(a * 2.0) * b);
  }
  epykos::standard_passes(s.tape);
  s.program = ir::infer(s.tape);
  s.groups = {{0, 1}, {2, 3}};
  return s;
}

}  // namespace

TEST(Sharing, ReachMasksAndReaders) {
  const Small s = make_small();
  const std::vector<std::uint32_t> mask = ir::reach(s.program, s.groups);
  const std::vector<std::vector<ir::domain_id>> rd = ir::readers(s.program);
  ASSERT_EQ(mask.size(), s.program.domains.size());
  // The Input domain feeds both groups; every output's own domain feeds its group.
  const ir::domain_id input_domain = s.program.domain_of(s.program.inputs[0]);
  EXPECT_EQ(mask[static_cast<std::size_t>(input_domain)], 3u);
  for (std::size_t g = 0; g < s.groups.size(); ++g) {
    for (int o : s.groups[g]) {
      const ir::domain_id d = s.program.domain_of(s.program.outputs[static_cast<std::size_t>(o)]);
      EXPECT_TRUE((mask[static_cast<std::size_t>(d)] >> g) & 1u) << "output " << o;
    }
  }
  EXPECT_GE(rd[static_cast<std::size_t>(input_domain)].size(), 1u);
}

TEST(Sharing, ReportSeparatesSharedFromPartialExpDomains) {
  const Small s = make_small();
  const ir::SharingReport rep = ir::sharing(s.program, s.groups, Op::Exp);
  std::cout << rep.to_string();
  EXPECT_EQ(rep.n_groups, 2);
  EXPECT_EQ(rep.matching.size(), 2u) << "exp(mul(@,$)) and exp(@)";
  EXPECT_EQ(rep.shared.size(), 1u);
  EXPECT_EQ(rep.partial.size(), 1u);
  EXPECT_TRUE(rep.unshared.empty());
  // The shared one is the 2-row exp of a product; the partial one the 1-row exp of an exp.
  EXPECT_EQ(rep.rows[static_cast<std::size_t>(rep.shared[0])].rows, 2);
  EXPECT_EQ(rep.rows[static_cast<std::size_t>(rep.partial[0])].rows, 1);
  std::string why;
  EXPECT_FALSE(ir::assert_shared(rep, 2, &why));
  EXPECT_FALSE(why.empty());
  EXPECT_FALSE(ir::assert_shared(rep, 1, &why));
}

TEST(Sharing, AssertionHoldsWhenEveryExpDomainFeedsEveryGroup) {
  Tape tape;
  {
    Tape::Scope scope(tape);
    const Rec a = epykos::make_input(tape, 0.1);
    const Rec b = epykos::make_input(tape, 0.2);
    epykos::register_output(tape, exp(a * 2.0) + b);
    epykos::register_output(tape, exp(a * 2.0) * b);
  }
  epykos::standard_passes(tape);
  const ir::Program p = ir::infer(tape);
  const ir::SharingReport rep = ir::sharing(p, {{0}, {1}}, Op::Exp);
  std::string why;
  EXPECT_TRUE(ir::assert_shared(rep, 1, &why)) << why;
  EXPECT_THROW(ir::reach(p, {{0}, {7}}), std::invalid_argument);
}
