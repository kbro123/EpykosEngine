#include "epykos/rewrite/r1_fold_uniform_columns.hpp"

#include <cstdint>
#include <cstring>

#include "epykos/mutation/mutation.hpp"
#include "epykos/rewrite/bucket_split_edit.hpp"

namespace epykos::rewrite {

namespace {

std::size_t idx(std::int32_t i) noexcept { return static_cast<std::size_t>(i); }

std::uint64_t bits_of(double v) noexcept {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// R1's own uniformity check: bit-identical across every row, per column. Mutant r1.ignores_last_row
// (a defect no gate written FOR this mutant needs to know about: it is caught by the ordinary
// differential gates, exactly like every other mutant in the registry) stops one row early, so a
// column that differs ONLY in its last row is wrongly folded.
bool column_is_uniform(const std::vector<double>& values) {
  if (values.empty()) return true;
  const std::uint64_t first = bits_of(values.front());
  std::size_t n = values.size();
  if (epykos::mutant("r1.ignores_last_row") && n > 1) --n;
  for (std::size_t i = 1; i < n; ++i) {
    if (bits_of(values[i]) != first) return false;
  }
  return true;
}

// The domain's own columns that are uniform AND read by one of its steps.
std::vector<std::int32_t> foldable_columns(const ir::Program& program, ir::domain_id d) {
  std::vector<std::int32_t> out;
  for (std::int32_t c = 0; c < static_cast<std::int32_t>(program.columns.size()); ++c) {
    const ir::Column& col = program.columns[idx(c)];
    if (col.domain != d) continue;
    if (!column_is_uniform(col.values)) continue;
    bool referenced = false;
    for (const ir::Step& s : program.groups[idx(d)].steps) {
      for (const ir::Slot* sl : {&s.a, &s.b, &s.c, &s.konst}) {
        if (sl->kind == ir::SlotKind::Column && sl->index == c) referenced = true;
      }
    }
    if (referenced) out.push_back(c);
  }
  return out;
}

}  // namespace

const std::string& R1FoldUniformColumns::name() const noexcept {
  static const std::string n = "r1.fold_uniform_columns";
  return n;
}

std::vector<MatchSite> R1FoldUniformColumns::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  std::vector<MatchSite> sites;
  for (std::int32_t d = 0; d < static_cast<std::int32_t>(program.domains.size()); ++d) {
    if (!foldable_columns(program, d).empty()) sites.push_back(MatchSite::of_domain(d));
  }
  return sites;
}

Proposal R1FoldUniformColumns::propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite& site) const {
  const ir::domain_id d = site.domain;
  const std::vector<std::int32_t> columns = foldable_columns(program, d);

  ir::Program out = program;
  for (std::int32_t c : columns) {
    const std::int32_t lit = static_cast<std::int32_t>(out.literals.size());
    out.literals.push_back(out.columns[idx(c)].values.front());
    for (ir::Step& s : out.groups[idx(d)].steps) {
      ir::Slot* slots[4] = {&s.a, &s.b, &s.c, &s.konst};
      for (ir::Slot* sl : slots) {
        if (sl->kind != ir::SlotKind::Column || sl->index != c) continue;
        // Mutant r1.wrong_slot: always overwrites `a`, even when the uniform column was read from
        // b / c / konst -- corrupting whichever operand actually held it while leaving the real
        // slot as a dangling Column reference (or, if `a` held something else, replacing that
        // operand's value with the literal instead).
        ir::Slot* target = epykos::mutant("r1.wrong_slot") ? &s.a : sl;
        *target = ir::Slot{ir::SlotKind::Literal, lit};
      }
    }
  }
  Proposal p;
  p.program = std::move(out);
  return p;
}

}  // namespace epykos::rewrite
