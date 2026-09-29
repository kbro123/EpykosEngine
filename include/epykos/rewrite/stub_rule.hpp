// EpykosEngine — the identity-stub base for R1-R7 (M4/R0; DESIGN.md §6).
//
// R1-R7 are DESIGN.md §6's fusion rewrites; each has its own file under include/epykos/rewrite/
// (r1_fold_uniform_columns.hpp .. r7_block_linmap.hpp) so R-a / R-b / R-c (RESUME.md §3, M4:
// R1-R3 / R4-R5 / R6-R7) can fill them in independently without editing a shared file. Until
// then each is IdentityStubRule<Tag>: `match` always returns no sites (never fires, so including
// a stub in any rule pipeline or e-graph run is a safe no-op) and, purely so `propose` is a
// total function rather than "unreachable", returns the input Program unchanged if ever called
// directly outside of a driver.
//
// Filling one in: replace IdentityStubRule<Tag> with a real class implementing `match` (the
// pattern: a real predicate over groups / steps / gathers / segments, see rewrite/rule.hpp) and
// `propose` (the constructor of the rewritten Program at one matched site — DESIGN.md §6 names
// the target of each rule and its exactness class, both restated in the stub file's own comment)
// and land it with its differential test AND its mutation test in the same commit (CLAUDE.md,
// D8): `epykos::mutant("rewrite.r<n>.<defect>")`, registered in include/epykos/mutation/
// mutation.hpp and docs/WORKLOADS.md §M2 per D32 and verified with rewrite::verify_rule
// (rewrite/verifier.hpp) in addition to whatever gate the mutation harness already runs.
#pragma once

#include <string>
#include <vector>

#include "epykos/rewrite/rule.hpp"

namespace epykos::rewrite {

template <class Tag>
class IdentityStubRule : public Rule {
 public:
  IdentityStubRule() : name_(Tag::rule_name()) {}
  const std::string& name() const noexcept override { return name_; }
  Exactness exactness_class() const noexcept override { return Tag::exactness(); }
  std::vector<MatchSite> match(const ir::Program&, const ir::PlanAnnotations&) const override { return {}; }
  Proposal propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite&) const override {
    Proposal p;
    p.program = program;  // identity: unchanged, in case a caller invokes propose directly
    return p;
  }

 private:
  std::string name_;
};

}  // namespace epykos::rewrite
