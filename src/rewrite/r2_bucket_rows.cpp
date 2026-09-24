#include "epykos/rewrite/r2_bucket_rows.hpp"

#include <cstdint>
#include <cstring>
#include <utility>

#include "epykos/mutation/mutation.hpp"
#include "epykos/rewrite/ir_edit.hpp"

namespace epykos::rewrite {

namespace {

using detail::all_bit_identical;

std::size_t idx(std::int32_t i) noexcept { return static_cast<std::size_t>(i); }

std::uint64_t bits_of(double v) noexcept {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

bool eligible_domain(const ir::Domain& dom) { return !dom.recurrent && !dom.scan_class && dom.scan < 0; }

// One column domain `d` owns (used only to pick the bucketing signature -- run_buckets below;
// the actual split of every Column / Gather / Segment `d` owns is expand_owned's job, further
// down, which finds them itself so it can preserve their ARRAY POSITION relative to every entry
// NOT owned by `d`).
struct OwnedColumn {
  std::int32_t old_index = -1;
  ir::Column data;
};

std::vector<OwnedColumn> owned_columns_of(const ir::Program& program, ir::domain_id d) {
  std::vector<OwnedColumn> out;
  for (std::int32_t c = 0; c < static_cast<std::int32_t>(program.columns.size()); ++c) {
    if (program.columns[idx(c)].domain == d) out.push_back({c, program.columns[idx(c)]});
  }
  return out;
}

// Maximal contiguous runs of matching joint signature over `owned`'s NOT-already-uniform columns
// (an already-uniform column adds nothing to the key: R1 already can fold it regardless of which
// bucket a row lands in). Empty: no varying column to bucket on, or bucketing would not consolidate
// anything (every run has length 1 -- as many buckets as rows, no gain over the domain as it is).
std::vector<std::pair<std::int32_t, std::int32_t>> run_buckets(std::int32_t rows, const std::vector<OwnedColumn>& owned) {
  std::vector<const ir::Column*> varying;
  for (const OwnedColumn& oc : owned) {
    if (!all_bit_identical(oc.data.values)) varying.push_back(&oc.data);
  }
  if (varying.empty()) return {};

  // Bit pattern, not IEEE ==, matching R1's own uniformity check (all_bit_identical): grouping
  // never changes a computed VALUE either way (every row keeps its own column/gather/segment
  // slice regardless of which bucket it lands in -- see run_buckets's own header comment), but
  // bit-pattern equality keeps "signature" a well-defined, total order-independent partition even
  // over a NaN or a signed zero, rather than leaning on the (also safe, but easy to misread) fact
  // that IEEE `!=` would happen to do no harm here.
  auto same_signature = [&](std::int32_t r1, std::int32_t r2) {
    for (const ir::Column* col : varying) {
      if (bits_of(col->values[idx(r1)]) != bits_of(col->values[idx(r2)])) return false;
    }
    return true;
  };
  std::vector<std::pair<std::int32_t, std::int32_t>> ranges;
  std::int32_t lo = 0;
  for (std::int32_t r = 1; r <= rows; ++r) {
    bool boundary = (r == rows);
    if (!boundary) {
      const bool cmp_wrong_row = epykos::mutant("r2.wrong_run_boundary") && r >= 2;
      // Mutant r2.wrong_run_boundary: compares row r against r-2 instead of r-1, skipping a row --
      // a run boundary can be missed or invented one row early. The resulting ranges are still a
      // complete, non-overlapping partition of [0, rows) (this loop always closes a run at `r ==
      // rows` regardless), so this alone changes only how many buckets form, not any value; it is
      // caught together with r2.column_slice_uses_wrong_bucket below by the SAME differential
      // test, whose synthetic domain has a THIRD column whose per-row value the mutant's coarser
      // grouping would place in the wrong bucket if that bucket's slice boundaries did not line up
      // with this function's own ranges -- see r2_bucket_rows_e0_test.cpp.
      boundary = !same_signature(r, cmp_wrong_row ? r - 2 : r - 1);
    }
    if (boundary) {
      ranges.emplace_back(lo, r);
      lo = r;
    }
  }
  // Reject a split with ANY singleton run: DESIGN.md §6's own point for R2 is forming the "per-kind
  // BATCHES a hand-written kernel would form" (this file's own header) -- a run of exactly one row
  // is not a kind's batch, it is one row, and this landing does not additionally try to fold it into
  // a neighbouring bucket. This is also a safety gate, not just a quality one: measured directly,
  // splitting a 177-row Stage A domain into 168 buckets (159 of them singletons) crashes building
  // the SPLIT program's adjoint::Adjoint (a heap-corruption-shaped SIGABRT some way past the actual
  // fault, src/adjoint/plan.cpp / adjoint_e0.cpp, not this package's code) -- yet the SAME shape (a
  // 16,103-row M1 book domain splitting into 9,152 buckets, most of them singletons too) builds and
  // runs correctly, bit-exact including the adjoint, at the record point AND over the M1 differential
  // ball. The M1 program has no scan domain; the Stage A tape has five. This package did not have
  // the budget to chase that difference into src/adjoint/ (code R-a does not own) to confirm it is
  // the cause rather than something else Stage-A-scale-specific, so it closes the whole shape off by
  // construction instead of shipping a rule that can crash: R-a's notes flag this as a finding for
  // the adjoint's own owners, with the reproduction above.
  for (const auto& [lo, hi] : ranges) {
    if (hi - lo < 2) return {};
  }
  return ranges;
}

// Walks `src` in array order; an entry NOT owned by `d` is kept, UNMOVED relative to every other
// kept entry (`other_map[old] = its new index`); an entry owned by `d` (`.domain == d`) is
// replaced, AT THAT SAME POSITION, by B entries built by `make(old_index, item, bucket)`
// (`bucket_map[k][old] = that bucket's new index`). This -- not compacting `d`'s own entries away
// and appending B fresh ones at the tail -- is the point: `adjoint::build_plan` (src/adjoint/plan.cpp)
// builds each value's reader list by scanning `Program::gathers` / `Program::segments` in ARRAY
// ORDER, so an upstream value read by both one of `d`'s gathers and some UNRELATED gather
// accumulates its adjoint in THAT scan order -- moving `d`'s entries to the tail would reorder that
// sum (still mathematically the same total, but not the same BITS, an E0 gate this rule must pass).
template <class T, class Owned, class Make>
std::vector<T> expand_owned(const std::vector<T>& src, std::int32_t B, Owned owned, Make make, std::vector<std::int32_t>& other_map,
                            std::vector<std::vector<std::int32_t>>& bucket_map) {
  std::vector<T> out;
  out.reserve(src.size() + idx(B) - 1);
  other_map.assign(src.size(), -1);
  bucket_map.assign(idx(B), std::vector<std::int32_t>(src.size(), -1));
  for (std::size_t i = 0; i < src.size(); ++i) {
    if (owned(src[i])) {
      for (std::int32_t k = 0; k < B; ++k) {
        out.push_back(make(static_cast<std::int32_t>(i), src[i], k));
        bucket_map[idx(k)][i] = static_cast<std::int32_t>(out.size()) - 1;
      }
    } else {
      other_map[i] = static_cast<std::int32_t>(out.size());
      out.push_back(src[i]);
    }
  }
  return out;
}

}  // namespace

const std::string& R2BucketRows::name() const noexcept {
  static const std::string n = "r2.bucket_rows";
  return n;
}

std::vector<MatchSite> R2BucketRows::match(const ir::Program& program, const ir::PlanAnnotations&) const {
  std::vector<MatchSite> sites;
  for (std::int32_t d = 0; d < static_cast<std::int32_t>(program.domains.size()); ++d) {
    if (!eligible_domain(program.domains[idx(d)])) continue;
    if (!run_buckets(program.domains[idx(d)].rows, owned_columns_of(program, d)).empty()) sites.push_back(MatchSite::of_domain(d));
  }
  return sites;
}

Proposal R2BucketRows::propose(const ir::Program& program, const ir::PlanAnnotations&, const MatchSite& site) const {
  const ir::domain_id d = site.domain;
  const ir::Domain dom = program.domains[idx(d)];
  const ir::Group group = program.groups[idx(d)];

  const std::vector<OwnedColumn> owned_columns = owned_columns_of(program, d);
  const std::vector<std::pair<std::int32_t, std::int32_t>> ranges = run_buckets(dom.rows, owned_columns);
  const std::int32_t B = static_cast<std::int32_t>(ranges.size());

  ir::Program out = program;
  // Domain ids > d shift by (B - 1) BEFORE any array is expanded (d's own entries, `.domain == d`
  // exactly, are untouched by this regardless of order -- see shift_domain_ids's own contract).
  detail::shift_domain_ids(out, d, B - 1);

  auto slice = [&](std::int32_t src_k) { return ranges[idx((epykos::mutant("r2.column_slice_uses_wrong_bucket") && src_k > 0) ? src_k - 1 : src_k)]; };
  // Mutant r2.column_slice_uses_wrong_bucket: every bucket after the first slices the PREVIOUS
  // bucket's row range instead of its own, so bucket k (k >= 1) carries bucket (k-1)'s data (and,
  // since it is still sized to bucket k's OWN row count, a column that is too short for its
  // domain -- ir::validate rejects it outright).

  std::vector<std::int32_t> col_other, gath_other, seg_other;
  std::vector<std::vector<std::int32_t>> col_bucket, gath_bucket, seg_bucket;
  out.columns = expand_owned(
      out.columns, B, [&](const ir::Column& c) { return c.domain == d; },
      [&](std::int32_t, const ir::Column& c, std::int32_t k) {
        const auto [lo, hi] = slice(k);
        ir::Column nc;
        nc.domain = d + k;
        nc.values.assign(c.values.begin() + lo, c.values.begin() + hi);
        return nc;
      },
      col_other, col_bucket);
  out.gathers = expand_owned(
      out.gathers, B, [&](const ir::Gather& g) { return g.domain == d; },
      [&](std::int32_t, const ir::Gather& g, std::int32_t k) {
        const auto [lo, hi] = slice(k);
        ir::Gather ng;
        ng.domain = d + k;
        ng.index.assign(g.index.begin() + lo, g.index.begin() + hi);
        return ng;
      },
      gath_other, gath_bucket);
  out.segments = expand_owned(
      out.segments, B, [&](const ir::Segment& s) { return s.domain == d; },
      [&](std::int32_t, const ir::Segment& s, std::int32_t k) {
        const auto [lo, hi] = slice(k);
        ir::Segment ns;
        ns.domain = d + k;
        const std::int32_t m_lo = s.offsets[idx(lo)], m_hi = s.offsets[idx(hi)];
        ns.offsets.assign(s.offsets.begin() + lo, s.offsets.begin() + hi + 1);
        for (std::int32_t& o : ns.offsets) o -= m_lo;
        ns.members.assign(s.members.begin() + m_lo, s.members.begin() + m_hi);
        if (!s.coefs.empty()) ns.coefs.assign(s.coefs.begin() + m_lo, s.coefs.begin() + m_hi);
        return ns;
      },
      seg_other, seg_bucket);

  // Every OTHER domain's group referenced d's own entries nowhere (ir::validate's own ownership
  // rule); only its NON-owned Column/Gather/Segment slots ever need the plain shift.
  for (ir::Group& g : out.groups) {
    if (g.domain == d) continue;
    for (ir::Step& s : g.steps) {
      for (ir::Slot* sl : {&s.a, &s.b, &s.c, &s.konst}) {
        if (sl->kind == ir::SlotKind::Column) sl->index = col_other[idx(sl->index)];
        else if (sl->kind == ir::SlotKind::Gather) sl->index = gath_other[idx(sl->index)];
        else if (sl->kind == ir::SlotKind::Segment) sl->index = seg_other[idx(sl->index)];
      }
    }
  }
  for (ir::Scan& sc : out.scans) sc.carry_gather = gath_other[idx(sc.carry_gather)];  // d is never a scan domain

  auto remap_for_bucket = [&](ir::Slot s, std::int32_t k) {
    if (s.kind == ir::SlotKind::Column) return ir::Slot{ir::SlotKind::Column, col_bucket[idx(k)][idx(s.index)]};
    if (s.kind == ir::SlotKind::Gather) return ir::Slot{ir::SlotKind::Gather, gath_bucket[idx(k)][idx(s.index)]};
    if (s.kind == ir::SlotKind::Segment) return ir::Slot{ir::SlotKind::Segment, seg_bucket[idx(k)][idx(s.index)]};
    return s;  // Step / Literal / Input / None: identical for every bucket
  };

  std::vector<ir::Domain> new_domains(idx(B));
  std::vector<ir::Group> new_groups(idx(B));
  for (std::int32_t k = 0; k < B; ++k) {
    const auto [lo, hi] = ranges[idx(k)];
    ir::Domain nd;
    nd.name = dom.name;
    nd.rows = hi - lo;
    nd.value_base = dom.value_base + lo;
    nd.level = dom.level;
    nd.recurrent = false;
    nd.scan_class = false;
    nd.scan = -1;
    new_domains[idx(k)] = nd;

    ir::Group ng;
    ng.domain = d + k;
    ng.steps.reserve(group.steps.size());
    for (const ir::Step& s : group.steps) {
      ir::Step ns = s;
      ns.a = remap_for_bucket(s.a, k);
      ns.b = remap_for_bucket(s.b, k);
      ns.c = remap_for_bucket(s.c, k);
      ns.konst = remap_for_bucket(s.konst, k);
      ng.steps.push_back(ns);
    }
    new_groups[idx(k)] = std::move(ng);
  }

  std::vector<ir::Domain> domains;
  std::vector<ir::Group> groups;
  domains.reserve(out.domains.size() - 1 + idx(B));
  groups.reserve(out.groups.size() - 1 + idx(B));
  for (std::int32_t i = 0; i < d; ++i) {
    domains.push_back(out.domains[idx(i)]);
    groups.push_back(out.groups[idx(i)]);
  }
  for (std::int32_t k = 0; k < B; ++k) {
    domains.push_back(std::move(new_domains[idx(k)]));
    groups.push_back(std::move(new_groups[idx(k)]));
  }
  for (std::size_t i = idx(d) + 1; i < out.domains.size(); ++i) {
    domains.push_back(out.domains[i]);
    groups.push_back(out.groups[i]);
  }
  out.domains = std::move(domains);
  out.groups = std::move(groups);

  detail::recompute_reads(out);

  Proposal p;
  p.program = std::move(out);
  return p;
}

}  // namespace epykos::rewrite
