// EpykosEngine — the tape analysis `ir::infer` already computes, exposed so an earlier stage can
// use it (P1; DESIGN ONLY; D73). `docs/TERM_REWRITING.md` §4.1, which is the main structural
// question in this design.
//
// STATUS: an INTERFACE PROPOSAL. Nothing implements it, and `src/ir/signature.cpp` is untouched.
//
// ---------------------------------------------------------------------------------------------
// The problem
// ---------------------------------------------------------------------------------------------
//
// The symbolic algebraic stage runs BEFORE `ir::infer` (term.hpp). It needs two things inference
// already computes, and it cannot have either without a decision:
//
//   CHAINS.  A recurrence closed form needs to know a recurrence is there. `Inference::detect_chains`
//            (`src/ir/signature.cpp:408`) does exactly that, over tape nodes — but it runs as
//            phase five of `Inference::run`, after `compute_uses`, `initial_boundaries`,
//            `compute_deep` and `promote_shared`, and it both READS and WRITES `boundary_`
//            (it walks "stopping at boundaries", then marks each chain step and the chain's
//            initial value AS boundaries and clears the inner nodes). **It is not a function of
//            the tape. It is a function of (tape, boundary set), and it mutates the boundary set.**
//            That coupling is the finding; everything below follows from it.
//
//   SHAPES.  Class-uniform application (rule.hpp) needs the deep-hash partition that turns
//            517,036 tape nodes into 67 domains. Same machinery: `compute_deep` / `promote_shared`
//            / `hash_all` / `partition`.
//
// Both wanted things come out of the SAME prefix of `Inference::run`. That is the whole reason a
// clean answer exists.
//
// ---------------------------------------------------------------------------------------------
// Three options, and the recommendation
// ---------------------------------------------------------------------------------------------
//
// (a) FACTOR THE PREFIX OUT and let both consume it — recommended, in the cheap form below.
// (b) DUPLICATE a weaker detector in the algebraic stage, using only use counts rather than
//     inference's promoted boundaries. Cheap to write and wrong in a way that is hard to see: it
//     would find a DIFFERENT set of chains than `infer` will, with no test able to say which is
//     right, and D41's detection rules are subtle enough (a cut under an Add whose target is
//     single-use is a reduction and never a scan; the carry searched at most four operands deep;
//     three steps minimum) that a second implementation would drift.
// (c) RUN `ir::infer` TWICE — once as analysis, rewrite, then again for real. Correct and
//     wasteful: Stage A's inference is roughly 1.0 s of a 7.8 s record (M3 bench), and `Program`
//     carries no node_id backlink, so mapping scan domains back to tape nodes needs new plumbing
//     anyway. If new plumbing is needed regardless, (a) is the better place to put it.
//
// **The cheap form of (a), which is what this header proposes:** do not refactor `Inference` into
// pieces. Add ONE entry point that runs the existing phases up to and including `detect_chains`
// and returns what they found, leaving `infer` itself byte-for-byte unchanged and still computing
// everything from scratch. `analyse` and `infer` then share an implementation by construction
// rather than by discipline, and the algebraic stage sees exactly the chains inference sees.
//
// There is no staleness risk, and it is worth saying why explicitly: the stage REWRITES the tape,
// after which `infer` recomputes uses, boundaries, hashes, chains and domains from the rewritten
// tape as it always has. The analysis is ADVISORY INPUT TO THE REWRITER, never a contract the
// rewritten tape must honour. A chain the rewriter collapses simply is not there the second time.
//
// **What it costs, stated:** one extra pass of the analysis prefix per compilation (the phases are
// linear-ish in tape size; the whole of `infer` is ~1.0 s on Stage A's 517,036 nodes, and the
// prefix is a fraction of that), plus the risk that a future change to `Inference`'s phase order
// silently changes what `analyse` returns. The second is the real one, and the mitigation is that
// it is the SAME code — a change that breaks `analyse` breaks `infer` too, which has M1/M3
// round-trip gates on it.
#pragma once

#include <cstdint>
#include <vector>

#include "epykos/tape/tape.hpp"

namespace epykos::ir {

// One detected recurrence, in TAPE node ids — the form `Inference::detect_chains` already builds
// internally (`chains_`, `chain_of_`, `pos_in_chain_`). D41 for what qualifies: three or more
// identical steps, the carry found at most four operands deep, a cut under an Add or a Sum whose
// target is single-use rejected as a reduction rather than a scan.
struct Chain {
  std::vector<node_id> steps;   // in order; steps[0] reads `init`
  node_id init = invalid_node;  // the chain's initial value (a Const is a legal init)
  // Which operand position of the step carries the accumulator. The recurrence classifier needs
  // it to tell `acc * f` from `f * acc` without re-deriving the walk.
  int carry_operand = -1;
};

// The structural partition the deep hash induces over tape nodes: `shape_class[n]` is an index,
// equal for nodes `ir::infer` would put in one domain's group at the same position. This is the
// 517,036 -> 67 collapse, exposed. -1 for a node the analysis did not classify.
struct Analysis {
  std::vector<Chain> chains;
  std::vector<std::int32_t> chain_of;     // per node: index into `chains`, or -1
  std::vector<std::int32_t> shape_class;  // per node
  std::int32_t num_shape_classes = 0;
  std::vector<std::int32_t> shape_class_size;
  // Per node, from `compute_uses` — the algebraic stage wants it for its own reasons (a rewrite
  // that duplicates a multiply-used node is a cost decision, not a correctness one).
  std::vector<std::int32_t> uses;
};

// Runs the phases of `Inference::run` up to and including `detect_chains` and returns what they
// found. Pure: `tape` is not modified. Declared here, not defined — this header is a proposal, and
// implementing it means adding an entry point to `src/ir/signature.cpp`, which this package does
// not touch.
Analysis analyse(const Tape& tape);

}  // namespace epykos::ir
