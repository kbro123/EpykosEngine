// PRINCIPLES.md §1b invariant I1's gate: `adjoint(P)` yields an `ir::Program` that passes
// `ir::validate`, round-trips through `ir::serialize` / `ir::deserialize`, and that
// `exec::Interpreter` run on it reproduces `adjoint::Adjoint::run` **BITWISE**.
//
// Stages C2 (elementwise + gather) and C3a (segments) of that invariant. C3b (scans) is not
// implemented and `adjoint::adjoint_to_program` refuses a scan program outright; the refusal is
// gated here too, because an emitter that silently produced a nearly-right reverse scan would be
// worse than one that does not try.
//
// Bitwise, and why every clause of that word is load-bearing
// ----------------------------------------------------------
// §1b's gate says "the same program and the same inputs, so §5.2a case 1, and a failure is a
// defect" — it is an identity, not an agreement within a tolerance. Every comparison below is
// `std::memcmp` of eight bytes, so +0.0 and -0.0 are different answers. That is not pedantry:
// four of the five things the emitter has to get right are invisible under any tolerance (the
// pull's `+0.0` konst, the `1.0` reader coefficients, the CSR concatenation order, the
// `Add(Lit +0.0, ·)` that starts every accumulation chain), and the fifth — the aliased-`Div`
// dispatch — is a defect CI found on one compiler and not another (D84).
//
// The file name carries two requirements, neither optional:
//
//   * `_e0_test` compiles this TU with -ffp-contract=off in EVERY preset (root CMakeLists.txt,
//     D25). `ir::Evaluator` is header-only and takes the INCLUDING TU's flags, so without the
//     suffix the reference evaluator would contract `acc + coef·member` into an FMA here while
//     `src/adjoint/adjoint_e0.cpp` and `src/exec/kernels_l*_e0.cpp` did not, and the gate would
//     fail for a reason that has nothing to do with the emitter;
//   * `_e0_test$` also matches `scripts/mutation_test.sh`'s GATE_REGEX, so the two mutants this
//     file is the catcher for have a catcher at all. The trap is recorded in
//     tests/adjoint/div_aliased_verify_test.cpp's own header: a mutant whose only would-be
//     catcher is a test the harness does not select survives a full run.
//
// What the fixtures cover
// -----------------------
//   nearmiss  C2. 37 domains, 280 gathers, no segment and no scan; add sub mul div neg exp log
//             sqrt recip fma select cmp_lt cmp_le cmp_gt, and the near-miss shapes that put a
//             constant in each slot in turn. Its emitted reverse DOES contain segments — one
//             Affine pull per domain — so the pull is exercised here as well.
//   m1_book   C3a. The forward program's own Sum and Affine segments (the leg and book folds, and
//             the linear-zero interpolation whose Affine coefficients are the thing
//             `plan.cpp`'s transpose can get wrong).
//   by hand   an aliased Div (no fixture in the tree contains one, because `simplify` turns
//             div(x, x) into 1 — but `ir::validate` accepts one, so the engine must be right for
//             what its own contract allows), and a program whose state adjoint is a SIGNED ZERO.
//   scans     instrument_sample, rfr_book and compare_ois must all hit the C3b refusal cleanly.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/adjoint/adjoint_to_program.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/instrument_sample.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/nearmiss_shapes.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/rfr_book.hpp"
#include "epykos/fixtures/rfr_price.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/tape.hpp"

namespace {

using namespace epykos;
using ir::Program;

// ---- the comparison ---------------------------------------------------------------------------

struct Counts {
  long long comparisons = 0;
  long long mismatches = 0;
};

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

std::string bits_of(double v) {
  std::uint64_t b = 0;
  std::memcpy(&b, &v, sizeof b);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(b));
  return std::string(buf) + " (" + std::to_string(v) + ")";
}

// The reference: Adjoint::run's forward outputs then its state adjoints, in Q's own output
// layout (out[o·B + b] for o < n_out, then state_bar[k·B + b]).
std::vector<double> reference(const adjoint::Adjoint& ad, const std::vector<double>& state,
                              const std::vector<double>& out_bar, int B) {
  const std::size_t n_in = static_cast<std::size_t>(ad.n_inputs());
  const std::size_t n_out = static_cast<std::size_t>(ad.n_outputs());
  const std::size_t Bs = static_cast<std::size_t>(B);
  std::vector<double> out(n_out * Bs, 0.0), sb(n_in * Bs, 0.0);
  ad.run(state.data(), B, out_bar.data(), out.data(), sb.data());
  std::vector<double> ref;
  ref.reserve((n_in + n_out) * Bs);
  ref.insert(ref.end(), out.begin(), out.end());
  ref.insert(ref.end(), sb.begin(), sb.end());
  return ref;
}

// Q's input vector: the forward state, then the out_bar seed, batch innermost in both halves.
std::vector<double> q_inputs(const std::vector<double>& state, const std::vector<double>& out_bar) {
  std::vector<double> in;
  in.reserve(state.size() + out_bar.size());
  in.insert(in.end(), state.begin(), state.end());
  in.insert(in.end(), out_bar.begin(), out_bar.end());
  return in;
}

// memcmp of `got` against `ref`, reporting at most a few differences.
void expect_same_bits(const std::vector<double>& got, const std::vector<double>& ref, const std::string& what,
                      Counts* c) {
  ASSERT_EQ(got.size(), ref.size()) << what;
  int shown = 0;
  for (std::size_t i = 0; i < ref.size(); ++i) {
    ++c->comparisons;
    if (same_bits(got[i], ref[i])) continue;
    ++c->mismatches;
    if (shown++ < 4) {
      ADD_FAILURE() << what << ": element " << i << " is " << bits_of(got[i]) << ", Adjoint::run says "
                    << bits_of(ref[i]) << ". The gate is BITWISE (PRINCIPLES.md §1b, I1): this is a defect in "
                    << "src/adjoint/adjoint_to_program_e0.cpp, never a tolerance to widen.";
    }
  }
}

// One exec::Interpreter configuration.
struct ExecConfig {
  const char* name;
  exec::Options options;
};

std::vector<ExecConfig> exec_configs(int max_batch) {
  std::vector<ExecConfig> cfgs;
  auto base = [&](int tile, int lane_tile) {
    exec::Options o;
    o.tile = tile;
    o.lane_tile = lane_tile;
    o.max_batch = max_batch;
    o.exp = exec::ExpMode::std_exp;  // E0: the only mode the gate may use
    return o;
  };
  // Every combination of the three fusion switches, at the default tiling.
  for (int mask = 0; mask < 8; ++mask) {
    exec::Options o = base(256, 8);
    o.fuse_pairs = (mask & 1) == 0;
    o.fuse_reductions = (mask & 2) == 0;
    o.inline_producers = (mask & 4) == 0;
    static const char* names[8] = {"all-on",          "no-fuse_pairs",    "no-fuse_reductions", "no-pairs+reductions",
                                   "no-inline",       "no-pairs+inline",  "no-reductions+inline", "all-off"};
    cfgs.push_back({names[mask], o});
  }
  // Catalogue off, and tilings that split, exactly fill and overrun every group.
  exec::Options no_cat = base(256, 8);
  no_cat.use_catalogue = false;
  cfgs.push_back({"catalogue-off", no_cat});
  cfgs.push_back({"tile1-lane1", base(1, 1)});
  cfgs.push_back({"tile7-lane3", base(7, 3)});
  cfgs.push_back({"tile4096-lane64", base(4096, 64)});
  return cfgs;
}

// The gate on one forward program: validate, round-trip, and bitwise equality with Adjoint::run
// through BOTH ir::Evaluator and exec::Interpreter, over every state × seed × configuration × B.
Counts gate(const Program& p, const std::vector<std::vector<double>>& states, const std::vector<std::vector<double>>& seeds,
            const std::string& name, const std::vector<int>& batches = {1, 4, 5}) {
  Counts c;
  const int n_in = static_cast<int>(p.inputs.size());
  const int n_out = static_cast<int>(p.outputs.size());

  const adjoint::AdjointPlan plan = adjoint::build_plan(p);
  Program q = adjoint::adjoint_to_program(p, plan);
  EXPECT_NO_THROW(ir::validate(q)) << name << ": the emitted reverse must be a valid Program (I1's first clause)";
  {
    const Program round = ir::deserialize(ir::serialize(q));
    EXPECT_TRUE(round == q) << name << ": serialize/deserialize round trip (I1's second clause)";
  }
  std::cout << "[  " << name << "  ] emitted: " << q.num_values() << " values, " << q.domains.size() << " domains, "
            << q.gathers.size() << " gathers, " << q.segments.size() << " segments, " << q.columns.size()
            << " columns, " << q.literals.size() << " literals, " << q.inputs.size() << " inputs, "
            << q.outputs.size() << " outputs\n";

  int max_batch = 1;
  for (const int B : batches) max_batch = std::max(max_batch, B);

  adjoint::Options ao;
  ao.max_batch = max_batch;
  adjoint::Adjoint ad(p, ao);

  const std::vector<ExecConfig> cfgs = exec_configs(max_batch);
  std::vector<std::unique_ptr<exec::Interpreter>> interps;
  interps.reserve(cfgs.size());
  for (const ExecConfig& cfg : cfgs) interps.push_back(std::make_unique<exec::Interpreter>(q, cfg.options));

  for (const int B : batches) {
    const std::size_t Bs = static_cast<std::size_t>(B);
    // Lane b of a B-lane call takes state `b mod states.size()` and seed `b mod seeds.size()`,
    // so the lanes differ from one another and a cross-lane leak shows up as a mismatch.
    std::vector<double> state(static_cast<std::size_t>(n_in) * Bs, 0.0);
    std::vector<double> out_bar(static_cast<std::size_t>(n_out) * Bs, 0.0);
    for (std::size_t si = 0; si < states.size(); ++si) {
      for (std::size_t se = 0; se < seeds.size(); ++se) {
        for (std::size_t b = 0; b < Bs; ++b) {
          const std::vector<double>& s = states[(si + b) % states.size()];
          const std::vector<double>& o = seeds[(se + b) % seeds.size()];
          for (int k = 0; k < n_in; ++k) state[static_cast<std::size_t>(k) * Bs + b] = s[static_cast<std::size_t>(k)];
          for (int k = 0; k < n_out; ++k) out_bar[static_cast<std::size_t>(k) * Bs + b] = o[static_cast<std::size_t>(k)];
        }
        const std::vector<double> ref = reference(ad, state, out_bar, B);
        const std::vector<double> in = q_inputs(state, out_bar);
        const std::string where = name + " state " + std::to_string(si) + " seed " + std::to_string(se) + " B " +
                                  std::to_string(B);
        if (B == 1) {
          std::vector<double> got(q.outputs.size(), 0.0);
          ir::Evaluator(q).run(in.data(), got.data());
          expect_same_bits(got, ref, where + " [ir::Evaluator]", &c);
        }
        for (std::size_t ci = 0; ci < cfgs.size(); ++ci) {
          std::vector<double> got(q.outputs.size() * Bs, 0.0);
          interps[ci]->run(in.data(), B, got.data());
          expect_same_bits(got, ref, where + " [exec::Interpreter " + cfgs[ci].name + "]", &c);
        }
      }
    }
  }
  std::cout << "[  " << name << "  ] bitwise: " << c.mismatches << " mismatches / " << c.comparisons
            << " comparisons over " << states.size() << " states x " << seeds.size() << " seeds x "
            << (cfgs.size() + 1) << " evaluator configurations x " << batches.size() << " batch widths\n";
  return c;
}

// ---- seeds ---------------------------------------------------------------------------------

// out_bar seeds: all ones, each of a few unit vectors, a mixed-sign pattern, and all ZEROS --
// the zero seed is what makes a signed zero in the state adjoint observable.
std::vector<std::vector<double>> out_bar_seeds(int n_out) {
  const std::size_t n = static_cast<std::size_t>(n_out);
  std::vector<std::vector<double>> seeds;
  seeds.emplace_back(n, 1.0);
  seeds.emplace_back(n, 0.0);
  {
    std::vector<double> e(n, 0.0);
    e[n - 1] = 1.0;
    seeds.push_back(e);
  }
  {
    std::vector<double> e(n, 0.0);
    e[0] = -1.0;
    seeds.push_back(e);
  }
  {
    std::vector<double> mixed(n, 0.0);
    for (std::size_t o = 0; o < n; ++o) mixed[o] = static_cast<double>(static_cast<int>(o % 3) - 1);
    seeds.push_back(mixed);
  }
  return seeds;
}

// ---- hand-built programs ---------------------------------------------------------------------
//
// Built as data, not recorded: `simplify` turns div(x, x) into 1, so a RECORDING can no longer
// produce an aliased Div, while `ir::validate` accepts one (tests/adjoint/div_aliased_verify_test.cpp
// makes the same point). A one-input program whose state adjoint is a signed zero is likewise not
// something a fixture happens to contain.

// A one-row Input domain followed by one elementwise domain of `steps`, which reads the input
// through gather 0. `input_value` is the record point; the single output is the second domain's row.
Program one_input_program(const std::string& name, std::vector<ir::Step> steps, std::vector<double> literals,
                          double input_value) {
  Program p;
  {
    ir::Domain d;
    d.name = "in";
    d.rows = 1;
    d.value_base = 0;
    p.domains.push_back(d);
    ir::Group g;
    g.domain = 0;
    g.steps.push_back(ir::Step{Op::Input, ir::Slot{ir::SlotKind::Input, -1}, {}, {}, {}});
    p.groups.push_back(g);
  }
  {
    ir::Domain d;
    d.name = name;
    d.rows = 1;
    d.value_base = 1;
    d.reads = {0};
    p.domains.push_back(d);
    ir::Group g;
    g.domain = 1;
    g.steps = std::move(steps);
    p.groups.push_back(g);
  }
  ir::Gather gather;
  gather.domain = 1;
  gather.index = {0};
  p.gathers.push_back(gather);
  p.literals = std::move(literals);
  p.inputs = {0};
  p.input_values = {input_value};
  p.outputs = {1};
  return p;
}

const ir::Slot kGather0{ir::SlotKind::Gather, 0};
ir::Slot step_slot(std::int32_t k) { return ir::Slot{ir::SlotKind::Step, k}; }
ir::Slot literal_slot(std::int32_t k) { return ir::Slot{ir::SlotKind::Literal, k}; }

// x / x, with both operand slots the SAME gather: adjoint_e0.cpp dispatches acc_div_aliased.
Program aliased_div_program() {
  return one_input_program("div_xx", {ir::Step{Op::Div, kGather0, kGather0, {}, {}}}, {}, 0.7);
}

// The aliased Div's contribution lands in a gather edge slot that ALREADY holds a value, so the
// single fused form (ȳ/b)·(1 − y) and the two separate accumulations (+ ȳ/b, then − (ȳ/b)·y)
// differ by a rounding rather than only by the sign of a zero:
//
//   steps   t0 = G / G            (aliased; t0 = 1.0 exactly)
//           t1 = G * C
//           t2 = t0 + t1
//   reverse gbar  = +0.0 + ȳ·C        (from t1, which the reverse visits FIRST)
//                 + (ȳ/G)·(1 − t0)    = + (±0.0)                       <- what the engine does
//           gbar' = +0.0 + ȳ·C  + ȳ/G  − (ȳ/G)·t0                       <- the mutant
//
// With G = 2^53, C = 1 + 2^-52 and ȳ = 1: the engine answers 1 + 2^-52, the mutant 1 + 2^-51,
// because (C + 2^-53) rounds up (ties to even) and subtracting 2^-53 rounds up again.
Program aliased_div_rounding_program() {
  const double c = 1.0 + 0x1p-52;
  return one_input_program("div_xx_mix",
                           {ir::Step{Op::Div, kGather0, kGather0, {}, {}},
                            ir::Step{Op::Mul, kGather0, literal_slot(0), {}, {}},
                            ir::Step{Op::Add, step_slot(0), step_slot(1), {}, {}}},
                           {c}, 0x1p53);
}

// -x. With a ZERO seed the state adjoint is `+0.0 - +0.0` pulled from `+0.0`, i.e. +0.0 -- and
// `-(+0.0)` is -0.0, so eliding either leading zero is visible in the sign bit and nowhere else.
Program negate_program() { return one_input_program("neg", {ir::Step{Op::Neg, kGather0, {}, {}, {}}}, {}, 1.5); }

}  // namespace

// ---- C2: elementwise and gathers ----------------------------------------------------------------

TEST(AdjointToProgram, NearmissIsBitwiseTheAdjoint) {
  const Tape t = fixtures::record_nearmiss();
  const Program p = ir::infer(t);
  ASSERT_TRUE(ir::scan_domains(p).empty()) << "the near-miss fixture is the C2 gate: no scan";
  const Counts c = gate(p, fixtures::nearmiss_states(6), out_bar_seeds(static_cast<int>(p.outputs.size())), "nearmiss");
  EXPECT_EQ(c.mismatches, 0);
  EXPECT_GT(c.comparisons, 0);
}

// ---- C3a: segments ------------------------------------------------------------------------------

TEST(AdjointToProgram, M1BookIsBitwiseTheAdjoint) {
  const fixtures::Book b = fixtures::make_m1_book();
  const Tape t = fixtures::record_m1(b);
  const Program p = ir::infer(t);
  ASSERT_TRUE(ir::scan_domains(p).empty());
  int sums = 0, affines = 0;
  for (const ir::Group& g : p.groups) {
    for (const ir::Step& s : g.steps) {
      if (s.a.kind != ir::SlotKind::Segment) continue;
      if (s.op == Op::Sum) ++sums;
      if (s.op == Op::Affine) ++affines;
    }
  }
  ASSERT_GT(sums, 0) << "the M1 book is the C3a gate: it must carry Sum segments";
  ASSERT_GT(affines, 0) << "the M1 book is the C3a gate: it must carry an Affine segment (the transposed coefficients)";

  std::vector<std::vector<double>> states;
  for (int r = 0; r < 3; ++r) {
    std::vector<double> z(b.z0.begin(), b.z0.end());
    for (std::size_t k = 0; k < z.size(); ++k) {
      z[k] += 1e-4 * static_cast<double>((r + 1) * (static_cast<int>(k) % 5 - 2));
    }
    states.push_back(z);
  }
  // 1,001 outputs and 199,909 emitted values: two seeds and two batch widths keep the gate inside
  // a second without narrowing what it covers (every configuration still runs).
  std::vector<std::vector<double>> seeds = out_bar_seeds(static_cast<int>(p.outputs.size()));
  seeds.resize(3);
  const Counts c = gate(p, states, seeds, "m1_book", {1, 4});
  EXPECT_EQ(c.mismatches, 0);
  EXPECT_GT(c.comparisons, 0);
}

TEST(AdjointToProgram, CollapsedOisIsBitwiseTheAdjoint) {
  // The workload D81's collapse is about, after the collapse: a short-tenor compare_ois carries
  // no scan at all (the recurrence telescoped), so the whole calibration-instrument + book
  // pricing program is 131 values and this emitter covers it end to end.
  fixtures::CompareOisOptions o;
  o.trades = 2;
  o.tenors = {"1Y", "2Y", "5Y", "10Y"};
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  const Program p = ir::infer(fixtures::record_compare_ois(s).tape);
  ASSERT_TRUE(ir::scan_domains(p).empty()) << "the collapsed OIS program is a C3a gate: no scan left to refuse";

  std::vector<std::vector<double>> states;
  for (int r = 0; r < 3; ++r) {
    std::vector<double> z(p.input_values);
    for (std::size_t k = 0; k < z.size(); ++k) {
      z[k] += 1e-5 * static_cast<double>((r + 1) * (static_cast<int>(k) % 5 - 2));
    }
    states.push_back(z);
  }
  const Counts c = gate(p, states, out_bar_seeds(static_cast<int>(p.outputs.size())), "compare_ois");
  EXPECT_EQ(c.mismatches, 0);
  EXPECT_GT(c.comparisons, 0);
}

// ---- the aliased Div, which no fixture contains --------------------------------------------------

TEST(AdjointToProgram, AliasedDivIsBitwiseTheAdjoint) {
  for (const Program& p : {aliased_div_program(), aliased_div_rounding_program()}) {
    ASSERT_NO_THROW(ir::validate(p)) << "the IR contract accepts an aliased Div, which is the point";
    const std::string name = p.domains[1].name;
    const Counts c = gate(p, {{p.input_values[0]}}, out_bar_seeds(1), name);
    EXPECT_EQ(c.mismatches, 0)
        << name << ": an aliased Div must emit acc_div_aliased's single (ȳ/b)·(1 − y) contribution. "
        << "Two separate accumulations are a different rounding, not a different style.";
  }
}

// ---- signed zero: the leading +0.0 of every accumulation -----------------------------------------

TEST(AdjointToProgram, SignedZeroSurvivesTheEmission) {
  const Program p = negate_program();
  const Counts c = gate(p, {{1.5}}, out_bar_seeds(1), "neg");
  EXPECT_EQ(c.mismatches, 0);

  // Stated explicitly, because it is the whole reason the emitter writes a Literal +0.0 it could
  // "obviously" leave out: with a zero seed the state adjoint of -x is +0.0 and not -0.0.
  const Program q = adjoint::adjoint_to_program(p);
  std::vector<double> in = {1.5, 0.0}, out(q.outputs.size(), 0.0);
  ir::Evaluator(q).run(in.data(), out.data());
  ASSERT_EQ(q.outputs.size(), 2u);
  EXPECT_TRUE(same_bits(out[1], +0.0)) << "d(-x)/dx seeded with +0.0 is +0.0, not " << bits_of(out[1])
                                       << ": the accumulation starts at the +0.0 the runtime memsets";
}

// ---- C3b: scans are refused, not approximated ------------------------------------------------------

TEST(AdjointToProgram, RefusesAScanNamingC3b) {
  struct Named {
    const char* name;
    Program p;
  };
  std::vector<Named> scanned;
  {
    const fixtures::InstrumentSample s = fixtures::make_instrument_sample();
    scanned.push_back({"instrument_sample", ir::infer(fixtures::record_sample(s))});
  }
  {
    const fixtures::RfrBook b = fixtures::make_rfr_book();
    scanned.push_back({"rfr_book", ir::infer(fixtures::record_rfr(b))});
  }
  {
    // The DEFAULT tenor set, and it has to be. With a short set (measured: 2 trades over
    // 1Y/2Y/5Y/10Y, and over six tenors to 10Y) D81's collapse telescopes every compounded
    // coupon away and `ir::infer` finds NO scan at all -- that program is the C3a gate below.
    // The long end is what still carries one.
    fixtures::CompareOisOptions o;
    o.trades = 2;
    const fixtures::CompareOis s = fixtures::make_compare_ois(o);
    scanned.push_back({"compare_ois (default tenors)", ir::infer(fixtures::record_compare_ois(s).tape)});
  }
  for (const Named& n : scanned) {
    const std::size_t scans = ir::scan_domains(n.p).size();
    std::cout << "[  scan  ] " << n.name << ": " << scans << " scan domain(s)\n";
    EXPECT_GT(scans, 0u) << n.name << " was expected to contain a scan";
    try {
      (void)adjoint::adjoint_to_program(n.p);
      ADD_FAILURE() << n.name << ": a scan program must be refused, not emitted";
    } catch (const std::logic_error& e) {
      EXPECT_NE(std::string(e.what()).find("C3b"), std::string::npos)
          << n.name << ": the refusal must name the stage that lifts it; got: " << e.what();
    }
  }
}
