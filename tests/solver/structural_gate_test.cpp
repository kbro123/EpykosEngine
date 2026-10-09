// The structural gate (solver/sparsity.hpp): the dependency structure of a declared solve is
// COMPUTED, not assumed. Every unknown must reach at least one residual and every residual must be
// reached by at least one unknown, by reachability over the recorded forward program, checked once
// at block registration before any market data exists.
//
// The defect this exists to catch is D110 §3's, found by `tools/curveid/` with its own
// finite-difference solver and never put to the engine: our `(forward, flat)` curve has an EXACT
// ZERO COLUMN at the last knot. Flat makes the last knot's value govern only t >= t_last, which is
// invisible to every instrument maturing at or before it, and the knots sit AT the maturities — so
// the system is square by count and singular in substance. D110 recorded "and we do not detect
// it"; `forward_flat_last_knot_is_dead_and_the_gate_fires` is the measurement of that claim.
//
// The mechanism that makes a STRUCTURAL analysis sufficient for a numerically-stated defect is
// `curve/scheme.hpp`'s `flat_masses`: "Knots with zero mass are omitted". A knot contributing
// nothing to an integral is therefore ABSENT from the tape rather than present with weight 0, so
// reachability — a conservative superset of the nonzero pattern in general — is exact here.
// `pattern_is_tight_against_the_numerical_jacobian` measures that rather than assuming it, on the
// scheme the shipped fixtures use.
//
// Not an _e0_test: nothing here compares bits. The pattern is integer reachability and the one
// floating-point comparison (structural against numerical nonzeros) is an exact-zero test, which
// contraction cannot move off zero.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "epykos/maths/curve/curve.hpp"
#include "epykos/maths/curve/scheme.hpp"
#include "epykos/solver/implicit.hpp"
#include "epykos/solver/residual.hpp"
#include "epykos/solver/sparsity.hpp"
#include "epykos/tape/tape.hpp"

namespace solver = epykos::solver;
namespace curve = epykos::curve;
using epykos::Rec;
using epykos::Tape;

namespace {

// Knots AT the maturities, which is what every shipped problem does (blueprints/problems/*.json:
// "Knots are still at the instruments' own maturities and every block is still square") and what
// makes the Flat hole reachable at all.
const std::vector<double>& maturities() {
  static const std::vector<double> t = {1.0, 2.0, 3.0, 4.0, 5.0};
  return t;
}

// The par rate of an annual fixed-vs-float swap maturing at T on a curve: (1 − DF(T)) / Σ τ DF(t).
// One year fractions of 1.0, so the annuity is a plain sum of the annual discount factors. This is
// the same shape `tools/curveid/` prices and is deliberately the simplest thing that depends on
// the curve at every annual point up to T and nowhere beyond it.
template <class C, class S>
Rec par_rate(const C& c, const typename C::template State<Rec>& st, double T) {
  Rec ann = c.template df<Rec>(st, 1.0);
  for (double t = 2.0; t <= T + 0.5; t += 1.0) ann = ann + c.template df<Rec>(st, t);
  return (Rec(1.0) - c.template df<Rec>(st, T)) / ann;
}

struct Recorded {
  Tape tape;
  solver::ImplicitRegistry registry;
  std::vector<Rec> quotes;
  solver::ImplicitResult result;
};

// Records one block: n knots of `Scheme` on `V`, one par-rate residual per maturity. `coupled`
// false sets the opt-out. Throws out of `implicit()` when the gate fires.
template <class Scheme, curve::Variable V>
void record_block(Recorded& r, bool coupled = true) {
  Tape::Scope scope(r.tape);
  const std::vector<double>& t = maturities();
  const int n = static_cast<int>(t.size());
  curve::Curve<Scheme, V> c(t.data(), n);
  for (int i = 0; i < n; ++i) r.quotes.push_back(epykos::make_input(r.tape, 0.03));
  // A forward curve's state is the instantaneous forward; a logdf curve's is log DF, which must
  // decrease. Both starts are a flat 3% in the region's own variable.
  std::vector<double> z0(static_cast<std::size_t>(n));
  for (int k = 0; k < n; ++k) z0[static_cast<std::size_t>(k)] = V == curve::Variable::logdf ? -0.03 * t[static_cast<std::size_t>(k)] : 0.03;
  solver::SolveOptions opt;
  opt.require_structural_coupling = coupled;
  r.result = solver::implicit(
      r.tape, r.registry, "gate", z0, n,
      [&](const Rec* z, Rec* F) {
        auto st = c.template prepare<Rec>(z);
        for (int i = 0; i < n; ++i) F[i] = par_rate<curve::Curve<Scheme, V>, Rec>(c, st, t[static_cast<std::size_t>(i)]) - r.quotes[static_cast<std::size_t>(i)];
      },
      opt);
}

}  // namespace

// ---- the defect D110 §3 found -----------------------------------------------------------------

TEST(StructuralGate, forward_flat_last_knot_is_dead_and_the_gate_fires) {
  Recorded r;
  try {
    record_block<curve::Flat, curve::Variable::forward>(r);
    FAIL() << "(forward, flat) with knots at the maturities has an exact zero Jacobian column at "
              "the last knot (D110 §3) and the gate must refuse it";
  } catch (const std::invalid_argument& e) {
    const std::string why = e.what();
    // It must name WHICH unknown, and it must be the last one: Flat anchors value k to the
    // interval STARTING at knot k, so knot n−1 governs only t >= t_last and no instrument
    // maturing at or before t_last can see it.
    EXPECT_NE(why.find("z[4]"), std::string::npos) << why;
    EXPECT_NE(why.find("zero Jacobian column"), std::string::npos) << why;
    // And it must not claim a dead row: every residual reads knots 0..i.
    EXPECT_EQ(why.find("zero Jacobian row"), std::string::npos) << why;
  }
}

// The opt-out on a DEAD COLUMN does not admit the block, and this is the finding rather than a
// defect in the flag: an under-determined solve is UNREPRESENTABLE below the gate. `ResidualProgram`
// maps every unknown to a sub-input ordinal of the slice, `slice()` copies only the Inputs the
// residuals reach, so an unreached unknown has nothing to bind to and that constructor throws on
// its own — whatever `require_structural_coupling` says. D110 §3 recorded that an under-determined
// calibration goes undetected; measured here, the engine cannot carry one at all. What the gate
// adds in this direction is the moment (registration, not wherever a ResidualProgram is next
// built) and the message (which knot, and why).
TEST(StructuralGate, the_opt_out_cannot_admit_a_dead_column_because_the_solver_cannot_represent_one) {
  Recorded r;
  try {
    record_block<curve::Flat, curve::Variable::forward>(r, /*coupled=*/false);
    FAIL() << "the slice cannot bind an unknown no residual reads, so this must still throw";
  } catch (const std::invalid_argument& e) {
    const std::string why = e.what();
    // Not the gate's message — the gate was switched off. This is ResidualProgram's own bind
    // failure, which is why the opt-out's documentation says the column direction is not available.
    EXPECT_NE(why.find("ResidualProgram"), std::string::npos) << why;
    EXPECT_NE(why.find("zero Jacobian column"), std::string::npos) << why;
    EXPECT_EQ(why.find("require_structural_coupling"), std::string::npos) << why;
  }
  // The pattern itself is still computable and still names the knot: the analysis is independent
  // of whether anything can be done about the answer.
  ASSERT_EQ(r.registry.blocks.size(), 1u);
  const solver::ImplicitBlock& b = r.registry.blocks.at(0);
  const solver::SparsityPattern pat = solver::structural_pattern(r.tape, b);
  const std::vector<int> dead = pat.dead_unknowns();
  ASSERT_EQ(dead.size(), 1u) << pat.to_string();
  EXPECT_EQ(dead[0], b.n_unknowns() - 1);
  EXPECT_TRUE(pat.dead_residuals().empty()) << pat.to_string();
}

// The opt-out on a DEAD ROW does admit the block, because a residual that reads no unknown is an
// ordinary constant row the over-determined path carries. And the result is exactly the reason the
// gate is on by default: the solve returns NOT CONVERGED with a residual pinned at the constant
// row's value, and nothing in the report says the system was structurally unable to converge.
TEST(StructuralGate, the_opt_out_admits_a_dead_row_and_the_solve_then_cannot_converge) {
  Recorded r;
  Tape::Scope scope(r.tape);
  for (int i = 0; i < 2; ++i) r.quotes.push_back(epykos::make_input(r.tape, 0.03 + 0.001 * i));
  const std::vector<double> z0 = {0.03, 0.04};
  solver::SolveOptions opt;
  opt.require_structural_coupling = false;
  solver::ImplicitResult res;
  ASSERT_NO_THROW(({
    res = solver::implicit(
        r.tape, r.registry, "deadrow_optout", z0, 3,
        [&](const Rec* z, Rec* F) {
          F[0] = z[0] * 2.0 - r.quotes[0];
          F[1] = z[1] * 3.0 - r.quotes[1];
          F[2] = r.quotes[0] - r.quotes[1];
        },
        opt);
  }));
  EXPECT_FALSE(res.report.converged);
  // ‖F‖∞ is pinned at |q0 − q1| = 1e-3 and no step can move it.
  EXPECT_NEAR(res.report.residual_inf, 1e-3, 1e-12);
}

// (forward, linear) is the one cell of D110 §1's matrix that calibrates on both engines, and the
// reason is structural: `linear_masses` gives knot n−1 the `b` share of the last cell's integral,
// so an instrument maturing AT t_last reaches it. Same knots, same instruments, same variable.
TEST(StructuralGate, forward_linear_reaches_every_knot_and_is_admitted) {
  Recorded r;
  ASSERT_NO_THROW(({ record_block<curve::Linear, curve::Variable::forward>(r); }));
  const solver::SparsityPattern pat = solver::structural_pattern(r.tape, r.registry.blocks.at(0));
  EXPECT_TRUE(pat.dead_unknowns().empty()) << pat.to_string();
  EXPECT_TRUE(pat.dead_residuals().empty()) << pat.to_string();
}

// The shipped family. `fixtures::compare_ois` is (logdf, linear) and every blueprints/problems/
// curve is a -LOGDF definition, so this is the configuration that must keep passing.
TEST(StructuralGate, logdf_linear_the_shipped_family_is_admitted_and_lower_triangular) {
  Recorded r;
  ASSERT_NO_THROW(({ record_block<curve::Linear, curve::Variable::logdf>(r); }));
  const solver::SparsityPattern pat = solver::structural_pattern(r.tape, r.registry.blocks.at(0));
  EXPECT_TRUE(pat.dead_unknowns().empty()) << pat.to_string();
  EXPECT_TRUE(pat.dead_residuals().empty()) << pat.to_string();
  // Instrument i matures at T_i = t_i and reads every knot up to it: row i is nonzero in columns
  // 0..i exactly. That is the dense lower triangle, and it is why colouring buys nothing on a
  // single curve — column 0 appears in every row, so no two columns are ever orthogonal.
  for (int i = 0; i < pat.n_residuals; ++i) {
    for (int j = 0; j < pat.n_unknowns; ++j) {
      EXPECT_EQ(pat.reads(i, j), j <= i) << "row " << i << " column " << j;
    }
  }
  const solver::Colouring c = solver::colour_rows(pat);
  EXPECT_EQ(c.n_colours, pat.n_residuals);
}

// ---- the direction that did not exist --------------------------------------------------------
//
// `implicit_end` checked `Tape::tainted` per residual, which asks whether a residual depends on ANY
// input. A residual built from two quotes and no unknown satisfies it, binds a ResidualProgram
// without complaint, and leaves an exact zero Jacobian ROW that no step can move. `implicit.hpp`'s
// summary of that function claimed it "checks every unknown is read by some residual"; it never
// did, in either direction.
TEST(StructuralGate, a_residual_that_reads_only_quotes_is_a_dead_row_and_the_gate_fires) {
  Recorded r;
  Tape::Scope scope(r.tape);
  for (int i = 0; i < 2; ++i) r.quotes.push_back(epykos::make_input(r.tape, 0.03 + 0.001 * i));
  const std::vector<double> z0 = {0.03, 0.04};
  try {
    solver::implicit(r.tape, r.registry, "deadrow", z0, 3, [&](const Rec* z, Rec* F) {
      F[0] = z[0] * 2.0 - r.quotes[0];
      F[1] = z[1] * 3.0 - r.quotes[1];
      // Tainted — it depends on two inputs — and reads no unknown at all.
      F[2] = r.quotes[0] - r.quotes[1];
    });
    FAIL() << "a residual that reads no unknown is an exact zero Jacobian row and must be refused";
  } catch (const std::invalid_argument& e) {
    const std::string why = e.what();
    EXPECT_NE(why.find("F[2]"), std::string::npos) << why;
    EXPECT_NE(why.find("zero Jacobian row"), std::string::npos) << why;
    EXPECT_EQ(why.find("zero Jacobian column"), std::string::npos) << why;
  }
}

// ---- soundness: the structural superset is tight on the shipped scheme ------------------------
//
// `reads(i, j)` false implies ∂F_i/∂z_j is identically zero — that direction is what the gate
// rests on and it holds by construction. The converse does not hold in general: an expression may
// mention a value and have zero derivative in it. This measures the gap on the family the fixtures
// use, through the engine's own batched adjoint, so the claim in sparsity.hpp's header is a
// measurement and not an argument.
TEST(StructuralGate, pattern_is_tight_against_the_numerical_jacobian) {
  Recorded r;
  ASSERT_NO_THROW(({ record_block<curve::Linear, curve::Variable::logdf>(r); }));
  const solver::ImplicitBlock& b = r.registry.blocks.at(0);
  const solver::SparsityPattern pat = solver::structural_pattern(r.tape, b);
  solver::ResidualProgram rp(r.tape, b, /*passes=*/true);
  const std::vector<double> all = r.tape.input_values();
  std::vector<double> p(static_cast<std::size_t>(rp.n_params()));
  for (int m = 0; m < rp.n_params(); ++m) p[static_cast<std::size_t>(m)] = all[static_cast<std::size_t>(rp.param_ordinals()[static_cast<std::size_t>(m)])];
  std::vector<double> z(static_cast<std::size_t>(rp.n_unknowns()));
  for (int j = 0; j < rp.n_unknowns(); ++j) z[static_cast<std::size_t>(j)] = all[static_cast<std::size_t>(rp.unknown_ordinals()[static_cast<std::size_t>(j)])];
  std::vector<double> F(static_cast<std::size_t>(rp.n_residuals()));
  std::vector<double> Jz(static_cast<std::size_t>(rp.n_residuals()) * static_cast<std::size_t>(rp.n_unknowns()));
  std::vector<double> Jp(static_cast<std::size_t>(rp.n_residuals()) * static_cast<std::size_t>(rp.n_params() > 0 ? rp.n_params() : 1));
  rp.jacobian(z.data(), p.data(), F.data(), Jz.data(), rp.n_params() > 0 ? Jp.data() : nullptr);
  int structural = 0, numerical = 0;
  for (int i = 0; i < pat.n_residuals; ++i) {
    for (int j = 0; j < pat.n_unknowns; ++j) {
      const bool s = pat.reads(i, j);
      const bool num = Jz[static_cast<std::size_t>(i) * static_cast<std::size_t>(pat.n_unknowns) + static_cast<std::size_t>(j)] != 0.0;
      structural += s ? 1 : 0;
      numerical += num ? 1 : 0;
      // Soundness: a numerically nonzero entry MUST be structurally present. The other direction
      // is allowed to be loose and here is measured not to be.
      EXPECT_TRUE(!num || s) << "numerically nonzero entry (" << i << "," << j << ") is absent from the pattern";
    }
  }
  EXPECT_EQ(structural, numerical) << "structural " << structural << " vs numerical " << numerical;
}
