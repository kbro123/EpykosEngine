#include "epykos/rewrite/r7_block_linmap.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "epykos/mutation/mutation.hpp"

namespace epykos::rewrite {

namespace {

// Recomputes Domain::reads and Domain::recurrent for every domain of `p` from its groups' own
// Gather / Segment slot references (mirrors src/ir/signature.cpp's own derivation exactly: "does
// this domain's rows read this domain?"). Used after split_linmap_domain's domain insertion:
// simplest-correct way to fix up every OTHER domain's `reads` that may have named the
// now-split domain by its old, single id (a downstream reader may now span more than one of the
// new blocks) without threading an id-remap through the whole program by hand.
void recompute_reads(ir::Program& p) {
  const std::size_t nd = p.domains.size();
  for (std::size_t d = 0; d < nd; ++d) {
    std::vector<std::uint8_t> seen(nd, 0);
    std::vector<ir::domain_id> reads;
    auto note = [&](ir::value_id v) {
      const std::size_t sd = static_cast<std::size_t>(p.domain_of(v));
      if (!seen[sd]) {
        seen[sd] = 1;
        reads.push_back(static_cast<ir::domain_id>(sd));
      }
    };
    for (const ir::Step& st : p.groups[d].steps) {
      for (const ir::Slot* sl : {&st.a, &st.b, &st.c, &st.konst}) {
        if (sl->kind == ir::SlotKind::Gather) {
          for (ir::value_id v : p.gathers[static_cast<std::size_t>(sl->index)].index) note(v);
        } else if (sl->kind == ir::SlotKind::Segment) {
          for (ir::value_id v : p.segments[static_cast<std::size_t>(sl->index)].members) note(v);
        }
      }
    }
    std::sort(reads.begin(), reads.end());
    p.domains[d].reads = std::move(reads);
    p.domains[d].recurrent = seen[d] != 0;
  }
}

}  // namespace

bool is_linmap_domain(const ir::Program& program, ir::domain_id d) noexcept {
  if (d < 0 || static_cast<std::size_t>(d) >= program.domains.size()) return false;
  if (program.domains[static_cast<std::size_t>(d)].recurrent) return false;
  const ir::Group& g = program.groups[static_cast<std::size_t>(d)];
  return g.steps.size() == 1 && g.steps[0].op == Op::Affine;
}

std::vector<LinmapBlock> linmap_blocks(const ir::Program& program, ir::domain_id d) {
  std::vector<LinmapBlock> blocks;
  if (!is_linmap_domain(program, d)) return blocks;
  const ir::Domain& dom = program.domains[static_cast<std::size_t>(d)];
  const ir::Segment& seg = program.segments[static_cast<std::size_t>(program.groups[static_cast<std::size_t>(d)].steps[0].a.index)];
  ir::domain_id block_src = -1;
  ir::row_id block_hi = -1;  // one past the running block's highest row_of seen in block_src
  for (ir::row_id r = 0; r < dom.rows; ++r) {
    const std::int32_t lo = seg.offsets[static_cast<std::size_t>(r)];
    const std::int32_t hi = seg.offsets[static_cast<std::size_t>(r) + 1];
    ir::domain_id src = -1;
    ir::row_id rmin = std::numeric_limits<ir::row_id>::max();
    ir::row_id rmax = -1;
    bool uniform = true;
    for (std::int32_t m = lo; m < hi; ++m) {
      const ir::value_id v = seg.members[static_cast<std::size_t>(m)];
      const ir::domain_id sd = program.domain_of(v);
      if (src < 0) {
        src = sd;
      } else if (sd != src) {
        uniform = false;
      }
      const ir::row_id rr = program.row_of(v);
      rmin = std::min(rmin, rr);
      rmax = std::max(rmax, rr);
    }
    const ir::domain_id row_src = uniform ? src : -1;
    const bool same_source = !blocks.empty() && block_src >= 0 && row_src == block_src;
    const bool extends = same_source && rmin < block_hi;
    if (blocks.empty() || !extends) {
      blocks.push_back(LinmapBlock{r, r + 1});
      block_src = row_src;
      block_hi = rmax + 1;
    } else {
      blocks.back().hi = r + 1;
      block_hi = std::max(block_hi, rmax + 1);
    }
  }
  return blocks;
}

ir::Program split_linmap_domain(const ir::Program& program, ir::domain_id d, const std::vector<LinmapBlock>& blocks) {
  if (!is_linmap_domain(program, d)) throw std::invalid_argument("split_linmap_domain: domain is not a linmap");
  if (blocks.empty()) throw std::invalid_argument("split_linmap_domain: no blocks given");
  const std::size_t ud = static_cast<std::size_t>(d);
  const ir::Domain dom0 = program.domains[ud];
  {
    ir::row_id expect = 0;
    for (const LinmapBlock& b : blocks) {
      if (b.lo != expect || b.hi <= b.lo) throw std::invalid_argument("split_linmap_domain: blocks are not an ascending, gap-free partition");
      expect = b.hi;
    }
    if (expect != dom0.rows) throw std::invalid_argument("split_linmap_domain: blocks do not cover every row of the domain");
  }
  const std::size_t k = blocks.size();
  if (k == 1) return program;  // nothing to split

  ir::Program out = program;
  const std::int32_t delta = static_cast<std::int32_t>(k) - 1;

  // Snapshots of the ORIGINAL segment / (optional) column, taken before block 0's in-place
  // truncation (step 2) so blocks 1..k-1 (step 3) can still slice the untouched originals.
  const ir::Step step0 = program.groups[ud].steps[0];
  const ir::Segment seg0 = program.segments[static_cast<std::size_t>(step0.a.index)];
  const bool has_column_konst = step0.konst.kind == ir::SlotKind::Column;
  const ir::Column col0 = has_column_konst ? program.columns[static_cast<std::size_t>(step0.konst.index)] : ir::Column{};

  // 1) Every EXISTING column/gather/segment/scan positioned after `d` moves with its domain.
  //    Block 0 keeps id `d` (it is spliced in at exactly that position, step 4), so `> d`, not
  //    `>= d`, is the correct cut here.
  for (ir::Column& c : out.columns) {
    if (c.domain > d) c.domain += delta;
  }
  for (ir::Gather& g : out.gathers) {
    if (g.domain > d) g.domain += delta;
  }
  for (ir::Segment& s : out.segments) {
    if (s.domain > d) s.domain += delta;
  }
  for (ir::Scan& sc : out.scans) {
    if (sc.domain > d) sc.domain += delta;
  }

  // 2) Block 0: truncate the domain / segment / column already at `d` in place. Row order and
  //    value ids of rows [0, blocks[0].hi) are untouched; only the row COUNT the header claims
  //    shrinks, and the segment/column tables shrink to match.
  {
    const std::int32_t rows0 = blocks.front().hi;  // blocks.front().lo == 0 (checked above)
    out.domains[ud].rows = rows0;
    ir::Segment& seg = out.segments[static_cast<std::size_t>(step0.a.index)];
    seg.offsets.resize(static_cast<std::size_t>(rows0) + 1);
    const std::int32_t nmem = seg.offsets[static_cast<std::size_t>(rows0)];
    seg.members.resize(static_cast<std::size_t>(nmem));
    if (!seg.coefs.empty()) seg.coefs.resize(static_cast<std::size_t>(nmem));
    if (has_column_konst) {
      out.columns[static_cast<std::size_t>(step0.konst.index)].values.resize(static_cast<std::size_t>(rows0));
    }
  }

  // 3) Blocks 1..k-1: fresh Segment / (optional) Column / Domain / Group, sliced from the
  //    snapshots taken in step 0, at their final domain ids d+1 .. d+k-1.
  std::vector<ir::Domain> new_domains;
  std::vector<ir::Group> new_groups;
  new_domains.reserve(k - 1);
  new_groups.reserve(k - 1);
  ir::value_id running_base = out.domains[ud].value_base + out.domains[ud].rows;
  for (std::size_t bi = 1; bi < k; ++bi) {
    const LinmapBlock& b = blocks[bi];
    const std::int32_t rows_b = b.hi - b.lo;
    const ir::domain_id new_id = d + static_cast<ir::domain_id>(bi);

    ir::Segment seg;
    seg.domain = new_id;
    const std::int32_t off_lo = seg0.offsets[static_cast<std::size_t>(b.lo)];
    const std::int32_t off_hi = seg0.offsets[static_cast<std::size_t>(b.hi)];
    seg.offsets.resize(static_cast<std::size_t>(rows_b) + 1);
    for (std::int32_t r = 0; r <= rows_b; ++r) {
      const std::int32_t absolute = seg0.offsets[static_cast<std::size_t>(b.lo + r)];
      if (epykos::mutant("r7.no_offset_rebase")) {
        // Defect: the new segment's offsets keep the ORIGINAL (whole-domain) absolute values
        // instead of being rebased to the slice's own `members`/`coefs` arrays (which start at
        // index 0, not at off_lo) -- every row but the block's first then reads members::at() out
        // of range, or (past the end of a shorter `members`) silently the wrong ones.
        seg.offsets[static_cast<std::size_t>(r)] = absolute;
      } else {
        seg.offsets[static_cast<std::size_t>(r)] = absolute - off_lo;
      }
    }
    seg.members.assign(seg0.members.begin() + off_lo, seg0.members.begin() + off_hi);
    if (!seg0.coefs.empty()) seg.coefs.assign(seg0.coefs.begin() + off_lo, seg0.coefs.begin() + off_hi);
    out.segments.push_back(std::move(seg));
    const std::int32_t new_seg_index = static_cast<std::int32_t>(out.segments.size()) - 1;

    ir::Slot konst_slot = step0.konst;  // a Literal is global/shared: every block reuses it as-is
    if (has_column_konst) {
      ir::Column col;
      col.domain = new_id;
      col.values.assign(col0.values.begin() + b.lo, col0.values.begin() + b.hi);
      out.columns.push_back(std::move(col));
      konst_slot = ir::Slot{ir::SlotKind::Column, static_cast<std::int32_t>(out.columns.size()) - 1};
    }

    ir::Domain nd;
    nd.name = dom0.name;  // identical op shape (one Affine step); shape_string renders the same
    nd.rows = rows_b;
    nd.value_base = running_base;
    nd.level = dom0.level;
    nd.recurrent = false;
    nd.scan_class = false;
    nd.scan = -1;
    // reads: left empty; recompute_reads (step 5) derives it from the sliced segment above.
    new_domains.push_back(std::move(nd));

    ir::Group ng;
    ng.domain = new_id;
    ir::Step ns;
    ns.op = Op::Affine;
    ns.a = ir::Slot{ir::SlotKind::Segment, new_seg_index};
    ns.konst = konst_slot;
    ng.steps.push_back(ns);
    new_groups.push_back(std::move(ng));

    if (epykos::mutant("r7.wrong_block_value_base")) {
      // Defect: the running value-base accumulator is advanced by the block's SEGMENT length
      // (its members count) instead of its ROW count -- every block from the second on claims a
      // value_base that does not equal the sum of the earlier blocks' rows, so validate()'s own
      // "value_base is not contiguous" check fails, and (were that check skipped) every gather
      // elsewhere in the program that reads one of this domain's rows by value id would resolve
      // to the wrong row of the wrong block.
      running_base += static_cast<ir::value_id>(out.segments.back().members.size());
    } else {
      running_base += rows_b;
    }
  }

  // 4) Splice the new blocks in right after position `d` (block 0, already in place there).
  out.domains.insert(out.domains.begin() + d + 1, new_domains.begin(), new_domains.end());
  out.groups.insert(out.groups.begin() + d + 1, new_groups.begin(), new_groups.end());

  // 5) Group::domain is positional by convention (program.hpp: "groups[d] belongs to
  //    domains[d]"); reset it everywhere rather than tracking the shift by hand. Domain::reads
  //    (and, defensively, ::recurrent) are recomputed from the actual value ids for the same
  //    reason: correct regardless of how many blocks a downstream reader's gathers now span.
  for (std::size_t i = 0; i < out.groups.size(); ++i) out.groups[i].domain = static_cast<ir::domain_id>(i);
  recompute_reads(out);

  return out;
}

R7BlockLinmap::R7BlockLinmap(BlockLinmapParams params) : params_(params), name_("r7.block_linmap") {}

const std::string& R7BlockLinmap::name() const noexcept { return name_; }

std::vector<MatchSite> R7BlockLinmap::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  std::vector<MatchSite> sites;
  for (std::size_t d = 0; d < program.domains.size(); ++d) {
    const ir::domain_id did = static_cast<ir::domain_id>(d);
    if (!is_linmap_domain(program, did)) continue;
    const std::vector<LinmapBlock> blocks = linmap_blocks(program, did);
    if (blocks.size() < 2) continue;  // already one block: nothing for R7 to expose
    bool all_big_enough = true;
    for (const LinmapBlock& b : blocks) all_big_enough = all_big_enough && (b.hi - b.lo) >= params_.min_block_rows;
    if (!all_big_enough) continue;  // simplest policy: all-or-nothing rather than merging small blocks into a neighbour
    sites.push_back(MatchSite::of_domain(did));
  }
  return sites;
}

Proposal R7BlockLinmap::propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite& site) const {
  Proposal p;
  if (site.kind != SiteKind::Domain || site.domain < 0) return p;
  const std::vector<LinmapBlock> blocks = linmap_blocks(program, site.domain);
  if (blocks.size() < 2) return p;  // the program changed since match(); nothing to propose at a stale site
  p.program = split_linmap_domain(program, site.domain, blocks);
  return p;
}

}  // namespace epykos::rewrite
