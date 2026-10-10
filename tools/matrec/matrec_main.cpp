// tools/matrec — DOES READER COUNT PREDICT MATERIALISE-VS-RECOMPUTE? THE STRUCTURAL HALF.
//
// D31 chose, for `adjoint::Adjoint`, that the forward stores every ROW VALUE (the last step of
// every group) and that a group's INTERMEDIATE steps 0 .. last-1 are recomputed per tile in the
// reverse. `plan.hpp`'s header states it, `adjoint.hpp`'s cites it, and D103 §3 priced the
// consequence as a candidate: per operation the reverse runs ~1.90x less efficiently than the
// forward, with recomputed intermediates never entering an op count.
//
// The question this tool answers is NOT "what is right for Stage A". It is whether a DECISION
// PROCEDURE could be generic — whether some structural signal the engine already holds (reader
// count, cone cost, reuse distance, working-set size) separates the values that should be stored
// from the values that should be recomputed, at BOTH ends of the sharing spectrum:
//
//   high sharing  compare_ois / stage_a   — 1,000+ trades over one curve, D81's collapse 2,565x
//   no sharing    exotic_path (D106)      — one row per path-step, the same collapse 1.22x
//
// THE FIRST THING IT MEASURES IS WHETHER THE SIGNAL IS EVEN DEFINED ON THE RIGHT SET. The engine's
// reader count lives in `AdjointPlan`'s four "who reads me" CSR lists, which are indexed by VALUE
// ID. An intra-chain step has no value id, so it has no entry in any of them: the signal the
// engine holds is defined exactly on the class that is unconditionally materialised, and is
// silent on the class that is unconditionally recomputed. This tool therefore computes a SECOND
// reader count the engine does not hold — a step's readers inside its own group — and reports the
// two distributions side by side.
//
// NO TIMING. Every number here is a count, a byte size or a ratio, so it is quotable on a loaded
// box and this tool does not read the load or refuse one — the position tools/sparsity/,
// tools/coverage/ and tools/revcollapse/ take, and the opposite of tools/ladder/,
// tools/pullprobe/ and tools/exotics/. The timing half is tools/matprobe/.
//
// WHAT "FORWARD HALF" AND "REVERSE HALF" MEAN HERE. For the runtime there is one program P and
// `Adjoint::run` walks it twice, so "halves" are passes, not domains, and the per-pass columns
// below are labelled as such. For a domain-level reader count of the REVERSE as an object, the
// tool also builds Q = `adjoint(P)` (invariant I1, D98-D100) and splits Q's domains by the cone of
// P's own outputs — Q's forward half is that cone, its reverse half is everything else, the same
// split tools/revcollapse/ makes at tape level.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "epykos/adjoint/adjoint.hpp"
#include "epykos/adjoint/plan.hpp"
#include "epykos/compile.hpp"
#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/exotic_path.hpp"
#include "epykos/fixtures/spike_telescoped.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/tape.hpp"

namespace {

using namespace epykos;
using ir::Domain;
using ir::Group;
using ir::Program;
using ir::Slot;
using ir::SlotKind;
using ir::Step;

inline std::size_t idx(std::int32_t i) { return static_cast<std::size_t>(i); }

// ---- a weighted distribution, reported as a distribution and not as a mean -------------------
//
// A median hides a bimodal split and a bimodal split is exactly what would make a simple rule
// work, so every histogram prints its whole head plus the tail quantiles.
struct Hist {
  std::map<long long, long long> w;  // observed value -> weight (instances)

  void add(long long v, long long weight = 1) {
    if (weight > 0) w[v] += weight;
  }
  long long total() const {
    long long s = 0;
    for (const auto& kv : w) s += kv.second;
    return s;
  }
  double mean() const {
    long long n = 0;
    double s = 0;
    for (const auto& kv : w) {
      n += kv.second;
      s += static_cast<double>(kv.first) * static_cast<double>(kv.second);
    }
    return n ? s / static_cast<double>(n) : 0.0;
  }
  long long quantile(double q) const {
    const long long n = total();
    if (n == 0) return 0;
    const long long want = static_cast<long long>(std::ceil(q * static_cast<double>(n)));
    long long seen = 0;
    for (const auto& kv : w) {
      seen += kv.second;
      if (seen >= want) return kv.first;
    }
    return w.rbegin()->first;
  }
  long long max() const { return w.empty() ? 0 : w.rbegin()->first; }
  // The share of the total carried by the single most common observation: how close the
  // distribution is to a constant. A signal whose mass is one value carries no information.
  double modal_share() const {
    const long long n = total();
    if (n == 0) return 0.0;
    long long best = 0;
    for (const auto& kv : w) best = std::max(best, kv.second);
    return static_cast<double>(best) / static_cast<double>(n);
  }
  long long modal_value() const {
    long long best = 0, bv = 0;
    for (const auto& kv : w) {
      if (kv.second > best) {
        best = kv.second;
        bv = kv.first;
      }
    }
    return bv;
  }
};

void print_hist(const char* label, const Hist& h, int head = 6) {
  const long long n = h.total();
  if (n == 0) {
    std::printf("    %-26s  (empty)\n", label);
    return;
  }
  std::printf("    %-26s n=%-10lld mean %8.3f  p50 %6lld  p90 %6lld  p99 %6lld  max %8lld   "
              "modal %lld at %.2f%%\n",
              label, n, h.mean(), h.quantile(0.50), h.quantile(0.90), h.quantile(0.99), h.max(),
              h.modal_value(), 100.0 * h.modal_share());
  std::printf("      %-24s", "");
  int shown = 0;
  for (const auto& kv : h.w) {
    if (shown++ >= head) break;
    std::printf("  %lld:%.2f%%", kv.first, 100.0 * static_cast<double>(kv.second) / static_cast<double>(n));
  }
  if (static_cast<int>(h.w.size()) > head) {
    long long rest = 0;
    int i = 0;
    for (const auto& kv : h.w) {
      if (i++ >= head) rest += kv.second;
    }
    std::printf("  (%zu more buckets, %.2f%%)", h.w.size() - static_cast<std::size_t>(head),
                100.0 * static_cast<double>(rest) / static_cast<double>(n));
  }
  std::printf("\n");
}

// ---- the cost of a step, as the runtime pays it ----------------------------------------------
//
// Not a tape-node count. `adjoint_e0.cpp`'s `forward_step` is the authority: a Sum over a segment
// of m members is m loads and m-1 adds, an Affine is m loads, m multiplies and m adds plus the
// konst load, and an elementwise op is one kernel call over its already-loaded operands. Loads
// and flops are kept apart because D105 measured that an operation count which treats a store and
// a branch as commensurable can get the SIGN wrong.
struct Cost {
  long long flops = 0;   // adds, multiplies, divides, selects, compares
  long long trans = 0;   // exp / log / sqrt: a libm call, never commensurable with an add
  long long loads = 0;   // operand element loads (gather / column / literal / segment member)

  Cost& operator+=(const Cost& o) {
    flops += o.flops;
    trans += o.trans;
    loads += o.loads;
    return *this;
  }
  // One scalar for ordering cones. Labelled an ESTIMATE wherever it is used to rank: the weights
  // are nominal, not calibrated, and D105 is the standing warning against reading a time off them.
  double scalar() const {
    return static_cast<double>(flops) + 20.0 * static_cast<double>(trans) + 0.5 * static_cast<double>(loads);
  }
};

bool is_loadable(const Slot& s) {
  return s.kind == SlotKind::Literal || s.kind == SlotKind::Column || s.kind == SlotKind::Gather;
}

// The cost of step `s` of a group of domain `d` for ONE row `r`.
Cost step_cost(const Program& p, const Step& s, int r) {
  Cost c;
  switch (s.op) {
    case Op::Const:
      c.loads = 1;
      return c;
    case Op::Input:
      c.loads = 1;
      return c;
    case Op::Sum:
    case Op::Affine: {
      if (ir::is_fixed_sum(s)) {
        const int ar = ir::fixed_sum_arity(s);
        c.flops = ar - 1;
        c.loads = (is_loadable(s.a) ? 1 : 0) + (is_loadable(s.b) ? 1 : 0) + (is_loadable(s.c) ? 1 : 0);
        return c;
      }
      const ir::Segment& seg = p.segments[idx(s.a.index)];
      const long long m = seg.offsets[idx(r) + 1] - seg.offsets[idx(r)];
      c.loads = m;
      if (s.op == Op::Affine) {
        c.loads += 1;          // the konst (Literal or Column)
        c.flops = 2 * m;       // one multiply and one add per member
      } else {
        c.flops = m > 0 ? m - 1 : 0;
      }
      return c;
    }
    case Op::Exp:
    case Op::Log:
    case Op::Sqrt:
      c.trans = 1;
      c.loads = is_loadable(s.a) ? 1 : 0;
      return c;
    case Op::Fma:
      c.flops = 2;
      c.loads = (is_loadable(s.a) ? 1 : 0) + (is_loadable(s.b) ? 1 : 0) + (is_loadable(s.c) ? 1 : 0);
      return c;
    default:
      c.flops = 1;
      c.loads = (is_loadable(s.a) ? 1 : 0) + (is_loadable(s.b) ? 1 : 0) + (is_loadable(s.c) ? 1 : 0);
      return c;
  }
}

void step_slots(const Step& s, Slot out[4]) {
  out[0] = s.a;
  out[1] = s.b;
  out[2] = s.c;
  out[3] = s.konst;
}

// ---- one fixture ------------------------------------------------------------------------------

struct Totals {
  long long fwd_values = 0;     // domain values: the always-MATERIALISED class
  long long fwd_steps = 0;      // intra-chain step instances: the always-RECOMPUTED class
  Cost value_cost;              // cost of the last step of every group, every row
  Cost intra_cost;              // cost of steps 0..last-1 of every group, every row
};

void measure(const std::string& name, Tape raw, int lane_tile, int tile) {
  compile(raw);
  raw.validate();
  const Program p = ir::infer(raw);
  const adjoint::AdjointPlan plan = adjoint::build_plan(p);

  adjoint::Options ao;
  ao.lane_tile = lane_tile;
  ao.max_batch = lane_tile;
  ao.tile = tile;
  const adjoint::Adjoint ad(p, ao);

  const std::size_t D = p.domains.size();
  const std::size_t N = p.num_values();
  const int L = lane_tile;

  std::printf("\n======================================== %s ========================================\n",
              name.c_str());
  std::printf("  P  %zu values / %zu domains / %zu gathers / %zu segments / %zu in / %zu out / "
              "%zu scans   (max_steps %d)\n",
              N, D, p.gathers.size(), p.segments.size(), p.inputs.size(), p.outputs.size(),
              ir::scan_domains(p).size(), plan.max_steps);

  // ---------------------------------------------------------------------------------------------
  // 1. READER COUNT, the two classes side by side.
  // ---------------------------------------------------------------------------------------------
  Hist rd_value;        // domain values: the plan's four CSR lists. ALWAYS MATERIALISED.
  Hist rd_intra;        // intra-chain steps, readers inside their own group. ALWAYS RECOMPUTED.
  Hist rd_value_kinds;  // how many of the four reader KINDS a value has (1..4)

  for (std::size_t v = 0; v < N; ++v) {
    const long long o = plan.output_offsets[v + 1] - plan.output_offsets[v];
    const long long g = plan.gather_offsets[v + 1] - plan.gather_offsets[v];
    const long long s = plan.sum_offsets[v + 1] - plan.sum_offsets[v];
    const long long a = plan.affine_offsets[v + 1] - plan.affine_offsets[v];
    rd_value.add(o + g + s + a);
    rd_value_kinds.add((o > 0) + (g > 0) + (s > 0) + (a > 0));
  }

  long long last_step_internally_read = 0;
  Totals tot;
  Hist cone_intra, cone_value, chain_len;
  Hist intra_fanin;  // how many LIVE values an intra-chain step's cone touches

  for (std::size_t d = 0; d < D; ++d) {
    const Domain& dom = p.domains[d];
    const Group& g = p.groups[d];
    const int nsteps = static_cast<int>(g.steps.size());
    const int last = nsteps - 1;
    const long long rows = dom.rows;
    chain_len.add(nsteps, rows);

    // In-group reader count per step: how many later steps read it through a Step slot.
    std::vector<long long> rc(idx(nsteps), 0);
    for (int j = 0; j < nsteps; ++j) {
      Slot sl[4];
      step_slots(g.steps[idx(j)], sl);
      for (const Slot& s : sl) {
        if (s.kind == SlotKind::Step) rc[idx(s.index)] += 1;
      }
    }
    if (rc[idx(last)] != 0) last_step_internally_read += rows;

    // The cone of each step back to the nearest live values: the transitive closure over Step
    // slots only, because every other operand kind (Gather, Segment, Column, Literal, Input) IS
    // a live value the reverse loads anyway. Costed at row 0 and at the median row, then summed
    // over all rows for the totals (a segment's member count varies by row).
    for (int k = 0; k < nsteps; ++k) {
      // cone(k) as a set of step indices
      std::vector<char> in_cone(idx(nsteps), 0);
      std::vector<int> stack{k};
      in_cone[idx(k)] = 1;
      while (!stack.empty()) {
        const int j = stack.back();
        stack.pop_back();
        Slot sl[4];
        step_slots(g.steps[idx(j)], sl);
        for (const Slot& s : sl) {
          if (s.kind == SlotKind::Step && !in_cone[idx(s.index)]) {
            in_cone[idx(s.index)] = 1;
            stack.push_back(s.index);
          }
        }
      }
      // Its cost and its live fan-in at row 0 (rows differ only through segment lengths).
      Cost cc;
      long long live_in = 0;
      for (int j = 0; j < nsteps; ++j) {
        if (!in_cone[idx(j)]) continue;
        cc += step_cost(p, g.steps[idx(j)], 0);
        Slot sl[4];
        step_slots(g.steps[idx(j)], sl);
        for (const Slot& s : sl) {
          if (s.kind == SlotKind::Gather || s.kind == SlotKind::Column || s.kind == SlotKind::Literal) live_in++;
          if (s.kind == SlotKind::Segment) live_in += p.segments[idx(s.index)].offsets[1] - p.segments[idx(s.index)].offsets[0];
        }
      }
      if (k == last) {
        cone_value.add(static_cast<long long>(std::llround(cc.scalar())), rows);
      } else {
        rd_intra.add(rc[idx(k)], rows);
        cone_intra.add(static_cast<long long>(std::llround(cc.scalar())), rows);
        intra_fanin.add(live_in, rows);
      }
    }

    // Exact totals over every row.
    for (int r = 0; r < dom.rows; ++r) {
      for (int k = 0; k < nsteps; ++k) {
        const Cost c = step_cost(p, g.steps[idx(k)], r);
        if (k == last) tot.value_cost += c;
        else tot.intra_cost += c;
      }
    }
    tot.fwd_values += rows;
    tot.fwd_steps += rows * static_cast<long long>(last);
  }

  std::printf("\n  1. READER COUNT -- and the two classes are not the same set\n");
  std::printf("    the ALWAYS-MATERIALISED class: %lld domain values (one per group's last step per row)\n",
              tot.fwd_values);
  print_hist("readers (plan CSR)", rd_value);
  print_hist("reader KINDS of 4", rd_value_kinds);
  std::printf("    the ALWAYS-RECOMPUTED class: %lld intra-chain step instances (steps 0..last-1)\n",
              tot.fwd_steps);
  std::printf("      -- these have NO value id, so they appear in NONE of the plan's four CSR lists.\n");
  print_hist("readers (inside group)", rd_intra);
  if (last_step_internally_read)
    std::printf("      NOTE %lld rows whose LAST step is also read inside its own group\n", last_step_internally_read);

  // ---------------------------------------------------------------------------------------------
  // 2. RECOMPUTE COST: the cone back to the nearest live values.
  // ---------------------------------------------------------------------------------------------
  std::printf("\n  2. RECOMPUTE COST -- the cone back to the nearest live values (cost scalar is an ESTIMATE:\n");
  std::printf("     flops + 20*trans + 0.5*loads, nominal weights, never a time -- D105)\n");
  print_hist("cone of an intra step", cone_intra);
  print_hist("cone of a domain value", cone_value);
  print_hist("live fan-in of a cone", intra_fanin);
  print_hist("group chain length", chain_len);
  std::printf("    totals, every row:  domain-value steps  flops %-12lld trans %-10lld loads %-12lld\n",
              tot.value_cost.flops, tot.value_cost.trans, tot.value_cost.loads);
  std::printf("                        intra-chain steps   flops %-12lld trans %-10lld loads %-12lld\n",
              tot.intra_cost.flops, tot.intra_cost.trans, tot.intra_cost.loads);
  {
    const double vt = tot.value_cost.scalar(), it = tot.intra_cost.scalar();
    std::printf("    THE RECOMPUTE THE REVERSE PAYS TODAY: %.1f%% of the forward's arithmetic "
                "(%.0f of %.0f), and %lld of %lld transcendentals -- D31's reason for the split\n",
                vt + it > 0 ? 100.0 * it / (vt + it) : 0.0, it, vt + it, tot.intra_cost.trans,
                tot.intra_cost.trans + tot.value_cost.trans);
  }

  // ---------------------------------------------------------------------------------------------
  // 3. REUSE DISTANCE under the shipped schedule.
  // ---------------------------------------------------------------------------------------------
  // The schedule is: forward domains 0..D-1 ascending, then reverse domains D-1..0 descending.
  // Position is measured in VALUE SLOTS TOUCHED, which is the residency-relevant unit: a domain
  // of `rows` rows advances the clock by `rows`. The forward occupies [0, N) and the reverse
  // [N, 2N), so the reverse of domain d ENDS at N + (N - value_base[d]).
  //
  // A domain value v of domain d is defined in the forward of d and last read at:
  //   * the reverse of d itself, when d's own reverse reads it -- the last step's rule needs y
  //     (Div / Exp / Sqrt / Recip), or d is a scan and its own recompute re-gathers its rows; or
  //   * otherwise the reverse of the SMALLEST d' > d that reads v, because the reverse runs
  //     descending and so visits a larger d' EARLIER.
  // The forward uses of v are all strictly earlier than any of these, so they never decide.
  std::vector<std::int32_t> min_reader(N, -1);  // smallest domain index > own that reads the value
  for (const ir::Gather& g : p.gathers) {
    for (ir::value_id v : g.index) {
      if (v < 0) continue;
      if (min_reader[idx(v)] < 0 || g.domain < min_reader[idx(v)]) min_reader[idx(v)] = g.domain;
    }
  }
  for (const ir::Segment& s : p.segments) {
    for (ir::value_id v : s.members) {
      if (v < 0) continue;
      if (min_reader[idx(v)] < 0 || s.domain < min_reader[idx(v)]) min_reader[idx(v)] = s.domain;
    }
  }

  Hist reuse_pct;  // reuse distance as a percentage of the whole two-pass timeline
  Hist reuse_mb;   // ... and in MB of value slots touched in between, at this lane width
  long long live_to_own_reverse = 0, live_to_other = 0, never_read = 0;
  const double timeline = 2.0 * static_cast<double>(N);
  for (std::size_t d = 0; d < D; ++d) {
    const Domain& dom = p.domains[d];
    const Group& g = p.groups[d];
    const Step& lastst = g.steps.back();
    const bool own_reverse_reads_y = lastst.op == Op::Div || lastst.op == Op::Exp ||
                                     lastst.op == Op::Sqrt || lastst.op == Op::Recip;
    const bool is_scan = dom.scan >= 0;
    for (int r = 0; r < dom.rows; ++r) {
      const std::size_t v = idx(dom.value_base) + idx(r);
      const double def = static_cast<double>(dom.value_base) + static_cast<double>(r);
      double last_use;
      if (own_reverse_reads_y || is_scan) {
        last_use = static_cast<double>(N) + (static_cast<double>(N) - static_cast<double>(dom.value_base));
        live_to_own_reverse++;
      } else if (min_reader[v] >= 0) {
        const std::int32_t dr = min_reader[v];
        last_use = static_cast<double>(N) +
                   (static_cast<double>(N) - static_cast<double>(p.domains[idx(dr)].value_base));
        if (dr == static_cast<std::int32_t>(d)) live_to_own_reverse++;
        else live_to_other++;
      } else {
        // Read by an output seed only: the pull happens in the reverse of its own domain.
        last_use = static_cast<double>(N) + (static_cast<double>(N) - static_cast<double>(dom.value_base));
        never_read++;
      }
      const double dist = last_use - def;
      reuse_pct.add(static_cast<long long>(std::llround(100.0 * dist / timeline)));
      reuse_mb.add(static_cast<long long>(std::llround(dist * static_cast<double>(L) * 8.0 / (1024.0 * 1024.0))));
    }
  }
  std::printf("\n  3. REUSE DISTANCE under the shipped schedule (forward ascending then reverse descending;\n");
  std::printf("     the unit is value slots touched, the timeline is 2N = %.0f slots)\n", timeline);
  print_hist("distance, pct of timeline", reuse_pct);
  print_hist("distance, MB of slots", reuse_mb);
  std::printf("    last reader is its OWN domain's reverse %lld (%.1f%%) | another domain's reverse %lld "
              "(%.1f%%) | gathered/segmented by nothing %lld (%.1f%%)\n",
              live_to_own_reverse, 100.0 * double(live_to_own_reverse) / double(N), live_to_other,
              100.0 * double(live_to_other) / double(N), never_read, 100.0 * double(never_read) / double(N));

  // ---------------------------------------------------------------------------------------------
  // 4. WORKING SET, per domain and per tile.
  // ---------------------------------------------------------------------------------------------
  Hist dom_rows, dom_kb;
  for (std::size_t d = 0; d < D; ++d) {
    dom_rows.add(p.domains[d].rows);
    dom_kb.add(static_cast<long long>(std::llround(double(p.domains[d].rows) * double(L) * 8.0 / 1024.0)));
  }
  const double mb = 1024.0 * 1024.0;
  // What a full materialisation of the intra-chain steps would cost, as D31 priced it:
  // sum over domains of (steps-1) * rows * L doubles.
  long long mat_doubles = 0;
  for (std::size_t d = 0; d < D; ++d) {
    const int last = static_cast<int>(p.groups[d].steps.size()) - 1;
    mat_doubles += static_cast<long long>(last) * static_cast<long long>(p.domains[d].rows) * L;
  }
  std::printf("\n  4. WORKING SET at lane_tile=%d, tile=%d\n", L, tile);
  print_hist("rows per domain", dom_rows);
  print_hist("KB per domain", dom_kb);
  std::printf("    values %.2f MB | edge slots %.2f MB | per-tile scratch %.1f KB | plan tables %.2f MB\n",
              double(ad.value_bytes()) / mb, double(ad.edge_bytes()) / mb,
              double(ad.scratch_bytes()) / 1024.0, double(ad.table_bytes()) / mb);
  std::printf("    MATERIALISING every intra-chain step instead: +%.2f MB "
              "(%.2fx the value buffer, %.0fx the per-tile scratch) -- D31 estimated 4-6x on M1\n",
              double(mat_doubles) * 8.0 / mb,
              ad.value_bytes() ? double(mat_doubles) * 8.0 / double(ad.value_bytes()) : 0.0,
              ad.scratch_bytes() ? double(mat_doubles) * 8.0 / double(ad.scratch_bytes()) : 0.0);

  // ---------------------------------------------------------------------------------------------
  // 5. Q = adjoint(P): the reverse AS AN OBJECT, so "reverse half" has domains to count.
  // ---------------------------------------------------------------------------------------------
  const Program q = ad.to_program();
  const std::size_t QD = q.domains.size(), QN = q.num_values();
  // Q's reader counts are computed here directly from Q's own gathers, segments and outputs,
  // NOT through `build_plan(q)`: Q contains a RECURRENT NON-SCAN domain by design (the reverse of
  // a scan, adjoint_to_program.hpp's "Scans (C3b)") and `build_plan` refuses exactly that shape.
  // So `adjoint(adjoint(P))` does not exist for a scanning program — a real closure hole, noted
  // here and not this tool's question.
  std::vector<long long> q_readers(QN, 0);
  for (ir::value_id v : q.outputs) {
    if (v >= 0) q_readers[idx(v)]++;
  }
  for (const ir::Gather& g : q.gathers) {
    for (ir::value_id v : g.index) {
      if (v >= 0) q_readers[idx(v)]++;
    }
  }
  for (const ir::Segment& s : q.segments) {
    for (ir::value_id v : s.members) {
      if (v >= 0) q_readers[idx(v)]++;
    }
  }
  // Q's forward half = the domain cone of P's own outputs (Q's first p.outputs.size() outputs).
  std::vector<char> fwd_half(QD, 0);
  {
    std::vector<ir::domain_id> stack;
    std::vector<std::int32_t> owner(QN, -1);
    for (std::size_t d = 0; d < QD; ++d) {
      for (int r = 0; r < q.domains[d].rows; ++r) owner[idx(q.domains[d].value_base) + idx(r)] = static_cast<std::int32_t>(d);
    }
    for (std::size_t o = 0; o < p.outputs.size() && o < q.outputs.size(); ++o) {
      const std::int32_t d = owner[idx(q.outputs[o])];
      if (d >= 0 && !fwd_half[idx(d)]) {
        fwd_half[idx(d)] = 1;
        stack.push_back(d);
      }
    }
    while (!stack.empty()) {
      const ir::domain_id d = stack.back();
      stack.pop_back();
      for (ir::domain_id rd : q.domains[idx(d)].reads) {
        if (!fwd_half[idx(rd)]) {
          fwd_half[idx(rd)] = 1;
          stack.push_back(rd);
        }
      }
    }
  }
  Hist q_rd_fwd, q_rd_rev;
  long long qn_fwd = 0, qn_rev = 0, qd_fwd = 0, qd_rev = 0;
  for (std::size_t d = 0; d < QD; ++d) {
    const bool f = fwd_half[d] != 0;
    (f ? qd_fwd : qd_rev)++;
    for (int r = 0; r < q.domains[d].rows; ++r) {
      const std::size_t v = idx(q.domains[d].value_base) + idx(r);
      (f ? q_rd_fwd : q_rd_rev).add(q_readers[v]);
      (f ? qn_fwd : qn_rev)++;
    }
  }
  std::printf("\n  5. Q = adjoint(P), the reverse as an ir::Program -- EVERY intermediate materialised as a\n");
  std::printf("     domain of its own (adjoint_to_program.hpp: \"MATERIALISED rather than recomputed per\n");
  std::printf("     target\", which is a statement about Q's FORWARD half)\n");
  std::printf("    Q %zu values / %zu domains   forward half %lld values / %lld domains | reverse half %lld / %lld\n",
              QN, QD, qn_fwd, qd_fwd, qn_rev, qd_rev);
  print_hist("readers, Q forward half", q_rd_fwd);
  print_hist("readers, Q reverse half", q_rd_rev);
}

// ---- the fixture table ------------------------------------------------------------------------

struct Entry {
  const char* name;
  std::function<Tape()> build;
};

const std::vector<Entry>& fixture_table() {
  static const std::vector<Entry> kAll = {
      // HIGH SHARING: the rates book. 1,000+ trades over one curve; D81's collapse is 2,565x here.
      {"compare_ois(64)",
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 64;
         return fixtures::spike::record_compare_ois_spike(fixtures::make_compare_ois(o),
                                                          fixtures::spike::Form::telescoped, false)
             .tape;
       }},
      {"compare_ois(256)",
       [] {
         fixtures::CompareOisOptions o;
         o.trades = 256;
         return fixtures::spike::record_compare_ois_spike(fixtures::make_compare_ois(o),
                                                          fixtures::spike::Form::telescoped, false)
             .tape;
       }},
      // NO SHARING: the path grid (D106). One row per path-step; the same collapse is 1.22x.
      {"exotic_path(64x12)", [] { return fixtures::record_exotic_path(fixtures::make_exotic_path(64, 12), false, false); }},
      {"exotic_path(256x52)", [] { return fixtures::record_exotic_path(fixtures::make_exotic_path(256, 52), false, false); }},
      {"exotic_path(1024x252)",
       [] { return fixtures::record_exotic_path(fixtures::make_exotic_path(1024, 252), false, false); }},
      // The desk problem. Recording it peaks the process at ~3.8 GB (D100 §4), so ask by name.
      {"stage_a", [] { return fixtures::record_stage_a(fixtures::make_stage_a(), false).tape; }},
  };
  return kAll;
}

}  // namespace

int main(int argc, char** argv) {
  std::string which;
  int lane_tile = 8, tile = 256;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--list") {
      for (const Entry& e : fixture_table()) std::printf("%s\n", e.name);
      return 0;
    }
    if (a == "--lane-tile" && i + 1 < argc) lane_tile = std::atoi(argv[++i]);
    else if (a == "--tile" && i + 1 < argc) tile = std::atoi(argv[++i]);
    else which = a;
  }
  std::printf("matrec -- the structural half of materialise-vs-recompute. COUNTS ONLY, no timing,\n");
  std::printf("so no box is reserved and no load is read. lane_tile=%d tile=%d\n", lane_tile, tile);
  bool any = false;
  for (const Entry& e : fixture_table()) {
    const std::string n = e.name;
    const bool heavy = n == "stage_a";
    const bool selected = which.empty() ? !heavy : (which == "all" || n == which || n.rfind(which, 0) == 0);
    if (!selected) continue;
    any = true;
    measure(n, e.build(), lane_tile, tile);
  }
  if (!any) {
    std::fprintf(stderr, "matrec: no fixture matches \"%s\" (try --list, or \"all\")\n", which.c_str());
    return 2;
  }
  return 0;
}
