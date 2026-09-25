#include "epykos/rewrite/bucket_split_edit.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "epykos/rewrite/ir_edit.hpp"  // M4/R-b's recompute_reads (epykos::rewrite::detail): reused here rather
                                       // than duplicated -- same signature, same contract (see that header).

namespace epykos::rewrite::detail {

using ir::domain_id;
using ir::Program;
using ir::Slot;
using ir::SlotKind;
using ir::value_id;

namespace {

std::size_t idx(std::int32_t i) noexcept { return static_cast<std::size_t>(i); }

std::uint64_t bits_of(double v) noexcept {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

// Compacts `items`, dropping every entry for which `drop(item)` is true. Returns, per ORIGINAL
// index, the new index of a kept entry or -1 for a dropped one.
template <class T, class Pred>
std::vector<std::int32_t> compact(std::vector<T>& items, Pred drop) {
  std::vector<T> kept;
  kept.reserve(items.size());
  std::vector<std::int32_t> remap(items.size(), -1);
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (!drop(items[i])) {
      remap[i] = static_cast<std::int32_t>(kept.size());
      kept.push_back(std::move(items[i]));
    }
  }
  items = std::move(kept);
  return remap;
}

void remap_slot(Slot& s, const std::vector<std::int32_t>& column_map, const std::vector<std::int32_t>& gather_map,
                const std::vector<std::int32_t>& segment_map) {
  switch (s.kind) {
    case SlotKind::Column:
      s.index = column_map[idx(s.index)];
      break;
    case SlotKind::Gather:
      s.index = gather_map[idx(s.index)];
      break;
    case SlotKind::Segment:
      s.index = segment_map[idx(s.index)];
      break;
    default:
      break;
  }
}

}  // namespace

bool all_bit_identical(const std::vector<double>& values) {
  if (values.empty()) return true;
  const std::uint64_t first = bits_of(values.front());
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (bits_of(values[i]) != first) return false;
  }
  return true;
}

void drop_owned_entries(Program& p, domain_id d) {
  const std::vector<std::int32_t> column_map = compact(p.columns, [&](const ir::Column& c) { return c.domain == d; });
  const std::vector<std::int32_t> gather_map = compact(p.gathers, [&](const ir::Gather& g) { return g.domain == d; });
  const std::vector<std::int32_t> segment_map = compact(p.segments, [&](const ir::Segment& s) { return s.domain == d; });

  for (ir::Group& g : p.groups) {
    if (g.domain == d) continue;  // its own group is discarded by the caller; stale slots, unread
    for (ir::Step& st : g.steps) {
      remap_slot(st.a, column_map, gather_map, segment_map);
      remap_slot(st.b, column_map, gather_map, segment_map);
      remap_slot(st.c, column_map, gather_map, segment_map);
      remap_slot(st.konst, column_map, gather_map, segment_map);
    }
  }
  for (ir::Scan& sc : p.scans) {
    if (sc.domain == d) continue;  // d is never a scan domain (callers exclude recurrent domains)
    sc.carry_gather = gather_map[idx(sc.carry_gather)];
  }
}

void shift_domain_ids(Program& p, domain_id threshold, std::int32_t delta) {
  for (ir::Column& c : p.columns) {
    if (c.domain > threshold) c.domain += delta;
  }
  for (ir::Gather& g : p.gathers) {
    if (g.domain > threshold) g.domain += delta;
  }
  for (ir::Segment& s : p.segments) {
    if (s.domain > threshold) s.domain += delta;
  }
  for (ir::Scan& sc : p.scans) {
    if (sc.domain > threshold) sc.domain += delta;
  }
  for (ir::Group& g : p.groups) {
    if (g.domain > threshold) g.domain += delta;
  }
  for (ir::Domain& dom : p.domains) {
    for (domain_id& r : dom.reads) {
      if (r > threshold) r += delta;
    }
  }
}

Program eliminate_domain(const Program& program, domain_id d, const std::vector<value_id>& replacement) {
  Program out = program;
  const ir::Domain removed = out.domains[idx(d)];
  const value_id base = removed.value_base;
  const value_id rows = removed.rows;

  // Row r of `d` (value id base + r) becomes `replacement[r]`; every value id at or after the
  // domain's end closes the `rows`-wide gap it leaves; everything earlier is untouched (and so is
  // every `replacement[r]`, which the caller guarantees is < base already).
  auto subst = [&](value_id v) -> value_id {
    if (v >= base && v < base + rows) return replacement[idx(v - base)];
    if (v >= base + rows) return v - rows;
    return v;
  };

  for (ir::Gather& g : out.gathers) {
    if (g.domain == d) continue;  // dropped below, about to disappear
    for (value_id& v : g.index) v = subst(v);
  }
  for (ir::Segment& s : out.segments) {
    if (s.domain == d) continue;
    for (value_id& v : s.members) v = subst(v);
  }
  for (value_id& v : out.inputs) v = subst(v);
  for (value_id& v : out.outputs) v = subst(v);

  drop_owned_entries(out, d);
  shift_domain_ids(out, d, -1);

  out.domains.erase(out.domains.begin() + idx(d));
  out.groups.erase(out.groups.begin() + idx(d));
  for (std::size_t i = idx(d); i < out.domains.size(); ++i) out.domains[i].value_base -= rows;

  recompute_reads(out);
  return out;
}

}  // namespace epykos::rewrite::detail
