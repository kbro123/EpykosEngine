#include "epykos/rewrite/r3_elide_trivial_maps.hpp"

#include "epykos/mutation/mutation.hpp"
#include "epykos/rewrite/ir_edit.hpp"

namespace epykos::rewrite {

namespace {

std::size_t idx(std::int32_t i) noexcept { return static_cast<std::size_t>(i); }

// The segment index of a qualifying "trivial map" domain, or -1: group.steps.size() == 1, that one
// step is the VARIADIC form of Sum (a.kind == Segment, never the fixed-arity scan form), and every
// row of the segment it reads has exactly one member.
std::int32_t trivial_segment(const ir::Program& program, ir::domain_id d) {
  const ir::Domain& dom = program.domains[idx(d)];
  if (dom.recurrent || dom.scan_class || dom.scan >= 0) return -1;
  const ir::Group& g = program.groups[idx(d)];
  if (g.steps.size() != 1) return -1;
  const ir::Step& s = g.steps.front();
  if (s.op != epykos::Op::Sum || s.a.kind != ir::SlotKind::Segment) return -1;
  const ir::Segment& seg = program.segments[idx(s.a.index)];
  for (std::int32_t r = 0; r < dom.rows; ++r) {
    std::int32_t width = seg.offsets[idx(r) + 1] - seg.offsets[idx(r)];
    // Mutant r3.treats_length_two_as_trivial: a two-member row is wrongly accepted as trivial too
    // (the replacement below then reads only the FIRST of the two members, silently dropping the
    // second additive term of a genuine Sum).
    const std::int32_t max_width = epykos::mutant("r3.treats_length_two_as_trivial") ? 2 : 1;
    if (width < 1 || width > max_width) return -1;
  }
  return s.a.index;
}

}  // namespace

const std::string& R3ElideTrivialMaps::name() const noexcept {
  static const std::string n = "r3.elide_trivial_maps";
  return n;
}

std::vector<MatchSite> R3ElideTrivialMaps::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  std::vector<MatchSite> sites;
  for (std::int32_t d = 0; d < static_cast<std::int32_t>(program.domains.size()); ++d) {
    if (trivial_segment(program, d) >= 0) sites.push_back(MatchSite::of_domain(d));
  }
  return sites;
}

Proposal R3ElideTrivialMaps::propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite& site) const {
  const ir::domain_id d = site.domain;
  const std::int32_t seg_idx = trivial_segment(program, d);
  const ir::Segment& seg = program.segments[idx(seg_idx)];
  const ir::Domain& dom = program.domains[idx(d)];

  std::vector<ir::value_id> replacement(idx(dom.rows));
  for (std::int32_t r = 0; r < dom.rows; ++r) {
    // Mutant r3.off_by_one_member: reads the member ONE PAST the row's sole member (the next row's,
    // or one past the end of `members` for the domain's last row) instead of its own.
    const std::int32_t off = seg.offsets[idx(r)] + (epykos::mutant("r3.off_by_one_member") ? 1 : 0);
    replacement[idx(r)] = seg.members[idx(off)];
  }

  Proposal p;
  p.program = detail::eliminate_domain(program, d, replacement);
  return p;
}

}  // namespace epykos::rewrite
