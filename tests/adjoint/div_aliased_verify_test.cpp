// The adjoint of a Div whose two operand slots are THE SAME slot.
//
// `acc_div` is the only adjoint kernel that takes two output accumulators in one call, and both
// are declared `EPY_RESTRICT`. When the operands alias, that declaration is a lie: the compiler
// may keep the two accumulations in separate registers and write back independently, losing one.
// GCC 13 does exactly that; Apple clang 21 happens not to, which is why this was invisible until
// the branch reached CI (D84).
//
// Why this file exists SEPARATELY from tests/adjoint/review_probe_test.cpp, which found it:
//
//   * that file is an M2 review probe and its name matches none of `scripts/mutation_test.sh`'s
//     GATE_REGEX, so a mutant pinning this fix would have no catcher and would survive a full
//     harness run — the same trap D80 records walking into;
//   * and it only caught the defect ON GCC. A gate that passes on one compiler while the bug is
//     present is a weak gate. The mutant below makes the protection compiler-independent, because
//     it removes the aliased branch explicitly rather than relying on a compiler to exploit the
//     undefined behaviour.
//
// The aliasing is constructed by hand. `simplify`'s `div(x, x) -> 1` means a RECORDING can no
// longer produce one, but `ir::validate` accepts an aliased Div, so the IR contract permits it and
// any future rewrite could emit one. The engine should be correct for what its own contract allows.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/mutation/mutation.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/tape.hpp"

namespace {

namespace ir = epykos::ir;
namespace adjoint = epykos::adjoint;
using epykos::Rec;
using epykos::Tape;

// d out_o / d in_k for every output, by seeding one output adjoint at a time.
std::vector<std::vector<double>> adj_jac(const adjoint::Adjoint& ad, const std::vector<double>& x,
                                         std::vector<double>* fwd) {
  const int n_in = ad.n_inputs();
  const int n_out = ad.n_outputs();
  std::vector<std::vector<double>> J(static_cast<std::size_t>(n_out),
                                     std::vector<double>(static_cast<std::size_t>(n_in), 0.0));
  fwd->assign(static_cast<std::size_t>(n_out), 0.0);
  for (int o0 = 0; o0 < n_out; o0 += ad.max_batch()) {
    const int B = std::min(ad.max_batch(), n_out - o0);
    const std::size_t Bs = static_cast<std::size_t>(B);
    std::vector<double> st(static_cast<std::size_t>(n_in) * Bs), ob(static_cast<std::size_t>(n_out) * Bs, 0.0);
    std::vector<double> out(static_cast<std::size_t>(n_out) * Bs), sb(static_cast<std::size_t>(n_in) * Bs, 0.0);
    for (std::size_t k = 0; k < static_cast<std::size_t>(n_in); ++k) {
      for (std::size_t b = 0; b < Bs; ++b) st[k * Bs + b] = x[k];
    }
    for (std::size_t b = 0; b < Bs; ++b) ob[(static_cast<std::size_t>(o0) + b) * Bs + b] = 1.0;
    ad.run(st.data(), B, ob.data(), out.data(), sb.data());
    for (std::size_t b = 0; b < Bs; ++b) {
      for (std::size_t k = 0; k < static_cast<std::size_t>(n_in); ++k) {
        J[static_cast<std::size_t>(o0) + b][k] = sb[k * Bs + b];
      }
    }
    for (std::size_t o = 0; o < static_cast<std::size_t>(n_out); ++o) (*fwd)[o] = out[o * Bs];
  }
  return J;
}

// `a / b` over two inputs, with the Div's two operand slots then forced to the SAME slot, so the
// program computes a / a. Recording cannot produce this; the IR contract permits it.
ir::Program aliased_div_program() {
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(t, 0.7);
    const Rec b = epykos::make_input(t, 1.3);
    epykos::register_output(t, a / b);
  }
  ir::Program p = ir::infer(t);
  bool found = false;
  for (ir::Group& g : p.groups) {
    for (ir::Step& s : g.steps) {
      if (s.op == epykos::Op::Div) {
        s.b = s.a;   // <- the aliasing
        found = true;
      }
    }
  }
  EXPECT_TRUE(found) << ir::to_string(p);
  EXPECT_NO_THROW(ir::validate(p)) << "the IR contract accepts an aliased Div, which is the point";
  return p;
}

}  // namespace

TEST(DivAliasedVerify, TheAdjointOfXOverXIsZero) {
  const ir::Program p = aliased_div_program();
  const std::vector<double> x = {0.7, 1.3};
  // Every tile width: the accumulation is per element, so a tile that splits the row must not
  // change the answer either.
  for (const int tile : {1, 4, 256}) {
    adjoint::Options o;
    o.tile = tile;
    const adjoint::Adjoint ad(p, o);
    std::vector<double> fwd;
    const std::vector<std::vector<double>> J = adj_jac(ad, x, &fwd);
    ASSERT_FALSE(J.empty());
    EXPECT_DOUBLE_EQ(fwd[0], 1.0) << "tile " << tile << ": the forward value of x/x";
    EXPECT_LE(std::fabs(J[0][0]), 1e-12)
        << "tile " << tile << ": d(x/x)/dx = " << J[0][0] << ", expected 0. Both contributions of "
        << "an aliased Div must land in the one buffer (acc_div_aliased); if only the numerator's "
        << "does, this reads 1/x = " << 1.0 / x[0];
    // The second input is not read by the aliased program at all.
    EXPECT_LE(std::fabs(J[0][1]), 1e-12) << "tile " << tile;
  }
}

TEST(DivAliasedVerify, TheUnaliasedDivIsUnaffected) {
  // The control: the ordinary two-operand Div must be exactly what it always was, so a fix for
  // the aliased case cannot be a blanket change to the Div rule.
  Tape t;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(t, 0.7);
    const Rec b = epykos::make_input(t, 1.3);
    epykos::register_output(t, a / b);
  }
  const ir::Program p = ir::infer(t);
  adjoint::Options o;
  const adjoint::Adjoint ad(p, o);
  std::vector<double> fwd;
  const std::vector<std::vector<double>> J = adj_jac(ad, {0.7, 1.3}, &fwd);
  EXPECT_NEAR(fwd[0], 0.7 / 1.3, 1e-15);
  EXPECT_NEAR(J[0][0], 1.0 / 1.3, 1e-14);                    // d(a/b)/da
  EXPECT_NEAR(J[0][1], -0.7 / (1.3 * 1.3), 1e-14);           // d(a/b)/db
}
