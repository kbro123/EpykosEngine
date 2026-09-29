// EpykosEngine — tools/catalogue's own code generation (not engine API: this header lives under
// tools/, never under include/epykos/). Turns a epykos::catalogue::Signature into the C++ source
// of one kernel function matching include/epykos/catalogue/kernel.hpp's ABI, and a set of such
// kernels into the two files src/catalogue/generated/{kernels_e0.cpp,registry.cpp} carry.
#pragma once

#include <string>
#include <vector>

#include "epykos/catalogue/signature.hpp"

namespace epykos::catalogue::tool {

// One kernel the generator has decided to emit: its Signature, the workload(s) it was found in
// (for the source comment only) and the readable name (Signature::to_string()).
struct CatalogueEntry {
  Signature signature;
  std::string found_in;  // e.g. "stage_a, m1_book" — informational only, never read back
};

// A stable C++ identifier for `sig`: "kernel_" + 16 hex digits of `sig.hash()`. Deterministic
// across runs (same signature, same name), so regenerating from the same workloads reproduces
// the same source text (scripts/catalogue_regen.sh's `git diff --exit-code`).
std::string kernel_function_name(const Signature& sig);

// The full definition of `function_name` in `namespace epykos::catalogue::generated`,
// implementing `sig`'s op sequence per kernel.hpp's ABI: given (values, value_base, r0, n, L,
// literals, columns, gathers, seg_offsets, seg_members, seg_coefs), writes row r0..r0+n-1's
// final step value to values[(value_base+row)*L+l] for every lane l, in the same op order, the
// same operand rule per op (ir/evaluate.hpp) and the same left fold for Sum/Affine (Sum from the
// first member; Affine from konst) as the reference evaluator / tape replay / every other
// non-catalogued path. Throws std::invalid_argument if `sig` is structurally malformed (an
// Op::Input step, a Segment slot outside a lone terminal Sum/Affine step, an operand shape that
// does not match its op's arity) — a generator bug, never a condition a caller works around.
std::string generate_kernel(const Signature& sig, const std::string& function_name);

// src/catalogue/generated/kernels_e0.cpp's full text for `entries` (sorted by hash for a stable
// diff), one generate_kernel() per entry.
std::string generate_kernels_file(const std::vector<CatalogueEntry>& entries);

// src/catalogue/generated/registry.cpp's full text: the RegistryEntry table naming every kernel
// generate_kernels_file emitted, in the same (hash-sorted) order.
std::string generate_registry_file(const std::vector<CatalogueEntry>& entries);

}  // namespace epykos::catalogue::tool
