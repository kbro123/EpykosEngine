// INVARIANT I1'S GATE (PRINCIPLES.md §1b; M5 stage C4). The clause this file asserts, verbatim:
//
//   "I1. The IR is closed under differentiation. `adjoint(P)` yields an `ir::Program`.
//    Gate: it passes `ir::validate`, round-trips through `ir::serialize`/`deserialize`, and
//    `exec::Interpreter` run on it reproduces `adjoint::Adjoint::run` bitwise — the same program
//    and the same inputs, so §5.2a case 1, and a failure is a defect."
//
// One test, `InvariantI1.AdjointOfPIsAProgramAndIsBitwiseAdjointRun`, over EVERY fixture the
// adjoint is gated on in tests/adjoint/ — read off those files, not recalled: `nearmiss` (raw and
// after the passes; nearmiss_adjoint_test, rules_test), `m1_book` (m1_adjoint_*, review_probe),
// `rfr_book` and `affine_scan` in both recordings (scan_adjoint_*), `instrument_sample` and
// `compare_ois` in both tenor sets (adjoint_to_program_e0_test) and **`stage_a`, the desk problem**
// (adjoint_catalogue_e0_test). `adjoint(P)` is taken from `adjoint::Adjoint::to_program()`, the
// public face C4 added, so the gate runs the API a caller would.
//
// This file does not replace tests/adjoint/adjoint_to_program_e0_test.cpp and is not a restatement
// of it (§5.2a case 3 does not apply: nothing that file asserts is weakened or removed here). That
// one is the emitter's development gate — 13 evaluator configurations, the hand-built aliased `Div`
// and signed-zero programs, the refusal cases, the carry's CSR position. This one is the
// INVARIANT's gate: every gated fixture, the three clauses, and the measurement below.
//
//
// Why a gate must be asked whether it can SEE a defect
// ----------------------------------------------------
// Three stages of this invariant in a row produced a check that could not observe what it claimed
// to check. The aliased-`Div` case cannot catch its own mutant (`x/x` makes the forward value
// exactly 1.0, both forms reduce to ±0.0, and the chain's leading `Add(Lit +0.0, ·)` washes the
// sign). Eliding only that leading `+0.0` is unobservable, because every quantity reaching an
// output passes through a pull whose first term is `+0.0`. And `rfr_book` cannot catch a
// reverse-SCAN defect at all: 0 of its 40 scan rows depend on an input, so no error in the reverse
// scan reaches a state adjoint.
//
// A green comparison on a fixture whose state adjoints no defect can perturb is worth nothing, and
// nothing in the comparison says which kind of fixture it was. So this gate MEASURES it, per
// fixture, and prints the verdict next to the comparison count. The measurement is a defect
// injection, not an argument:
//
//   1. compute the Q values the FORWARD outputs transitively need (Q's first n_out outputs). A Q
//      domain none of whose rows are in that cone is REVERSE-HALF: only the state adjoints read it;
//   2. in the reverse half, injure one thing at a time and run Q again — flip an accumulation step
//      `Add` <-> `Sub` (the sign of one contribution), or negate one `Affine` pull coefficient;
//   3. compare the STATE-ADJOINT half of Q's outputs against the uninjured run, bitwise. The site
//      is OBSERVABLE if it moved; it is dead if it did not.
//
// A fixture with 0 observable sites is a STRUCTURAL CHECK — validate, round-trip and a comparison
// of numbers no defect can move — and is labelled one in the output and in `kFixtures` below. It
// is NOT removed: a structural check still catches an emitter that throws, emits an invalid
// program, or loses a value. It is simply not coverage, and the gate says so out loud rather than
// letting a reader count it.
//
// The same probe is run a second time restricted to Q's REVERSE-SCAN domains (its recurrent
// non-scan domains, one per forward scan — see adjoint_to_program.hpp "Scans"), because a fixture
// can be fully live everywhere else and dead exactly there. That is what `rfr_book` is, and the
// number below reproduces the hand finding mechanically instead of restating it.
//
// The expected class of every fixture is PINNED in `kFixtures` (`coverage` / `structural`), the
// idiom of tests/mutation/registry_test.cpp. A fixture that goes quietly dead — a recording change
// that disconnects its inputs, say — then fails this gate instead of continuing to look green.
//
//
// Stage A, measured (the memory sanity check C4 was asked for; not a performance claim)
// -------------------------------------------------------------------------------------
// The default desk problem, ~2,000 trades, is IN the gate. Measured on this box at authoring time:
// P is 119,503 values in 67 domains with 148 inputs and 8,191 outputs; Q is 468,790 values in 316
// domains (3.92x the values, 4.72x the domains), its index arrays 451,400 gather entries and
// 387,881 segment members, its tables 7.0 MB, and `ir::serialize(Q)` 12.8 MB of text. Emission
// itself is 0.02 s. Against that, RECORDING the fixture peaks the process at ~3.8 GB before the
// adjoint is touched at all, and building Q moves that figure by nothing measurable. The brief's
// worry — that Q might not fit — is not borne out: Q costs about 20 MB on a 3.8 GB fixture.
//
// The file name carries two requirements, neither optional (the same two
// adjoint_to_program_e0_test.cpp's header records):
//
//   * `_e0_test` compiles this TU with -ffp-contract=off in EVERY preset (root CMakeLists.txt,
//     D25). `ir::Evaluator` is header-only and takes the INCLUDING TU's flags, so without the
//     suffix it would contract `acc + coef·member` into an FMA here while
//     `src/adjoint/adjoint_e0.cpp` and `src/exec/kernels_l*_e0.cpp` did not;
//   * `_e0_test$` matches `scripts/mutation_test.sh`'s GATE_REGEX, so this file is selected as a
//     gate at all. A mutant whose only would-be catcher is a test the harness does not select
//     survives a full run (tests/adjoint/div_aliased_verify_test.cpp's header).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
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
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/evaluate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace {

using namespace epykos;
using ir::Program;

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }

std::string bits_of(double v) {
  std::uint64_t b = 0;
  std::memcpy(&b, &v, sizeof b);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(b));
  return std::string(buf) + " (" + std::to_string(v) + ")";
}

// ---- §1b's third clause: the comparison --------------------------------------------------------

struct Counts {
  long long comparisons = 0;
  long long mismatches = 0;
  long long out_mismatches = 0;
  long long state_bar_mismatches = 0;
};

// `Adjoint::run`'s forward outputs then its state adjoints, in Q's own output layout.
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
                    << ". I1's gate is BITWISE (PRINCIPLES.md §1b): this is a defect in "
                    << "src/adjoint/adjoint_to_program_e0.cpp, never a tolerance to widen.";
    }
  }
}

struct ExecConfig {
  const char* name;
  exec::Options options;
};

// Four `exec::Interpreter` configurations: the default, every fusion switch off, a tiling of one
// row and one lane, and a tiling that overruns every group. The emitter's own development gate
// (adjoint_to_program_e0_test.cpp) sweeps all thirteen; §1b asks for `exec::Interpreter`, and
// these four are the corners of its planning that a gate over EVERY fixture can afford.
std::vector<ExecConfig> exec_configs(int max_batch) {
  auto base = [&](int tile, int lane_tile) {
    exec::Options o;
    o.tile = tile;
    o.lane_tile = lane_tile;
    o.max_batch = max_batch;
    o.exp = exec::ExpMode::std_exp;  // E0: the only mode the gate may use
    return o;
  };
  std::vector<ExecConfig> cfgs;
  cfgs.push_back({"default", base(256, 8)});
  exec::Options off = base(256, 8);
  off.fuse_pairs = false;
  off.fuse_reductions = false;
  off.inline_producers = false;
  off.use_catalogue = false;
  cfgs.push_back({"all-off", off});
  cfgs.push_back({"tile1-lane1", base(1, 1)});
  cfgs.push_back({"tile4096-lane64", base(4096, 64)});
  return cfgs;
}

// ---- can a defect in the emitted reverse REACH an observable here? ------------------------------

// The Q values the FORWARD outputs (Q's first `n_fwd_out` outputs) transitively need.
std::vector<char> forward_cone(const Program& q, std::size_t n_fwd_out) {
  std::vector<char> need(q.num_values(), 0);
  std::vector<ir::value_id> work;
  auto push = [&](ir::value_id v) {
    if (v < 0 || static_cast<std::size_t>(v) >= need.size()) return;
    if (need[static_cast<std::size_t>(v)]) return;
    need[static_cast<std::size_t>(v)] = 1;
    work.push_back(v);
  };
  for (std::size_t o = 0; o < n_fwd_out && o < q.outputs.size(); ++o) push(q.outputs[o]);
  while (!work.empty()) {
    const ir::value_id v = work.back();
    work.pop_back();
    const ir::domain_id d = q.domain_of(v);
    const ir::row_id r = q.row_of(v);
    for (const ir::Step& s : q.groups[static_cast<std::size_t>(d)].steps) {
      for (const ir::Slot* sl : {&s.a, &s.b, &s.c, &s.konst}) {
        if (sl->kind == ir::SlotKind::Gather) {
          push(q.gathers[static_cast<std::size_t>(sl->index)].index[static_cast<std::size_t>(r)]);
        } else if (sl->kind == ir::SlotKind::Segment) {
          const ir::Segment& sg = q.segments[static_cast<std::size_t>(sl->index)];
          const std::int32_t lo = sg.offsets[static_cast<std::size_t>(r)];
          const std::int32_t hi = sg.offsets[static_cast<std::size_t>(r) + 1];
          for (std::int32_t m = lo; m < hi; ++m) push(sg.members[static_cast<std::size_t>(m)]);
        }
      }
    }
  }
  return need;
}

// One injury: flip an accumulation step's Add <-> Sub, or negate one Affine coefficient.
struct Site {
  bool is_coef = false;
  std::int32_t where = 0;  // domain (step flip) or segment (coef)
  std::int32_t index = 0;  // step index or member index
  bool in_reverse_scan = false;
};

void injure(Program* q, const Site& s) {
  if (s.is_coef) {
    double& c = q->segments[static_cast<std::size_t>(s.where)].coefs[static_cast<std::size_t>(s.index)];
    c = -c;
    return;
  }
  ir::Step& st = q->groups[static_cast<std::size_t>(s.where)].steps[static_cast<std::size_t>(s.index)];
  st.op = st.op == Op::Add ? Op::Sub : Op::Add;
}

struct Observability {
  long long reverse_domains = 0;
  long long sites = 0, observable = 0, sites_found = 0;
  long long scan_sites = 0, scan_observable = 0, scan_sites_found = 0;
  long long nonzero_state_bar = 0, state_bar_total = 0;
  bool has_reverse_scan = false;
  bool coverage() const { return observable > 0; }
  // Whether every site the reverse half offers was injured, or only a sample of them: a
  // STRUCTURAL verdict over an exhaustive set is a proof, over a sample it is evidence.
  bool exhaustive() const { return sites == sites_found; }
  bool scan_exhaustive() const { return scan_sites == scan_sites_found; }
};

// Enumerate the injury sites of the reverse half, subsample to `budget`, and measure which of them
// move a state adjoint. `state` / `out_bar` is the single (state, seed) the probe runs at: the
// all-ones seed at the record point, the one most likely to make a chain live.
Observability probe(Program q, std::size_t n_fwd_out, const std::vector<double>& state,
                    const std::vector<double>& out_bar, long long budget) {
  Observability ob;
  const std::vector<char> need = forward_cone(q, n_fwd_out);
  std::vector<char> reverse_only(q.domains.size(), 1);
  for (std::size_t v = 0; v < need.size(); ++v) {
    if (need[v]) reverse_only[static_cast<std::size_t>(q.domain_of(static_cast<ir::value_id>(v)))] = 0;
  }
  std::vector<char> is_reverse_scan(q.domains.size(), 0);
  for (std::size_t d = 0; d < q.domains.size(); ++d) {
    if (q.domains[d].recurrent && q.domains[d].scan < 0) {
      is_reverse_scan[d] = 1;
      ob.has_reverse_scan = true;
    }
    if (reverse_only[d]) ++ob.reverse_domains;
  }

  std::vector<Site> general, scan;
  auto add = [&](const Site& s) { (s.in_reverse_scan ? scan : general).push_back(s); };
  for (std::size_t d = 0; d < q.domains.size(); ++d) {
    if (!reverse_only[d]) continue;
    const bool rs = is_reverse_scan[d] != 0;
    const ir::Group& g = q.groups[d];
    for (std::size_t k = 0; k < g.steps.size(); ++k) {
      if (g.steps[k].op != Op::Add && g.steps[k].op != Op::Sub) continue;
      add({false, static_cast<std::int32_t>(d), static_cast<std::int32_t>(k), rs});
    }
  }
  for (std::size_t si = 0; si < q.segments.size(); ++si) {
    const ir::Segment& sg = q.segments[si];
    if (sg.coefs.empty()) continue;
    if (!reverse_only[static_cast<std::size_t>(sg.domain)]) continue;
    const bool rs = is_reverse_scan[static_cast<std::size_t>(sg.domain)] != 0;
    // One coefficient per segment row, skipping an exact zero (negating it only flips the sign of
    // a zero, which the pull's `+0.0` start then washes -- a dead injury, not a dead fixture).
    for (std::size_t r = 0; r + 1 < sg.offsets.size(); ++r) {
      const std::int32_t lo = sg.offsets[r], hi = sg.offsets[r + 1];
      if (lo >= hi) continue;
      if (sg.coefs[static_cast<std::size_t>(lo)] == 0.0) continue;
      add({true, static_cast<std::int32_t>(si), lo, rs});
      break;
    }
  }

  // Stratified subsample: every stride-th site, so the sample spans the whole reverse half rather
  // than its first few domains.
  auto thin = [](std::vector<Site>* v, long long cap) {
    if (cap <= 0 || static_cast<long long>(v->size()) <= cap) return;
    const std::size_t stride = (v->size() + static_cast<std::size_t>(cap) - 1) / static_cast<std::size_t>(cap);
    std::vector<Site> out;
    for (std::size_t i = 0; i < v->size(); i += stride) out.push_back((*v)[i]);
    *v = std::move(out);
  };
  ob.sites_found = static_cast<long long>(general.size() + scan.size());
  ob.scan_sites_found = static_cast<long long>(scan.size());
  thin(&general, budget);
  thin(&scan, budget);

  const std::vector<double> in = q_inputs(state, out_bar);
  ir::Evaluator ev(q);  // holds a pointer: `q` may be injured and re-run in place
  std::vector<double> base_out(q.outputs.size(), 0.0), got(q.outputs.size(), 0.0);
  ev.run(in.data(), base_out.data());
  ob.state_bar_total = static_cast<long long>(q.outputs.size() - n_fwd_out);
  for (std::size_t i = n_fwd_out; i < base_out.size(); ++i) {
    if (!same_bits(base_out[i], 0.0)) ++ob.nonzero_state_bar;
  }

  auto measure = [&](const std::vector<Site>& sites, long long* total, long long* seen) {
    for (const Site& s : sites) {
      injure(&q, s);
      ev.run(in.data(), got.data());
      injure(&q, s);  // Add <-> Sub and negation are each their own inverse
      ++*total;
      for (std::size_t i = n_fwd_out; i < got.size(); ++i) {
        if (!same_bits(got[i], base_out[i])) {
          ++*seen;
          break;
        }
      }
    }
  };
  measure(general, &ob.sites, &ob.observable);
  measure(scan, &ob.scan_sites, &ob.scan_observable);
  // The reverse-scan sites are part of the reverse half: fold them into the headline too.
  ob.sites += ob.scan_sites;
  ob.observable += ob.scan_observable;
  return ob;
}

// ---- seeds ---------------------------------------------------------------------------------

std::vector<std::vector<double>> out_bar_seeds(int n_out, std::size_t keep) {
  const std::size_t n = static_cast<std::size_t>(n_out);
  std::vector<std::vector<double>> seeds;
  seeds.emplace_back(n, 1.0);
  seeds.emplace_back(n, 0.0);  // the zero seed: what makes a signed zero observable
  {
    std::vector<double> e(n, 0.0);
    e[n - 1] = 1.0;
    seeds.push_back(e);
  }
  {
    std::vector<double> mixed(n, 0.0);
    for (std::size_t o = 0; o < n; ++o) mixed[o] = static_cast<double>(static_cast<int>(o % 3) - 1);
    seeds.push_back(mixed);
  }
  if (keep > 0 && seeds.size() > keep) seeds.resize(keep);
  return seeds;
}

std::vector<std::vector<double>> ball_states(const std::vector<double>& x0, int n, double scale) {
  std::vector<std::vector<double>> states;
  for (int r = 0; r < n; ++r) {
    std::vector<double> z(x0);
    for (std::size_t k = 0; k < z.size(); ++k) {
      z[k] += scale * static_cast<double>((r + 1) * (static_cast<int>(k) % 5 - 2));
    }
    states.push_back(z);
  }
  return states;
}

// ---- the fixture table -------------------------------------------------------------------------

// `coverage`: the probe must find at least one reverse-half injury that moves a state adjoint.
// `structural`: it must find none -- validate, round-trip and a comparison of numbers no defect
// can move. Both are PINNED, so a fixture that changes class fails the gate instead of going
// quietly green.
enum class Class { coverage, structural };

struct Entry {
  const char* name;
  Class expect;              // the reverse half as a whole
  Class expect_scan;         // its reverse-SCAN domains (ignored when the fixture has no scan)
  std::function<Program()> build;
  std::function<std::vector<std::vector<double>>(const Program&)> states;
  std::size_t seeds;         // 0 = all four
  std::vector<int> batches;
};

const std::vector<Entry>& fixtures_under_gate() {
  static const std::vector<Entry> kFixtures = {
      // C2's fixture: 37 domains, 280 gathers, every elementwise op and the near-miss shapes.
      {"nearmiss(raw)", Class::coverage, Class::coverage, [] { return ir::infer(fixtures::record_nearmiss()); },
       [](const Program&) { return fixtures::nearmiss_states(4); }, 0, {1, 4, 5}},
      {"nearmiss(passed)", Class::coverage, Class::coverage,
       [] {
         Tape t = fixtures::record_nearmiss();
         epykos::standard_passes(t);
         return ir::infer(t);
       },
       [](const Program&) { return fixtures::nearmiss_states(4); }, 0, {1, 4, 5}},
      // C3a's: the leg and book Sum folds and the linear-zero interpolation's Affine coefficients.
      {"m1_book", Class::coverage, Class::coverage,
       [] { return ir::infer(fixtures::record_m1(fixtures::make_m1_book())); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-4); }, 2, {1, 4}},
      // compare_ois, both tenor sets: short telescopes the recurrence away (no scan), default
      // keeps one, and the default's carry sits FIRST in its pull where affine_scan's sits last.
      {"compare_ois(short)", Class::coverage, Class::coverage,
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 2;
         o.tenors = {"1Y", "2Y", "5Y", "10Y"};
         return ir::infer(fixtures::record_compare_ois(fixtures::make_compare_ois(o)).tape);
       },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-5); }, 0, {1, 4, 5}},
      {"compare_ois(default)", Class::coverage, Class::coverage,
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 2;
         return ir::infer(fixtures::record_compare_ois(fixtures::make_compare_ois(o)).tape);
       },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-5); }, 0, {1, 4, 5}},
      // D41's two scan fixtures, in both of affine_scan's recordings.
      {"affine_scan(final)", Class::coverage, Class::coverage,
       [] { return ir::infer(fixtures::record_affine_scan(fixtures::make_affine_scan(), false)); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-3); }, 0, {1, 4, 5}},
      {"affine_scan(path)", Class::coverage, Class::coverage,
       [] { return ir::infer(fixtures::record_affine_scan(fixtures::make_affine_scan(), true)); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-3); }, 2, {1, 4}},
      {"instrument_sample", Class::coverage, Class::coverage,
       [] { return ir::infer(fixtures::record_sample(fixtures::make_instrument_sample())); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-4); }, 0, {1, 4, 5}},
      // rfr_book's scan compounds REALISED fixings: 0 of its 40 scan rows depend on an input, so
      // no error in the reverse scan can reach a state adjoint. The `structural` pin on the scan
      // column is that finding, asserted rather than recalled.
      {"rfr_book", Class::coverage, Class::structural,
       [] { return ir::infer(fixtures::record_rfr(fixtures::make_rfr_book())); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-4); }, 2, {1, 4}},
      // The desk problem (PROBLEM.md Stage A), the default ~2,000-trade book. See the file header
      // for its measured emitted size; it is here because it fits, not because it was assumed to.
      {"stage_a(default)", Class::coverage, Class::coverage,
       [] { return ir::infer(fixtures::record_stage_a(fixtures::make_stage_a()).tape); },
       [](const Program& p) { return ball_states(p.input_values, 2, 1e-6); }, 2, {1, 4}},
  };
  return kFixtures;
}

struct Row {
  std::string name;
  std::size_t q_values = 0, q_domains = 0;
  Counts counts;
  Observability ob;
  bool has_scan = false;
};

}  // namespace

// ============================================================================================
// I1's gate.
// ============================================================================================

TEST(InvariantI1, AdjointOfPIsAProgramAndIsBitwiseAdjointRun) {
  std::vector<Row> rows;
  long long total_comparisons = 0, total_mismatches = 0;

  for (const Entry& e : fixtures_under_gate()) {
    SCOPED_TRACE(e.name);
    const Program p = e.build();
    const std::size_t n_scans = ir::scan_domains(p).size();
    const int n_in = static_cast<int>(p.inputs.size());
    const int n_out = static_cast<int>(p.outputs.size());
    ASSERT_GT(n_in, 0) << e.name;
    ASSERT_GT(n_out, 0) << e.name;

    int max_batch = 1;
    for (const int B : e.batches) max_batch = std::max(max_batch, B);
    adjoint::Options ao;
    ao.max_batch = max_batch;
    const adjoint::Adjoint ad(p, ao);

    // ---- I1, first sentence: `adjoint(P)` yields an `ir::Program` ----------------------------
    // Through the public face C4 added. `ad.program()` is the FORWARD program and must stay so.
    const Program q = ad.to_program();
    EXPECT_FALSE(q == ad.program())
        << e.name << ": to_program() returned the forward program -- that is §1b's symptom, not its fix";
    EXPECT_EQ(q.inputs.size(), p.inputs.size() + p.outputs.size()) << e.name << ": Q takes (state, out_bar)";
    EXPECT_EQ(q.outputs.size(), p.outputs.size() + p.inputs.size()) << e.name << ": Q returns (out, state_bar)";
    // ... and it is the same object the free function gives, so the API added nothing of its own.
    EXPECT_TRUE(q == adjoint::adjoint_to_program(p, ad.plan())) << e.name;

    // ---- I1's gate, clause 1: it passes `ir::validate` ----------------------------------------
    EXPECT_NO_THROW(ir::validate(q)) << e.name << ": the emitted reverse must be a valid Program";

    // ---- I1's gate, clause 2: it round-trips through serialize / deserialize ------------------
    {
      const std::string blob = ir::serialize(q);
      const Program round = ir::deserialize(blob);
      EXPECT_TRUE(round == q) << e.name << ": serialize/deserialize round trip";
    }

    // ---- I1's gate, clause 3: `exec::Interpreter` on Q reproduces `Adjoint::run` BITWISE ------
    Counts c;
    const std::vector<std::vector<double>> states = e.states(p);
    const std::vector<std::vector<double>> seeds = out_bar_seeds(n_out, e.seeds);
    ASSERT_FALSE(states.empty());
    const std::vector<ExecConfig> cfgs = exec_configs(max_batch);
    std::vector<std::unique_ptr<exec::Interpreter>> interps;
    interps.reserve(cfgs.size());
    for (const ExecConfig& cfg : cfgs) interps.push_back(std::make_unique<exec::Interpreter>(q, cfg.options));

    for (const int B : e.batches) {
      const std::size_t Bs = static_cast<std::size_t>(B);
      std::vector<double> state(static_cast<std::size_t>(n_in) * Bs, 0.0);
      std::vector<double> out_bar(static_cast<std::size_t>(n_out) * Bs, 0.0);
      for (std::size_t si = 0; si < states.size(); ++si) {
        for (std::size_t se = 0; se < seeds.size(); ++se) {
          // Lane b takes a different state and a different seed, so a cross-lane leak shows up.
          for (std::size_t b = 0; b < Bs; ++b) {
            const std::vector<double>& s = states[(si + b) % states.size()];
            const std::vector<double>& o = seeds[(se + b) % seeds.size()];
            for (int k = 0; k < n_in; ++k) state[static_cast<std::size_t>(k) * Bs + b] = s[static_cast<std::size_t>(k)];
            for (int k = 0; k < n_out; ++k)
              out_bar[static_cast<std::size_t>(k) * Bs + b] = o[static_cast<std::size_t>(k)];
          }
          const std::vector<double> ref = reference(ad, state, out_bar, B);
          const std::vector<double> in = q_inputs(state, out_bar);
          const std::size_t n_out_elems = static_cast<std::size_t>(n_out) * Bs;
          const std::string where =
              std::string(e.name) + " state " + std::to_string(si) + " seed " + std::to_string(se) + " B " +
              std::to_string(B);
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
    EXPECT_EQ(c.mismatches, 0) << e.name;
    EXPECT_GT(c.comparisons, 0) << e.name << ": a gate that compared nothing is not a gate";

    // ---- and: can a defect in that reverse REACH a state adjoint here? ------------------------
    // Budgeted so the probe's cost tracks the fixture: ~24M value-evaluations in total.
    const long long budget =
        std::max<long long>(4, std::min<long long>(48, 24000000 / static_cast<long long>(q.num_values() + 1)));
    const Observability ob = probe(q, p.outputs.size(), states.front(),
                                   std::vector<double>(static_cast<std::size_t>(n_out), 1.0), budget);
    rows.push_back({e.name, q.num_values(), q.domains.size(), c, ob, n_scans > 0});

    EXPECT_GT(ob.sites, 0) << e.name << ": the probe found nothing in the reverse half to injure, so this "
                              "fixture's verdict is not measured but assumed";
    EXPECT_EQ(ob.coverage(), e.expect == Class::coverage)
        << e.name << ": measured " << ob.observable << " of " << ob.sites
        << " reverse-half injuries observable at a state adjoint, which is not the class pinned in kFixtures. "
        << "A fixture that has gone structural is not a failure of the emitter -- re-pin it and say so -- but it "
        << "is no longer coverage and must not be counted as any.";
    if (n_scans > 0) {
      EXPECT_TRUE(ob.has_reverse_scan) << e.name << ": " << n_scans
                                       << " forward scan(s) but no recurrent non-scan domain in Q";
      EXPECT_GT(ob.scan_sites, 0) << e.name;
      EXPECT_EQ(ob.scan_observable > 0, e.expect_scan == Class::coverage)
          << e.name << ": measured " << ob.scan_observable << " of " << ob.scan_sites
          << " reverse-SCAN injuries observable, which is not the class pinned in kFixtures";
    } else {
      EXPECT_FALSE(ob.has_reverse_scan) << e.name << ": no forward scan, so Q must hold no reverse scan";
    }

    total_comparisons += c.comparisons;
    total_mismatches += c.mismatches;
  }

  // ---- the table ----------------------------------------------------------------------------
  std::printf(
      "\n[ I1 gate ] PRINCIPLES.md §1b: adjoint(P) is an ir::Program; validate, serialize/deserialize round trip,\n"
      "[ I1 gate ] and exec::Interpreter on it reproduces adjoint::Adjoint::run BITWISE.\n\n");
  std::printf("%-22s %10s %7s %11s %9s %11s  %-22s  %-22s\n", "fixture", "Q values", "Q doms", "comparisons",
              "mismatch", "nonzero sbar", "reverse half", "reverse scan");
  std::printf("%-22s %10s %7s %11s %9s %11s  %-22s  %-22s\n", "----------------------", "----------", "-------",
              "-----------", "---------", "-----------", "----------------------", "----------------------");
  for (const Row& r : rows) {
    char rev[80], scn[80], nz[32];
    std::snprintf(nz, sizeof nz, "%lld/%lld", r.ob.nonzero_state_bar, r.ob.state_bar_total);
    std::snprintf(rev, sizeof rev, "%lld/%lld %s %s", r.ob.observable, r.ob.sites,
                  r.ob.coverage() ? "LIVE" : "STRUCTURAL", r.ob.exhaustive() ? "(all)" : "(sampled)");
    if (!r.has_scan) {
      std::snprintf(scn, sizeof scn, "%s", "-- (no scan)");
    } else {
      std::snprintf(scn, sizeof scn, "%lld/%lld %s %s", r.ob.scan_observable, r.ob.scan_sites,
                    r.ob.scan_observable > 0 ? "LIVE" : "STRUCTURAL", r.ob.scan_exhaustive() ? "(all)" : "(sampled)");
    }
    std::printf("%-22s %10zu %7zu %11lld %9lld %11s  %-22s  %-22s\n", r.name.c_str(), r.q_values, r.q_domains,
                r.counts.comparisons, r.counts.mismatches, nz, rev, scn);
  }
  std::printf(
      "\n[ I1 gate ] \"n/m LIVE\" = n of m single-point injuries to the reverse half (an accumulation step's\n"
      "[ I1 gate ] Add<->Sub, an Affine pull coefficient's sign) moved a state adjoint, bitwise, at the record\n"
      "[ I1 gate ] point under the all-ones seed; \"(all)\" = every site the reverse half offers was injured,\n"
      "[ I1 gate ] \"(sampled)\" = a stride-sample of them. STRUCTURAL = none did, so on that fixture this gate\n"
      "[ I1 gate ] checks validity, the round trip and numbers no defect can move -- real, and not coverage.\n"
      "[ I1 gate ] \"nonzero sbar\" = state adjoints that are not bitwise +0.0 at that same point.\n");
  std::printf("[ I1 gate ] %lld comparisons, %lld mismatches, over %zu fixtures.\n\n", total_comparisons,
              total_mismatches, rows.size());

  EXPECT_EQ(total_mismatches, 0);
  EXPECT_GT(total_comparisons, 0);
}
