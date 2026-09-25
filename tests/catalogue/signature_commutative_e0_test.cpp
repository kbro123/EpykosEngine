// D61 regression gate: `catalogue::Signature` is invariant under the operand order of a
// COMMUTATIVE step, and the catalogue serves both orders with the one generated kernel, bitwise.
//
// Why this is a real invariant and not a convenience. `ir::infer`'s own signature pass already
// treats `a op b` and `b op a` as ONE isomorphism class for a commutative op (ir/signature.cpp's
// `hash_all` / `extract_tree` sort the two operand tokens); which of the two orders the emitted
// `ir::Step` then carries is `Class::emit_swapped`, "the first instance's RECORDED operand
// order". Tape order is not stable across compilers — C++ leaves a binary operator's operands
// unsequenced, so sibling subexpressions of one recorded statement reach the tape in whichever
// order the compiler chose — and on the default Stage A tape GCC 13 and Apple clang really do
// disagree: the same 517,036 nodes in a different order, and nine domains emitted as
// `mul(lit, gat)` / `mul(col, gat)` under one and `mul(gat, lit)` / `mul(gat, col)` under the
// other. Before D61 that made the committed registry compiler-dependent (63/66 groups catalogued
// on GCC vs 66/66 on Apple clang, CI red on ubuntu-latest only). The property below is what makes
// it impossible for that to recur without a test failing on EITHER compiler, rather than only on
// the one the registry was not generated on.
//
// An _e0_test.cpp TU (-ffp-contract=off in every preset): the bitwise leg compares the
// catalogue's generated kernels (src/catalogue/generated/kernels_e0.cpp, themselves E0) against
// the interpreter's generic per-step path, so it must hold under release and reference alike.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/catalogue/kernel.hpp"
#include "epykos/catalogue/registry.hpp"
#include "epykos/catalogue/signature.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/op.hpp"
#include "epykos/tape/tape.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace catalogue = epykos::catalogue;
namespace exec = epykos::exec;
namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// `program` with the two operands of EVERY commutative step of every group transposed. This is
// exactly the difference ir::infer's `Class::emit_swapped` can introduce between two recordings
// of the same maths, applied to all of them at once: a valid Program computing the identical
// values (IEEE-754 `+` and `*` are exactly commutative), which the catalogue must therefore
// fingerprint, dispatch and evaluate identically.
ir::Program transpose_commutative_operands(ir::Program p, std::size_t* transposed) {
  *transposed = 0;
  for (ir::Group& g : p.groups) {
    for (ir::Step& st : g.steps) {
      if (!epykos::op_is_commutative(st.op)) continue;
      if (st.a.kind == ir::SlotKind::None || st.b.kind == ir::SlotKind::None) continue;
      if (st.a == st.b) continue;  // nothing to observe
      std::swap(st.a, st.b);
      ++*transposed;
    }
  }
  return p;
}

std::vector<double> run_all(const ir::Program& program, bool use_catalogue, catalogue::Coverage* cov) {
  exec::Options o;
  o.use_catalogue = use_catalogue;
  const exec::Interpreter in(program, o);
  if (cov != nullptr) *cov = in.coverage();
  std::vector<double> out(static_cast<std::size_t>(in.n_outputs()));
  in.run(program.input_values.data(), 1, out.data());
  return out;
}

// The whole gate for one reference Program.
void check_commutative_invariance(const ir::Program& program, const std::string& label) {
  std::size_t transposed = 0;
  const ir::Program swapped = transpose_commutative_operands(program, &transposed);
  ASSERT_NO_THROW(ir::validate(swapped)) << label << ": transposing a commutative step's operands must stay valid";
  EXPECT_GT(transposed, 0u) << label << ": no commutative two-operand step -- this case would pass vacuously";

  // 1. The fingerprint itself: same Signature, same hash, same registry entry, domain by domain.
  std::size_t eligible = 0, catalogued = 0;
  ASSERT_EQ(program.domains.size(), swapped.domains.size()) << label;
  for (std::size_t d = 0; d < program.domains.size(); ++d) {
    const ir::domain_id did = static_cast<ir::domain_id>(d);
    ASSERT_EQ(catalogue::is_cataloguable(program, did), catalogue::is_cataloguable(swapped, did)) << label << " domain " << d;
    if (!catalogue::is_cataloguable(program, did)) continue;
    ++eligible;
    const catalogue::Signature a = catalogue::signature_of(program, did);
    const catalogue::Signature b = catalogue::signature_of(swapped, did);
    EXPECT_TRUE(a == b) << label << " domain " << d << ": " << a.to_string() << " vs " << b.to_string();
    EXPECT_EQ(a.hash(), b.hash()) << label << " domain " << d;
    EXPECT_EQ(a.to_string(), b.to_string()) << label << " domain " << d;
    EXPECT_EQ(catalogue::lookup(a), catalogue::lookup(b)) << label << " domain " << d << ": " << a.to_string();
    if (catalogue::lookup(a) != nullptr) ++catalogued;
  }
  std::cout << "[ commutative ] " << label << ": " << transposed << " commutative step(s) transposed, " << eligible
            << " catalogue-eligible domain(s), " << catalogued << " with a registry entry\n";
  EXPECT_GT(catalogued, 0u) << label << ": nothing dispatches -- this case would prove nothing";

  // 2. The binding and the kernel: the one catalogued kernel must evaluate both operand orders to
  //    the same bits, and to the same bits as the generic per-step path.
  catalogue::Coverage cov_a, cov_b;
  const std::vector<double> generic = run_all(program, /*use_catalogue=*/false, nullptr);
  const std::vector<double> cat_a = run_all(program, /*use_catalogue=*/true, &cov_a);
  const std::vector<double> cat_b = run_all(swapped, /*use_catalogue=*/true, &cov_b);
  EXPECT_EQ(cov_a.groups_catalogued, cov_b.groups_catalogued) << label;
  EXPECT_EQ(cov_a.groups_total, cov_b.groups_total) << label;
  ASSERT_EQ(generic.size(), cat_a.size()) << label;
  ASSERT_EQ(generic.size(), cat_b.size()) << label;
  std::size_t bad_a = 0, bad_b = 0;
  for (std::size_t k = 0; k < generic.size(); ++k) {
    if (bits(generic[k]) != bits(cat_a[k])) ++bad_a;
    if (bits(generic[k]) != bits(cat_b[k])) ++bad_b;
  }
  EXPECT_EQ(bad_a, 0u) << label << ": catalogue on/off differs on the original operand order";
  EXPECT_EQ(bad_b, 0u) << label << ": the catalogue evaluates the transposed operand order to different bits";
}

}  // namespace

TEST(CatalogueSignatureCommutativeE0, M1Book) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  check_commutative_invariance(ir::infer(tape), "M1 book");
}

TEST(CatalogueSignatureCommutativeE0, StageA) {
  fixtures::StageAOptions opt;
  opt.trades = 60;
  opt.scenarios = 0;
  const fixtures::StageA s = fixtures::make_stage_a(opt);
  const fixtures::StageATape tape = fixtures::record_stage_a(s);
  check_commutative_invariance(ir::infer(tape.tape), "Stage A (60 trades)");
}
