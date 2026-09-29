#include "epykos/rewrite/r4b_shared_reciprocal.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "epykos/mutation/mutation.hpp"
#include "epykos/rewrite/ir_edit.hpp"

namespace epykos::rewrite {

namespace {

using ir::domain_id;
using ir::Gather;
using ir::Program;
using ir::Slot;
using ir::SlotKind;
using ir::Step;
using ir::value_id;

struct Candidate {
  domain_id domain = -1;
  std::int32_t step = -1;
  domain_id source = -1;
  bool self_redundant = false;  // this site's OWN gather reuses at least one S row
};

domain_id homogeneous_source(const Program& program, const Gather& g) {
  if (g.index.empty()) return -1;
  const domain_id first = program.domain_of(g.index.front());
  for (value_id v : g.index) {
    if (program.domain_of(v) != first) return -1;
  }
  return first;
}

bool has_redundancy(const Gather& g) {
  std::set<value_id> distinct(g.index.begin(), g.index.end());
  return distinct.size() < g.index.size();
}

std::optional<Candidate> as_candidate(const Program& program, domain_id d, std::int32_t i) {
  const Step& st = program.groups[static_cast<std::size_t>(d)].steps[static_cast<std::size_t>(i)];
  if (st.op != epykos::Op::Div || st.b.kind != SlotKind::Gather) return std::nullopt;
  const Gather& g = program.gathers[static_cast<std::size_t>(st.b.index)];
  const domain_id source = homogeneous_source(program, g);
  if (source < 0) return std::nullopt;
  return Candidate{d, i, source, has_redundancy(g)};
}

std::vector<Candidate> find_all_candidates(const Program& program) {
  std::vector<Candidate> out;
  for (std::size_t d = 0; d < program.groups.size(); ++d) {
    if (program.domains[d].recurrent) continue;
    const std::vector<Step>& steps = program.groups[d].steps;
    for (std::size_t i = 0; i < steps.size(); ++i) {
      if (std::optional<Candidate> c = as_candidate(program, static_cast<domain_id>(d), static_cast<std::int32_t>(i))) {
        out.push_back(*c);
      }
    }
  }
  return out;
}

// Candidates for one source `S`, filtered to the profitable ones (file header: self-redundant, or
// shared by more than one site).
std::vector<Candidate> profitable_candidates_for(const Program& program, domain_id source) {
  std::vector<Candidate> all;
  for (const Candidate& c : find_all_candidates(program)) {
    if (c.source == source) all.push_back(c);
  }
  if (all.size() > 1) return all;  // shared: every one of them benefits
  if (all.size() == 1 && all.front().self_redundant) return all;
  return {};
}

Program relocate(const Program& program, domain_id source) {
  const std::vector<Candidate> sites = profitable_candidates_for(program, source);

  const ir::Domain& s_dom = program.domains[static_cast<std::size_t>(source)];
  ir::Domain s_recip_domain;
  s_recip_domain.name = "recip(@0)";
  s_recip_domain.rows = s_dom.rows;
  ir::Gather identity{detail::new_domain_marker(), {}};
  identity.index.reserve(static_cast<std::size_t>(s_dom.rows));
  for (std::int32_t r = 0; r < s_dom.rows; ++r) identity.index.push_back(ir::value_of(program, source, r));
  const std::int32_t identity_gather_id = static_cast<std::int32_t>(program.gathers.size());
  ir::Group s_recip_group;
  s_recip_group.steps = {Step{epykos::Op::Recip, Slot{SlotKind::Gather, identity_gather_id}, {}, {}, {}}};

  detail::InsertResult inserted = detail::insert_domain_after(program, source, s_recip_domain, s_recip_group, {identity}, {});
  Program out = std::move(inserted.program);
  const value_id s_recip_base = inserted.new_base;

  for (const Candidate& c : sites) {
    const domain_id new_d = c.domain <= source ? c.domain : c.domain + 1;  // always > source
    const Step old_step = out.groups[static_cast<std::size_t>(new_d)].steps[static_cast<std::size_t>(c.step)];
    const Gather& old_gather = out.gathers[static_cast<std::size_t>(old_step.b.index)];  // already value-remapped

    Gather redirected{new_d, {}};
    redirected.index.reserve(old_gather.index.size());
    for (value_id v : old_gather.index) {
      // `r4b.wrong_row_map`: reads S_recip row `d_row` directly instead of the S row `d_row`
      // actually divided by -- wrong whenever the relocated gather is not already the identity.
      const ir::row_id s_row = epykos::mutant("r4b.wrong_row_map") ? static_cast<ir::row_id>(redirected.index.size())
                                                                    : program.row_of(v);
      redirected.index.push_back(s_recip_base + s_row);
    }
    const std::int32_t redirected_id = static_cast<std::int32_t>(out.gathers.size());
    out.gathers.push_back(std::move(redirected));

    Step replacement;
    // `r4b.wrong_op`: combines with Add instead of Mul -- a real wrong value (a + 1/b, not a/b).
    replacement.op = epykos::mutant("r4b.wrong_op") ? epykos::Op::Add : epykos::Op::Mul;
    replacement.a = old_step.a;
    replacement.b = Slot{SlotKind::Gather, redirected_id};
    out.groups[static_cast<std::size_t>(new_d)].steps[static_cast<std::size_t>(c.step)] = replacement;
  }

  detail::recompute_reads(out);
  return out;
}

}  // namespace

const std::string& R4bSharedReciprocal::name() const noexcept {
  static const std::string n = "r4b.shared_reciprocal";
  return n;
}

std::vector<MatchSite> R4bSharedReciprocal::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  std::set<domain_id> sources;
  for (const Candidate& c : find_all_candidates(program)) sources.insert(c.source);
  std::vector<MatchSite> sites;
  for (domain_id s : sources) {
    if (!profitable_candidates_for(program, s).empty()) sites.push_back(MatchSite::of_domain(s));
  }
  return sites;
}

Proposal R4bSharedReciprocal::propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite& site) const {
  Proposal p;
  p.program = relocate(program, site.domain);
  return p;
}

}  // namespace epykos::rewrite
