// EpykosEngine — the catalogue's kernel ABI and per-domain operand binding (M4/C1).
//
// A catalogued kernel evaluates domain `d`'s rows [r0, r0 + n) at lane width L, writing each
// row's final step value into the SAME shared value buffer both engines already use
// (exec::Interpreter's `values`, D15; adjoint::Adjoint's `Ctx::values`, adjoint/plan.hpp):
// batch innermost, value v lane l at values[v·L + l]. A Gather operand reads `values` directly
// (never a separate "already computed" side table), so a self-referencing gather — a scan's
// carry (ir::Program::scans, D41) — sees whatever row this same domain's own evaluation already
// wrote, exactly as the generic per-step evaluators do; this holds for a kernel call so long as
// its caller invokes rows in ascending order, which every kernel this package emits does
// internally by construction, and which both engines' own call sites already guarantee (D15;
// adjoint/plan.hpp point on materialisation order).
//
// One kernel per Signature (signature.hpp); a domain dispatches to it once its own binding — the
// specific Program-table pointers its op sequence's Literal / Column / Gather / Segment slots
// resolve to, in this domain's own instance — is built. `bind_domain` builds that binding by
// walking the domain's steps in the same order `signature_of` did, appending ONE array entry per
// OCCURRENCE of a Literal / Column / Gather slot (never deduplicated by which Program-global
// table entry two occurrences happen to share — a Signature cannot see that, so a generated
// kernel and bind_domain must not either, or two same-signature domains could hand one kernel
// bindings of different lengths; see kernel.cpp). A generated kernel (tools/catalogue/codegen.cpp)
// reads "the k-th Literal slot in step order" and bind_domain builds "the k-th Literal slot in
// step order" — the two agree by construction, without either naming the other's Program-global
// table indices.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "epykos/ir/program.hpp"

namespace epykos::catalogue {

using Kernel = void (*)(double* values, ir::value_id value_base, int r0, int n, int L,
                        const double* const* literals, const double* const* columns,
                        const std::int32_t* const* gathers, const std::int32_t* seg_offsets,
                        const ir::value_id* seg_members, const double* seg_coefs);

// The operand tables one domain's catalogued kernel reads, in the first-use order
// `signature_of` and every generated kernel agree on. Non-owning: every pointer points into
// `program`'s own storage (Program::literals / columns / gathers / segments), never a copy — the
// same contract exec::Interpreter::Impl::resolve() and adjoint's Ctx already keep. `program`
// (and, since a gather may name it, every domain of `program`'s own value buffer) must outlive
// the binding.
struct DomainBinding {
  std::vector<const double*> literals;
  std::vector<const double*> columns;
  std::vector<const std::int32_t*> gathers;
  const std::int32_t* seg_offsets = nullptr;   // the domain's own segment table, or null
  const ir::value_id* seg_members = nullptr;
  const double* seg_coefs = nullptr;           // null for a Sum segment (no coefficients)

  void call(Kernel k, double* values, ir::value_id value_base, int r0, int n, int L) const {
    k(values, value_base, r0, n, L, literals.data(), columns.data(), gathers.data(), seg_offsets, seg_members,
      seg_coefs);
  }
};

// Builds the binding for domain `d` of `program`. Precondition: is_cataloguable(program, d).
DomainBinding bind_domain(const ir::Program& program, ir::domain_id d);

}  // namespace epykos::catalogue
