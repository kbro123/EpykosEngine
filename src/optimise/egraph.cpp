#include "epykos/optimise/egraph.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "epykos/rewrite/greedy.hpp"

namespace epykos::optimise {

namespace {

std::size_t hash_combine(std::size_t seed, std::size_t v) noexcept {
  // boost::hash_combine's constant, ubiquitous and adequate for a dedup key we always
  // double-check with a real equality test on collision.
  seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
  return seed;
}

rewrite::Exactness combine_exactness(rewrite::Exactness a, rewrite::Exactness b) noexcept {
  return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}

}  // namespace

// ---- UnionFind --------------------------------------------------------------------------------

int EGraph::UnionFind::make() {
  int id = static_cast<int>(parent.size());
  parent.push_back(id);
  rank.push_back(0);
  return id;
}

int EGraph::UnionFind::find(int x) {
  while (parent[static_cast<std::size_t>(x)] != x) {
    parent[static_cast<std::size_t>(x)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(x)])];
    x = parent[static_cast<std::size_t>(x)];
  }
  return x;
}

void EGraph::UnionFind::unite(int a, int b) {
  a = find(a);
  b = find(b);
  if (a == b) return;
  if (rank[static_cast<std::size_t>(a)] < rank[static_cast<std::size_t>(b)]) std::swap(a, b);
  parent[static_cast<std::size_t>(b)] = a;
  if (rank[static_cast<std::size_t>(a)] == rank[static_cast<std::size_t>(b)]) ++rank[static_cast<std::size_t>(a)];
}

// ---- plan content equality / hashing (never ir::PlanAnnotations::operator==) -------------------

bool EGraph::plan_content_equal(const ir::PlanAnnotations& a, const ir::PlanAnnotations& b) {
  if (a.domain.size() != b.domain.size()) return false;
  for (std::size_t i = 0; i < a.domain.size(); ++i) {
    const ir::DomainPlan& da = a.domain[i];
    const ir::DomainPlan& db = b.domain[i];
    if (da.choice != db.choice) return false;
    if (da.inline_consumer != db.inline_consumer) return false;
    if (da.keep_rows != db.keep_rows) return false;
  }
  if (a.group.size() != b.group.size()) return false;
  for (std::size_t i = 0; i < a.group.size(); ++i) {
    const std::vector<ir::StepPairing>& pa = a.group[i].pairings;
    const std::vector<ir::StepPairing>& pb = b.group[i].pairings;
    if (pa.size() != pb.size()) return false;
    for (std::size_t j = 0; j < pa.size(); ++j) {
      if (pa[j].first != pb[j].first) return false;
      if (pa[j].second != pb[j].second) return false;
      if (pa[j].tail != pb[j].tail) return false;
    }
  }
  if (a.emitted != b.emitted) return false;
  if (a.jacobian.mode.size() != b.jacobian.mode.size()) return false;
  for (const auto& kv : a.jacobian.mode) {
    auto it = b.jacobian.mode.find(kv.first);
    if (it == b.jacobian.mode.end() || it->second != kv.second) return false;
  }
  return true;
}

std::size_t EGraph::plan_content_hash(const ir::PlanAnnotations& a) {
  std::size_t h = 1469598103934665603ULL;  // FNV offset basis; only ever a dedup pre-filter
  h = hash_combine(h, a.domain.size());
  for (const ir::DomainPlan& d : a.domain) {
    h = hash_combine(h, static_cast<std::size_t>(d.choice));
    h = hash_combine(h, static_cast<std::size_t>(d.inline_consumer));
    h = hash_combine(h, d.keep_rows.size());
    for (std::int32_t r : d.keep_rows) h = hash_combine(h, static_cast<std::size_t>(r));
  }
  h = hash_combine(h, a.group.size());
  for (const ir::GroupPlan& g : a.group) {
    h = hash_combine(h, g.pairings.size());
    for (const ir::StepPairing& p : g.pairings) {
      h = hash_combine(h, static_cast<std::size_t>(p.first));
      h = hash_combine(h, static_cast<std::size_t>(p.second));
      for (std::int32_t t : p.tail) h = hash_combine(h, static_cast<std::size_t>(t));
    }
  }
  h = hash_combine(h, a.emitted.size());
  for (std::uint8_t e : a.emitted) h = hash_combine(h, static_cast<std::size_t>(e));
  std::size_t jh = 0;  // order-independent: jacobian.mode is an unordered_map
  for (const auto& kv : a.jacobian.mode) {
    std::size_t kh = std::hash<std::string>{}(kv.first);
    kh = hash_combine(kh, static_cast<std::size_t>(kv.second));
    jh ^= kh;
  }
  h = hash_combine(h, jh);
  return h;
}

// ---- construction / hash-consing ----------------------------------------------------------

EGraph::EGraph(ir::Program root) {
  root.plan = ir::PlanAnnotations{};  // a caller's incoming .plan is ignored (annotate.hpp pt. 3)
  ProgramNode node;
  node.program = std::move(root);
  node.parent = -1;
  node.exactness = rewrite::Exactness::E0;
  programs_.push_back(std::move(node));
  int pid = program_uf_.make();
  program_key_to_rep_.emplace(ir::serialize(programs_[0].program), pid);

  plans_.emplace_back();
  plan_uf_.emplace_back();
  plan_key_to_rep_.emplace_back();
  PlanNode plan_node;
  plan_node.parent = -1;
  plan_node.exactness = rewrite::Exactness::E0;
  plans_[0].push_back(std::move(plan_node));
  int plid = plan_uf_[0].make();
  plan_key_to_rep_[0][plan_content_hash(plans_[0][0].content)].push_back(plid);
}

int EGraph::hash_cons_program(int id, const ir::Program& program) {
  const std::string key = ir::serialize(program);
  auto it = program_key_to_rep_.find(key);
  if (it != program_key_to_rep_.end()) {
    program_uf_.unite(id, it->second);
    return program_uf_.find(it->second);
  }
  program_key_to_rep_.emplace(key, id);
  return id;
}

bool EGraph::program_known(const ir::Program& program) const {
  return program_key_to_rep_.find(ir::serialize(program)) != program_key_to_rep_.end();
}

bool EGraph::plan_known(int program_id, const ir::PlanAnnotations& content) const {
  const auto& map = plan_key_to_rep_[static_cast<std::size_t>(program_id)];
  auto it = map.find(plan_content_hash(content));
  if (it == map.end()) return false;
  for (int candidate : it->second) {
    if (plan_content_equal(plans_[static_cast<std::size_t>(program_id)][static_cast<std::size_t>(candidate)].content,
                           content)) {
      return true;
    }
  }
  return false;
}

int EGraph::hash_cons_plan(int program_id, int id, const ir::PlanAnnotations& content) {
  std::vector<int>& bucket = plan_key_to_rep_[static_cast<std::size_t>(program_id)][plan_content_hash(content)];
  for (int candidate : bucket) {
    if (plan_content_equal(plans_[static_cast<std::size_t>(program_id)][static_cast<std::size_t>(candidate)].content,
                           content)) {
      plan_uf_[static_cast<std::size_t>(program_id)].unite(id, candidate);
      return plan_uf_[static_cast<std::size_t>(program_id)].find(candidate);
    }
  }
  bucket.push_back(id);
  return id;
}

// ---- accessors ------------------------------------------------------------------------------

ClassId EGraph::program_class(int id) const { return program_uf_.find(id); }

std::vector<int> EGraph::program_class_representatives() const {
  std::unordered_map<ClassId, int> seen;
  std::vector<int> order;
  for (int i = 0; i < num_programs(); ++i) {
    ClassId c = program_uf_.find(i);
    if (seen.emplace(c, i).second) order.push_back(i);
  }
  return order;
}

int EGraph::num_plan_nodes(int program_id) const {
  return static_cast<int>(plans_.at(static_cast<std::size_t>(program_id)).size());
}

const PlanNode& EGraph::plan_node(int program_id, int id) const {
  return plans_.at(static_cast<std::size_t>(program_id)).at(static_cast<std::size_t>(id));
}

ClassId EGraph::plan_class(int program_id, int id) const {
  return plan_uf_[static_cast<std::size_t>(program_id)].find(id);
}

std::vector<int> EGraph::plan_class_representatives(int program_id) const {
  std::unordered_map<ClassId, int> seen;
  std::vector<int> order;
  for (int i = 0; i < num_plan_nodes(program_id); ++i) {
    ClassId c = plan_class(program_id, i);
    if (seen.emplace(c, i).second) order.push_back(i);
  }
  return order;
}

// ---- saturation -------------------------------------------------------------------------------

SaturationReport EGraph::saturate(const std::vector<const rewrite::Rule*>& rules, const SaturationLimits& limits) {
  SaturationReport report;

  auto total_plan_count = [this]() {
    std::size_t total = 0;
    for (const std::vector<PlanNode>& v : plans_) total += v.size();
    return total;
  };

  for (int iter = 0; iter < limits.max_iterations; ++iter) {
    report.iterations_run = iter + 1;
    pending_programs_.clear();
    pending_plans_.clear();

    const int program_frontier = num_programs();
    std::vector<int> plan_frontier(plans_.size());
    for (std::size_t p = 0; p < plans_.size(); ++p) plan_frontier[p] = static_cast<int>(plans_[p].size());

    std::size_t program_total = programs_.size();
    std::size_t plan_total = total_plan_count();
    bool any_queued = false;
    bool bound_hit_this_round = false;
    std::string bound_reason_this_round;

    for (const rewrite::Rule* rule : rules) {
      if (rule == nullptr) continue;
      std::size_t sites_matched = 0;
      std::size_t nodes_added = 0;
      bool cut = false;
      std::string note;

      for (int pid = 0; pid < program_frontier && !cut; ++pid) {
        const ir::Program& prog = programs_[static_cast<std::size_t>(pid)].program;
        // Structural rules (R1-R7) never depend on plan content today (their own `match` is
        // unconditionally empty); a rule that DOES want plan context for a Program-kind site
        // sees the program's OWN root (unplanned) annotation, node 0 of its plan tier.
        const ir::PlanAnnotations& ctx = plans_[static_cast<std::size_t>(pid)][0].content;
        std::vector<rewrite::MatchSite> sites = rule->match(prog, ctx);
        sites_matched += sites.size();

        for (const rewrite::MatchSite& site : sites) {
          rewrite::Proposal proposal = rule->propose(prog, ctx, site);

          if (proposal.is_structural()) {
            if (program_total + pending_programs_.size() >= limits.max_program_nodes) {
              cut = true;
              bound_hit_this_round = true;
              note = "max_program_nodes";
              bound_reason_this_round = "max_program_nodes";
              break;
            }
            try {
              ir::validate(*proposal.program);
            } catch (const std::exception& e) {
              // A malformed proposal from THIS site is logged and skipped, never a `cut`: that
              // field means "a resource bound stopped exploration", not "one bad proposal was
              // seen" — the rest of this rule's other sites still get a fair chance this round.
              note = std::string("rejected (invalid program): ") + e.what();
              continue;
            }
            ir::Program np = *proposal.program;
            np.plan = ir::PlanAnnotations{};
            if (!program_known(np)) {  // real hash-consing: skip a proposal that reproduces a
                                       // node already discovered in an earlier round
              pending_programs_.push_back(
                  PendingProgram{pid, std::move(np), rule->name(), rule->exactness_class()});
              ++nodes_added;
              any_queued = true;
            }
          } else if (proposal.is_annotation()) {
            for (int plid = 0; plid < plan_frontier[static_cast<std::size_t>(pid)]; ++plid) {
              const PlanNode& base = plans_[static_cast<std::size_t>(pid)][static_cast<std::size_t>(plid)];
              ir::PlanAnnotations merged = base.content;
              rewrite::merge_annotations(merged, *proposal.annotations);
              if (plan_known(pid, merged)) continue;  // ditto, for the plan tier
              if (plan_total + pending_plans_.size() >= limits.max_plan_nodes) {
                cut = true;
                bound_hit_this_round = true;
                note = "max_plan_nodes";
                bound_reason_this_round = "max_plan_nodes";
                break;
              }
              rewrite::Exactness ex = combine_exactness(base.exactness, rule->exactness_class());
              pending_plans_.push_back(
                  PendingPlan{pid, plid, std::move(merged), rule->name(), ex});
              ++nodes_added;
              any_queued = true;
            }
          } else {
            // rule.hpp's own contract: a site match() returned must never propose() empty.
            cut = true;
            note = "rejected (empty Proposal from a matched site: " + rule->name() + ")";
          }
        }
      }
      report.log.push_back(SaturationLogEntry{iter, rule->name(), sites_matched, nodes_added, cut, note});
    }

    rebuild();

    if (bound_hit_this_round) {
      report.bound_hit = true;
      report.bound_reason = bound_reason_this_round;
      break;
    }
    if (!any_queued) break;  // fixpoint: nothing new to explore
  }

  report.total_program_nodes = programs_.size();
  report.total_plan_nodes = total_plan_count();
  return report;
}

void EGraph::rebuild() {
  // Programs first: a pending plan's `program_id` names the node it was proposed against, which
  // already exists (never itself pending), so no ordering dependency the other way round.
  for (PendingProgram& pending : pending_programs_) {
    ProgramNode node;
    node.program = std::move(pending.program);
    node.parent = pending.parent;
    node.rule = pending.rule;
    node.exactness = combine_exactness(programs_[static_cast<std::size_t>(pending.parent)].exactness, pending.exactness);
    node.history = programs_[static_cast<std::size_t>(pending.parent)].history;
    node.history.push_back(pending.rule);
    int id = program_uf_.make();
    programs_.push_back(std::move(node));
    hash_cons_program(id, programs_[static_cast<std::size_t>(id)].program);

    // Every program node gets its own fresh plan tier (see the header's "Known limitation": a
    // plan tier is per NODE, not per program-tier CLASS, even when hash-consing above just
    // unioned this node into an existing class).
    plans_.emplace_back();
    plan_uf_.emplace_back();
    plan_key_to_rep_.emplace_back();
    PlanNode root_plan;
    plans_.back().push_back(std::move(root_plan));
    int plid = plan_uf_.back().make();
    hash_cons_plan(id, plid, plans_.back()[0].content);
  }
  pending_programs_.clear();

  for (PendingPlan& pending : pending_plans_) {
    PlanNode node;
    node.content = std::move(pending.content);
    node.parent = pending.plan_parent;
    node.rule = pending.rule;
    const PlanNode& parent = plans_[static_cast<std::size_t>(pending.program_id)][static_cast<std::size_t>(pending.plan_parent)];
    node.exactness = combine_exactness(parent.exactness, pending.exactness);
    node.history = parent.history;
    node.history.push_back(pending.rule);
    int id = plan_uf_[static_cast<std::size_t>(pending.program_id)].make();
    plans_[static_cast<std::size_t>(pending.program_id)].push_back(std::move(node));
    hash_cons_plan(pending.program_id, id,
                   plans_[static_cast<std::size_t>(pending.program_id)][static_cast<std::size_t>(id)].content);
  }
  pending_plans_.clear();
}

bool EGraph::congruent() {
  rebuild();  // no-op when nothing is pending; here so a caller need not call it first

  // Program tier.
  {
    std::unordered_map<ClassId, int> class_witness;  // class -> one member id, to compare against
    for (int i = 0; i < num_programs(); ++i) {
      ClassId c = program_class(i);
      auto it = class_witness.find(c);
      if (it == class_witness.end()) {
        class_witness.emplace(c, i);
      } else if (!(programs_[static_cast<std::size_t>(i)].program == programs_[static_cast<std::size_t>(it->second)].program)) {
        return false;  // same class, different content
      }
    }
    // No two nodes with EQUAL content in different classes.
    for (int i = 0; i < num_programs(); ++i) {
      for (int j = i + 1; j < num_programs(); ++j) {
        if (programs_[static_cast<std::size_t>(i)].program == programs_[static_cast<std::size_t>(j)].program &&
            program_class(i) != program_class(j)) {
          return false;
        }
      }
    }
  }

  // Plan tier, per program node.
  for (int pid = 0; pid < num_programs(); ++pid) {
    const int n = num_plan_nodes(pid);
    for (int i = 0; i < n; ++i) {
      for (int j = i + 1; j < n; ++j) {
        const bool equal = plan_content_equal(plan_node(pid, i).content, plan_node(pid, j).content);
        const bool same_class = plan_class(pid, i) == plan_class(pid, j);
        if (same_class && !equal) return false;
        if (equal && !same_class) return false;
      }
    }
  }
  return true;
}

}  // namespace epykos::optimise
