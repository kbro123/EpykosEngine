#include "epykos/rewrite/ir_edit.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace epykos::rewrite::detail {

using ir::domain_id;
using ir::Gather;
using ir::Program;
using ir::Segment;
using ir::Slot;
using ir::SlotKind;
using ir::Step;
using ir::value_id;

// Recomputes every domain's `reads` from the FINAL gather/segment tables (file header: preferred
// over incrementally patching the old lists). `reads` is sorted, unique, excludes self unless
// recurrent -- exactly ir::validate's own expectations.
void recompute_reads(Program& p) {
  std::vector<std::vector<domain_id>> reads(p.domains.size());
  for (const Gather& g : p.gathers) {
    for (value_id v : g.index) reads[static_cast<std::size_t>(g.domain)].push_back(p.domain_of(v));
  }
  for (const Segment& s : p.segments) {
    for (value_id v : s.members) reads[static_cast<std::size_t>(s.domain)].push_back(p.domain_of(v));
  }
  for (std::size_t d = 0; d < p.domains.size(); ++d) {
    std::vector<domain_id>& r = reads[d];
    std::sort(r.begin(), r.end());
    r.erase(std::unique(r.begin(), r.end()), r.end());
    r.erase(std::remove(r.begin(), r.end(), static_cast<domain_id>(d)), r.end());
    p.domains[d].reads = std::move(r);
  }
}

InsertResult insert_domain_after(const Program& src, domain_id after, ir::Domain new_domain, ir::Group new_group,
                                  std::vector<Gather> new_gathers, std::vector<Segment> new_segments) {
  if (after < 0 || static_cast<std::size_t>(after) >= src.domains.size()) {
    throw std::invalid_argument("rewrite::detail::insert_domain_after: `after` out of range");
  }
  if (new_domain.recurrent) {
    throw std::invalid_argument("rewrite::detail::insert_domain_after: a recurrent new domain is not supported");
  }
  const value_id shift = static_cast<value_id>(new_domain.rows);
  const domain_id new_id = after + 1;
  const value_id new_base = src.domains[static_cast<std::size_t>(after)].value_base + src.domains[static_cast<std::size_t>(after)].rows;

  auto remap_domain = [&](domain_id d) { return d <= after ? d : d + 1; };
  auto remap_value = [&](value_id v) {
    const domain_id d = src.domain_of(v);
    return d <= after ? v : v + shift;
  };

  Program out;
  out.literals = src.literals;
  out.domains.reserve(src.domains.size() + 1);
  out.groups.reserve(src.groups.size() + 1);

  for (std::size_t d = 0; d < src.domains.size(); ++d) {
    if (static_cast<domain_id>(d) == after) {
      out.domains.push_back(src.domains[d]);  // unchanged: position and value_base both untouched
      out.groups.push_back(src.groups[d]);

      new_domain.value_base = new_base;
      new_domain.reads = {after};  // recomputed exactly below anyway; a safe starting point
      new_group.domain = new_id;
      out.domains.push_back(new_domain);
      out.groups.push_back(new_group);
      continue;
    }
    ir::Domain dom = src.domains[d];
    dom.value_base = remap_value(dom.value_base);
    for (domain_id& r : dom.reads) r = remap_domain(r);
    out.domains.push_back(dom);
    ir::Group grp = src.groups[d];
    grp.domain = remap_domain(grp.domain);
    out.groups.push_back(grp);
  }

  out.columns = src.columns;
  for (ir::Column& c : out.columns) c.domain = remap_domain(c.domain);

  out.gathers = src.gathers;
  for (Gather& g : out.gathers) {
    g.domain = remap_domain(g.domain);
    for (value_id& v : g.index) v = remap_value(v);
  }
  for (Gather g : new_gathers) {
    if (g.domain != new_domain_marker()) {
      throw std::invalid_argument("rewrite::detail::insert_domain_after: new_gathers must carry new_domain_marker()");
    }
    g.domain = new_id;
    out.gathers.push_back(std::move(g));
  }

  out.segments = src.segments;
  for (Segment& s : out.segments) {
    s.domain = remap_domain(s.domain);
    for (value_id& v : s.members) v = remap_value(v);
  }
  for (Segment s : new_segments) {
    if (s.domain != new_domain_marker()) {
      throw std::invalid_argument("rewrite::detail::insert_domain_after: new_segments must carry new_domain_marker()");
    }
    s.domain = new_id;
    out.segments.push_back(std::move(s));
  }

  out.scans = src.scans;
  for (ir::Scan& sc : out.scans) sc.domain = remap_domain(sc.domain);

  out.inputs.reserve(src.inputs.size());
  for (value_id v : src.inputs) out.inputs.push_back(remap_value(v));
  out.input_values = src.input_values;
  out.outputs.reserve(src.outputs.size());
  for (value_id v : src.outputs) out.outputs.push_back(remap_value(v));

  recompute_reads(out);
  return InsertResult{std::move(out), new_id, new_base};
}

ir::Program merge_producer_into_consumer(const Program& src, domain_id producer, domain_id consumer, std::int32_t consumer_gather) {
  if (producer < 0 || consumer < 0 || static_cast<std::size_t>(producer) >= src.domains.size() ||
      static_cast<std::size_t>(consumer) >= src.domains.size() || producer >= consumer) {
    throw std::invalid_argument("rewrite::detail::merge_producer_into_consumer: bad domain ids");
  }
  if (src.domains[static_cast<std::size_t>(producer)].recurrent || src.domains[static_cast<std::size_t>(consumer)].recurrent) {
    throw std::invalid_argument("rewrite::detail::merge_producer_into_consumer: recurrent domains are not supported");
  }
  const Gather& cg = src.gathers[static_cast<std::size_t>(consumer_gather)];
  if (cg.domain != consumer) throw std::invalid_argument("rewrite::detail::merge_producer_into_consumer: consumer_gather is not owned by consumer");

  const std::vector<Step>& producer_steps = src.groups[static_cast<std::size_t>(producer)].steps;
  const std::vector<Step>& consumer_steps = src.groups[static_cast<std::size_t>(consumer)].steps;
  const std::int32_t offset = static_cast<std::int32_t>(producer_steps.size());
  const std::int32_t producer_last = offset - 1;

  // `gathers` loses exactly one entry (`consumer_gather`, orphaned by the substitution below: a
  // Gather table is only ever referenced by its OWNING domain's own steps -- program.hpp's own
  // "one value id per row of the reading domain" -- and `consumer_gather.domain == consumer`, so
  // it is consumer's group, and no other domain's, that could reference it). EVERY step of EVERY
  // domain that names a gather positioned after it must shift down by one to stay valid; this is
  // applied uniformly below, not just to the two domains being merged.
  auto remap_gather_index = [&](std::int32_t idx) { return idx > consumer_gather ? idx - 1 : idx; };
  auto fix_gather_ref = [&](Slot s) -> Slot {
    if (s.kind == SlotKind::Gather) s.index = remap_gather_index(s.index);
    return s;
  };
  // Consumer's own steps, additionally: internal Step-slot cross references shift by `offset`
  // (the producer's steps now occupy positions [0, offset) ahead of them), and the specific
  // slot(s) that used to read `consumer_gather` read the producer's own last step directly.
  auto fix_consumer_slot = [&](Slot s) -> Slot {
    if (s.kind == SlotKind::Step) {
      s.index += offset;
      return s;
    }
    if (s.kind == SlotKind::Gather && s.index == consumer_gather) {
      return Slot{SlotKind::Step, producer_last};  // == offset - 1, already in the MERGED numbering
    }
    return fix_gather_ref(s);
  };

  std::vector<Step> merged;
  merged.reserve(producer_steps.size() + consumer_steps.size());
  for (const Step& s : producer_steps) {
    // positions [0, offset): unchanged numbering, but still renumbered around the dropped gather.
    Step ns = s;
    ns.a = fix_gather_ref(ns.a);
    ns.b = fix_gather_ref(ns.b);
    ns.c = fix_gather_ref(ns.c);
    ns.konst = fix_gather_ref(ns.konst);
    merged.push_back(ns);
  }
  for (const Step& s : consumer_steps) {
    Step ns = s;
    ns.a = fix_consumer_slot(ns.a);
    ns.b = fix_consumer_slot(ns.b);
    ns.c = fix_consumer_slot(ns.c);
    ns.konst = fix_consumer_slot(ns.konst);
    merged.push_back(ns);
  }

  // `consumer` always shifts down by one itself (it is positioned after `producer`, which is
  // removed), so a table producer's OWN steps used (now adopted by the merged consumer) must be
  // relabelled to consumer's OWN, already-shifted, final id -- NOT its pre-shift one.
  auto remap_domain = [&](domain_id d) {
    if (d == producer) return consumer - 1;  // producer's rows are adopted by the merged consumer
    return d < producer ? d : d - 1;
  };
  const value_id shift = static_cast<value_id>(src.domains[static_cast<std::size_t>(producer)].rows);
  auto remap_value = [&](value_id v) {
    const domain_id d = src.domain_of(v);
    if (d == producer) {
      throw std::logic_error("rewrite::detail::merge_producer_into_consumer: a live reference to the producer's own "
                              "values survived past the one gather this rewrite was told is its only reader");
    }
    return d < producer ? v : v - shift;
  };

  Program out;
  out.literals = src.literals;
  out.domains.reserve(src.domains.size() - 1);
  out.groups.reserve(src.groups.size() - 1);
  for (std::size_t d = 0; d < src.domains.size(); ++d) {
    if (static_cast<domain_id>(d) == producer) continue;  // absorbed
    ir::Domain dom = src.domains[d];
    dom.value_base = remap_value(dom.value_base);
    for (domain_id& r : dom.reads) r = remap_domain(r);
    ir::Group grp = src.groups[d];
    grp.domain = remap_domain(grp.domain);
    if (static_cast<domain_id>(d) == consumer) {
      grp.steps = merged;
    } else {
      // Not one of the two domains being merged: its own steps are otherwise untouched, but any
      // Slot{Gather,*} they hold must still shift around the one dropped gather.
      for (Step& st : grp.steps) {
        st.a = fix_gather_ref(st.a);
        st.b = fix_gather_ref(st.b);
        st.c = fix_gather_ref(st.c);
        st.konst = fix_gather_ref(st.konst);
      }
    }
    out.domains.push_back(dom);
    out.groups.push_back(grp);
  }

  out.columns = src.columns;
  for (ir::Column& c : out.columns) c.domain = remap_domain(c.domain);

  out.gathers.reserve(src.gathers.size() - 1);
  for (std::int32_t gi = 0; gi < static_cast<std::int32_t>(src.gathers.size()); ++gi) {
    if (gi == consumer_gather) continue;  // dropped: nothing references it after the splice above
    Gather g = src.gathers[static_cast<std::size_t>(gi)];
    g.domain = remap_domain(g.domain);
    for (value_id& v : g.index) v = remap_value(v);
    out.gathers.push_back(std::move(g));
  }

  out.segments = src.segments;
  for (Segment& s : out.segments) {
    s.domain = remap_domain(s.domain);
    for (value_id& v : s.members) v = remap_value(v);
  }

  out.scans = src.scans;
  for (ir::Scan& sc : out.scans) {
    sc.domain = remap_domain(sc.domain);
    // A scan's carry gather is a position into `gathers` like any Slot{Gather,*}: it must shift
    // around the one dropped entry exactly like every step's own Gather-kind slot does above (a
    // real bug this rewrite's own development caught: a scan positioned after the merged pair,
    // sharing the compacted gathers vector, silently read the WRONG table entry until this line
    // existed -- tests/rewrite/r5_group_formation_e0_test.cpp's Stage A gate is what caught it).
    sc.carry_gather = remap_gather_index(sc.carry_gather);
  }

  out.inputs.reserve(src.inputs.size());
  for (value_id v : src.inputs) out.inputs.push_back(remap_value(v));
  out.input_values = src.input_values;
  out.outputs.reserve(src.outputs.size());
  for (value_id v : src.outputs) out.outputs.push_back(remap_value(v));

  recompute_reads(out);
  return out;
}

}  // namespace epykos::rewrite::detail
