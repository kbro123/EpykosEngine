// EpykosEngine — the cost model (M4/CM; docs/PROBLEM.md §7, docs/RESUME.md §3 "M4 — optimise the
// totality"). A static estimate of an ir::Program's evaluation time, over an *annotated* Program:
// a per-domain Treatment (materialised / fused into a reduction / inlined / a scan; DESIGN.md §7)
// that this header defines rather than importing from exec::Interpreter, because R0 (the sibling
// M4 package that turns the interpreter's hard-coded planner into a Rule framework) and CM run in
// parallel (docs/RESUME.md §3: "Order: {R0, CM} -> ...") and neither may depend on the other's
// deliverable. The same annotation type is therefore also what EG (M4's equality-saturation
// search) will cost a *candidate* rewritten program against, before any interpreter is built for
// it: `infer_plan` reproduces the planner's own documented rules from IR structure alone (HARD
// RULE 9: rules are keyed on IR structure, never a magic number for one fixture).
//
// What is priced (the package's own list):
//   - per domain: rows x lanes x (sum of per-op costs), DESIGN.md §7's tile-and-dispatch model;
//   - memory traffic for anything materialised: bytes written once plus bytes read by every
//     consumer, at a per-byte rate that depends on whether the WORKING SET fits L1 / L2 / L3 —
//     one tile's worth (tile rows x lane width), not the whole domain (DESIGN.md §4 "Layout":
//     "an intermediate is a contiguous vector of tile.L doubles in a preallocated scratch,
//     L1-sized at the default tile for small L"), so a wide lane chunk or a large tile can push
//     an otherwise-small domain into a worse tier while the total bytes moved stay the domain's
//     own size; see "What this cannot capture" below for what a per-domain check still misses;
//   - a gather cost per indirect read (Program::gathers; the scan carry is a gather like any
//     other, DESIGN.md §7, so it is priced the same way, no special case);
//   - a per-kernel dispatch cost (DESIGN.md §7: "dispatch per op per tile, not per element" —
//     charged once per (step, tile), not per row);
//   - a reduction epilogue cost per fold step of a whole-domain Sum / Affine (its members, not
//     its rows: DESIGN.md §6 R5, R7's "segment-sum epilogue");
//   - the Jacobian block's cost by AD mode (PROBLEM.md §7): forward (n_inputs passes of the
//     block's own program, M2's Dual<N> shape), reverse (n_outputs adjoint lanes), closed-form
//     affine (the block's constant Jacobian as a dense matrix product, DESIGN.md §6 R7).
//
// What this cannot capture (stated, D9 "estimates are labelled"; the calibration tool's
// validation report restates this against measured numbers):
//   - libm `std::exp` vs `exp_poly` (DESIGN.md §7, exec::ExpMode): both cost the SAME coefficient
//     slot (Op::Exp) here, because the op set has one Exp opcode; a fingerprint's fitted
//     coefficient is whichever ExpMode the calibration run used (stated in cost_model.json).
//     Comparing an ExpMode change needs two fits, one per mode, not one model.
//   - cache effects ACROSS domains sharing L1/L2 at once (two small domains each individually
//     inside L1 can still evict each other): the byte-tier classification here is per domain,
//     not of the working set of everything live at that point in the plan.
//   - fused *pairs* and chain tails (exec::Interpreter's fuse_pairs, "two steps in one kernel,
//     the middle value never stored"): priced here as two ordinary per-op costs plus two kernel
//     dispatches, which double-counts the dispatch overhead R0's real planner would collapse to
//     one. A rewrite that changes the fusion of adjacent steps is undercosted as "more expensive
//     than it will be", never the other way — a safe bias for extraction, not a free one.
//   - the interpreter's fused-into-reduction "kept rows" (rows also read elsewhere) and the
//     inliner's exact re-fetch count are approximated from IR structure (DomainFacts::gather_refs,
//     DomainPlan::kept_rows), not from a live plan; see infer_plan's own comment.
//
// Exactness class: none (CLAUDE.md's E0/E1 classes are for rewrites that change what a tape
// computes; this is estimation-only tooling that changes nothing about any recorded program, so
// it ships with unit tests, not a differential or mutation test — PROBLEM.md §7's own gate for
// this package is a stated prediction error, not a bitwise or tolerance gate).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/tape/op.hpp"

namespace epykos::optimise {

// ---- lane-width variants (mirrors exec::Interpreter's kernel specialisation, D15; no header
// dependency on exec/interpreter.hpp so a candidate program that never becomes a real
// Interpreter — EG's extraction search — can still be costed) --------------------------------

inline constexpr int lane_variants[] = {1, 4, 8, 16, 32, 64};
inline constexpr int n_lane_variants = 6;

// The variant index (0..n_lane_variants-1) priced for a chunk of `lane_width` lanes: the widest
// variant not exceeding it, or the narrowest (index 0) when lane_width < 1. A chunk wider than 64
// (not a real interpreter chunk; only reachable by calling estimate_domain directly) prices at
// the widest variant's per-lane rate, same as the interpreter's own "runtime" kernel does not
// exist for L > 64 (max_batch bounds chunk width there).
int lane_variant_index(int lane_width) noexcept;

// ---- the annotation: how a domain is evaluated (DESIGN.md §7) --------------------------------

enum class Treatment : std::uint8_t {
  Materialized,        // rows written to, and (by its readers) read from, the value buffer
  FusedIntoReduction,  // evaluated only where a whole-domain Sum/Affine reduction needs its
                       // members (plus `kept_rows` of its own, see DomainPlan); never gathered
  Inlined,             // evaluated inside its one consumer's tiles, for the rows it gathers; never materialised
};

struct DomainPlan {
  ir::domain_id domain = -1;
  Treatment treatment = Treatment::Materialized;
  // FusedIntoReduction only: rows of this domain ALSO read by something else (a gather, an
  // output, or a second reduction) and therefore still materialised (exec/interpreter.cpp's
  // `keep`); 0 (the common case) means every row is folded away.
  std::size_t kept_rows = 0;
  // FusedIntoReduction / Inlined only: the domain(s) this one is folded into. Exactly one for
  // Inlined (the planner requires a single consumer); one or more for FusedIntoReduction.
  std::vector<ir::domain_id> consumers;
};

struct Plan {
  int tile = 256;
  int lane_tile = 8;
  std::vector<DomainPlan> domains;  // one per Program::domains, in domain-id order

  const DomainPlan& of(ir::domain_id d) const;
};

// Per-domain structural facts derived purely from the Program (no interpreter dependency):
// which domains read this one through a gather, through a segment (Sum / Affine membership), and
// through a scan's carry; how many of its rows are tape outputs; how many gather-index entries
// (from OTHER domains) land on it. Generalises tests/stage_a/gate_lanes_test.cpp's ad hoc dump
// into engine code so the cost model, its calibration tool and that test's own coverage report
// (scripts/exec_coverage.py) can share one source of these facts.
struct DomainFacts {
  std::vector<ir::domain_id> gather_readers;
  std::vector<ir::domain_id> segment_readers;
  std::vector<ir::domain_id> scan_readers;  // subset of gather_readers that are scan domains (the carry)
  std::size_t output_rows = 0;
  std::size_t gather_refs = 0;  // gather index entries from other domains landing on this one
};

std::vector<DomainFacts> analyze(const ir::Program& program);

// Reproduces the planner's documented rules (DESIGN.md §6 R5/R6, §7; exec/interpreter.cpp's
// `row_fusion_pays`, `inline_max_refs_per_row` and the "whole-domain Sum/Affine producers inline
// too, when every row has the same member count" clause of `decide_inline`, restated here as
// pure IR-structure predicates, HARD RULE 9) to build a first Plan for `program` at the given
// tile / lane_tile. This is an approximation of the real interpreter's decision in three stated
// ways: (1) it does not know a domain's `kept_rows` (rows fused into a reduction, or a uniform
// whole-domain producer inlined into a consumer, that are ALSO read elsewhere) without a second
// pass it does not perform, so it always returns kept_rows = 0 (every row folded away) — a
// caller validating against a real Interpreter should overwrite `kept_rows` per domain from
// DomainFacts::output_rows/gather_readers/segment_readers directly (see cost_test.cpp); (2) it
// does not model fused pairs / chain tails at all (a per-op cost, not a per-kernel one, see
// cost.hpp's header comment); (3) it does not split a self-reading class by dependency level —
// scan-candidate domains that the signature pass could not lay out as a scan (Domain::scan_class
// but not recurrent) are costed as ordinary Materialized domains, which is what they are.
Plan infer_plan(const ir::Program& program, int tile, int lane_tile);

// ---- coefficients -------------------------------------------------------------------------

enum class ByteTier : std::uint8_t { L1 = 0, L2 = 1, L3 = 2, Dram = 3 };

ByteTier classify_bytes(std::size_t bytes, std::size_t l1_bytes, std::size_t l2_bytes, std::size_t l3_bytes) noexcept;

struct CostCoefficients {
  // ns per (row * lane) for op `o` at lane-width variant `lane_variants[v]`.
  std::array<std::array<double, epykos::op_count>, n_lane_variants> op_ns{};
  double byte_ns[4] = {0.0, 0.0, 0.0, 0.0};  // indexed by ByteTier: ns per byte moved
  double gather_ns = 0.0;                    // ns per indirect read (per row * lane), on top of the op cost
  double dispatch_ns = 0.0;                  // ns per kernel dispatch (one call over one tile / block / wave)
  double reduction_epilogue_ns = 0.0;        // ns per fold step (per member * lane) of a Sum / Affine segment
  std::size_t l1_bytes = 32 * 1024;
  std::size_t l2_bytes = 1024 * 1024;
  std::size_t l3_bytes = 8 * 1024 * 1024;

  // Documented fallback (labelled an estimate, CLAUDE.md "estimates are labelled"): every op 1 ns
  // per row per lane at every lane width (no vectorisation benefit assumed); byte costs a rough
  // memory-latency ladder at ~3 GHz (0.3 / 1 / 3 / 12 ns per byte for L1 / L2 / L3 / DRAM); 2 ns
  // per gather; 20 ns per kernel dispatch; 1 ns per reduction fold step. These are plausible
  // orders of magnitude, not a substitute for the calibration tool's fit (tools/costmodel/).
  static CostCoefficients defaults();
};

struct CostModel {
  CostCoefficients coeffs;
  std::string fingerprint;      // scripts/fingerprint.sh --id, or "" for CostCoefficients::defaults()
  bool loaded_from_file = false;

  // Reads bench/results/<fingerprint_id>/cost_model.json ("epykos-cost-model 1", written by
  // tools/costmodel/). Throws epykos::json::JsonError on a malformed file, std::runtime_error
  // when the file does not exist.
  static CostModel load(const std::string& fingerprint_id, const std::string& results_dir = "bench/results");

  // load(), or CostCoefficients::defaults() with a one-line warning on `warn` (std::cerr when
  // null) when the file is missing, unreadable or names a different fingerprint. Never throws:
  // this is the path production code (a future EG / catalogue consumer) uses, per the package
  // spec ("falls back to documented defaults with a warning").
  static CostModel load_or_default(const std::string& fingerprint_id, const std::string& results_dir = "bench/results", std::ostream* warn = nullptr);

  // Writes bench/results/<fingerprint>/cost_model.json (creating the directory).
  void save(const std::string& results_dir = "bench/results") const;
};

// ---- estimation -----------------------------------------------------------------------------

struct DomainCost {
  double op_ns = 0.0;         // arithmetic: fixed-arity steps (rows x lanes) + reduction fold steps (members x lanes)
  double write_ns = 0.0;      // bytes this domain writes, once, at its own region's cache tier (Materialized only)
  double read_ns = 0.0;       // bytes read back by every OTHER domain that consumes it (Materialized only)
  double gather_ns = 0.0;     // indirect reads this domain's own steps perform
  double dispatch_ns = 0.0;   // kernel-dispatch overhead (tiles/blocks/waves x steps)

  double total() const noexcept { return op_ns + write_ns + read_ns + gather_ns + dispatch_ns; }
};

// One domain's OWN predicted cost at lane width `L` (1 <= L <= 64, the interpreter's chunk
// width), NOT folding in what a FusedIntoReduction or Inlined domain contributes to its
// consumer(s) — that fold is estimate_program's job, because it must land on the consumer's
// entry, not the producer's (DESIGN.md §7: an inlined or fused-away domain's own measured slot is
// ~0, its work shows up inside the consumer it was folded into). For a FusedIntoReduction domain
// this prices only `plan.of(d).kept_rows` (0 by default): the rest is priced as pure op + gather
// cost with no dispatch/write/read of its own, returned by `folded_cost` below for the caller to
// add to the consumer(s).
DomainCost estimate_domain(const ir::Program& program, const std::vector<DomainFacts>& facts, const Plan& plan, ir::domain_id d, int L,
                            const CostModel& model);

// The op + gather cost of the ROWS of a FusedIntoReduction / Inlined domain `d` that are not
// priced by estimate_domain (rows - kept_rows for a fused domain; every row, gather_refs times
// over, for an inlined one — DESIGN.md §7: inlining recomputes the producer once per gathered
// reference, not once per row). 0 for a Materialized domain. estimate_program adds this to the
// domain's consumer(s), split evenly when there is more than one.
double folded_cost_ns(const ir::Program& program, const std::vector<DomainFacts>& facts, const Plan& plan, ir::domain_id d, int L,
                       const CostModel& model);

struct ProgramCost {
  double total_ns = 0.0;
  std::vector<double> per_domain_ns;  // size == program.domains.size(); a folded-away domain reads ~0 here (see above)
};

// The whole program's predicted time (ns) for B lanes, evaluated in chunks of `plan.lane_tile`
// (exec::Interpreter's own chunking, DESIGN.md §4/§7): ceil(B / lane_tile) chunks, the last one
// possibly narrower. `per_domain_ns` is intended to line up, domain by domain, against a
// `-DEPYKOS_EXEC_PROFILE` run's slots (tools/costmodel/'s validation report).
ProgramCost estimate_program(const ir::Program& program, const Plan& plan, int B, const CostModel& model);

// Convenience: infer_plan + analyze + estimate_program in one call, at B lanes of lane_tile
// `plan_lane_tile` and tile `plan_tile`.
ProgramCost estimate_program(const ir::Program& program, int B, int plan_tile, int plan_lane_tile, const CostModel& model);

// ---- Jacobian block cost by AD mode (PROBLEM.md §7) -------------------------------------------

enum class ADMode : std::uint8_t { Forward, Reverse, ClosedFormAffine };

// `one_pass_ns`: the cost of one B=1 evaluation of the block's own program (estimate_program at
// B=1). Forward: n_inputs passes (docs/RESUME.md §5 "M3 result": the whole problem on one
// Dual<N>, cost ~ N x one pass, the forward ladder's measured shape); reverse: n_outputs adjoint
// lanes, each `adjoint_multiplier` x one_pass_ns (DESIGN.md §7 measured the M1 book's value+
// adjoint at 7.4x-9.3x a value-only pass; the model's own `reduction_epilogue_ns` / dispatch
// terms do not separate a reverse pass from a forward one, so the caller states this multiplier —
// see cost.cpp's default of 8.0, the midpoint of that measured range, labelled an estimate);
// closed-form affine: an n_outputs x n_inputs dense matrix product (2 flops per entry: one Mul,
// one Add), priced at the model's L=1 Mul/Add rate — DESIGN.md §6 R7's block `linmap`, whose
// Jacobian is exact and constant so no repeated evaluation is needed at all.
double estimate_jacobian_ns(double one_pass_ns, int n_inputs, int n_outputs, ADMode mode, const CostModel& model, double adjoint_multiplier = 8.0);

}  // namespace epykos::optimise
