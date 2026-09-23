// EpykosEngine — the mutation-testing selector (DESIGN.md §11 "mutation testing", D29; M2/Q4).
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
// Adding a mutant: add the name here and to the registry test's expected list, then the guarded
// line in the pass. A name is "<pass>.<defect>", lower case, dot-separated.
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
    "expander.drop_gather",          // expand: gather 0 reads value id r (the identity index) instead of index[r]
    "expander.segment_off_by_one",   // expand: every segment loses its last member
    "signature.merge_classes",       // infer: the const-slot pattern is not part of the signature (a constant slot is a reference)
    "interpreter.tile_boundary",     // Interpreter::run: the last row of every elementwise tile is skipped
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
