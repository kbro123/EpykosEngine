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
    "adjoint.wrong_transpose",       // build_plan: gather 0's pull reads the slot of row index[r] (the forward index array) instead of row r
    "adjoint.drop_broadcast",        // build_plan: the last member of every Sum row gets no reader entry (the broadcast skips it)
    "adjoint.affine_not_transposed", // build_plan: Affine reader coefficients read from the forward table at the transposed position (W, not W^T)
    "adjoint.select_wrong_arm",      // Adjoint::run: the select rule routes the adjoint to the other arm (no select on the M1 book: the near-miss gate)
    "adjoint.recip_rule_sign",       // Adjoint::run: recip's rule accumulates +(ybar*y)*y instead of -(ybar*y)*y (no recip on the M1 book: the near-miss gate)
    "implicit.ift_not_transposed",   // Factors::solve_transposed: the IFT multiplier solves F_z lambda = z_bar instead of F_z^T lambda = z_bar
    "implicit.ift_drop_fp",          // Factors::ift_adjoint: the parameter pull p_bar -= F_p^T lambda is skipped (the quotes receive no adjoint)
    "implicit.stale_jacobian",       // BlockSolver::solve: the IFT uses the Jacobian of the last iterate before convergence, not the solution's
    "expander.scan_carry_from_init", // expand: every step of a scan chain reads the chain's initial value instead of the previous step (no scan on the M1 book: the scan fixtures' round-trip gates)
    "interpreter.scan_drop_last_wave", // Interpreter::run: the last wave of every scan (the last step of its longest chains) is not evaluated (the scan fixtures' E0 gates)
    "adjoint.scan_forward_order",    // Adjoint::run: the reverse scan visits the rows forwards, so the carried adjoint arrives after it was pulled (the scan fixtures' adjoint gates)
    "r1.ignores_last_row",           // rewrite::R1FoldUniformColumns: the uniformity check stops one row early, so a column differing only in its last row is wrongly folded
    "r1.wrong_slot",                 // rewrite::R1FoldUniformColumns: the literal is always written into operand `a`, even when the uniform column was read from b / c / konst
    "r2.wrong_run_boundary",         // rewrite::R2BucketRows: a run boundary compares row r to r-2 instead of r-1, mis-sizing the buckets
    "r2.column_slice_uses_wrong_bucket", // rewrite::R2BucketRows: bucket k (k>0) is built from bucket (k-1)'s row range instead of its own
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
