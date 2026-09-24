#include "epykos/catalogue/signature.hpp"

#include <cstddef>
#include <sstream>

#include "epykos/mutation/mutation.hpp"

namespace epykos::catalogue {

const char* to_string(SlotShape s) noexcept {
  switch (s) {
    case SlotShape::None: return "-";
    case SlotShape::Step: return "step";
    case SlotShape::Literal: return "lit";
    case SlotShape::Column: return "col";
    case SlotShape::Gather: return "gat";
    case SlotShape::Segment: return "seg";
  }
  return "?";
}

namespace {

SlotShape shape_of(const ir::Slot& s) noexcept {
  switch (s.kind) {
    case ir::SlotKind::None: return SlotShape::None;
    case ir::SlotKind::Step: return SlotShape::Step;
    case ir::SlotKind::Literal: return SlotShape::Literal;
    case ir::SlotKind::Column: return SlotShape::Column;
    case ir::SlotKind::Gather: return SlotShape::Gather;
    case ir::SlotKind::Segment: return SlotShape::Segment;
    case ir::SlotKind::Input: return SlotShape::None;  // never reached: is_cataloguable excludes Input steps
  }
  return SlotShape::None;
}

// How many steps back a Step-kind slot points, from step `k`; 0 (unused) for every other shape.
std::int32_t back_of(const ir::Slot& s, std::size_t k) noexcept {
  if (s.kind != ir::SlotKind::Step) return 0;
  return static_cast<std::int32_t>(k) - s.index;
}

}  // namespace

bool is_cataloguable(const ir::Program& program, ir::domain_id d) noexcept {
  if (d < 0 || static_cast<std::size_t>(d) >= program.groups.size()) return false;
  const ir::Group& g = program.groups[static_cast<std::size_t>(d)];
  if (g.steps.empty()) return false;
  for (const ir::Step& st : g.steps) {
    if (st.op == Op::Input) return false;
    if (!op_is_supported(st.op)) return false;
  }
  return true;
}

Signature signature_of(const ir::Program& program, ir::domain_id d) {
  Signature sig;
  const ir::Group& g = program.groups[static_cast<std::size_t>(d)];
  sig.steps.reserve(g.steps.size());
  // Mutant catalogue.signature_ignores_konst: konst contributes no shape or back-reference, so
  // an Affine step with a Literal konst and one with a Column konst (or any other konst-kind
  // difference) collide onto the same Signature -- the registry (generated from the OTHER
  // shape) then either fails to match one of them (coverage drops below the catalogue's own e0
  // gates' 100% expectation) or, worse, matches a kernel built for the wrong konst kind. Read
  // once per call, not once per field, so this is one guarded line, not two.
  const bool ignore_konst = epykos::mutant("catalogue.signature_ignores_konst");
  for (std::size_t k = 0; k < g.steps.size(); ++k) {
    const ir::Step& st = g.steps[k];
    StepShape ss;
    ss.op = st.op;
    ss.a = shape_of(st.a);
    ss.b = shape_of(st.b);
    ss.c = shape_of(st.c);
    ss.konst = ignore_konst ? SlotShape::None : shape_of(st.konst);
    ss.a_back = back_of(st.a, k);
    ss.b_back = back_of(st.b, k);
    ss.c_back = back_of(st.c, k);
    ss.konst_back = ignore_konst ? 0 : back_of(st.konst, k);
    sig.steps.push_back(ss);
  }
  return sig;
}

std::uint64_t Signature::hash() const noexcept {
  // FNV-1a over the raw StepShape bytes. Deterministic across processes and platforms (no
  // pointer, no address, no floating point ever enters this fingerprint), which is what lets the
  // generated registry table be produced once and checked in verbatim (scripts/catalogue_regen.sh).
  std::uint64_t h = 1469598103934665603ull;  // FNV offset basis
  constexpr std::uint64_t prime = 1099511628211ull;
  auto mix = [&](std::uint8_t byte) {
    h ^= byte;
    h *= prime;
  };
  mix(0xC1);  // a fixed prefix byte: keeps this hash namespace-distinct from an incidental
              // coincidence with some other FNV-1a use elsewhere in the codebase
  auto mix_back = [&](std::int32_t back) {
    // Small deltas in practice (a step's operand rarely looks back more than a handful of
    // steps); folding all 4 bytes keeps the hash well-defined for any value regardless.
    const auto u = static_cast<std::uint32_t>(back);
    mix(static_cast<std::uint8_t>(u));
    mix(static_cast<std::uint8_t>(u >> 8));
    mix(static_cast<std::uint8_t>(u >> 16));
    mix(static_cast<std::uint8_t>(u >> 24));
  };
  for (const StepShape& s : steps) {
    mix(static_cast<std::uint8_t>(s.op));
    mix(static_cast<std::uint8_t>(s.a));
    mix(static_cast<std::uint8_t>(s.b));
    mix(static_cast<std::uint8_t>(s.c));
    mix(static_cast<std::uint8_t>(s.konst));
    mix_back(s.a_back);
    mix_back(s.b_back);
    mix_back(s.c_back);
    mix_back(s.konst_back);
  }
  return h;
}

std::string Signature::to_string() const {
  std::ostringstream os;
  auto put = [&](SlotShape sh, std::int32_t back) {
    os << catalogue::to_string(sh);
    if (sh == SlotShape::Step) os << '-' << back;
  };
  for (std::size_t k = 0; k < steps.size(); ++k) {
    if (k) os << ';';
    const StepShape& s = steps[k];
    os << epykos::to_string(s.op) << '(';
    put(s.a, s.a_back);
    if (s.b != SlotShape::None) {
      os << ',';
      put(s.b, s.b_back);
    }
    if (s.c != SlotShape::None) {
      os << ',';
      put(s.c, s.c_back);
    }
    if (s.konst != SlotShape::None) {
      os << ";k=";
      put(s.konst, s.konst_back);
    }
    os << ')';
  }
  return os.str();
}

}  // namespace epykos::catalogue
