// EpykosEngine — cross-stage sharing in the domain IR (PROBLEM.md §5; M3/G4).
//
// A gate of every stage: the IR shows one DF domain read by both the calibration residuals and
// the book. These helpers make that a checkable statement over an ir::Program:
//
//   reach(p, groups)        per domain, which output groups it feeds (ancestor of some output
//                           of the group), as a bit mask over up to 32 groups;
//   readers(p)              per domain, the domains that read it (through gathers or segments);
//   sharing(p, groups, op)  the report: every domain with its rows, reader count and group
//                           mask, and the domains whose group's op sequence ends in `op`
//                           (Op::Exp: the discount factors) split into shared by every group,
//                           partial, and unshared;
//   assert_shared(report, expected, why)   exactly `expected` such domains exist and every one
//                           feeds every group — else `why` says which domain fails.
//
// G5 / G6 use them on the Stage A tape with groups = {residual outputs, book outputs}.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "epykos/ir/program.hpp"
#include "epykos/tape/op.hpp"

namespace epykos::ir {

// mask[d] bit g is set when domain d is an ancestor of some output ordinal in groups[g]
// (the output's own domain included). Throws std::invalid_argument for more than 32 groups
// or an ordinal out of range.
std::vector<std::uint32_t> reach(const Program& p, const std::vector<std::vector<int>>& groups);

// readers[d] = the domains whose gathers or segments read rows of d, ascending, unique.
std::vector<std::vector<domain_id>> readers(const Program& p);

struct SharingReport {
  struct Row {
    domain_id domain = -1;
    std::string name;            // shape_string
    std::int32_t rows = 0;
    std::uint32_t groups = 0;    // reach mask
    std::int32_t n_readers = 0;  // distinct reader domains
    bool matches = false;        // the group's last op is `op`
  };
  std::vector<Row> rows;             // per domain
  int n_groups = 0;
  Op op = Op::Exp;
  std::vector<domain_id> matching;   // domains whose last op is `op`
  std::vector<domain_id> shared;     // of those, read by every group
  std::vector<domain_id> partial;    // by some groups, not all
  std::vector<domain_id> unshared;   // by no group at all (dead) or one group when n_groups == 1
  std::string to_string() const;
};

SharingReport sharing(const Program& p, const std::vector<std::vector<int>>& groups, Op op = Op::Exp);

// true when report.matching.size() == expected and every matching domain feeds every group.
bool assert_shared(const SharingReport& report, int expected, std::string* why = nullptr);
// true when every matching domain feeds every group (any number of matching domains: the
// knot-time / interpolated DF buckets of D22 are two domains of disjoint times).
bool assert_all_shared(const SharingReport& report, std::string* why = nullptr);

// The number of `op` computations the program holds twice: expand(p) is re-hashed by cse and
// the `op` nodes it merges are counted. A residual sub-program that computed its own DF(t)
// beside the book's DF(t) (the same knots, the same time) shows up here; a program whose tape
// was cse'd before inference reports 0.
std::size_t duplicates(const Program& p, Op op);

}  // namespace epykos::ir
