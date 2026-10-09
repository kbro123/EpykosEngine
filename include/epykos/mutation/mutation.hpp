// EpykosEngine — the mutation-testing selector (DESIGN.md §11 "mutation testing", D32; M2/Q4).
//
// A mutant is a deliberate one-line defect inside a real pass, guarded by
// `epykos::mutant("<pass>.<defect>")` and listed in `mutation::registry` below. Mutants exist only in
// a build configured with the CMake option EPYKOS_MUTATIONS=ON (the `mutation` preset: the
// reference preset plus that option). There, the environment variable EPYKOS_MUTANT=<name> selects
// exactly one mutant per process; it is read once, on the first query. In every other build
// `epykos::mutant()` is a constexpr false, so the defects compile away: zero overhead, no
// environment read, nothing to select.
//
// The point of a mutant is to prove that the GATES catch it — round-trip identity, the E0
// differential tests, the verify tests — not a test written for the mutant. scripts/mutation_test.sh
// builds the mutation preset once and, for every registered name, runs the gate tests with
// EPYKOS_MUTANT set; a mutant that no gate fails against has survived and the script fails.
// tests/mutation/registry_test.cpp pins the registry (so the script cannot silently skip a
// mutant), checks that every name has exactly one use site under src/ and that every use site
// names a registered mutant, and checks the selector against the environment.
//
// Adding a mutant: add the name here, to the registry test's expected list and to the table in
// docs/WORKLOADS.md §M2, then the guarded line in the pass. A name is "<pass>.<defect>", lower
// case, dot-separated. A mutant of an op the M1 book does not contain (select, recip) is
// exercised by the near-miss shapes fixture's gates, a mutant of the scan machinery (M3/G3) by
// the scan fixtures' gates; the table says which fixture exercises each.
#pragma once

#include <cstddef>
#include <string_view>

namespace epykos::mutation {

// Every mutant the engine carries, by name. The order is the order the harness runs them in.
inline constexpr std::string_view registry[] = {
    "cse.merge_nonequal",            // cse: a Const operand contributes no identity to the key (its bit pattern is ignored)
    "fold_sum.wrong_order",          // fold_sum: the Sum's operands are emitted in reverse fold order
    "affine.wrong_coefficient",      // affine_collapse: one coefficient (the first of the first Affine emitted) is off by one ulp
    "affine.drop_offset",            // affine_collapse: the leading constant c_0 is dropped (-0.0 emitted instead)
    "affine.single_term_unscaled",   // affine_collapse: a one-term Affine (a scaled atom read as a value, M3/G1) drops its coefficient (1·x instead of c·x); no such product on the M1 book: the curve gates
    "expander.drop_gather",          // expand: gather 0 reads value id r (the identity index) instead of index[r]
    "expander.segment_off_by_one",   // expand: every segment loses its last member
    "signature.merge_classes",       // infer: the const-slot pattern is not part of the signature (a constant slot is a reference)
    "interpreter.tile_boundary",     // Interpreter::run: the last row of every elementwise tile is skipped
    "lanes.drop_split_remainder",    // exec::next_lane_chunk: after a chunk group is split into specialised lane widths, the cursor advances past the whole GROUP instead of past the chunk it just produced, so every lane the split left for a later chunk is never evaluated and the caller reads back whatever its output buffer held. Drives BOTH runtimes' chunk loops, so it is one defect in the interpreter and the adjoint at once; a no-op on a group whose width is already a variant, so it bites at B = 2, 3, 5, 6, 7 and at any B whose last group is one of those (tests/exec/lane_chunking_e0_test.cpp, and the odd-B batches of tests/exec/m1_interp_e0_test.cpp and tests/adjoint/m1_adjoint_e0_test.cpp)
    "adjoint.wrong_transpose",       // build_plan: gather 0's pull reads the slot of row index[r] (the forward index array) instead of row r
    "adjoint.drop_broadcast",        // build_plan: the last member of every Sum row gets no reader entry (the broadcast skips it)
    "adjoint.affine_not_transposed", // build_plan: Affine reader coefficients read from the forward table at the transposed position (W, not W^T)
    "adjoint.select_wrong_arm",      // Adjoint::run: the select rule routes the adjoint to the other arm (no select on the M1 book: the near-miss gate)
    "adjoint.recip_rule_sign",       // Adjoint::run: recip's rule accumulates +(ybar*y)*y instead of -(ybar*y)*y (no recip on the M1 book: the near-miss gate)
    "implicit.ift_not_transposed",   // Factors::solve_transposed: the IFT multiplier solves F_z lambda = z_bar instead of F_z^T lambda = z_bar
    "implicit.ift_drop_fp",          // Factors::ift_adjoint: the parameter pull p_bar -= F_p^T lambda is skipped (the quotes receive no adjoint)
    "implicit.stale_jacobian",       // BlockSolver::solve: the IFT uses the Jacobian of the last iterate before convergence, not the solution's
    "solver.diagnostic_returns_zero", // BlockSolver::solve: with the optimality diagnostic switched off, report 0.0 instead of NaN -- and zero is the signature of a perfectly converged solve, so the switched-off diagnostic would announce success (D94)
    "solver.lazy_jacobian_never_builds", // BlockSolver::solve: the deferred exit Jacobian is never built even when the caller asked for it, so the IFT runs on a stale or absent one (visible on a MOVED market, not at the record point: D85)
    "expander.scan_carry_from_init", // expand: every step of a scan chain reads the chain's initial value instead of the previous step (no scan on the M1 book: the scan fixtures' round-trip gates)
    "interpreter.scan_drop_last_wave", // Interpreter::run: the last wave of every scan (the last step of its longest chains) is not evaluated (the scan fixtures' E0 gates)
    "adjoint.scan_forward_order",    // Adjoint::run: the reverse scan visits the rows forwards, so the carried adjoint arrives after it was pulled (the scan fixtures' adjoint gates)
    "r1.ignores_last_row",           // rewrite::R1FoldUniformColumns: the uniformity check stops one row early, so a column differing only in its last row is wrongly folded
    "r1.wrong_slot",                 // rewrite::R1FoldUniformColumns: the literal is always written into operand `a`, even when the uniform column was read from b / c / konst
    "r2.wrong_run_boundary",         // rewrite::R2BucketRows: a run boundary compares row r to r-2 instead of r-1, mis-sizing the buckets
    "r2.column_slice_uses_wrong_bucket", // rewrite::R2BucketRows: bucket k (k>0) is built from bucket (k-1)'s row range instead of its own
    "r2.accepts_full_singleton_split", // rewrite::R2BucketRows: run_buckets drops its "consolidate something" floor, so a domain whose every row has a distinct signature is split into one one-row domain per row -- more IR, no batch formed (D65; tests/rewrite/r2_bucket_rows_e0_test.cpp's all-distinct synthetic domain)
    "r2.accepts_fragmented_split", // rewrite::R2BucketRows: run_buckets drops its per-kind floor, so a domain whose kinds are INTERLEAVED is split into one domain per contiguous run -- hundreds of fragments where a handful of kinds exist (D65; tests/rewrite/r2_bucket_rows_e0_test.cpp's interleaved synthetic domain)
    "r3.off_by_one_member",          // rewrite::R3ElideTrivialMaps: a domain's row substitutes the NEXT row's sole member instead of its own
    "r3.treats_length_two_as_trivial", // rewrite::R3ElideTrivialMaps: a two-member segment row is wrongly accepted as trivial, dropping the second term
    "r6.ignore_reduction_boundary",  // R6MaterialiseBoundaries: a producer feeding a Sum/Affine segment (or an output) is inlined away anyway -- its consumer's segment then reads an unmaterialised row
    "r6.ignore_fanout_boundary",     // R6MaterialiseBoundaries: only the first reader gather's consumer is checked, so a producer read by several distinct consumers is still inlined into just one of them
    "r7.no_offset_rebase",           // R7BlockLinmap::split_linmap_domain: a block's sliced segment keeps the ORIGINAL whole-domain absolute offsets instead of rebasing them to its own members array
    "r7.wrong_block_value_base",     // R7BlockLinmap::split_linmap_domain: the running value_base accumulator advances by a block's segment length instead of its row count
    "r4a.wrong_literal",             // rewrite::push_unary_through_gathers: the relocated step multiplies by 0.0 instead of 1.0 (M4/R-b's own verify_rule gate)
    "r4a.wrong_row_map",             // rewrite::push_unary_through_gathers: the new gather reads S_op row `d_row` instead of `row_of(S, old_gather.index[d_row])` (M4/R-b's own verify_rule gate)
    "r4b.wrong_op",                  // rewrite::shared_reciprocal: combines a and the reciprocal with Add instead of Mul (M4/R-b's own verify_rule gate)
    "r4b.wrong_row_map",             // rewrite::shared_reciprocal: the new gather reads S_recip row `d_row` instead of `row_of(S, old_gather.index[d_row])` (M4/R-b's own verify_rule gate)
    "r5.wrong_step_index",           // rewrite::group_formation: the consumer's gather-replacement points at the producer's FIRST step instead of its last (M4/R-b's own verify_rule gate)
    "r5.drop_last_step",             // rewrite::group_formation: the merged group drops the producer's last step when splicing (M4/R-b's own verify_rule gate)
    "fma.wrong_operand",             // rewrite::fma_contraction: fma(a,b,c) built as fma(a,b,b), dropping the Add's real other operand (M4/R-b's own verify_rule gate)
    "fma.drop_remap",                // rewrite::fma_contraction: kept steps after the fused one are not reindexed after the Mul step is removed (M4/R-b's own verify_rule gate)
    "eg.ad_mode_ignores_cost",       // rewrite::decide_ad_mode: always chooses Reverse regardless of the three priced modes (tests/rewrite/ad_mode_rule_e0_test.cpp)
    "eg.ad_mode_affine_without_check", // rewrite::decide_ad_mode: marks every block ClosedFormAffine-eligible without checking is_linmap_domain (tests/rewrite/ad_mode_rule_e0_test.cpp)
    "eg.block_jacobian_wrong_coefficient_index", // adjoint::block_jacobian (ClosedFormAffine): reads a row's coefficient one member off (tests/rewrite/ad_mode_rule_e0_test.cpp)
    "catalogue.binding_wrong_operand_order",  // catalogue::bind_domain: a step's operand tables are built visiting b before a, silently transposing the operands of every asymmetric two-input catalogued op (div, sub, ...) (M4/C1's own catalogue on/off e0 gates)
    "catalogue.signature_ignores_konst",      // catalogue::signature_of: a step's konst slot contributes no shape (or back-reference) to its Signature, so two domains differing only in konst's kind collide onto one signature and the catalogue silently stops matching one of them (M4/C1's own catalogue on/off e0 gates, which require full coverage of their fixtures' candidate domains)
    "catalogue.signature_ignores_commutativity", // catalogue::canonical_swap_ab: a commutative step's operands are fingerprinted in their recorded order instead of the canonical one, so mul(gat,lit) and mul(lit,gat) -- one isomorphism class to ir::infer, and whichever order the first recorded instance took is compiler-dependent -- become two Signatures and the registry stops matching one of them (D61; M4/C1's own catalogue on/off e0 gates and tests/catalogue/signature_commutative_e0_test.cpp)
    "eg.memo_ignores_site",          // EGraph::saturate's application memo (D62): a (program node, rule) pair is retired after its FIRST matched site, so every later site of a multi-site rule on that node is never applied -- the dedup silently narrowing the search instead of only skipping repeated work (tests/optimise/egraph_saturation_memo_verify_test.cpp)
    "eg.refire_blocks_self_chain",   // EGraph::saturate's RefirePolicy (D62): the re-firing restriction also refuses a rule its OWN output, so a structural rewrite can never chain onto itself -- it would drop the r5.group_formation-applied-twice candidate D54 found (tests/optimise/egraph_saturation_memo_verify_test.cpp)
    "cost.pairing_unpriced",         // optimise::estimate_domain (D63): dispatch is counted per IR STEP instead of per kernel call and the per-step scratch traffic is charged at zero -- the pre-D63 model, in which a fused pair costs exactly what its unfused twin costs, so every fusion candidate ties with its unfused counterpart and extraction's tie-break decides (tests/optimise/cost_step_pairing_verify_test.cpp)
    "plan_bridge.discards_group",    // optimise::plan_from_annotations (D63): `ir::PlanAnnotations::group` (the interpreter's fused pairs and chain tails) is dropped on the way to optimise::Plan, so a rewrite that changes only step pairing is invisible to the price (tests/optimise/cost_step_pairing_verify_test.cpp)
    "oracle.error_in_double",        // verify::error_against_truth (D72): the error against ground truth is formed as `approx - truth.hi` in DOUBLE instead of `Wide(approx) - truth` at the oracle's precision, discarding the oracle's low word -- so a value whose entire error lies below a double ulp is reported as exact, which is PRINCIPLES.md §4's defect reappearing inside the instrument meant to end it (tests/verify/oracle_truth_test.cpp)
    "interpreter.duplicate_output_unwritten",  // exec::Interpreter::Impl::decide_fusion: the ordinals that share an IR value with one the reduction block emits in their place are not filled, so every ordinal but the last of a duplicated output is written NOWHERE and the caller reads back whatever its buffer held -- silent zeros, the defect this list entry exists because of (tests/stage_a/variants_verify_test.cpp -- renamed INTO the gate set: the old variants_test.cpp did not match mutation_test.sh's filename regex and this mutant survived a full run)
    "simplify.cancel_mismatched_operand",  // tape simplify: mul(div(x, c), d) -> x WITHOUT checking that c and d are the same node, so a divide by one value and a multiply by another cancel each other. The guard is the entire rule -- without it the pass silently returns a different function (tests/tape/simplify_differential_test.cpp)
    "simplify.telescope_wrong_endpoint",
    "adjoint.div_aliased_targets",         // adjoint::acc_div_aliased: only the numerator's contribution is accumulated and the denominator's -(ybar/b)*y is dropped, so d(x/x)/dx reads 1/x instead of 0 -- the observable symptom of the restrict-aliasing defect CI found on GCC 13, reproduced arithmetically so it is caught on every compiler (tests/adjoint/div_aliased_verify_test.cpp)   // tape simplify: mul(div(p, q), div(q, s)) -> div(p, q) instead of div(p, s), keeping the INNER endpoint. A telescoped chain then collapses to its first ratio rather than its endpoints, which is right for a one-step chain and wrong for every longer one (tests/tape/simplify_differential_test.cpp)
    "a2p.drop_zero_start",                 // adjoint::adjoint_to_program: the leading +0.0 of an accumulation is elided -- a chain's first Add(Lit 0.0, t) becomes t and its first Sub(Lit 0.0, t) becomes Neg(t), and a pull whose every coefficient is 1.0 is emitted as a Sum instead of an Affine with konst +0.0. Every one of those is exact EXCEPT at a zero, and 0.0 + (-0.0) is +0.0 while -0.0 alone is not: the emitted state adjoint then carries the wrong sign bit on a zero (tests/adjoint/adjoint_to_program_e0_test.cpp)
    "a2p.div_not_aliased",                 // adjoint::adjoint_to_program: an aliased Div (both operand slots the same Step or the same Gather) emits acc_div's two separate accumulations, + ybar/b then - (ybar/b)*y, instead of acc_div_aliased's single (ybar/b)*(1 - y). Both are the right derivative and they round differently once the target already holds a value -- the bitwise side of the defect D84 found (tests/adjoint/adjoint_to_program_e0_test.cpp)
    "adjoint.seed_input_pull",             // Adjoint::run (Options::seed_pull, D102 §4 item 1): the pull of an INPUT domain is seeded with its first contribution like every other pull, instead of keeping its zero start. `Op::Input`'s rule copies the pulled adjoint into `state_bar` (`dst[l] = src[l]`, not `+=`), so that pull is the one whose signed zero is an output of the engine: with the carve-out gone, an input whose only reader is an Affine coefficient of -1 answers `-0.0` where `0.0 + (-0.0)` is `+0.0`. Exact everywhere else and invisible under any tolerance, which is why the gate is a memcmp (tests/adjoint/seed_pull_e0_test.cpp's `affine_neg` program)
    "sparsity.gate_never_fires",           // solver::check_structure: the structural pattern of a declared block is computed and then not read, so a dead Jacobian column or row passes registration. The FALSE NEGATIVE direction, and the one this gate exists to prevent: what it replaces is a calibration that returns a state whose unconstrained entries sit wherever the solve left them, with no diagnostic (D110 §3). The only catcher is the gate's own (forward, flat) test, because no shipped fixture is structurally singular -- and that test is named *_verify_test deliberately: as `structural_gate_test` it did not match mutation_test.sh's gate regex and this mutant SURVIVED a full run while failing that test by hand, the same way `interpreter.duplicate_output_unwritten` survived against `variants_test.cpp`
    "sparsity.closure_forward_order",      // solver::structural_pattern: the reverse reachability sweep visits the nodes ASCENDING. Operands are always earlier nodes, so a mark propagated downwards lands behind the cursor and the transitive closure stops one edge deep. The FALSE POSITIVE direction -- nearly every unknown looks unreachable and the gate fires on correct maths -- so its catchers are every gate that records an implicit block, compare_ois and Stage A included. Deliberately not a variadic-skipping mutant: the Rec operators never emit Sum or Affine (only the passes do, and this gate runs before them), so a variadic mutant would be unreachable here
    "a2p.scan_forward_order",              // adjoint::adjoint_to_program (C3b): the reverse of a scan is emitted as a recurrent domain whose rows run BACKWARDS, so that the edge slot (carry, r+1) the pull of row r reads is literally the previous emitted row. This mutant publishes those rows to the rest of the emitted program in FORWARD order, so every reader outside the recurrence -- the vbar pull, and the pull of the domain holding the chains' initial values -- takes row r's carried adjoint from the row the forward scan would have had. The emitter's form of adjoint.scan_forward_order, and the only form of it a Program can express: laying the domain out forwards would make the self-read a FORWARD read, which ir::validate rejects (tests/adjoint/adjoint_to_program_e0_test.cpp, the affine_scan and compare_ois default-tenor gates)
};
inline constexpr std::size_t registry_size = sizeof(registry) / sizeof(registry[0]);

constexpr bool registered(std::string_view name) noexcept {
  for (std::string_view r : registry) {
    if (r == name) return true;
  }
  return false;
}

#if defined(EPYKOS_MUTATIONS) && EPYKOS_MUTATIONS
inline constexpr bool compiled_in = true;

// The selected mutant: the value of EPYKOS_MUTANT, read once; "" when the variable is unset or
// empty. Throws std::invalid_argument (on this and every later call) when the value is not a
// registered name, so a typo in the environment is loud rather than a run with no mutant.
std::string_view active();

// active() == name. Throws std::logic_error when `name` is not registered (a use-site typo would
// otherwise be a mutant that can never be selected).
bool query(std::string_view name);
#else
inline constexpr bool compiled_in = false;

constexpr std::string_view active() noexcept { return {}; }
#endif

}  // namespace epykos::mutation

namespace epykos {

// Is mutant `name` selected? The passes write `if (mutant("pass.defect")) ...` around the
// defective line. Mutation build: a string comparison against the selection read once from the
// environment (query it once per pass call, never per element). Every other build: constexpr false.
#if defined(EPYKOS_MUTATIONS) && EPYKOS_MUTATIONS
inline bool mutant(std::string_view name) { return mutation::query(name); }
#else
constexpr bool mutant(std::string_view) noexcept { return false; }
#endif

}  // namespace epykos
