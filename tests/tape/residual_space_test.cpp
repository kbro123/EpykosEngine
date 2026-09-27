// What is LEFT on the pinned tape, and what of it directed rewriting cannot decide.
//
// Two jobs, and only the first is a gate.
//
// THE GATE: `simplify` reached its fixpoint on a real fixture. Not "it ran" and not "it removed
// something" — that no site of any of its own rules survives on the tape it produced. A rule that
// silently stops matching (an operand order changes, a pass upstream rewrites a shape) would
// otherwise show up only as a performance regression nobody traces back.
//
// THE MEASUREMENT: `PRINCIPLES.md` §10 step 7 — the residual BIDIRECTIONAL space, which is what a
// term-level e-graph would be built against and what decides whether the quarantined search code
// is revived or deleted. Printed, never asserted: §5 and HARD RULE 9 both say a measured finding
// is reported, not hard-coded into a gate.
//
// This file is deliberately NOT named to match `scripts/mutation_test.sh`'s GATE_REGEX. It is not
// a mutation catcher and should not slow that harness down.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/tape/op.hpp"
#include "epykos/tape/tape.hpp"

namespace {

using epykos::Node;
using epykos::Op;
using epykos::Tape;
using epykos::node_id;

std::vector<std::vector<node_id>> user_lists(const Tape& t) {
  std::vector<std::vector<node_id>> users(t.size());
  for (node_id i = 0; i < static_cast<node_id>(t.size()); ++i) {
    const Node& n = t[i];
    for (node_id a : {n.a, n.b, n.c}) {
      if (a != epykos::invalid_node) users[static_cast<std::size_t>(a)].push_back(i);
    }
  }
  return users;
}

const Tape& pinned_tape() {
  static const epykos::fixtures::CompareOisTape t = [] {
    epykos::fixtures::CompareOisOptions o;
    o.trades = 16;
    return epykos::fixtures::record_compare_ois(epykos::fixtures::make_compare_ois(o));
  }();
  return t.tape;
}

}  // namespace

// ---- the gate ----------------------------------------------------------------------------------

TEST(ResidualSpace, SimplifyLeavesNoSiteOfItsOwnRules) {
  const Tape& t = pinned_tape();
  std::size_t cancel = 0, telescope = 0, additive = 0, identity = 0;
  for (node_id i = 0; i < static_cast<node_id>(t.size()); ++i) {
    const Node& n = t[i];
    if (n.a == epykos::invalid_node || n.b == epykos::invalid_node) continue;
    const Node& a = t[n.a];
    const Node& b = t[n.b];
    const auto one = [&](node_id x) { return t[x].op == Op::Const && t[x].konst == 1.0; };
    if (n.op == Op::Mul) {
      if (one(n.a) || one(n.b)) ++identity;
      if ((a.op == Op::Div && a.b == n.b) || (b.op == Op::Div && b.b == n.a)) ++cancel;
      if (a.op == Op::Div && b.op == Op::Div && (a.b == b.a || b.b == a.a)) ++telescope;
    }
    if (n.op == Op::Div && ((a.op == Op::Mul && a.b == n.b) || (a.op == Op::Mul && a.a == n.b))) ++cancel;
    if (n.op == Op::Add && ((b.op == Op::Sub && b.b == n.a) || (a.op == Op::Sub && a.b == n.b))) ++additive;
  }
  EXPECT_EQ(cancel, 0u) << "a multiply/divide cancellation survived the pin";
  EXPECT_EQ(telescope, 0u) << "a telescopable pair of consecutive ratios survived the pin";
  EXPECT_EQ(additive, 0u) << "an add/subtract cancellation survived the pin";
  EXPECT_EQ(identity, 0u) << "a multiply by one survived the pin";
}

// ---- the measurement ---------------------------------------------------------------------------

TEST(ResidualSpace, ReportTheBidirectionalSpace) {
  const Tape& t = pinned_tape();
  const std::vector<std::vector<node_id>> users = user_lists(t);

  std::map<std::string, std::size_t> hist;
  for (const Node& n : t.nodes()) hist[epykos::to_string(n.op)]++;
  std::printf("[ residual ] pinned compare_ois tape, 16 trades: %zu nodes\n", t.size());
  for (const auto& kv : hist) std::printf("[ residual ]   %-8s %6zu\n", kv.first.c_str(), kv.second);

  // Factoring: two Mul children of one Sum sharing an operand. a*b + a*c -> a*(b+c) removes a
  // multiply, but only when neither product is used anywhere else — so whether it wins is a
  // property of the whole program, not of the site. Counted as an upper bound on opportunity.
  std::size_t pairs = 0, factorable = 0;
  for (const Node& n : t.nodes()) {
    if (n.op != Op::Sum) continue;
    std::vector<node_id> muls;
    for (node_id a : t.args(n)) {
      if (t[a].op == Op::Mul) muls.push_back(a);
    }
    for (std::size_t x = 0; x < muls.size(); ++x) {
      for (std::size_t y = x + 1; y < muls.size(); ++y) {
        ++pairs;
        const Node& mx = t[muls[x]];
        const Node& my = t[muls[y]];
        if (mx.a == my.a || mx.a == my.b || mx.b == my.a || mx.b == my.b) ++factorable;
      }
    }
  }

  // div(exp(a), exp(b)) -> exp(a - b): one transcendental and a subtract for two transcendentals
  // and a divide. Obviously good in isolation, and on this fixture it LOSES every time, because
  // consecutive telescoped coupons share their endpoint discount factors — the exps stay live and
  // the rewrite only adds nodes. The clearest evidence in the repository that the residual space
  // needs a cost-aware search rather than another directed rule (PRINCIPLES.md §7.1).
  std::size_t div_exp = 0, div_exp_single_use = 0;
  for (node_id i = 0; i < static_cast<node_id>(t.size()); ++i) {
    const Node& n = t[i];
    if (n.op != Op::Div || n.a == epykos::invalid_node || n.b == epykos::invalid_node) continue;
    if (t[n.a].op != Op::Exp || t[n.b].op != Op::Exp) continue;
    ++div_exp;
    if (users[static_cast<std::size_t>(n.a)].size() == 1 && users[static_cast<std::size_t>(n.b)].size() == 1) {
      ++div_exp_single_use;
    }
  }

  std::printf("[ residual ] bidirectional sites (PRINCIPLES.md §10 step 7):\n");
  std::printf("[ residual ]   factorable Mul pairs under a Sum   %6zu of %zu pairs\n", factorable, pairs);
  std::printf("[ residual ]   div(exp,exp) -> exp(sub) sites     %6zu\n", div_exp);
  std::printf("[ residual ]     both exps single-use (a win)     %6zu\n", div_exp_single_use);
  std::printf("[ residual ]     shared, where the rule LOSES     %6zu\n", div_exp - div_exp_single_use);
  SUCCEED();
}
