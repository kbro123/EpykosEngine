#include "epykos/rewrite/fma_contraction.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#include "epykos/mutation/mutation.hpp"

namespace epykos::rewrite {

namespace {

using ir::Group;
using ir::Program;
using ir::Slot;
using ir::SlotKind;
using ir::Step;

bool references_step(const Slot& s, std::int32_t idx) noexcept { return s.kind == SlotKind::Step && s.index == idx; }

bool step_references(const Step& st, std::int32_t idx) noexcept {
  return references_step(st.a, idx) || references_step(st.b, idx) || references_step(st.c, idx) ||
         references_step(st.konst, idx);
}

// The single step (if any) of `steps` other than `mul_idx` itself that reads Slot{Step,mul_idx}.
// Returns -1 if none does, or if more than one does (the fan-out-1 requirement: a Mul read by two
// or more later steps is not a candidate -- fusing would duplicate the product's rounding, not
// remove one).
std::int32_t sole_reader(const std::vector<Step>& steps, std::int32_t mul_idx) {
  std::int32_t reader = -1;
  for (std::size_t k = 0; k < steps.size(); ++k) {
    if (static_cast<std::int32_t>(k) == mul_idx) continue;
    if (step_references(steps[k], mul_idx)) {
      if (reader != -1) return -1;  // more than one reader: not fusable
      reader = static_cast<std::int32_t>(k);
    }
  }
  return reader;
}

// Is `steps[sum_idx]` a FIXED-ARITY, exactly-2-member Sum (fold_sum's own "a*x + b" shape,
// DESIGN.md §5.6: the scan carry path keeps a short "+" as a fixed-arity Sum step over its own
// group's `a`/`b` slots rather than a Segment of global value ids -- the segment-based Sum an
// elementwise domain's reduction epilogue uses is NOT this shape: its members are value ids
// materialised in other domains, not local steps this rewrite could remove) with the Mul at
// `mul_idx` as one of its two members? Returns the OTHER member slot (the addend `c` of the fma)
// if so, else nullopt. (fold_sum turns every recorded `Add` into a `Sum`, including 2-term ones,
// DESIGN.md §5.2 "By default every Add becomes a Sum" -- so `Op::Add` itself does not survive
// into the domain IR and is not this rewrite's target.)
std::optional<Slot> sum_other_operand(const std::vector<Step>& steps, std::int32_t sum_idx, std::int32_t mul_idx) {
  const Step& sum = steps[static_cast<std::size_t>(sum_idx)];
  if (sum.op != epykos::Op::Sum) return std::nullopt;
  if (!ir::is_fixed_sum(sum) || ir::fixed_sum_arity(sum) != 2) return std::nullopt;
  if (references_step(sum.a, mul_idx)) return sum.b;
  if (references_step(sum.b, mul_idx)) return sum.a;
  return std::nullopt;
}

struct Candidate {
  ir::domain_id domain = -1;
  std::int32_t mul_step = -1;
};

// Every (domain, mul-step) pair whose product feeds exactly one fixed-arity 2-member Sum in the
// same group. Recomputed fresh each call (rule.hpp point 6: no cross-call caching).
std::vector<Candidate> find_candidates(const Program& program) {
  std::vector<Candidate> out;
  for (std::size_t d = 0; d < program.groups.size(); ++d) {
    const std::vector<Step>& steps = program.groups[d].steps;
    for (std::size_t i = 0; i < steps.size(); ++i) {
      if (steps[i].op != epykos::Op::Mul) continue;
      const std::int32_t mul_idx = static_cast<std::int32_t>(i);
      const std::int32_t reader = sole_reader(steps, mul_idx);
      if (reader < 0) continue;
      if (!sum_other_operand(steps, reader, mul_idx).has_value()) continue;
      out.push_back(Candidate{static_cast<ir::domain_id>(d), mul_idx});
    }
  }
  return out;
}

// Rebuilds domain `d`'s group, fusing the Mul at `mul_idx` into its sole fixed-arity-Sum reader.
// Every other domain, and every gather/segment/literal/column table, is untouched (this rewrite
// never crosses a domain boundary).
Program fuse_one(const Program& program, ir::domain_id d, std::int32_t mul_idx) {
  Program out = program;
  std::vector<Step>& steps = out.groups[static_cast<std::size_t>(d)].steps;
  const std::int32_t add_idx = sole_reader(steps, mul_idx);
  const Step mul = steps[static_cast<std::size_t>(mul_idx)];
  const std::optional<Slot> c = sum_other_operand(steps, add_idx, mul_idx);

  // Index map: every kept step's new position (mul_idx is dropped, so everything after it shifts
  // down by one); mul_idx itself maps nowhere (nothing may reference it after this rewrite: the
  // fan-out-1 check made `add_idx` its only reader, and `add_idx`'s new content below no longer
  // needs a Step-slot for it at all).
  std::vector<std::int32_t> new_index(steps.size(), -1);
  std::int32_t next = 0;
  for (std::int32_t k = 0; k < static_cast<std::int32_t>(steps.size()); ++k) {
    if (k == mul_idx) continue;
    new_index[static_cast<std::size_t>(k)] = next++;
  }

  auto remap = [&](Slot s) -> Slot {
    if (s.kind == SlotKind::Step) s.index = new_index[static_cast<std::size_t>(s.index)];
    return s;
  };

  std::vector<Step> rebuilt;
  rebuilt.reserve(steps.size() - 1);
  for (std::int32_t k = 0; k < static_cast<std::int32_t>(steps.size()); ++k) {
    if (k == mul_idx) continue;
    if (k == add_idx) {
      // fma(a, b, c) = a*b + c, one rounding (DESIGN.md §10) -- the mutant below swaps `c` for the
      // Mul's own `b`, i.e. produces fma(a, b, b), a real wrong VALUE whenever c != b.
      Step fused;
      fused.op = epykos::Op::Fma;
      fused.a = remap(mul.a);
      fused.b = remap(mul.b);
      fused.c = epykos::mutant("fma.wrong_operand") ? remap(mul.b) : remap(*c);
      rebuilt.push_back(fused);
      continue;
    }
    Step st = steps[static_cast<std::size_t>(k)];
    // `fma.drop_remap`: the mutant skips renumbering this kept step's own Step-kind slots,
    // so any reference to a step positioned after the removed Mul keeps its STALE (pre-removal)
    // index -- an out-of-range or wrong-step read once the group has any step after `mul_idx`
    // other than the fused Add itself.
    if (!epykos::mutant("fma.drop_remap")) {
      st.a = remap(st.a);
      st.b = remap(st.b);
      st.c = remap(st.c);
      st.konst = remap(st.konst);
    }
    rebuilt.push_back(st);
  }
  steps = std::move(rebuilt);
  return out;
}

}  // namespace

const std::string& FmaContractionRule::name() const noexcept {
  static const std::string n = "fma.contraction";
  return n;
}

std::vector<MatchSite> FmaContractionRule::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  std::vector<MatchSite> sites;
  for (const Candidate& c : find_candidates(program)) sites.push_back(MatchSite::of_step(c.domain, c.mul_step));
  return sites;
}

Proposal FmaContractionRule::propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite& site) const {
  Proposal p;
  p.program = fuse_one(program, site.domain, site.step);
  return p;
}

}  // namespace epykos::rewrite
