// PRINCIPLES.md §1b invariant I1's gate: `adjoint(P)` yields an `ir::Program` that passes
// `ir::validate`, round-trips through `ir::serialize` / `ir::deserialize`, and that
// `exec::Interpreter` run on it reproduces `adjoint::Adjoint::run` **BITWISE**.
//
// Stages C2 (elementwise + gather), C3a (segments) and C3b (scans) of that invariant.
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
//
//   affine_scan   C3b, both recordings. 400 rows in 4 chains; with `output_path` every scan row
//             is also an output, so the carry's edge slot is the LAST of 2 members in the pull of
//             all 396 carry-bearing rows, and without it the FIRST of 1.
//   compare_ois (DEFAULT tenors)
//             C3b, and the one that matters. Measured: the carry sits at position 0 of a 2-entry
//             pull in every carry-bearing row, so an encoding that appended the carry to the pull
//             — `Affine` over the non-carry readers, then `Add(·, carry)` — passes `affine_scan`
//             and fails here. It also has to be the DEFAULT tenor set: with any set ending at 10Y
//             D81's collapse telescopes the recurrence away and there is no scan left at all,
//             which is the C3a case `CollapsedOisIsBitwiseTheAdjoint` below pins.
//
// What C3b changed about this file's refusal case. Until 2026-10-07 `RefusesAScanNamingC3b`
// asserted that `instrument_sample`, `rfr_book` and default-tenor `compare_ois` were all refused
// with a message naming C3b. They are now emitted, so that assertion is false about a better
// emitter (§5.2a case 3). What is still refused is narrower and is gated by
// `RefusesASecondCarriedQuantity`: a recurrent domain that is not a scan, and a scan whose rows
// read themselves through anything but the carry — either needs TWO carried quantities per row,
// and a domain row produces exactly one value.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/adjoint/adjoint_to_program.hpp"
#include "epykos/exec/interpreter.hpp"
#include "epykos/fixtures/affine_scan.hpp"
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
  long long out_mismatches = 0;        // in the forward half of Q's outputs
  long long state_bar_mismatches = 0;  // in the state-adjoint half
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

// memcmp of `got` against `ref`, reporting at most a few differences. `n_out_elems` is where the
// forward outputs end and the state adjoints begin, so a failure says WHICH HALF it is in — the
// question a mutant has to answer (a mutant that only perturbs an intermediate, or that is
// washed out by a signed zero, is a dead gate; see the file header of
// tests/adjoint/div_aliased_verify_test.cpp for the one that nearly was).
void expect_same_bits(const std::vector<double>& got, const std::vector<double>& ref, const std::string& what,
                      std::size_t n_out_elems, Counts* c) {
  ASSERT_EQ(got.size(), ref.size()) << what;
  int shown = 0;
  for (std::size_t i = 0; i < ref.size(); ++i) {
    ++c->comparisons;
    if (same_bits(got[i], ref[i])) continue;
    ++c->mismatches;
    const bool in_state_bar = i >= n_out_elems;
    if (in_state_bar) {
      ++c->state_bar_mismatches;
    } else {
      ++c->out_mismatches;
    }
    if (shown++ < 4) {
      ADD_FAILURE() << what << ": element " << i << " (" << (in_state_bar ? "state_bar" : "out") << ") is "
                    << bits_of(got[i]) << ", Adjoint::run says " << bits_of(ref[i])
                    << ". The gate is BITWISE (PRINCIPLES.md §1b, I1): this is a defect in "
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
  std::size_t q_scans = 0, q_recurrent = 0;
  for (const ir::Domain& d : q.domains) {
    if (d.scan >= 0) ++q_scans;
    if (d.recurrent && d.scan < 0) ++q_recurrent;
  }
  // One reverse-scan domain per forward scan domain, and not one more: the forward half re-emits
  // the scan as a scan, the reverse half as a recurrent non-scan domain (C3b).
  EXPECT_EQ(q_scans, ir::scan_domains(p).size()) << name;
  EXPECT_EQ(q_recurrent, ir::scan_domains(p).size()) << name;
  std::cout << "[  " << name << "  ] emitted: " << q.num_values() << " values, " << q.domains.size() << " domains ("
            << q_scans << " scan, " << q_recurrent << " recurrent non-scan), " << q.gathers.size() << " gathers, "
            << q.segments.size() << " segments, " << q.columns.size() << " columns, " << q.literals.size()
            << " literals, " << q.inputs.size() << " inputs, " << q.outputs.size() << " outputs\n";

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
        const std::size_t n_out_elems = static_cast<std::size_t>(n_out) * Bs;
        if (B == 1) {
          std::vector<double> got(q.outputs.size(), 0.0);
          ir::Evaluator(q).run(in.data(), got.data());
          expect_same_bits(got, ref, where + " [ir::Evaluator]", n_out_elems, &c);
        }
        for (std::size_t ci = 0; ci < cfgs.size(); ++ci) {
          std::vector<double> got(q.outputs.size() * Bs, 0.0);
          interps[ci]->run(in.data(), B, got.data());
          expect_same_bits(got, ref, where + " [exec::Interpreter " + cfgs[ci].name + "]", n_out_elems, &c);
        }
      }
    }
  }
  std::cout << "[  " << name << "  ] bitwise: " << c.mismatches << " mismatches (" << c.out_mismatches << " out, "
            << c.state_bar_mismatches << " state_bar) / " << c.comparisons << " comparisons over " << states.size()
            << " states x " << seeds.size() << " seeds x " << (cfgs.size() + 1) << " evaluator configurations x "
            << batches.size() << " batch widths\n";
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

// ---- C3b: the reverse of a scan ----------------------------------------------------------------
//
// `affine_scan` FIRST, because it is the small one and its failures are readable; then
// `compare_ois` with the default tenors, which is the gate that matters. A green `affine_scan`
// alone proves nothing: its carry sits LAST in the pull of all 396 carry-bearing rows, and
// `compare_ois`'s sits FIRST in all of its, so the one encoding that cannot work — append the
// carry to the pull — passes the first and fails the second.

TEST(AdjointToProgram, AffineScanIsBitwiseTheAdjoint) {
  const fixtures::AffineScanFixture f = fixtures::make_affine_scan();
  for (const bool output_path : {false, true}) {
    const Program p = ir::infer(fixtures::record_affine_scan(f, output_path));
    ASSERT_EQ(ir::scan_domains(p).size(), 1u) << "the affine scan fixture is a C3b gate: one scan domain";
    const std::string name = std::string("affine_scan(") + (output_path ? "path" : "final") + ")";
    const std::vector<std::vector<double>> states = fixtures::affine_scan_states(f, 3);
    std::vector<std::vector<double>> seeds = out_bar_seeds(static_cast<int>(p.outputs.size()));
    // 401 outputs in the `path` recording: three seeds and two batch widths keep the gate inside
    // a second without narrowing what it covers (every configuration still runs).
    if (output_path) seeds.resize(3);
    const Counts c = gate(p, states, seeds, name, output_path ? std::vector<int>{1, 4} : std::vector<int>{1, 4, 5});
    EXPECT_EQ(c.mismatches, 0);
    EXPECT_GT(c.comparisons, 0);
  }
}

TEST(AdjointToProgram, DefaultTenorOisIsBitwiseTheAdjoint) {
  // The DEFAULT tenor set, and it has to be. With any set ending at 10Y (measured: 2 trades over
  // 1Y/2Y/5Y/10Y, and over six tenors to 10Y) D81's collapse telescopes every compounded coupon
  // away and `ir::infer` finds NO scan at all -- that program is CollapsedOisIsBitwiseTheAdjoint
  // above. The long end is what still carries one.
  fixtures::CompareOisOptions o;
  o.trades = 2;
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  const Program p = ir::infer(fixtures::record_compare_ois(s).tape);
  ASSERT_EQ(ir::scan_domains(p).size(), 1u) << "default-tenor compare_ois is THE C3b gate: it must carry a scan";

  std::vector<std::vector<double>> states;
  for (int r = 0; r < 3; ++r) {
    std::vector<double> z(p.input_values);
    for (std::size_t k = 0; k < z.size(); ++k) {
      z[k] += 1e-5 * static_cast<double>((r + 1) * (static_cast<int>(k) % 5 - 2));
    }
    states.push_back(z);
  }
  const Counts c = gate(p, states, out_bar_seeds(static_cast<int>(p.outputs.size())), "compare_ois (default tenors)");
  EXPECT_EQ(c.mismatches, 0);
  EXPECT_GT(c.comparisons, 0);
}

TEST(AdjointToProgram, OtherScanFixturesAreBitwiseTheAdjoint) {
  // The two fixtures D41 built the scan machinery against. Until C3b they were this file's
  // refusal cases.
  {
    const fixtures::InstrumentSample s = fixtures::make_instrument_sample();
    const Program p = ir::infer(fixtures::record_sample(s));
    ASSERT_GT(ir::scan_domains(p).size(), 0u);
    std::vector<std::vector<double>> states;
    for (int r = 0; r < 2; ++r) {
      std::vector<double> z(p.input_values);
      for (std::size_t k = 0; k < z.size(); ++k) z[k] *= 1.0 + 1e-3 * static_cast<double>(r + 1);
      states.push_back(z);
    }
    const Counts c = gate(p, states, out_bar_seeds(static_cast<int>(p.outputs.size())), "instrument_sample");
    EXPECT_EQ(c.mismatches, 0);
  }
  {
    const fixtures::RfrBook b = fixtures::make_rfr_book();
    const Program p = ir::infer(fixtures::record_rfr(b));
    ASSERT_GT(ir::scan_domains(p).size(), 0u);
    std::vector<std::vector<double>> states;
    for (int r = 0; r < 2; ++r) {
      std::vector<double> z(p.input_values);
      for (std::size_t k = 0; k < z.size(); ++k) z[k] *= 1.0 + 1e-3 * static_cast<double>(r + 1);
      states.push_back(z);
    }
    std::vector<std::vector<double>> seeds = out_bar_seeds(static_cast<int>(p.outputs.size()));
    seeds.resize(3);
    const Counts c = gate(p, states, seeds, "rfr_book", {1, 4});
    EXPECT_EQ(c.mismatches, 0);
  }
}

// The asymmetry D96 §3 measured, asserted on the emitted program rather than recalled: the
// carry's edge slot is a member of the pull AT ITS TRUE CSR POSITION, which is the first of two
// on default-tenor compare_ois and the last of two on affine_scan(path). Mutant
// a2p.scan_forward_order is about which ROW that member names; this test is about which
// POSITION it occupies, and it is the one that fails if anyone "fixes" the reverse scan by
// appending the carry to the pull — which would change Adjoint::run's bits and is a §5.2a
// case-1 question reserved to the owner (D96 §3's recorded, not-taken, owner flag).
TEST(AdjointToProgram, TheReverseScanKeepsTheCarryAtItsCsrPosition) {
  struct Case {
    const char* name;
    Program p;
    bool carry_first;  // else last
  };
  std::vector<Case> cases;
  {
    fixtures::CompareOisOptions o;
    o.trades = 2;
    const fixtures::CompareOis s = fixtures::make_compare_ois(o);
    cases.push_back({"compare_ois (default tenors)", ir::infer(fixtures::record_compare_ois(s).tape), true});
  }
  {
    const fixtures::AffineScanFixture f = fixtures::make_affine_scan();
    cases.push_back({"affine_scan(path)", ir::infer(fixtures::record_affine_scan(f, true)), false});
  }
  for (const Case& cs : cases) {
    const Program q = adjoint::adjoint_to_program(cs.p);
    // The reverse scan's domain: the one recurrent non-scan domain of Q.
    int rec = -1;
    for (std::size_t d = 0; d < q.domains.size(); ++d) {
      if (q.domains[d].recurrent && q.domains[d].scan < 0) rec = static_cast<int>(d);
    }
    ASSERT_GE(rec, 0) << cs.name << ": the reverse of a scan is a recurrent non-scan domain";
    const ir::Domain& dm = q.domains[static_cast<std::size_t>(rec)];
    // Its pull is the group's FIRST step, over a segment; its last step is the carry's edge slot.
    const ir::Step& first = q.groups[static_cast<std::size_t>(rec)].steps.front();
    ASSERT_EQ(first.a.kind, ir::SlotKind::Segment) << cs.name;
    ASSERT_GT(q.groups[static_cast<std::size_t>(rec)].steps.size(), 1u)
        << cs.name << ": the carry's edge-slot chain follows the pull in the same group";
    const ir::Segment& sg = q.segments[static_cast<std::size_t>(first.a.index)];
    int self_first = 0, self_last = 0, self_mid = 0, rows_with_carry = 0;
    for (std::int32_t r = 0; r < dm.rows; ++r) {
      const std::int32_t lo = sg.offsets[static_cast<std::size_t>(r)];
      const std::int32_t hi = sg.offsets[static_cast<std::size_t>(r) + 1];
      for (std::int32_t m = lo; m < hi; ++m) {
        const ir::value_id v = sg.members[static_cast<std::size_t>(m)];
        if (v < dm.value_base || v >= dm.value_base + r) continue;  // not a self-read
        ++rows_with_carry;
        // The rows run BACKWARDS, so the carried adjoint is the PREVIOUS emitted row and nothing
        // else. That is what makes `exec::Interpreter`'s one-row-at-a-time order sufficient.
        EXPECT_EQ(v, dm.value_base + r - 1) << cs.name << " row " << r;
        if (m == lo) ++self_first;
        else if (m == hi - 1) ++self_last;
        else ++self_mid;
      }
    }
    std::cout << "[  carry  ] " << cs.name << ": " << rows_with_carry << " carry-bearing rows of " << dm.rows
              << "; position first " << self_first << ", last " << self_last << ", middle " << self_mid << "\n";
    EXPECT_GT(rows_with_carry, 0) << cs.name;
    EXPECT_EQ(self_mid, 0) << cs.name << ": the fixtures' pulls are 1 or 2 members wide";
    if (cs.carry_first) {
      EXPECT_EQ(self_last, 0) << cs.name
                              << ": the carry is FIRST in every carry-bearing row of this fixture (D96 §3). "
                              << "A pull that appended it would be a different accumulation order and a "
                              << "different answer -- §5.2a case 1, the owner's call, not this emitter's.";
      EXPECT_EQ(self_first, rows_with_carry) << cs.name;
    } else {
      EXPECT_EQ(self_first, 0) << cs.name << ": the carry is LAST in every carry-bearing row of this fixture";
      EXPECT_EQ(self_last, rows_with_carry) << cs.name;
    }
  }
}

// ---- what is still refused: a SECOND carried quantity ---------------------------------------------
//
// Until 2026-10-07 this file asserted that every scan was refused with a message naming C3b.
// That assertion is now false about a better emitter (§5.2a case 3). What remains refused is a
// recurrence whose reverse would need to carry more than one value per row, which no domain can
// express, because a domain row produces exactly one value.
TEST(AdjointToProgram, RefusesASecondCarriedQuantity) {
  // A recurrent domain that is not a scan. `ir::validate` accepts it and `ir::Evaluator` runs it
  // (D96 §3); its REVERSE is what has no encoding, because there is no carry gather whose edge
  // slot could be the carried value.
  {
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
    const std::int32_t rows = 4;
    {
      ir::Domain d;
      d.name = "rec";
      d.rows = rows;
      d.value_base = 1;
      d.recurrent = true;
      d.reads = {0, 1};
      p.domains.push_back(d);
      ir::Segment s;
      s.domain = 1;
      s.offsets.push_back(0);
      for (std::int32_t r = 0; r < rows; ++r) {
        s.members.push_back(0);
        s.coefs.push_back(1.0);
        if (r > 0) {
          s.members.push_back(1 + r - 1);
          s.coefs.push_back(0.5);
        }
        s.offsets.push_back(static_cast<std::int32_t>(s.members.size()));
      }
      p.segments.push_back(s);
      p.literals = {0.0};
      ir::Group g;
      g.domain = 1;
      g.steps.push_back(
          ir::Step{Op::Affine, ir::Slot{ir::SlotKind::Segment, 0}, {}, {}, ir::Slot{ir::SlotKind::Literal, 0}});
      p.groups.push_back(g);
    }
    p.inputs = {0};
    p.input_values = {2.0};
    for (std::int32_t r = 0; r < rows; ++r) p.outputs.push_back(1 + r);
    ASSERT_NO_THROW(ir::validate(p)) << "the IR accepts a recurrent non-scan domain, which is the point";
    try {
      (void)adjoint::adjoint_to_program(p);
      ADD_FAILURE() << "a recurrent non-scan domain must be refused, not emitted";
    } catch (const std::logic_error& e) {
      // `build_plan` reaches this one FIRST (`adjoint: domain N is recurrent but not a scan`),
      // and std::invalid_argument IS a std::logic_error, so the emitter's own check of the same
      // condition is defence in depth rather than the message seen here. Both say the same
      // thing, which is what this asserts.
      EXPECT_NE(std::string(e.what()).find("recurrent but not a scan"), std::string::npos) << e.what();
    }
  }
  // A scan that reads its own rows through a SECOND gather. Taken from the real affine_scan
  // program and bent by hand: no fixture produces one, and the refusal exists so that a future
  // program that does is loud rather than silently wrong.
  {
    const fixtures::AffineScanFixture f = fixtures::make_affine_scan();
    Program p = ir::infer(fixtures::record_affine_scan(f, false));
    const std::vector<ir::domain_id> scans = ir::scan_domains(p);
    ASSERT_EQ(scans.size(), 1u);
    const ir::domain_id sd = scans.front();
    const std::int32_t carry = p.scans[static_cast<std::size_t>(p.domains[static_cast<std::size_t>(sd)].scan)].carry_gather;
    // Some other gather of the same domain, pointed at an earlier row of the scan.
    int other = -1;
    for (std::size_t gi = 0; gi < p.gathers.size(); ++gi) {
      if (p.gathers[gi].domain == sd && static_cast<std::int32_t>(gi) != carry) other = static_cast<int>(gi);
    }
    ASSERT_GE(other, 0);
    const ir::Domain& dm = p.domains[static_cast<std::size_t>(sd)];
    p.gathers[static_cast<std::size_t>(other)].index.back() = dm.value_base;
    ASSERT_NO_THROW(ir::validate(p)) << "ir::validate accepts it: only the REVERSE has no encoding";
    try {
      (void)adjoint::adjoint_to_program(p);
      ADD_FAILURE() << "a scan reading itself through a non-carry gather must be refused";
    } catch (const std::logic_error& e) {
      EXPECT_NE(std::string(e.what()).find("which is not its carry"), std::string::npos) << e.what();
    }
  }
}
