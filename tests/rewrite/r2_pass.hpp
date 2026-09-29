// Shared helper for tests/rewrite/r1_fold_uniform_columns_e0_test.cpp: one full R2 pass.
//
// R1 exists to clean up after R2 (D52 point 1: the signature pass already folds every uniform
// column a domain straight out of ir::infer could have, so R1's only non-synthetic source of work
// is a domain some REWRITE has since changed underneath that classification -- concretely a fresh
// bucket split, whose every column is uniform by construction). Measuring whether R1 actually
// fires therefore means running R2 first, and D53/D64 require that measurement to be a stated
// number on each named fixture rather than an argument from the rule's shape.
//
// Applies R2 at every site ONE match() over the original program found, HIGHEST DOMAIN ID FIRST.
// A split shifts only domain ids above its own (rewrite::detail::shift_domain_ids) and leaves
// every lower domain's id, group and owned entries exactly as they were, so descending order lets
// one match() drive the whole pass. rewrite::apply_greedy would also be correct -- it applies one
// structural proposal per call and re-matches -- but R2's match() is O(domains x columns) and the
// pass grows the Stage A tape from 67 domains to 64,732, so re-matching per site is quadratic in
// a number that is already five figures.
#pragma once

#include <cstddef>
#include <vector>

#include "epykos/ir/annotate.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/rewrite/r2_bucket_rows.hpp"

namespace epykos::test {

inline ir::Program apply_r2_everywhere(const ir::Program& program, std::size_t& sites_found) {
  const rewrite::R2BucketRows r2;
  const std::vector<rewrite::MatchSite> sites = r2.match(program, ir::PlanAnnotations{});
  sites_found = sites.size();
  ir::Program working = program;
  for (std::size_t i = sites.size(); i-- > 0;) {
    const rewrite::Proposal p = r2.propose(working, ir::PlanAnnotations{}, sites[i]);
    working = *p.program;
  }
  ir::validate(working);
  return working;
}

}  // namespace epykos::test
