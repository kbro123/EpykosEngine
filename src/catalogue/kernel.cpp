#include "epykos/catalogue/kernel.hpp"

#include <cstddef>

#include "epykos/catalogue/signature.hpp"
#include "epykos/mutation/mutation.hpp"

namespace epykos::catalogue {

// One binding array slot PER OCCURRENCE of a Literal / Column / Gather operand in step order (a,
// b, c, konst per step) — deliberately NOT deduplicated by Program-global table index. A
// Signature (signature.hpp) records only which KIND each slot is, never which global entry two
// occurrences happen to share, so it cannot tell "domain A's two Literal slots are the same
// constant twice" from "domain B's two Literal slots are two different constants" — both are the
// same Signature. A generated kernel is therefore compiled to expect exactly one array entry per
// Literal/Column/Gather-shaped slot its Signature has, in step order, full stop; deduplicating
// here would make the binding's array LENGTH depend on data the Signature does not capture, so
// two same-signature domains could hand the one kernel bindings of different sizes — silently
// reading past the end of the shorter one. Duplicate pointers in the arrays below (when a domain
// really does reuse one literal/column/gather twice) cost a few redundant words of setup, never
// correctness: `tools/catalogue/codegen.cpp` walks a Signature with this exact same one-slot-one-
// array-entry rule, so the two always agree.
DomainBinding bind_domain(const ir::Program& program, ir::domain_id d) {
  DomainBinding binding;
  const ir::Group& g = program.groups[static_cast<std::size_t>(d)];

  auto visit = [&](const ir::Slot& s) {
    switch (s.kind) {
      case ir::SlotKind::Literal:
        binding.literals.push_back(&program.literals[static_cast<std::size_t>(s.index)]);
        break;
      case ir::SlotKind::Column:
        binding.columns.push_back(program.columns[static_cast<std::size_t>(s.index)].values.data());
        break;
      case ir::SlotKind::Gather:
        binding.gathers.push_back(program.gathers[static_cast<std::size_t>(s.index)].index.data());
        break;
      case ir::SlotKind::Segment: {
        const ir::Segment& seg = program.segments[static_cast<std::size_t>(s.index)];
        binding.seg_offsets = seg.offsets.data();
        binding.seg_members = seg.members.data();
        binding.seg_coefs = seg.coefs.empty() ? nullptr : seg.coefs.data();
        break;
      }
      default:
        break;
    }
  };

  for (std::size_t k = 0; k < g.steps.size(); ++k) {
    const ir::Step& st = g.steps[k];
    // A commutative step's operands are bound in the SAME canonical order signature_of put them
    // in (D61, signature.hpp's file header): the generated kernel reads "the k-th Gather slot in
    // canonical step order", so this walk must produce exactly that sequence.
    const bool swap = canonical_swap_ab(st, k);
    const ir::Slot& first = swap ? st.b : st.a;
    const ir::Slot& second = swap ? st.a : st.b;
    // Mutant catalogue.binding_wrong_operand_order: visits the second operand before the first,
    // transposing the operand tables of every step whose a and b are both Gather (or both
    // Column, or both Literal) — an asymmetric op (div, sub, ...) then silently computes with
    // its operands swapped. (A commutative step is unaffected by construction: swapping the
    // operands of `+` or `*` is the same IEEE-754 result, which is exactly why canonicalising
    // them above is E0-safe.)
    if (epykos::mutant("catalogue.binding_wrong_operand_order")) {
      visit(second);
      visit(first);
    } else {
      visit(first);
      visit(second);
    }
    visit(st.c);
    visit(st.konst);
  }
  return binding;
}

}  // namespace epykos::catalogue
