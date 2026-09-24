#include "epykos/rewrite/r5_group_formation.hpp"

#include <cstdint>
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
using ir::row_id;
using ir::Segment;
using ir::value_id;

struct Candidate {
  domain_id producer = -1;
  domain_id consumer = -1;
  std::int32_t gather_index = -1;
};

std::vector<Candidate> find_candidates(const Program& program) {
  const std::size_t nd = program.domains.size();
  std::vector<long> reader_count(nd, 0);
  std::vector<std::int32_t> sole_gather(nd, -1);
  std::set<domain_id> feeds_reduction;

  for (std::size_t gi = 0; gi < program.gathers.size(); ++gi) {
    const Gather& g = program.gathers[gi];
    std::set<domain_id> touched;
    for (value_id v : g.index) touched.insert(program.domain_of(v));
    for (domain_id d : touched) {
      reader_count[static_cast<std::size_t>(d)] += 1;
      if (touched.size() == 1) sole_gather[static_cast<std::size_t>(d)] = static_cast<std::int32_t>(gi);
    }
  }
  for (const Segment& s : program.segments) {
    std::set<domain_id> touched;
    for (value_id v : s.members) touched.insert(program.domain_of(v));
    for (domain_id d : touched) {
      reader_count[static_cast<std::size_t>(d)] += 1;
      feeds_reduction.insert(d);
    }
  }
  std::set<value_id> outputs(program.outputs.begin(), program.outputs.end());

  std::vector<Candidate> out;
  for (std::size_t p = 0; p < nd; ++p) {
    if (reader_count[p] != 1 || sole_gather[p] < 0) continue;
    const domain_id producer = static_cast<domain_id>(p);
    if (program.domains[p].recurrent) continue;
    const std::int32_t gi = sole_gather[p];
    const Gather& g = program.gathers[static_cast<std::size_t>(gi)];
    const domain_id consumer = g.domain;
    if (consumer <= producer) continue;  // validate()'s own "no forward reads" makes this impossible, but stay defensive
    if (program.domains[static_cast<std::size_t>(consumer)].recurrent) continue;
    if (g.index.size() != static_cast<std::size_t>(program.domains[p].rows)) continue;
    bool identity = true;
    for (std::size_t r = 0; r < g.index.size(); ++r) {
      if (g.index[r] != ir::value_of(program, producer, static_cast<row_id>(r))) {
        identity = false;
        break;
      }
    }
    if (!identity) continue;
    if (!feeds_reduction.count(consumer)) continue;  // DESIGN.md §6: "... with a segment-sum epilogue"
    bool producer_is_an_output = false;
    for (row_id r = 0; r < program.domains[p].rows; ++r) {
      if (outputs.count(ir::value_of(program, producer, r))) {
        producer_is_an_output = true;
        break;
      }
    }
    if (producer_is_an_output) continue;
    out.push_back(Candidate{producer, consumer, gi});
  }
  return out;
}

}  // namespace

const std::string& R5GroupFormation::name() const noexcept {
  static const std::string n = "r5.group_formation";
  return n;
}

std::vector<MatchSite> R5GroupFormation::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  std::vector<MatchSite> sites;
  for (const Candidate& c : find_candidates(program)) {
    sites.push_back(MatchSite{SiteKind::Domain, c.producer, -1, c.gather_index});
  }
  return sites;
}

Proposal R5GroupFormation::propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite& site) const {
  const domain_id producer = site.domain;
  const std::int32_t gather_index = site.index;
  const domain_id consumer = program.gathers[static_cast<std::size_t>(gather_index)].domain;

  Proposal p;
  if (epykos::mutant("r5.wrong_step_index")) {
    // Points the consumer's replacement slot at the producer's FIRST step instead of its last --
    // wrong whenever the producer's group has more than one step (its intermediate value, not
    // its actual row value, would feed the consumer).
    Program mutated = detail::merge_producer_into_consumer(program, producer, consumer, gather_index);
    const std::int32_t np = static_cast<std::int32_t>(program.groups[static_cast<std::size_t>(producer)].steps.size());
    if (np > 1) {
      for (ir::Step& st : mutated.groups[static_cast<std::size_t>(consumer <= producer ? consumer : consumer - 1)].steps) {
        for (ir::Slot* sl : {&st.a, &st.b, &st.c, &st.konst}) {
          if (sl->kind == ir::SlotKind::Step && sl->index == np - 1) sl->index = 0;
        }
      }
    }
    p.program = std::move(mutated);
  } else if (epykos::mutant("r5.drop_last_step")) {
    // Drops the producer's last step when splicing: build the merge normally, then delete it from
    // the merged group (renumbering nothing further -- a real wrong value / an out-of-range Step
    // slot for any later step that referenced it).
    Program mutated = detail::merge_producer_into_consumer(program, producer, consumer, gather_index);
    const domain_id merged = consumer <= producer ? consumer : consumer - 1;
    const std::int32_t np = static_cast<std::int32_t>(program.groups[static_cast<std::size_t>(producer)].steps.size());
    std::vector<ir::Step>& steps = mutated.groups[static_cast<std::size_t>(merged)].steps;
    if (np >= 1 && static_cast<std::int32_t>(steps.size()) > np - 1) steps.erase(steps.begin() + (np - 1));
    p.program = std::move(mutated);
  } else {
    p.program = detail::merge_producer_into_consumer(program, producer, consumer, gather_index);
  }
  return p;
}

}  // namespace epykos::rewrite
