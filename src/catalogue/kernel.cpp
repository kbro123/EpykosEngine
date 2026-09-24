#include "epykos/catalogue/kernel.hpp"

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

  for (const ir::Step& st : g.steps) {
    // Mutant catalogue.binding_wrong_operand_order: visits b before a, transposing the operand
    // tables of every step whose a and b are both Gather (or both Column, or both Literal) — an
    // asymmetric op (div, sub, ...) then silently computes with its operands swapped.
    if (epykos::mutant("catalogue.binding_wrong_operand_order")) {
      visit(st.b);
      visit(st.a);
    } else {
      visit(st.a);
      visit(st.b);
    }
    visit(st.c);
    visit(st.konst);
  }
  return binding;
}

}  // namespace epykos::catalogue
