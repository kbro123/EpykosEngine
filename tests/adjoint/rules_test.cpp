// M2/Q3: the local adjoint rules and the plan's transposes on small recorded programs — every
// op the recorder produces (add sub mul div neg exp log sqrt recip fma select and the
// comparisons, Sum and Affine after the passes), a value gathered twice by one group, a value
// read by several domains and registered as an output as well, and the M1 book has none of
// select / recip / fma / log / sqrt. Gradients are checked against a Richardson-extrapolated
// central difference of the same maths on double (accurate to ~1e-10 here), and the Select rule
// against the analytic derivative of the selected arm on both sides of the branch.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/adjoint/plan.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/scalar/select.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace ir = epykos::ir;
namespace adjoint = epykos::adjoint;
using epykos::Op;
using epykos::Rec;
using epykos::Tape;

namespace {

// Every op, with fan-out so that a, b, lin become boundaries read through gathers; three
// outputs (f, a, lin) so that interior values carry output seeds as well as reader pulls.
template <class Scalar>
void all_ops(const Scalar* x, Scalar* out) {
  using epykos::exp;
  using epykos::fma;
  using epykos::log;
  using epykos::recip;
  using epykos::select;
  using epykos::sqrt;
  const Scalar a = x[0] * x[1] + x[2];                 // mul, add
  const Scalar b = exp(a) / sqrt(x[2] + 2.0);          // exp, div, sqrt
  const Scalar c = log(x[1] * x[1]) - recip(x[0] + 3.0);  // log, a value gathered twice (x1*x1), sub, recip
  const Scalar d = fma(x[0], b, c);                    // fma
  const Scalar e = select(x[0] < x[1], d, -d);         // cmp, select, neg
  const Scalar g = select(x[2] >= 0.5, e * 0.5, e + 1.0);
  const Scalar lin = 2.0 * x[0] + 3.0 * x[1] - x[2] + 1.0;  // affine after the passes
  const Scalar h = g * (a + b) + lin * lin;            // a, b: fan-out > 1; lin gathered twice
  out[0] = h + b;
  out[1] = a;
  out[2] = lin;
}

constexpr int n_in = 3;
constexpr int n_out = 3;

Tape record(const std::vector<double>& x0, bool passes) {
  Tape t;
  {
    Tape::Scope scope(t);
    Rec x[n_in];
    for (int k = 0; k < n_in; ++k) x[k] = epykos::make_input(t, x0[static_cast<std::size_t>(k)]);
    Rec out[n_out];
    all_ops<Rec>(x, out);
    for (const Rec& r : out) epykos::register_output(t, r);
  }
  if (passes) epykos::standard_passes(t);
  t.validate();
  return t;
}

std::vector<double> eval(const std::vector<double>& x) {
  std::vector<double> out(n_out);
  all_ops<double>(x.data(), out.data());
  return out;
}

// d out[o] / d x[k] by Richardson-extrapolated central differences.
double fd(const std::vector<double>& x, int o, int k) {
  auto central = [&](double h) {
    std::vector<double> xp = x, xm = x;
    xp[static_cast<std::size_t>(k)] += h;
    xm[static_cast<std::size_t>(k)] -= h;
    return (eval(xp)[static_cast<std::size_t>(o)] - eval(xm)[static_cast<std::size_t>(o)]) / (2.0 * h);
  };
  const double h = 1e-4;
  return (4.0 * central(h / 2.0) - central(h)) / 3.0;
}

// The gradient of output o at x from the adjoint (B = 1).
std::vector<double> grad(const adjoint::Adjoint& ad, const std::vector<double>& x, int o) {
  std::vector<double> out_bar(n_out, 0.0), out(n_out), state_bar(n_in);
  out_bar[static_cast<std::size_t>(o)] = 1.0;
  ad.run(x.data(), 1, out_bar.data(), out.data(), state_bar.data());
  const std::vector<double> ref = eval(x);
  for (int j = 0; j < n_out; ++j) EXPECT_NEAR(out[static_cast<std::size_t>(j)], ref[static_cast<std::size_t>(j)], 1e-12 * std::fabs(ref[static_cast<std::size_t>(j)]));
  return state_bar;
}

void check_gradients(const ir::Program& p, const std::vector<double>& x, const std::string& where) {
  adjoint::Adjoint ad(p);
  for (int o = 0; o < n_out; ++o) {
    const std::vector<double> g = grad(ad, x, o);
    for (int k = 0; k < n_in; ++k) {
      const double f = fd(x, o, k);
      EXPECT_NEAR(g[static_cast<std::size_t>(k)], f, 1e-8 * std::max(1.0, std::fabs(f))) << where << ", output " << o << ", input " << k;
    }
  }
}

bool program_has(const ir::Program& p, Op op) {
  for (const ir::Group& g : p.groups) {
    for (const ir::Step& s : g.steps) {
      if (s.op == op) return true;
    }
  }
  return false;
}

}  // namespace

TEST(AdjointRules, EveryOpMatchesFiniteDifferencesRawAndAfterThePasses) {
  // Points on both sides of both branches (x0 < x1, x2 >= 0.5).
  const std::vector<std::vector<double>> points = {{0.3, 0.7, 0.9}, {0.8, 0.2, 0.9}, {0.3, 0.7, 0.1}, {0.8, 0.2, 0.1}, {-0.4, 1.3, 0.6}};
  for (bool passes : {false, true}) {
    const Tape t = record(points[0], passes);
    const ir::Program p = ir::infer(t);
    for (Op op : {Op::Mul, Op::Div, Op::Neg, Op::Exp, Op::Log, Op::Sqrt, Op::Recip, Op::Fma, Op::Select, Op::CmpLt, Op::CmpGe}) {
      EXPECT_TRUE(program_has(p, op)) << to_string(op) << (passes ? " after the passes" : " raw");
    }
    // Raw: Add and Sub as recorded. After the passes: every Add is a Sum (fold_sum, min_terms 2)
    // and the linear combination is an Affine.
    EXPECT_EQ(program_has(p, Op::Add), !passes);
    EXPECT_EQ(program_has(p, Op::Sum), passes);
    EXPECT_EQ(program_has(p, Op::Affine), passes);
    EXPECT_TRUE(program_has(p, Op::Sub) || passes);
    for (const std::vector<double>& x : points) check_gradients(p, x, passes ? "after the passes" : "raw");
  }
}

TEST(AdjointRules, SelectRoutesTheAdjointToTheSelectedArmOnly) {
  // y = select(x0 < x1, x0 * 2, x1 * 3): dy/dx0 = 2, dy/dx1 = 0 when x0 < x1; 0 and 3 otherwise.
  auto rec = [](double a, double b) {
    Tape t;
    Tape::Scope scope(t);
    const Rec x0 = epykos::make_input(t, a);
    const Rec x1 = epykos::make_input(t, b);
    const Rec y = epykos::select(x0 < x1, x0 * 2.0, x1 * 3.0);
    epykos::register_output(t, y);
    return t;
  };
  const Tape t = rec(0.25, 0.75);
  const ir::Program p = ir::infer(t);
  adjoint::Adjoint ad(p);
  const double one = 1.0;
  double out, sb[2];
  double x[2] = {0.25, 0.75};
  ad.run(x, 1, &one, &out, sb);
  EXPECT_EQ(out, 0.5);
  EXPECT_EQ(sb[0], 2.0);
  EXPECT_EQ(sb[1], 0.0);
  x[0] = 0.9;
  ad.run(x, 1, &one, &out, sb);
  EXPECT_EQ(out, 2.25);
  EXPECT_EQ(sb[0], 0.0);
  EXPECT_EQ(sb[1], 3.0);
  // A lane per branch in one batched call.
  const double xb[4] = {0.25, 0.9, 0.75, 0.75};  // x0 of lanes 0, 1; x1 of lanes 0, 1
  const double ob[2] = {1.0, 1.0};
  double outb[2], sbb[4];
  ad.run(xb, 2, ob, outb, sbb);
  EXPECT_EQ(sbb[0], 2.0);
  EXPECT_EQ(sbb[1], 0.0);
  EXPECT_EQ(sbb[2], 0.0);
  EXPECT_EQ(sbb[3], 3.0);
}

TEST(AdjointRules, RecipExpLogSqrtDivFmaAnalytic) {
  // y = recip(x0) + exp(x1) + log(x2) + sqrt(x0) + x1 / x2 + fma(x0, x1, x2), each analytic.
  Tape t;
  const double x0 = 0.7, x1 = 0.3, x2 = 1.9;
  {
    Tape::Scope scope(t);
    const Rec a = epykos::make_input(t, x0), b = epykos::make_input(t, x1), c = epykos::make_input(t, x2);
    const Rec y = epykos::recip(a) + epykos::exp(b) + epykos::log(c) + epykos::sqrt(a) + b / c + epykos::fma(a, b, c);
    epykos::register_output(t, y);
  }
  const ir::Program p = ir::infer(t);
  adjoint::Adjoint ad(p);
  const double x[3] = {x0, x1, x2}, one = 1.0;
  double out, sb[3];
  ad.run(x, 1, &one, &out, sb);
  EXPECT_NEAR(out, 1.0 / x0 + std::exp(x1) + std::log(x2) + std::sqrt(x0) + x1 / x2 + std::fma(x0, x1, x2), 1e-14);
  EXPECT_NEAR(sb[0], -1.0 / (x0 * x0) + 0.5 / std::sqrt(x0) + x1, 1e-13);
  EXPECT_NEAR(sb[1], std::exp(x1) + 1.0 / x2 + x0, 1e-13);
  EXPECT_NEAR(sb[2], 1.0 / x2 - x1 / (x2 * x2) + 1.0, 1e-13);
}

TEST(AdjointRules, PlanTransposesAreTheReverseOfTheProgramsIndexArrays) {
  const std::vector<double> x = {0.3, 0.7, 0.9};
  const Tape t = record(x, true);
  const ir::Program p = ir::infer(t);
  const adjoint::AdjointPlan plan = adjoint::build_plan(p);
  ASSERT_EQ(plan.num_values, p.num_values());
  // Gather readers: rebuilding gathers[g].index from the CSR gives the program's tables.
  std::vector<std::vector<ir::value_id>> index(p.gathers.size());
  for (std::size_t g = 0; g < p.gathers.size(); ++g) index[g].assign(p.gathers[g].index.size(), -1);
  std::size_t total = 0;
  for (std::size_t v = 0; v < plan.num_values; ++v) {
    for (std::int32_t e = plan.gather_offsets[v]; e < plan.gather_offsets[v + 1]; ++e) {
      const std::int32_t slot = plan.gather_readers[static_cast<std::size_t>(e)];
      std::size_t g = 0;
      while (g + 1 < p.gathers.size() && plan.gather_slot_base[g + 1] <= slot) ++g;
      const std::int32_t r = slot - plan.gather_slot_base[g];
      ASSERT_EQ(index[g][static_cast<std::size_t>(r)], -1) << "gather slot read twice";
      index[g][static_cast<std::size_t>(r)] = static_cast<ir::value_id>(v);
      ++total;
    }
  }
  EXPECT_EQ(total, static_cast<std::size_t>(plan.n_gather_slots));
  for (std::size_t g = 0; g < p.gathers.size(); ++g) EXPECT_EQ(index[g], p.gathers[g].index) << "gather " << g;
  // Segment readers: every (segment, row, member) appears once, with its coefficient for Affine.
  std::size_t members = 0;
  for (const ir::Segment& s : p.segments) members += s.members.size();
  EXPECT_EQ(plan.sum_readers.size() + plan.affine_readers.size(), members);
  for (std::size_t s = 0; s < p.segments.size(); ++s) {
    const ir::Segment& seg = p.segments[s];
    const bool affine = plan.segment_is_affine[s] != 0;
    EXPECT_EQ(affine, !seg.coefs.empty());
    const std::int32_t rows = p.domains[static_cast<std::size_t>(seg.domain)].rows;
    for (std::int32_t r = 0; r < rows; ++r) {
      const std::int32_t slot = plan.segment_slot_base[s] + r;
      for (std::int32_t m = seg.offsets[static_cast<std::size_t>(r)]; m < seg.offsets[static_cast<std::size_t>(r) + 1]; ++m) {
        const std::size_t v = static_cast<std::size_t>(seg.members[static_cast<std::size_t>(m)]);
        bool found = false;
        if (affine) {
          for (std::int32_t e = plan.affine_offsets[v]; e < plan.affine_offsets[v + 1]; ++e) {
            if (plan.affine_readers[static_cast<std::size_t>(e)] == slot && plan.affine_coefs[static_cast<std::size_t>(e)] == seg.coefs[static_cast<std::size_t>(m)]) found = true;
          }
        } else {
          for (std::int32_t e = plan.sum_offsets[v]; e < plan.sum_offsets[v + 1]; ++e) {
            if (plan.sum_readers[static_cast<std::size_t>(e)] == slot) found = true;
          }
        }
        EXPECT_TRUE(found) << "segment " << s << " row " << r << " member " << m;
      }
    }
  }
  // Output readers: the three outputs, by ordinal.
  std::size_t outs = 0;
  for (std::size_t v = 0; v < plan.num_values; ++v) {
    for (std::int32_t e = plan.output_offsets[v]; e < plan.output_offsets[v + 1]; ++e) {
      EXPECT_EQ(p.outputs[static_cast<std::size_t>(plan.output_readers[static_cast<std::size_t>(e)])], static_cast<ir::value_id>(v));
      ++outs;
    }
  }
  EXPECT_EQ(outs, p.outputs.size());
  const std::string d = adjoint::describe(plan, p);
  EXPECT_NE(d.find("affine: coef * segbar"), std::string::npos) << d;
  EXPECT_NE(d.find("select: bbar += a ? ybar : 0"), std::string::npos) << d;
}

TEST(AdjointRules, BatchedLanesEqualSingleRunsBitwise) {
  const std::vector<std::vector<double>> points = {{0.3, 0.7, 0.9}, {0.8, 0.2, 0.9}, {0.3, 0.7, 0.1}};
  const Tape t = record(points[0], true);
  const ir::Program p = ir::infer(t);
  adjoint::Options o;
  o.lane_tile = 3;
  adjoint::Adjoint ad(p, o);
  const int B = 3;
  std::vector<double> state(n_in * B), out_bar(n_out * B), out(n_out * B), sb(n_in * B);
  for (int b = 0; b < B; ++b) {
    for (int k = 0; k < n_in; ++k) state[static_cast<std::size_t>(k * B + b)] = points[static_cast<std::size_t>(b)][static_cast<std::size_t>(k)];
    for (int j = 0; j < n_out; ++j) out_bar[static_cast<std::size_t>(j * B + b)] = 0.5 + j + 0.25 * b;
  }
  ad.run(state.data(), B, out_bar.data(), out.data(), sb.data());
  for (int b = 0; b < B; ++b) {
    std::vector<double> ob(n_out), o1(n_out), s1(n_in);
    for (int j = 0; j < n_out; ++j) ob[static_cast<std::size_t>(j)] = out_bar[static_cast<std::size_t>(j * B + b)];
    ad.run(points[static_cast<std::size_t>(b)].data(), 1, ob.data(), o1.data(), s1.data());
    for (int j = 0; j < n_out; ++j) EXPECT_EQ(std::memcmp(&o1[static_cast<std::size_t>(j)], &out[static_cast<std::size_t>(j * B + b)], sizeof(double)), 0);
    for (int k = 0; k < n_in; ++k) EXPECT_EQ(std::memcmp(&s1[static_cast<std::size_t>(k)], &sb[static_cast<std::size_t>(k * B + b)], sizeof(double)), 0) << "lane " << b << " input " << k;
  }
}

TEST(AdjointRules, RejectsReservedOpsRecurrentDomainsAndBadOptions) {
  const std::vector<double> x = {0.3, 0.7, 0.9};
  const Tape t = record(x, true);
  ir::Program p = ir::infer(t);
  EXPECT_THROW(adjoint::Adjoint(p, adjoint::Options{0, 64, 8}), std::invalid_argument);
  EXPECT_THROW(adjoint::Adjoint(p, adjoint::Options{256, 0, 8}), std::invalid_argument);
  EXPECT_THROW(adjoint::Adjoint(p, adjoint::Options{256, 64, 0}), std::invalid_argument);
  ir::Program bad = p;
  bad.groups.back().steps.back().op = Op::Gather;  // reserved
  EXPECT_THROW(adjoint::build_plan(bad), std::exception);
  ir::Program rec = p;
  rec.domains.back().recurrent = true;
  EXPECT_THROW(adjoint::build_plan(rec), std::invalid_argument);
  EXPECT_STREQ(adjoint::adjoint_rule(Op::Scan).formula, "unsupported");
  EXPECT_TRUE(adjoint::adjoint_rule(Op::Fma).c);
  EXPECT_FALSE(adjoint::adjoint_rule(Op::Select).a);
}
