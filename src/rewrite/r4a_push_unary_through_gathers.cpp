#include "epykos/rewrite/r4a_push_unary_through_gathers.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <utility>
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

bool is_target_op(epykos::Op op) noexcept {
  return op == epykos::Op::Exp || op == epykos::Op::Log || op == epykos::Op::Sqrt || op == epykos::Op::Neg;
}

// A candidate step `y = op(gather(g))`. `source` is the single domain every row of `g` reads.
struct Candidate {
  domain_id domain = -1;
  std::int32_t step = -1;
  epykos::Op op = epykos::Op::Const;
  domain_id source = -1;
};

// The homogeneous source domain of `g`, or -1 if its rows come from more than one domain.
domain_id homogeneous_source(const Program& program, const Gather& g) {
  if (g.index.empty()) return -1;
  const domain_id first = program.domain_of(g.index.front());
  for (value_id v : g.index) {
    if (program.domain_of(v) != first) return -1;
  }
  return first;
}

// Real redundancy: some row of `S` is fetched by more than one row of the gather (DESIGN.md's own
// framing, "toward the smaller domain" -- pushing `op` to `S` is only a win when `S` has fewer
// distinct rows in play than the gather has entries).
bool has_redundancy(const Gather& g) {
  std::set<value_id> distinct(g.index.begin(), g.index.end());
  return distinct.size() < g.index.size();
}

std::optional<Candidate> as_candidate(const Program& program, domain_id d, std::int32_t i) {
  const Step& st = program.groups[static_cast<std::size_t>(d)].steps[static_cast<std::size_t>(i)];
  if (!is_target_op(st.op)) return std::nullopt;
  if (st.a.kind != SlotKind::Gather || st.b.kind != SlotKind::None || st.c.kind != SlotKind::None ||
      st.konst.kind != SlotKind::None) {
    return std::nullopt;
  }
  const Gather& g = program.gathers[static_cast<std::size_t>(st.a.index)];
  const domain_id source = homogeneous_source(program, g);
  if (source < 0) return std::nullopt;
  if (!has_redundancy(g)) return std::nullopt;
  return Candidate{d, i, st.op, source};
}

std::vector<Candidate> find_candidates(const Program& program) {
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

std::uint64_t bits_of(double v) noexcept {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

std::int32_t find_or_append_literal(std::vector<double>& literals, double value) {
  const std::uint64_t bits = bits_of(value);
  for (std::size_t i = 0; i < literals.size(); ++i) {
    if (bits_of(literals[i]) == bits) return static_cast<std::int32_t>(i);
  }
  literals.push_back(value);
  return static_cast<std::int32_t>(literals.size() - 1);
}

// Handles every candidate sharing `(source, op)` in one Program rebuild.
Program relocate(const Program& program, domain_id source, epykos::Op op) {
  std::vector<Candidate> sites;
  for (const Candidate& c : find_candidates(program)) {
    if (c.source == source && c.op == op) sites.push_back(c);
  }

  const ir::Domain& s_dom = program.domains[static_cast<std::size_t>(source)];
  ir::Domain s_op_domain;
  s_op_domain.name = std::string(epykos::to_string(op)) + "(@0)";
  s_op_domain.rows = s_dom.rows;
  ir::Gather identity{detail::new_domain_marker(), {}};
  identity.index.reserve(static_cast<std::size_t>(s_dom.rows));
  for (std::int32_t r = 0; r < s_dom.rows; ++r) identity.index.push_back(ir::value_of(program, source, r));
  const std::int32_t identity_gather_id = static_cast<std::int32_t>(program.gathers.size());
  ir::Group s_op_group;
  s_op_group.steps = {Step{op, Slot{SlotKind::Gather, identity_gather_id}, {}, {}, {}}};

  detail::InsertResult inserted = detail::insert_domain_after(program, source, s_op_domain, s_op_group, {identity}, {});
  Program out = std::move(inserted.program);
  const value_id s_op_base = inserted.new_base;

  const std::int32_t one = find_or_append_literal(out.literals, epykos::mutant("r4a.wrong_literal") ? 0.0 : 1.0);

  for (const Candidate& c : sites) {
    const domain_id new_d = c.domain <= source ? c.domain : c.domain + 1;  // always > source, so always +1
    const Step& old_step = out.groups[static_cast<std::size_t>(new_d)].steps[static_cast<std::size_t>(c.step)];
    const Gather& old_gather = out.gathers[static_cast<std::size_t>(old_step.a.index)];  // already value-remapped by insert

    Gather redirected{new_d, {}};
    redirected.index.reserve(old_gather.index.size());
    for (value_id v : old_gather.index) {
      // `r4a.wrong_row_map`: reads S_op row `d_row` directly instead of the row `S` row `d_row`
      // actually gathered -- wrong whenever the relocated gather is not already the identity.
      const ir::row_id s_row = epykos::mutant("r4a.wrong_row_map") ? static_cast<ir::row_id>(redirected.index.size())
                                                                    : program.row_of(v);
      redirected.index.push_back(s_op_base + s_row);
    }
    const std::int32_t redirected_id = static_cast<std::int32_t>(out.gathers.size());
    out.gathers.push_back(std::move(redirected));

    Step replacement;
    replacement.op = epykos::Op::Mul;
    replacement.a = Slot{SlotKind::Gather, redirected_id};
    replacement.b = Slot{SlotKind::Literal, one};
    out.groups[static_cast<std::size_t>(new_d)].steps[static_cast<std::size_t>(c.step)] = replacement;
  }

  detail::recompute_reads(out);
  return out;
}

}  // namespace

const std::string& R4aPushUnaryThroughGathers::name() const noexcept {
  static const std::string n = "r4a.push_unary_through_gathers";
  return n;
}

std::vector<MatchSite> R4aPushUnaryThroughGathers::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  std::set<std::pair<domain_id, epykos::Op>> seen;
  std::vector<MatchSite> sites;
  for (const Candidate& c : find_candidates(program)) {
    if (seen.insert({c.source, c.op}).second) {
      sites.push_back(MatchSite{SiteKind::Domain, c.source, -1, static_cast<std::int32_t>(c.op)});
    }
  }
  return sites;
}

Proposal R4aPushUnaryThroughGathers::propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite& site) const {
  Proposal p;
  p.program = relocate(program, site.domain, static_cast<epykos::Op>(site.index));
  return p;
}

}  // namespace epykos::rewrite
