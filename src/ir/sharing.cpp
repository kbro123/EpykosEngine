#include "epykos/ir/sharing.hpp"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>

#include "epykos/ir/expand.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::ir {

namespace {
std::size_t idx(std::int32_t i) noexcept { return static_cast<std::size_t>(i); }
}  // namespace

std::vector<std::uint32_t> reach(const Program& p, const std::vector<std::vector<int>>& groups) {
  if (groups.size() > 32) throw std::invalid_argument("ir::reach: at most 32 output groups");
  std::vector<std::uint32_t> mask(p.domains.size(), 0);
  for (std::size_t g = 0; g < groups.size(); ++g) {
    for (int o : groups[g]) {
      if (o < 0 || static_cast<std::size_t>(o) >= p.outputs.size()) throw std::invalid_argument("ir::reach: output ordinal out of range");
      mask[idx(p.domain_of(p.outputs[static_cast<std::size_t>(o)]))] |= (1u << g);
    }
  }
  // Domains are in evaluation order and `reads` lists earlier domains: one reverse pass.
  for (std::size_t d = p.domains.size(); d-- > 0;) {
    if (mask[d] == 0) continue;
    for (domain_id r : p.domains[d].reads) mask[idx(r)] |= mask[d];
  }
  return mask;
}

std::vector<std::vector<domain_id>> readers(const Program& p) {
  std::vector<std::vector<domain_id>> rd(p.domains.size());
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    for (domain_id r : p.domains[d].reads) rd[idx(r)].push_back(static_cast<domain_id>(d));
  }
  return rd;
}

SharingReport sharing(const Program& p, const std::vector<std::vector<int>>& groups, Op op) {
  SharingReport rep;
  rep.n_groups = static_cast<int>(groups.size());
  rep.op = op;
  const std::vector<std::uint32_t> mask = reach(p, groups);
  const std::vector<std::vector<domain_id>> rd = readers(p);
  const std::uint32_t all = groups.empty() ? 0u : (groups.size() == 32 ? 0xFFFFFFFFu : ((1u << groups.size()) - 1u));
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    SharingReport::Row row;
    row.domain = static_cast<domain_id>(d);
    row.name = shape_string(p, row.domain);
    row.rows = p.domains[d].rows;
    row.groups = mask[d];
    row.n_readers = static_cast<std::int32_t>(rd[d].size());
    row.matches = !p.groups[d].steps.empty() && p.groups[d].steps.back().op == op;
    if (row.matches) {
      rep.matching.push_back(row.domain);
      if (mask[d] == all && all != 0) {
        rep.shared.push_back(row.domain);
      } else if (mask[d] == 0) {
        rep.unshared.push_back(row.domain);
      } else {
        rep.partial.push_back(row.domain);
      }
    }
    rep.rows.push_back(std::move(row));
  }
  return rep;
}

bool assert_shared(const SharingReport& report, int expected, std::string* why) {
  std::ostringstream os;
  bool ok = true;
  if (static_cast<int>(report.matching.size()) != expected) {
    ok = false;
    os << "expected " << expected << " domain(s) ending in " << to_string(report.op) << ", found " << report.matching.size() << ";";
  }
  for (domain_id d : report.partial) {
    ok = false;
    const SharingReport::Row& r = report.rows[idx(d)];
    os << " domain " << d << " " << r.name << " (" << r.rows << " rows) feeds groups mask " << r.groups << " of " << report.n_groups << ";";
  }
  for (domain_id d : report.unshared) {
    ok = false;
    const SharingReport::Row& r = report.rows[idx(d)];
    os << " domain " << d << " " << r.name << " (" << r.rows << " rows) feeds no group;";
  }
  if (why != nullptr) *why = os.str();
  return ok;
}

bool assert_all_shared(const SharingReport& report, std::string* why) {
  std::ostringstream os;
  bool ok = true;
  if (report.matching.empty()) {
    ok = false;
    os << "no domain ends in " << to_string(report.op) << ";";
  }
  for (domain_id d : report.partial) {
    ok = false;
    const SharingReport::Row& r = report.rows[idx(d)];
    os << " domain " << d << " " << r.name << " (" << r.rows << " rows) feeds groups mask " << r.groups << " of " << report.n_groups << ";";
  }
  for (domain_id d : report.unshared) {
    ok = false;
    const SharingReport::Row& r = report.rows[idx(d)];
    os << " domain " << d << " " << r.name << " (" << r.rows << " rows) feeds no group;";
  }
  if (why != nullptr) *why = os.str();
  return ok;
}

std::size_t duplicates(const Program& p, Op op) {
  Tape t = expand(p);
  const std::vector<std::size_t> before = t.op_histogram();
  cse(t);
  const std::vector<std::size_t> after = t.op_histogram();
  const std::size_t o = static_cast<std::size_t>(op);
  return before[o] - after[o];
}

std::string SharingReport::to_string() const {
  std::ostringstream os;
  os << "sharing over " << n_groups << " output group(s), op " << epykos::to_string(op) << ": " << matching.size() << " matching domain(s), "
     << shared.size() << " shared by every group, " << partial.size() << " partial, " << unshared.size() << " unshared\n";
  for (const Row& r : rows) {
    os << "  d" << r.domain << (r.matches ? " *" : "  ") << ' ' << r.name << " rows " << r.rows << " readers " << r.n_readers << " groups ";
    for (int g = 0; g < n_groups; ++g) os << (((r.groups >> g) & 1u) ? '1' : '0');
    os << '\n';
  }
  return os.str();
}

}  // namespace epykos::ir
