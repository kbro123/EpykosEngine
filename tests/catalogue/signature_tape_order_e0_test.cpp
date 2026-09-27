// D69 regression gate: `catalogue::Signature` is invariant under the ORDER in which the recorder
// appended the tape's nodes — the whole freedom C++ gives a compiler, not just the one symptom
// D61 happened to observe.
//
// Why the tape's ORDER is a compiler's choice at all. C++ leaves the evaluation order of a binary
// operator's operands (and of a call's arguments) unsequenced, and `Rec` appends one node per
// operation as it is evaluated, so two sibling subexpressions of one recorded statement reach the
// tape in whichever order the compiler chose. Node CONTENT is fixed by the source — `x * y`
// always records `mul` with `a = <the x node>` and `b = <the y node>` — but node POSITION is not.
// D25 first saw this break `tape_replay_e0_test`; D61 measured it on the default Stage A tape,
// where GCC 13 and Apple clang record the same 517,036 nodes with the same operand slots in a
// different order, and traced the catalogue's GCC-only coverage shortfall to it.
//
// Why a Signature can feel that order at all, when it reads only structure. `ir::infer`'s class
// formation is already order-blind: `extract_tree` builds a class prototype with a commutative
// node's operands sorted by their CONTENT hash (`token_at`), so `a op b` and `b op a` produce one
// prototype and one class. Exactly one artefact of the recording order survives into the emitted
// Program: `Class::emit_swapped`, "the first instance's recorded operand order", which `assemble`
// re-applies to every row of the class. Which instance is FIRST is a position, not a content —
// so reordering the tape can transpose the `a` / `b` of every emitted step of a class whose
// members were not all written the same way round. D61 made `catalogue::Signature` canonical
// under precisely that transposition (`canonical_swap_ab`); this gate is the property that fix
// was for, stated over the cause (the tape's order) rather than over the one symptom
// (`tests/catalogue/signature_commutative_e0_test.cpp` transposes an already-emitted Program's
// commutative steps). A re-recording can differ in ways a post-hoc transposition of the Program
// cannot express — which class instance is first, which Const node is created first, which domain
// is discovered first, and therefore the whole emitted domain order — and all of them are checked
// here.
//
// The permutation is not a synthetic shuffle. `reorder` below re-records the DAG through a ready
// list — at every point, any node whose operands are already recorded may be appended next — with
// byte-identical node CONTENT throughout: the same nodes, the same ops, the same operand wiring,
// the same constants, the same input and output ordinals. Only the order, and hence the numbering,
// differs. The gate then asserts that the catalogue cannot tell: the same multiset of Signatures,
// the same hashes, the same registry entries, the same coverage, and bitwise-identical interpreter
// output with the catalogue on and off.
//
// That this degree of freedom is REAL and not hypothetical was measured directly for D69, on this
// machine and in `gcc:13`, by digesting the recorded tape and the emitted Program under each
// compiler: the M1 book and the default Stage A tape have the same node COUNT and the same
// per-node operand slots under both, and a different node ORDER under each (the node digests
// differ on both fixtures); the default Stage A tape's emitted `ir::Domain::name` list — the
// recorded operand order that `Class::emit_swapped` propagates — ALSO differs between the two
// compilers, and still does, because D61 fixed the catalogue's fingerprint and deliberately left
// `ir::assemble` alone; and the digest of the catalogue Signatures is IDENTICAL on both. The last
// of those three is the property this file gates.
//
// What this file does NOT do, said plainly because the measurement above says the opposite happens
// in practice. A bounded re-recording here never changes the emitted operand order on either
// fixture, while two real compilers do. `emit_swapped` is taken from whichever instance of a class
// the tape holds FIRST, and one class's instances are thousands of nodes apart — one curve's
// `c * x` and another's `x * c`, in different statements of different loops. A compiler reaches
// that by composing intra-expression reorderings across whole statements; a sliding window over
// ready nodes is a local shuffle and cannot. So this file is the WIDE audit — it is what turned up
// the `ir::infer` level-assignment finding below — and `signature_commutative_e0_test`'s
// subset leg is what covers the `emit_swapped` degree of freedom exactly, by enumerating it
// directly rather than trying to provoke it through the recorder. Both are needed (D69 §4).
//
// An _e0_test.cpp TU (-ffp-contract=off in every preset): the bitwise legs compare the
// catalogue's generated kernels (src/catalogue/generated/kernels_e0.cpp, themselves E0) against
// the interpreter's generic per-step path, so they must hold under release and reference alike.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
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

using epykos::node_id;

namespace {

std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// How one re-recording schedules the DAG. At every point a recorder may append any node whose
// operands are all recorded already — that set is the "ready list", and which member of it a given
// recording reaches first is the degree of freedom this gate explores.
//
// `window` bounds how far a schedule may stray from the original recording, which is exactly what
// separates a recording a COMPILER could have produced from one it could not. A compiler's freedom
// here is local and bounded: it may evaluate the unsequenced sibling subexpressions of one
// full-expression in either order, but it may not move work across a sequence point, because
// appending to the tape is an observable side effect. `window = k` picks pseudo-randomly among the
// k lowest-numbered ready nodes: k = 1 reproduces the original recording exactly (the fixture's
// tape is topological, so the next original node is always the lowest ready one), and a k of a few
// dozen is already far larger than the expression a compiler gets to shuffle. `window = 0` means
// unbounded — every topological order, including ones no conforming compiler can produce; D69
// measures what that costs and why it is reported rather than gated.
struct Walk {
  std::size_t window = 1;  // 0 = unbounded
  std::uint64_t seed = 0;
  const char* name = "";
};

// splitmix64, so a seeded schedule is reproducible on every platform and depends on nothing but
// the seed and how many nodes have been appended so far.
std::uint64_t splitmix64(std::uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// `src` re-recorded: the same nodes, with the same ops, the same operand wiring, the same
// constants, the same input ordinals and the same output ordinals — appended in the order `w`'s
// schedule takes them off the ready list. Every result is a tape a conforming recorder could have
// produced from the same source, with byte-identical node CONTENT throughout; only the order, and
// therefore the numbering, differs. `moved` receives how many nodes ended up at a different index.
epykos::Tape reorder(const epykos::Tape& src, const Walk& w, std::size_t* moved) {
  const std::size_t n = src.size();
  epykos::Tape dst;
  std::vector<node_id> map(n, epykos::invalid_node);

  auto operand_count = [&](node_id id) -> int {
    const epykos::Node& nd = src[id];
    return epykos::op_is_variadic(nd.op) ? nd.nargs : epykos::op_arity(nd.op);
  };
  auto operand_at = [&](node_id id, int j) -> node_id {
    const epykos::Node& nd = src[id];
    if (epykos::op_is_variadic(nd.op)) return src.args(nd)[static_cast<std::size_t>(j)];
    return j == 0 ? nd.a : (j == 1 ? nd.b : nd.c);
  };

  // One edge per operand OCCURRENCE (so `x * x` is counted twice and released twice).
  std::vector<std::int32_t> pending(n, 0);
  std::vector<std::vector<node_id>> consumers(n);
  for (std::size_t i = 0; i < n; ++i) {
    const node_id id = static_cast<node_id>(i);
    const int arity = operand_count(id);
    pending[i] = arity;
    for (int j = 0; j < arity; ++j) consumers[static_cast<std::size_t>(operand_at(id, j))].push_back(id);
  }

  // The ready list is a min-heap on the original node number, so a bounded window is its k
  // smallest entries and a 500k-node tape still schedules in O(n log n).
  std::vector<node_id> ready;
  auto later = [](node_id x, node_id y) { return x > y; };  // std::*_heap with this is a min-heap
  auto push_ready = [&](node_id id) {
    ready.push_back(id);
    std::push_heap(ready.begin(), ready.end(), later);
  };
  auto release = [&](node_id produced) {
    for (node_id cons : consumers[static_cast<std::size_t>(produced)]) {
      if (--pending[static_cast<std::size_t>(cons)] == 0) push_ready(cons);
    }
  };

  // Seed the ready list with the leaves that are not Inputs (the Const nodes), BEFORE the inputs
  // are released below — a node released by an input must be queued once, by that release, and
  // not a second time by this scan.
  for (std::size_t i = 0; i < n; ++i) {
    if (pending[i] == 0 && src[static_cast<node_id>(i)].op != epykos::Op::Input) push_ready(static_cast<node_id>(i));
  }
  // Inputs first, in ordinal order: an input's ordinal is its creation order, so a re-recording
  // that is to answer the same `input_values` must create them in the same sequence. They are
  // leaves, so this is a legal start for any schedule.
  for (node_id in_id : src.inputs()) {
    map[static_cast<std::size_t>(in_id)] = dst.input(src[in_id].konst);
    release(in_id);
  }

  std::vector<node_id> operands;
  std::vector<node_id> window;
  std::uint64_t picks = 0;
  while (!ready.empty()) {
    node_id id = epykos::invalid_node;
    if (w.window == 0) {
      // Unbounded: any ready node at all. Picked by swap-remove, so this case stays O(1) per node
      // instead of draining the heap; the heap order is never consulted when the window is
      // unbounded, so leaving it unmaintained here costs nothing.
      const std::size_t pick = static_cast<std::size_t>(splitmix64(w.seed + picks) % ready.size());
      id = ready[pick];
      ready[pick] = ready.back();
      ready.pop_back();
    } else {
      const std::size_t k = std::min(w.window, ready.size());
      window.clear();
      for (std::size_t q = 0; q < k; ++q) {
        std::pop_heap(ready.begin(), ready.end(), later);
        window.push_back(ready.back());
        ready.pop_back();
      }
      const std::size_t pick = (k == 1) ? 0 : static_cast<std::size_t>(splitmix64(w.seed + picks) % k);
      id = window[pick];
      for (std::size_t q = 0; q < k; ++q) {
        if (q != pick) push_ready(window[q]);
      }
    }
    ++picks;
    if (map[static_cast<std::size_t>(id)] != epykos::invalid_node) {
      throw std::logic_error("reorder: node " + std::to_string(id) + " was queued twice");
    }

    const epykos::Node& nd = src[id];
    node_id made = epykos::invalid_node;
    switch (nd.op) {
      case epykos::Op::Const:
        made = dst.constant(nd.konst);
        break;
      case epykos::Op::Sum:
      case epykos::Op::Affine: {
        operands.clear();
        for (node_id a : src.args(nd)) operands.push_back(map[static_cast<std::size_t>(a)]);
        const std::span<const double> cf = src.coefs(nd);
        made = dst.variadic(nd.op, operands, cf, nd.konst);
        break;
      }
      default: {
        const int a = epykos::op_arity(nd.op);
        if (a == 1) {
          made = dst.unary(nd.op, map[static_cast<std::size_t>(nd.a)]);
        } else if (a == 2) {
          made = dst.binary(nd.op, map[static_cast<std::size_t>(nd.a)], map[static_cast<std::size_t>(nd.b)]);
        } else {
          made = dst.ternary(nd.op, map[static_cast<std::size_t>(nd.a)], map[static_cast<std::size_t>(nd.b)],
                             map[static_cast<std::size_t>(nd.c)]);
        }
        break;
      }
    }
    map[static_cast<std::size_t>(id)] = made;
    release(id);
  }
  for (node_id o : src.outputs()) dst.output(map[static_cast<std::size_t>(o)]);

  *moved = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (map[i] != static_cast<node_id>(i)) ++*moved;
  }
  return dst;
}

// The catalogue's view of one Program: the sorted multiset of its catalogue-eligible domains'
// signature renderings, with the registry entry each one resolves to. Sorted because a
// re-recording legitimately renumbers the domains — what must not change is WHICH shapes the
// Program contains and which of them the catalogue serves, not the order they appear in.
struct CatalogueView {
  std::vector<std::string> shapes;   // "<signature>#<hash> -> <kernel name or ->", sorted
  std::size_t eligible = 0;
  std::size_t catalogued = 0;
};

CatalogueView view_of(const ir::Program& p) {
  CatalogueView v;
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    const ir::domain_id did = static_cast<ir::domain_id>(d);
    if (!catalogue::is_cataloguable(p, did)) continue;
    ++v.eligible;
    const catalogue::Signature sig = catalogue::signature_of(p, did);
    const catalogue::Kernel k = catalogue::lookup(sig);
    if (k != nullptr) ++v.catalogued;
    v.shapes.push_back(sig.to_string() + "#" + std::to_string(sig.hash()) + " -> " +
                       (k != nullptr ? "kernel" : "-"));
  }
  std::sort(v.shapes.begin(), v.shapes.end());
  return v;
}

// The multiset of PRE-D61 shapes: each catalogue-eligible domain's op sequence with its operand
// KINDS written in the order `assemble` emitted them, i.e. `Class::emit_swapped`'s order, with no
// commutative canonicalisation. This is exactly what `catalogue::Signature` was before D61 —
// computed here from the Program rather than through `signature_of`, so the witness does not
// depend on the very code under test.
//
// It is the gate's own non-vacuity witness (D53). Reordering the tape is only an interesting
// thing to have done if the recording order actually reached the emitted Program; if it did, this
// multiset changes, and the canonical multiset above must not. If it did not change either, the
// walk exercised nothing and says nothing, and this gate must not report it as evidence.
std::vector<std::string> raw_shapes(const ir::Program& p) {
  auto kind = [](const ir::Slot& s) -> const char* {
    switch (s.kind) {
      case ir::SlotKind::None: return "-";
      case ir::SlotKind::Step: return "step";
      case ir::SlotKind::Literal: return "lit";
      case ir::SlotKind::Column: return "col";
      case ir::SlotKind::Gather: return "gat";
      case ir::SlotKind::Segment: return "seg";
      case ir::SlotKind::Input: return "in";
    }
    return "?";
  };
  std::vector<std::string> out;
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    const ir::domain_id did = static_cast<ir::domain_id>(d);
    if (!catalogue::is_cataloguable(p, did)) continue;
    std::string s;
    for (const ir::Step& st : p.groups[d].steps) {
      s += epykos::to_string(st.op);
      s += '(';
      s += kind(st.a);
      s += ',';
      s += kind(st.b);
      s += ',';
      s += kind(st.c);
      s += ";k=";
      s += kind(st.konst);
      s += ");";
    }
    out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end());
  return out;
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

// The whole gate for one recorded fixture.
//
// `strict` is the difference between the two regimes D69 measured.
//
// At a BOUNDED window — a recording that stays as close to the original as a compiler's own local
// freedom keeps it — everything must match: the catalogue's whole view, its coverage, and the
// values. At an UNBOUNDED window a re-recording may additionally move a row between two levels of
// a non-trivial SCC. That is `ir::infer`'s own level assignment, not the fingerprint: a domain
// gains or loses a row, a constant slot that was uniform over the old row set stops being uniform,
// and `assemble` emits a Column where it emitted a Literal — a genuinely different shape, and
// correctly fingerprinted as one. Measured on the 60-trade Stage A fixture, three seeds per
// window: at windows 1, 2, 4, 8 and 16 nothing moves at all; from window 64 upwards, one domain
// goes from 27 rows at level 8 and 3 rows at level 9 to 25 and 5, its `mul(step, lit)` becomes
// `mul(step, col)`, and the catalogue serves 54 rather than 53 of that fixture's 56 eligible
// domains.
//
// That second regime is reported rather than gated because it is not something the two compilers
// do. Appending to the tape is an observable side effect, so a conforming compiler may reorder
// within one full-expression but never across a sequence point; and directly measured (D69), GCC
// 13.5.0 and Apple clang 21 emit the same 67 domains, 6 literals, 24 columns, 77 gathers and 18
// segments for the default Stage A tape with an identical Signature digest, differing only in the
// recorded node order and in `ir::Domain::name`. Neither compiler moves a row between levels. So
// the unbounded case is held to what must be true of it regardless: the values, and the catalogue
// never being WRONG where it does dispatch.
void check_tape_order_invariance(const epykos::Tape& tape, const std::string& label,
                                 const std::vector<Walk>& walks, bool strict) {
  ASSERT_NO_THROW(tape.validate()) << label << ": the fixture's own tape";
  const ir::Program base = ir::infer(tape);
  const CatalogueView base_view = view_of(base);
  const std::vector<std::string> base_raw = raw_shapes(base);
  ASSERT_GT(base_view.eligible, 0u) << label << ": no catalogue-eligible domain -- this case would prove nothing";
  ASSERT_GT(base_view.catalogued, 0u) << label << ": nothing dispatches -- this case would prove nothing";

  catalogue::Coverage base_cov;
  const std::vector<double> base_generic = run_all(base, /*use_catalogue=*/false, nullptr);
  const std::vector<double> base_cat = run_all(base, /*use_catalogue=*/true, &base_cov);

  std::size_t witnessing_walks = 0;
  for (const Walk& w : walks) {
    const std::string what = label + ", walk '" + w.name + "'";
    std::size_t moved = 0;
    const epykos::Tape re = reorder(tape, w, &moved);
    ASSERT_NO_THROW(re.validate()) << what << ": a re-recording must be a valid tape";

    // The re-recording is the same computation: same node count, same op histogram, same inputs
    // and outputs. Anything else and this gate would be comparing two different programs.
    ASSERT_EQ(re.size(), tape.size()) << what;
    ASSERT_EQ(re.op_histogram(), tape.op_histogram()) << what;
    ASSERT_EQ(re.num_inputs(), tape.num_inputs()) << what;
    ASSERT_EQ(re.num_outputs(), tape.num_outputs()) << what;
    ASSERT_EQ(re.input_values(), tape.input_values()) << what;

    const ir::Program p = ir::infer(re);
    ASSERT_NO_THROW(ir::validate(p)) << what;
    const bool witnessed = raw_shapes(p) != base_raw;
    if (witnessed) ++witnessing_walks;

    // 1. The fingerprint: the same multiset of shapes, resolving to the same registry entries.
    const CatalogueView v = view_of(p);
    if (strict) {
      EXPECT_EQ(base_view.eligible, v.eligible) << what;
      EXPECT_EQ(base_view.catalogued, v.catalogued) << what;
      EXPECT_EQ(base_view.shapes, v.shapes) << what << ": the catalogue's view of the re-recorded tape differs";
    }

    // 2. The engine: the same coverage, and bitwise-identical values with the catalogue on and
    //    off. Reordering independent nodes cannot change any single op's operands or result, so
    //    the outputs must equal the original recording's, bit for bit, on both paths -- and that
    //    holds at ANY window, bounded or not.
    catalogue::Coverage cov;
    const std::vector<double> generic = run_all(p, /*use_catalogue=*/false, nullptr);
    const std::vector<double> cat = run_all(p, /*use_catalogue=*/true, &cov);
    if (strict) {
      EXPECT_EQ(base_cov.groups_total, cov.groups_total) << what;
      EXPECT_EQ(base_cov.groups_catalogued, cov.groups_catalogued) << what;
      EXPECT_EQ(base_cov.rows_total, cov.rows_total) << what;
      EXPECT_EQ(base_cov.rows_catalogued, cov.rows_catalogued) << what;
    }
    ASSERT_EQ(base_generic.size(), generic.size()) << what;
    ASSERT_EQ(base_cat.size(), cat.size()) << what;
    std::size_t bad_generic = 0, bad_cat = 0;
    for (std::size_t k = 0; k < base_generic.size(); ++k) {
      if (bits(base_generic[k]) != bits(generic[k])) ++bad_generic;
      if (bits(base_cat[k]) != bits(cat[k])) ++bad_cat;
    }
    EXPECT_EQ(bad_generic, 0u) << what << ": the generic path disagrees with the original recording";
    EXPECT_EQ(bad_cat, 0u) << what << ": the catalogued path disagrees with the original recording";
    std::size_t bad_onoff = 0;
    for (std::size_t k = 0; k < generic.size(); ++k) {
      if (bits(generic[k]) != bits(cat[k])) ++bad_onoff;
    }
    EXPECT_EQ(bad_onoff, 0u) << what << ": catalogue on/off differs on the re-recorded tape";

    std::cout << "[ tape order ] " << what << ": " << moved << " of the " << tape.size()
              << " recorded nodes appended at a different index; the pre-D61 (uncanonicalised) shapes "
              << (witnessed ? "DIFFER" : "are unchanged") << "; " << v.catalogued << " of the " << v.eligible
              << " catalogue-eligible domain(s) dispatch (the original recording: " << base_view.catalogued
              << " of " << base_view.eligible << ")\n";
  }

  // D53: a gate must say whether its deliverable actually fires. At a bounded window it does not,
  // on either reference fixture, and that IS the measured result -- the recording order never
  // reaches the emitted Program at the scale a compiler can reorder -- so it is reported here, not
  // asserted. At an unbounded window it must fire, or that case is measuring nothing.
  if (!strict) {
    EXPECT_GT(witnessing_walks, 0u)
        << label << ": no re-recording changed the emitted operand order at all -- this case is vacuous";
  } else {
    std::cout << "[ tape order ] " << label << ": " << witnessing_walks << " of the " << walks.size()
              << " compiler-realisable re-recordings changed the emitted operand order\n";
  }
}

// The recordings a conforming compiler could really have produced: a bounded reordering window,
// several seeds. `window = 1` is the control and must reproduce the original recording exactly.
std::vector<Walk> compiler_walks() {
  return {Walk{1, 0, "window 1 (reproduces the recording)"},
          Walk{4, 0x5EED0001ull, "window 4"},
          Walk{4, 0x5EED0002ull, "window 4, second seed"},
          Walk{16, 0x5EED0003ull, "window 16"},
          Walk{16, 0x5EED0004ull, "window 16, second seed"}};
}

// Recordings no compiler can produce, because they move work across a sequence point: any
// topological order at all. Held only to what must be true regardless -- see the `strict` comment
// on check_tape_order_invariance.
std::vector<Walk> unbounded_walks() {
  return {Walk{0, 0x5EED0011ull, "unbounded"}, Walk{0, 0x5EED0012ull, "unbounded, second seed"}};
}

}  // namespace

// ---- the gate: a compiler-realisable re-recording changes nothing the catalogue can see --------

TEST(CatalogueSignatureTapeOrderE0, M1Book) {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  check_tape_order_invariance(tape, "M1 book", compiler_walks(), /*strict=*/true);
}

TEST(CatalogueSignatureTapeOrderE0, StageA) {
  fixtures::StageAOptions opt;
  opt.trades = 60;
  opt.scenarios = 0;
  const fixtures::StageA s = fixtures::make_stage_a(opt);
  const fixtures::StageATape tape = fixtures::record_stage_a(s);
  check_tape_order_invariance(tape.tape, "Stage A (60 trades)", compiler_walks(), /*strict=*/true);
}

// ---- the finding, executed rather than asserted: beyond what a compiler can do ------------------
//
// D69's measured boundary. These recordings are not reachable by any conforming compiler, and the
// catalogue's coverage really does move on them, because `ir::infer` reassigns a row between two
// levels of a non-trivial SCC and a constant slot stops being uniform over its domain. What must
// still hold, and is asserted, is that the values do not change and that the catalogue is never
// WRONG -- every kernel it dispatches still reproduces the generic per-step path bit for bit.

TEST(CatalogueSignatureTapeOrderE0, StageAUnboundedReorderingIsValuePreservingButNotCoveragePreserving) {
  fixtures::StageAOptions opt;
  opt.trades = 60;
  opt.scenarios = 0;
  const fixtures::StageA s = fixtures::make_stage_a(opt);
  const fixtures::StageATape tape = fixtures::record_stage_a(s);
  check_tape_order_invariance(tape.tape, "Stage A (60 trades), beyond compiler freedom", unbounded_walks(),
                              /*strict=*/false);
}
