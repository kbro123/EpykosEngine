// EpykosEngine — the tiled vector interpreter: plan construction, the run loop and describe().
// See include/epykos/exec/interpreter.hpp for the design. No floating-point arithmetic happens
// in this TU (the kernels live in kernels_impl.hpp, instantiated per lane width in kernels_l*.cpp
// with contraction off).
#include "epykos/exec/interpreter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/tape/op.hpp"
#include "plan.hpp"

// Optional per-domain timers (compile with -DEPYKOS_EXEC_PROFILE): run() accumulates the wall
// time of each domain's pass and of the output copy, and the totals are printed to stderr at
// exit as microseconds per run. Off by default: no code in the run loop.
#ifdef EPYKOS_EXEC_PROFILE
#include <chrono>
#include <cstdio>
namespace {
struct ProfileTable {
  static constexpr int slots = 64;   // domain id, or slots - 1 for the output copy
  double ns[slots] = {};
  long runs = 0;
  ~ProfileTable() {
    if (runs == 0) return;
    double total = 0.0;
    for (double v : ns) total += v;
    std::fprintf(stderr, "exec profile over %ld runs (us per run):\n", runs);
    for (int i = 0; i < slots; ++i) {
      if (ns[i] > 0.0) std::fprintf(stderr, "  slot %2d: %9.2f us (%5.1f%%)\n", i, ns[i] / static_cast<double>(runs) / 1e3, 100.0 * ns[i] / total);
    }
    std::fprintf(stderr, "  total  : %9.2f us\n", total / static_cast<double>(runs) / 1e3);
  }
} g_profile;
struct ProfileScope {
  int slot;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  explicit ProfileScope(int s) : slot(s) {}
  ~ProfileScope() { g_profile.ns[slot] += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count(); }
};
}  // namespace
#define EPYKOS_EXEC_PROFILE_SCOPE(slot) ProfileScope profile_scope_(slot)
#define EPYKOS_EXEC_PROFILE_RUN() ++g_profile.runs
#else
#define EPYKOS_EXEC_PROFILE_SCOPE(slot)
#define EPYKOS_EXEC_PROFILE_RUN()
#endif

namespace epykos::exec {

using namespace detail;

const char* to_string(ExpMode mode) noexcept {
  switch (mode) {
    case ExpMode::std_exp: return "std::exp";
    case ExpMode::poly: return "exp_poly";
  }
  return "?";
}

namespace detail {
const char* to_string(Kind k) noexcept {
  switch (k) {
    case Kind::Vec: return "vec";
    case Kind::Lit: return "lit";
    case Kind::Col: return "col";
    case Kind::Gat: return "gat";
  }
  return "?";
}
const char* to_string(PK k) noexcept {
  switch (k) {
    case PK::S: return "s";
    case PK::G: return "g";
    case PK::None: return "-";
  }
  return "?";
}
}  // namespace detail

namespace {

// Kernel table lookups across the lane-width variants (plan.hpp: lane_variants).
#define EPYKOS_EXEC_DISPATCH(v, call)         \
  switch (v) {                                \
    case 0: return KernelTable<0>::call;      \
    case 1: return KernelTable<1>::call;      \
    case 2: return KernelTable<4>::call;      \
    case 3: return KernelTable<8>::call;      \
    case 4: return KernelTable<16>::call;     \
    case 5: return KernelTable<32>::call;     \
    case 6: return KernelTable<64>::call;     \
    default: return nullptr;                  \
  }

OpKernel binary_kernel(int v, Op op, Kind ka, Kind kb, bool ind) { EPYKOS_EXEC_DISPATCH(v, binary(op, ka, kb, ind)) }
OpKernel unary_kernel(int v, Op op, Kind ka, bool ind) { EPYKOS_EXEC_DISPATCH(v, unary(op, ka, ind)) }
OpKernel exp_poly_kernel(int v, Kind ka, bool ind) { EPYKOS_EXEC_DISPATCH(v, exp_poly(ka, ind)) }
OpKernel ternary_kernel(int v, Op op) { EPYKOS_EXEC_DISPATCH(v, ternary(op)) }
OpKernel konst_kernel(int v, Kind kk, bool ind) { EPYKOS_EXEC_DISPATCH(v, konst(kk, ind)) }
OpKernel input_kernel(int v, bool ind) { EPYKOS_EXEC_DISPATCH(v, input(ind)) }
OpKernel seg_rows_kernel(int v, bool affine, bool ind) { EPYKOS_EXEC_DISPATCH(v, seg_rows(affine, ind)) }
LoadKernel load_kernel(int v, Kind k, bool ind) { EPYKOS_EXEC_DISPATCH(v, load(k, ind)) }
SegKernel seg_whole_kernel(int v, bool affine) { EPYKOS_EXEC_DISPATCH(v, seg_whole(affine)) }
SegListKernel seg_list_kernel(int v, bool affine) { EPYKOS_EXEC_DISPATCH(v, seg_list(affine)) }
AccKernel binary_acc_kernel(int v, Op op, Kind ka, Kind kb, bool affine) { EPYKOS_EXEC_DISPATCH(v, binary_acc(op, ka, kb, affine)) }
AccKernel unary_acc_kernel(int v, Op op, Kind ka, bool affine) { EPYKOS_EXEC_DISPATCH(v, unary_acc(op, ka, affine)) }
AccKernel konst_acc_kernel(int v, Kind kk, bool affine) { EPYKOS_EXEC_DISPATCH(v, konst_acc(kk, affine)) }
LoadAccKernel load_acc_kernel(int v, Kind k, bool affine, bool ind) { EPYKOS_EXEC_DISPATCH(v, load_acc(k, affine, ind)) }
OpKernel pair_kernel(int v, Op op, PK ka, PK kb, Op op2, PK kc, bool pr, bool ind) { EPYKOS_EXEC_DISPATCH(v, pair(op, ka, kb, op2, kc, pr, ind)) }
AccKernel pair_acc_kernel(int v, Op op, PK ka, PK kb, Op op2, PK kc, bool pr, bool affine) { EPYKOS_EXEC_DISPATCH(v, pair_acc(op, ka, kb, op2, kc, pr, affine)) }

void copy_out(int v, const double* values, const std::int32_t* ids, const std::int32_t* ords, int n_out, double* out, int B, int b0, int L) {
  switch (v) {
    case 0: return KernelTable<0>::copy_out(values, ids, ords, n_out, out, B, b0, L);
    case 1: return KernelTable<1>::copy_out(values, ids, ords, n_out, out, B, b0, L);
    case 2: return KernelTable<4>::copy_out(values, ids, ords, n_out, out, B, b0, L);
    case 3: return KernelTable<8>::copy_out(values, ids, ords, n_out, out, B, b0, L);
    case 4: return KernelTable<16>::copy_out(values, ids, ords, n_out, out, B, b0, L);
    case 5: return KernelTable<32>::copy_out(values, ids, ords, n_out, out, B, b0, L);
    case 6: return KernelTable<64>::copy_out(values, ids, ords, n_out, out, B, b0, L);
    default: return;
  }
}

#undef EPYKOS_EXEC_DISPATCH

std::string human_bytes(std::size_t bytes) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(1);
  if (bytes >= (std::size_t{1} << 20)) {
    os << static_cast<double>(bytes) / 1048576.0 << " MB";
  } else if (bytes >= (std::size_t{1} << 10)) {
    os << static_cast<double>(bytes) / 1024.0 << " KB";
  } else {
    os << bytes << " B";
  }
  return os.str();
}

// A group that runs as a whole-domain Sum / Affine pass: exactly one step, a Sum or Affine over
// a segment.
bool is_whole_segment(const ir::Group& g) {
  if (g.steps.size() != 1) return false;
  const ir::Step& s = g.steps.front();
  return (s.op == Op::Sum || s.op == Op::Affine) && s.a.kind == ir::SlotKind::Segment;
}

// Fused producers are evaluated more than once when a row is a member of several rows; past
// this many member references per row the domain is materialised instead. Rows read by a
// gather, an output or a per-row Sum / Affine are kept (materialised on their own); past this
// fraction of kept rows the domain is materialised as a whole.
constexpr double fuse_max_refs_per_row = 2.0;
constexpr double fuse_max_kept_fraction = 0.5;

// An inlined producer is evaluated once per gather reference; past this many references per
// row it is materialised instead.
constexpr double inline_max_refs_per_row = 1.25;

// Output rows of a fused producer are emitted from the reduction blocks (rather than kept and
// copied from the value buffer) only when the region they would otherwise occupy, rows × lanes
// × 8 bytes, is at least this large: below it the region stays cache-resident and the member
// buffer path's extra kernel calls (one per producer run) cost more than the write it avoids;
// above it the write goes to lines evicted since the previous run (measured: 969 swap PVs at
// 32 lanes, 248 KB, 49 us as a pass + 31 us to copy vs 17 us less in all emitted; at 1 lane,
// 7.7 KB, emitting cost 0.9 us more).
constexpr std::size_t emit_min_bytes = std::size_t{64} << 10;

}  // namespace

struct Interpreter::Impl {
  const ir::Program* p = nullptr;
  Options opt;
  int Lt = 0;              // lanes the buffers hold (min(lane_tile, max_batch))
  int max_steps = 0;       // longest elementwise group
  std::size_t tile_elems = 0;  // tile · Lt doubles per scratch buffer
  std::size_t table_bytes = 0;
  std::size_t fused_values = 0;  // rows of fused domains

  mutable std::vector<double> values;   // num_values · Lt
  mutable std::vector<double> scratch;  // (max_steps + 3 temporaries + accumulator + member) · tile_elems
  std::vector<GroupPlan> groups;
  std::vector<char> fused;              // per domain: evaluated inside its consumers' reductions
  std::vector<std::vector<std::int32_t>> keep_rows;  // per fused domain: rows materialised anyway
  std::vector<char> emitted;            // per value: an output written by the reduction block that computes it
  std::vector<std::int32_t> late_ids, late_ords;   // outputs copied from the value buffer after each chunk
  std::vector<std::int32_t> inlined_into;   // per domain: the consumer whose tiles evaluate it, or -1
  struct InlineRef { std::size_t gather; std::int32_t producer; };
  std::vector<std::vector<InlineRef>> inline_refs;   // per consumer: its inlined gathers
  int n_inline_temps = 0;               // most inlined gathers of one consumer (temporaries shared across consumers)

  double* step_buffer(int k) const { return scratch.data() + static_cast<std::size_t>(k) * tile_elems; }
  double* tmp_buffer(int j) const { return step_buffer(max_steps + j); }
  double* acc_buffer() const { return step_buffer(max_steps + 3); }
  double* member_buffer() const { return step_buffer(max_steps + 4); }
  double* inline_temp(int t) const { return step_buffer(max_steps + 5 + t); }

  Operand resolve(const ir::Slot& s) const;
  void decide_fusion();
  void decide_inline();
  void build_group(std::size_t d, GroupPlan& g);
  void build_segment(std::int32_t consumer, const ir::Segment& seg, bool affine, const ir::Slot& konst, std::int32_t rows,
                     SegPlan& sp);
  void build_step(const ir::Step& st, std::size_t k, std::size_t n_steps, GroupPlan& g, StepPlan& sp);
  bool build_pair_step(const ir::Group& grp, const std::vector<int>& uses, std::size_t k, StepPlan& sp);
  bool add_tail_step(const ir::Step& st, std::size_t prev, StepPlan& sp);
};

Operand Interpreter::Impl::resolve(const ir::Slot& s) const {
  Operand o;
  switch (s.kind) {
    case ir::SlotKind::Step:
      o.kind = Kind::Vec;
      o.data = step_buffer(s.index);
      break;
    case ir::SlotKind::Literal:
      o.kind = Kind::Lit;
      o.data = &p->literals[static_cast<std::size_t>(s.index)];
      o.stride = 0;
      break;
    case ir::SlotKind::Column:
      o.kind = Kind::Col;
      o.data = p->columns[static_cast<std::size_t>(s.index)].values.data();
      o.stride = 1;
      break;
    case ir::SlotKind::Gather:
      o.kind = Kind::Gat;
      o.data = values.data();
      o.index = p->gathers[static_cast<std::size_t>(s.index)].index.data();
      break;
    default:
      throw std::invalid_argument("exec: operand slot of kind " + std::string(ir::to_string(s.kind)) +
                                  " cannot be an elementwise operand");
  }
  return o;
}

// Which domains are evaluated inside the reductions that read them rather than materialised: an
// elementwise group (not itself a whole-domain reduction), non-recurrent, with rows, read as
// members of whole-domain Sum / Affine groups with at most fuse_max_refs_per_row member
// references per row, and with at most fuse_max_kept_fraction of its rows read any other way
// (gather, per-row Sum / Affine, or an output that is not such a member) — those rows are kept:
// evaluated separately and written to the value buffer. An output row that is a member is not
// kept when the domain's region is at least emit_min_bytes: the reduction block that computes
// it writes it to `out` (emitted). On the M1 book at 32 lanes the swap PVs (969 + 31 rows, all
// outputs, members of the book sum) are emitted from the book sum's blocks and never occupy
// the value buffer; at 1 lane they are a pass and a copy as before. A structure-only decision;
// the arithmetic is the same either way.
void Interpreter::Impl::decide_fusion() {
  const ir::Program& prog = *p;
  const std::size_t nd = prog.domains.size();
  const std::size_t nv = prog.num_values();
  fused.assign(nd, 0);
  keep_rows.assign(nd, {});
  emitted.assign(nv, 0);
  std::vector<char> is_output(nv, 0);
  for (ir::value_id v : prog.outputs) is_output[static_cast<std::size_t>(v)] = 1;
  if (opt.fuse_reductions) {
    std::vector<char> marked(nv, 0);         // value read by a gather or a per-row Sum / Affine
    std::vector<char> whole_member(nv, 0);   // value is a member of a whole-domain reduction
    std::vector<std::size_t> member_refs(nd, 0);
    auto dom_of = [&](ir::value_id v) { return static_cast<std::size_t>(prog.domain_of(v)); };
    for (const ir::Gather& g : prog.gathers) {
      for (ir::value_id v : g.index) marked[static_cast<std::size_t>(v)] = 1;
    }
    for (const ir::Segment& seg : prog.segments) {
      const bool whole = is_whole_segment(prog.groups[static_cast<std::size_t>(seg.domain)]);
      for (ir::value_id v : seg.members) {
        if (whole) {
          ++member_refs[dom_of(v)];
          whole_member[static_cast<std::size_t>(v)] = 1;
        } else {
          marked[static_cast<std::size_t>(v)] = 1;
        }
      }
    }
    for (std::size_t d = 0; d < nd; ++d) {
      const ir::Domain& dom = prog.domains[d];
      if (dom.recurrent || dom.rows < 1 || member_refs[d] == 0) continue;
      if (is_whole_segment(prog.groups[d])) continue;
      if (static_cast<double>(member_refs[d]) > fuse_max_refs_per_row * static_cast<double>(dom.rows)) continue;
      const bool emit_ok = static_cast<std::size_t>(dom.rows) * static_cast<std::size_t>(Lt) * sizeof(double) >= emit_min_bytes;
      std::vector<std::int32_t> keep;
      for (std::int32_t r = 0; r < dom.rows; ++r) {
        const std::size_t v = static_cast<std::size_t>(dom.value_base + r);
        if (marked[v] || (is_output[v] && !(emit_ok && whole_member[v]))) keep.push_back(r);
      }
      if (static_cast<double>(keep.size()) > fuse_max_kept_fraction * static_cast<double>(dom.rows)) continue;
      fused[d] = 1;
      fused_values += static_cast<std::size_t>(dom.rows) - keep.size();
      for (std::int32_t r = 0; r < dom.rows; ++r) {
        const std::size_t v = static_cast<std::size_t>(dom.value_base + r);
        if (is_output[v] && emit_ok && whole_member[v] && !marked[v]) emitted[v] = 1;
      }
      keep_rows[d] = std::move(keep);
    }
  }
  // The outputs still copied from the value buffer after each chunk.
  for (std::size_t o = 0; o < prog.outputs.size(); ++o) {
    const ir::value_id v = prog.outputs[o];
    if (emitted[static_cast<std::size_t>(v)]) continue;
    late_ids.push_back(v);
    late_ords.push_back(static_cast<std::int32_t>(o));
  }
  table_bytes += (late_ids.size() + late_ords.size()) * sizeof(std::int32_t);
}

// Which domains are evaluated per tile of the one elementwise domain that gathers them
// (plan.hpp: InlinedProducer) rather than materialised: not recurrent, with rows, not fused into
// reductions, read by nothing but the gathers of one consumer (no output, no segment
// membership, no gather from any other domain), with at most inline_max_refs_per_row gather
// references per row (a row is evaluated once per reference); a whole-domain Sum / Affine only
// when every row has the same number of members, all of them materialised (an interpolation:
// the per-row fold then vectorises like the bucketed pass; a reduction over fused producers
// keeps its blocks). The consumer is a materialised elementwise domain (not a whole-domain
// reduction, not fused, not recurrent, not itself inlined), and a domain that inlines producers
// is never inlined itself (it would run in the indirect row mode, whose rows its inlined operands
// cannot address). A gather may hold a few ids of other domains (a time that is a knot reads the
// input directly): those rows are copied from the value buffer into the temporary, so they must
// be materialised. Decided in Program order (producers precede consumers). A structure-only
// decision; the arithmetic is the same.
void Interpreter::Impl::decide_inline() {
  const ir::Program& prog = *p;
  const std::size_t nd = prog.domains.size();
  inlined_into.assign(nd, -1);
  inline_refs.assign(nd, {});
  n_inline_temps = 0;
  if (!opt.inline_producers) return;
  auto dom_of = [&](ir::value_id v) { return static_cast<std::int32_t>(prog.domain_of(v)); };
  std::vector<char> blocked(nd, 0);   // read other than through a gather
  for (ir::value_id v : prog.outputs) blocked[static_cast<std::size_t>(dom_of(v))] = 1;
  for (const ir::Segment& seg : prog.segments) {
    for (ir::value_id v : seg.members) blocked[static_cast<std::size_t>(dom_of(v))] = 1;
  }
  const std::size_t ng = prog.gathers.size();
  std::vector<std::int32_t> user(ng, -1);   // the domain whose steps read the gather slot (-2: several)
  for (std::size_t d = 0; d < nd; ++d) {
    for (const ir::Step& st : prog.groups[d].steps) {
      for (const ir::Slot* sl : {&st.a, &st.b, &st.c, &st.konst}) {
        if (sl->kind != ir::SlotKind::Gather) continue;
        std::int32_t& u = user[static_cast<std::size_t>(sl->index)];
        if (u == -1 || u == static_cast<std::int32_t>(d)) u = static_cast<std::int32_t>(d);
        else u = -2;
      }
    }
  }
  // Per domain: the gather slots holding some of its ids, and how many ids in all.
  std::vector<std::vector<std::size_t>> readers(nd);
  std::vector<std::size_t> refs(nd, 0);
  for (std::size_t k = 0; k < ng; ++k) {
    std::vector<char> seen(nd, 0);
    for (ir::value_id v : prog.gathers[k].index) {
      const std::size_t t = static_cast<std::size_t>(dom_of(v));
      ++refs[t];
      if (!seen[t]) {
        seen[t] = 1;
        readers[t].push_back(k);
      }
    }
  }
  for (std::size_t d = 0; d < nd; ++d) {
    const ir::Domain& dom = prog.domains[d];
    if (dom.recurrent || dom.rows < 1 || blocked[d] || fused[d] || readers[d].empty()) continue;
    if (!inline_refs[d].empty()) continue;   // inlines producers itself: stays a contiguous-row group
    const ir::Group& grp = prog.groups[d];
    if (is_whole_segment(grp)) {
      const ir::Segment& seg = prog.segments[static_cast<std::size_t>(grp.steps.front().a.index)];
      bool uniform = true, materialised = true;
      const std::int32_t len0 = seg.offsets.size() > 1 ? seg.offsets[1] - seg.offsets[0] : 0;
      for (std::size_t r = 0; r + 1 < seg.offsets.size(); ++r) uniform &= (seg.offsets[r + 1] - seg.offsets[r]) == len0;
      for (ir::value_id v : seg.members) {
        const std::size_t t = static_cast<std::size_t>(dom_of(v));
        materialised &= !fused[t] && inlined_into[t] < 0;
      }
      if (!uniform || !materialised) continue;
    }
    const std::int32_t c = user[readers[d][0]];
    if (c < 0 || c == static_cast<std::int32_t>(d)) continue;
    bool ok = true;
    for (std::size_t k : readers[d]) ok &= user[k] == c;
    if (!ok) continue;
    const std::size_t cd = static_cast<std::size_t>(c);
    if (is_whole_segment(prog.groups[cd]) || fused[cd] || prog.domains[cd].recurrent || inlined_into[cd] >= 0) continue;
    if (static_cast<double>(refs[d]) > inline_max_refs_per_row * static_cast<double>(dom.rows)) continue;
    // Foreign ids of those gathers must be materialised rows (the copy reads the value buffer).
    for (std::size_t k : readers[d]) {
      for (ir::value_id v : prog.gathers[k].index) {
        const std::size_t t = static_cast<std::size_t>(dom_of(v));
        if (t != d && inlined_into[t] >= 0) ok = false;
      }
    }
    if (!ok) continue;
    inlined_into[d] = c;
    for (std::size_t k : readers[d]) inline_refs[cd].push_back({k, static_cast<std::int32_t>(d)});
    n_inline_temps = std::max(n_inline_temps, static_cast<int>(inline_refs[cd].size()));
    fused_values += static_cast<std::size_t>(dom.rows);
  }
}

void Interpreter::Impl::build_segment(std::int32_t consumer, const ir::Segment& seg, bool affine, const ir::Slot& konst,
                                      std::int32_t rows, SegPlan& sp) {
  const ir::Program& prog = *p;
  sp.affine = affine;
  if (affine) sp.konst = resolve(konst);
  for (int v = 0; v < n_lane_variants; ++v) {
    sp.load_acc_gat[v] = load_acc_kernel(v, Kind::Gat, affine, false);
    sp.load_acc_vec[v] = load_acc_kernel(v, Kind::Vec, affine, false);
  }
  sp.offsets = seg.offsets.data();
  sp.members = seg.members.data();
  sp.coefs = seg.coefs.data();
  if (static_cast<std::int32_t>(seg.offsets.size()) != rows + 1) {
    throw std::invalid_argument("exec: segment offsets do not match the domain's rows");
  }
  auto len_of = [&](std::int32_t r) { return seg.offsets[static_cast<std::size_t>(r) + 1] - seg.offsets[static_cast<std::size_t>(r)]; };
  sp.min_len = rows > 0 ? len_of(0) : 0;
  sp.max_len = sp.min_len;
  for (std::int32_t r = 1; r < rows; ++r) {
    sp.min_len = std::min(sp.min_len, len_of(r));
    sp.max_len = std::max(sp.max_len, len_of(r));
  }
  if (!affine && rows > 0 && sp.min_len < 1) throw std::invalid_argument("exec: a Sum row with no members");

  // The producer pattern of a row: per member, the fused domain it comes from or -1 (value
  // buffer). Rows are keyed by (length, pattern id); pattern ids are assigned in row order.
  std::vector<std::int32_t> pattern_of(static_cast<std::size_t>(rows), 0);
  std::vector<std::vector<std::int32_t>> patterns;
  bool any_fused = false;
  for (ir::value_id v : seg.members) any_fused |= fused[static_cast<std::size_t>(prog.domain_of(v))] != 0;
  if (any_fused) {
    std::map<std::vector<std::int32_t>, std::int32_t> ids;
    std::vector<std::int32_t> key;
    for (std::int32_t r = 0; r < rows; ++r) {
      key.clear();
      const std::size_t lo = static_cast<std::size_t>(seg.offsets[static_cast<std::size_t>(r)]);
      const std::size_t hi = static_cast<std::size_t>(seg.offsets[static_cast<std::size_t>(r) + 1]);
      for (std::size_t m = lo; m < hi; ++m) {
        const ir::domain_id d = prog.domain_of(seg.members[m]);
        key.push_back(fused[static_cast<std::size_t>(d)] ? d : -1);
      }
      auto it = ids.find(key);
      if (it == ids.end()) {
        it = ids.emplace(key, static_cast<std::int32_t>(patterns.size())).first;
        patterns.push_back(key);
      }
      pattern_of[static_cast<std::size_t>(r)] = it->second;
    }
  } else {
    patterns.emplace_back();  // unused: every member is gathered
  }

  // Rows ordered by (segment length, pattern) (stable: ties keep row order), then buckets of
  // equal key split into blocks of at most `tile` rows.
  std::vector<std::int32_t> order(static_cast<std::size_t>(rows));
  std::iota(order.begin(), order.end(), 0);
  auto key_less = [&](std::int32_t x, std::int32_t y) {
    const std::int32_t lx = len_of(x), ly = len_of(y);
    if (lx != ly) return lx < ly;
    return pattern_of[static_cast<std::size_t>(x)] < pattern_of[static_cast<std::size_t>(y)];
  };
  auto key_equal = [&](std::int32_t x, std::int32_t y) {
    return len_of(x) == len_of(y) && pattern_of[static_cast<std::size_t>(x)] == pattern_of[static_cast<std::size_t>(y)];
  };
  std::stable_sort(order.begin(), order.end(), key_less);
  sp.rows.reserve(order.size());
  sp.memT.reserve(seg.members.size());
  if (affine) sp.coefT.reserve(seg.members.size());
  sp.patterns = 0;
  const std::size_t tile = static_cast<std::size_t>(opt.tile);
  std::size_t pos = 0;
  while (pos < order.size()) {
    const std::int32_t len = len_of(order[pos]);
    // A fused bucket (some member position from a fused producer) is cut into blocks of at most
    // `tile` members so that one kernel call evaluates a whole producer run; a gathered bucket
    // into blocks of at most `tile` rows.
    bool bucket_fused = false;
    if (any_fused) {
      for (std::int32_t d : patterns[static_cast<std::size_t>(pattern_of[static_cast<std::size_t>(order[pos])])]) {
        bucket_fused |= d >= 0;
      }
    }
    // Fused: as many rows as fit `tile` members, but at least the epilogue's rows in flight for
    // the lane width the buffers hold (positions are then chunked, kc below).
    const std::size_t rows_in_flight = static_cast<std::size_t>(acc_rows_in_flight_for(Lt));
    std::size_t max_rows =
        bucket_fused ? std::min(tile, std::max(rows_in_flight, tile / static_cast<std::size_t>(std::max(len, 1)))) : tile;
    if (bucket_fused && max_rows > rows_in_flight) max_rows -= max_rows % rows_in_flight;  // whole accumulator groups
    std::size_t end = pos;
    while (end < order.size() && key_equal(order[pos], order[end]) && end - pos < max_rows) ++end;
    if (pos == 0 || !key_equal(order[pos - 1], order[pos])) ++sp.patterns;
    SegBlock blk;
    blk.n = static_cast<std::int32_t>(end - pos);
    blk.len = len;
    blk.kc = bucket_fused ? static_cast<std::int32_t>(std::max<std::size_t>(1, std::min<std::size_t>(static_cast<std::size_t>(len), tile / static_cast<std::size_t>(blk.n))))
                          : len;
    blk.row0 = sp.rows.size();
    blk.memT = sp.memT.size();
    blk.prod = sp.prod.size();
    for (std::size_t i = pos; i < end; ++i) sp.rows.push_back(order[i]);
    const std::size_t n = static_cast<std::size_t>(blk.n);
    sp.memT.resize(blk.memT + n * static_cast<std::size_t>(len));
    if (affine) sp.coefT.resize(blk.memT + n * static_cast<std::size_t>(len));
    for (std::size_t i = 0; i < n; ++i) {
      const std::int32_t r = order[pos + i];
      const std::size_t lo = static_cast<std::size_t>(seg.offsets[static_cast<std::size_t>(r)]);
      for (std::size_t k = 0; k < static_cast<std::size_t>(len); ++k) {
        sp.memT[blk.memT + k * n + i] = seg.members[lo + k];
        if (affine) sp.coefT[blk.memT + k * n + i] = seg.coefs[lo + k];
      }
    }
    if (any_fused) {
      const std::vector<std::int32_t>& pat = patterns[static_cast<std::size_t>(pattern_of[static_cast<std::size_t>(order[pos])])];
      for (std::int32_t d : pat) {
        sp.prod.push_back(d);
        if (d >= 0) {
          blk.fused = true;
          sp.fused_members += n;
          GroupPlan& producer = groups[static_cast<std::size_t>(d)];
          if (std::find(producer.consumers.begin(), producer.consumers.end(), consumer) == producer.consumers.end()) {
            producer.consumers.push_back(consumer);
          }
        }
      }
    } else {
      sp.prod.resize(sp.prod.size() + static_cast<std::size_t>(len), -1);
    }
    sp.blocks.push_back(blk);
    pos = end;
  }

  // Fused producers: `prod` re-indexed from domain to producer entry; per position the offset of
  // its members in the producer's permuted tables (positions of one block are consecutive, so a
  // run of positions is one contiguous range); then the producer's steps copied with every
  // row-addressed operand permuted into this member order.
  sp.prod_off.assign(sp.prod.size(), 0);
  std::vector<std::int32_t> producer_of(prog.domains.size(), -1);
  std::vector<std::int32_t> count;
  for (const SegBlock& blk : sp.blocks) {
    for (std::int32_t k = 0; k < blk.len; ++k) {
      std::int32_t& d = sp.prod[blk.prod + static_cast<std::size_t>(k)];
      if (d < 0) continue;
      if (producer_of[static_cast<std::size_t>(d)] < 0) {
        producer_of[static_cast<std::size_t>(d)] = static_cast<std::int32_t>(sp.producers.size());
        sp.producers.emplace_back();
        sp.producers.back().domain = d;
        count.push_back(0);
      }
      d = producer_of[static_cast<std::size_t>(d)];
      sp.prod_off[blk.prod + static_cast<std::size_t>(k)] = count[static_cast<std::size_t>(d)];
      count[static_cast<std::size_t>(d)] += blk.n;
    }
  }
  for (std::size_t pi = 0; pi < sp.producers.size(); ++pi) {
    FusedProducer& fp = sp.producers[pi];
    const GroupPlan& g = groups[static_cast<std::size_t>(fp.domain)];
    fp.block_mode = true;
    for (const StepPlan& s : g.steps) {
      if (s.op == Op::Sum || s.op == Op::Affine) fp.block_mode = false;
    }
    if (!fp.block_mode) {
      fp.has_outputs = g.emitted > 0;
      continue;
    }
    const std::size_t total = static_cast<std::size_t>(count[pi]);
    std::vector<std::int32_t> rowsP(total);
    for (const SegBlock& blk : sp.blocks) {
      const std::size_t n = static_cast<std::size_t>(blk.n);
      for (std::int32_t k = 0; k < blk.len; ++k) {
        if (sp.prod[blk.prod + static_cast<std::size_t>(k)] != static_cast<std::int32_t>(pi)) continue;
        const std::size_t off = static_cast<std::size_t>(sp.prod_off[blk.prod + static_cast<std::size_t>(k)]);
        const std::int32_t* mem = sp.memT.data() + blk.memT + static_cast<std::size_t>(k) * n;
        for (std::size_t i = 0; i < n; ++i) rowsP[off + i] = mem[i] - g.value_base;
      }
    }
    fp.steps = g.steps;
    for (std::size_t m = 0; m < total; ++m) fp.has_outputs |= g.emit_ordinal[static_cast<std::size_t>(rowsP[m])] >= 0;
    if (fp.has_outputs) {
      fp.out_ordinal.resize(total);
      for (std::size_t m = 0; m < total; ++m) fp.out_ordinal[m] = g.emit_ordinal[static_cast<std::size_t>(rowsP[m])];
      table_bytes += total * sizeof(std::int32_t);
    }
    std::size_t n_cols = 0, n_gats = 0;
    auto count_kind = [&](const Operand& o) {
      n_cols += o.kind == Kind::Col ? 1 : 0;
      n_gats += o.kind == Kind::Gat ? 1 : 0;
    };
    for (const StepPlan& s : fp.steps) {
      count_kind(s.a);
      count_kind(s.b);
      count_kind(s.c);
      count_kind(s.konst);
      for (int q = 0; q < s.n_pre; ++q) count_kind(s.pre[q].src);
    }
    fp.cols.reserve(n_cols);
    fp.gats.reserve(n_gats);
    auto permute = [&](Operand& o) {
      if (o.kind == Kind::Col) {
        std::vector<double> t(total);
        for (std::size_t m = 0; m < total; ++m) t[m] = o.data[rowsP[m]];
        fp.cols.push_back(std::move(t));
        o.data = fp.cols.back().data();
        table_bytes += total * sizeof(double);
      } else if (o.kind == Kind::Gat) {
        std::vector<std::int32_t> t(total);
        for (std::size_t m = 0; m < total; ++m) t[m] = o.index[rowsP[m]];
        fp.gats.push_back(std::move(t));
        o.index = fp.gats.back().data();
        table_bytes += total * sizeof(std::int32_t);
      }
    };
    for (StepPlan& s : fp.steps) {
      permute(s.a);
      permute(s.b);
      permute(s.c);
      permute(s.konst);
      for (int q = 0; q < s.n_pre; ++q) permute(s.pre[q].src);
      if (s.op == Op::Input) {
        if (fp.ordinal.empty()) {
          fp.ordinal.resize(total);
          for (std::size_t m = 0; m < total; ++m) fp.ordinal[m] = g.ordinal[static_cast<std::size_t>(rowsP[m])];
          table_bytes += total * sizeof(std::int32_t);
        }
        s.ordinal = fp.ordinal.data();
      }
    }
  }
  table_bytes += sp.rows.size() * sizeof(std::int32_t) + sp.memT.size() * sizeof(std::int32_t) +
                 sp.coefT.size() * sizeof(double) + (sp.prod.size() + sp.prod_off.size()) * sizeof(std::int32_t) +
                 sp.blocks.size() * sizeof(SegBlock);
}

void Interpreter::Impl::build_step(const ir::Step& st, std::size_t k, std::size_t n_steps, GroupPlan& g, StepPlan& sp) {
  sp.op = st.op;
  sp.out = (k + 1 == n_steps) ? nullptr : step_buffer(static_cast<int>(k));
  std::ostringstream name;
  name << to_string(st.op);
  auto kind_name = [](const Operand& o) { return std::string(to_string(o.kind)); };
  auto set_fn = [&](auto&& pick) {
    for (int v = 0; v < n_lane_variants; ++v) {
      sp.fn[row_contiguous][v] = pick(v, false);
      sp.fn[row_indirect][v] = pick(v, true);
    }
  };
  // Reduction epilogues for a last step of a fused producer: Const, the unary ops under
  // std::exp, and the binary ops. The others (Input, Fma / Select, exp_poly, per-row Sum /
  // Affine) go through the member buffer (acc_fn stays null).
  auto set_acc = [&](auto&& pick) {
    for (int v = 0; v < n_lane_variants; ++v) {
      sp.acc_fn[0][v] = pick(v, false);
      sp.acc_fn[1][v] = pick(v, true);
    }
  };
  switch (st.op) {
    case Op::Const: {
      sp.konst = resolve(st.konst);
      set_fn([&](int v, bool ind) { return konst_kernel(v, sp.konst.kind, ind); });
      set_acc([&](int v, bool affine) { return konst_acc_kernel(v, sp.konst.kind, affine); });
      name << "(" << kind_name(sp.konst) << ")";
      break;
    }
    case Op::Input: {
      if (g.ordinal.empty()) {
        g.ordinal.assign(static_cast<std::size_t>(g.rows), -1);
        for (std::size_t o = 0; o < p->inputs.size(); ++o) {
          const ir::value_id v = p->inputs[o];
          if (v >= g.value_base && v < g.value_base + g.rows) {
            g.ordinal[static_cast<std::size_t>(v - g.value_base)] = static_cast<std::int32_t>(o);
          }
        }
        for (std::int32_t r = 0; r < g.rows; ++r) {
          if (g.ordinal[static_cast<std::size_t>(r)] < 0) {
            throw std::invalid_argument("exec: an Input row without an input ordinal");
          }
        }
        table_bytes += g.ordinal.size() * sizeof(std::int32_t);
      }
      sp.ordinal = g.ordinal.data();
      set_fn([&](int v, bool ind) { return input_kernel(v, ind); });
      break;
    }
    case Op::Neg:
    case Op::Exp:
    case Op::Log:
    case Op::Sqrt:
    case Op::Recip: {
      sp.a = resolve(st.a);
      const bool poly = st.op == Op::Exp && opt.exp == ExpMode::poly;
      set_fn([&](int v, bool ind) { return poly ? exp_poly_kernel(v, sp.a.kind, ind) : unary_kernel(v, st.op, sp.a.kind, ind); });
      if (!poly) set_acc([&](int v, bool affine) { return unary_acc_kernel(v, st.op, sp.a.kind, affine); });
      name << (poly ? "_poly(" : "(") << kind_name(sp.a) << ")";
      break;
    }
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
    case Op::Div:
    case Op::CmpLt:
    case Op::CmpLe:
    case Op::CmpGt:
    case Op::CmpGe:
    case Op::CmpEq: {
      sp.a = resolve(st.a);
      sp.b = resolve(st.b);
      set_fn([&](int v, bool ind) { return binary_kernel(v, st.op, sp.a.kind, sp.b.kind, ind); });
      set_acc([&](int v, bool affine) { return binary_acc_kernel(v, st.op, sp.a.kind, sp.b.kind, affine); });
      name << "(" << kind_name(sp.a) << "," << kind_name(sp.b) << ")";
      break;
    }
    case Op::Fma:
    case Op::Select: {
      Operand ops[3] = {resolve(st.a), resolve(st.b), resolve(st.c)};
      name << "(";
      for (int j = 0; j < 3; ++j) {
        name << (j ? "," : "") << kind_name(ops[j]);
        if (ops[j].kind != Kind::Vec) {
          PreLoad& pre = sp.pre[sp.n_pre++];
          pre.src = ops[j];
          pre.dst = tmp_buffer(j);
          for (int v = 0; v < n_lane_variants; ++v) {
            pre.fn[row_contiguous][v] = load_kernel(v, ops[j].kind, false);
            pre.fn[row_indirect][v] = load_kernel(v, ops[j].kind, true);
          }
          ops[j] = Operand{Kind::Vec, pre.dst, nullptr};
        }
      }
      name << ")";
      sp.a = ops[0];
      sp.b = ops[1];
      sp.c = ops[2];
      set_fn([&](int v, bool) { return ternary_kernel(v, st.op); });
      break;
    }
    case Op::Sum:
    case Op::Affine: {
      // A Sum / Affine sharing its group with other steps: per-tile, row by row.
      if (st.a.kind != ir::SlotKind::Segment) throw std::invalid_argument("exec: Sum/Affine without a segment");
      const bool affine = st.op == Op::Affine;
      g.seg.affine = affine;
      const ir::Segment& seg = p->segments[static_cast<std::size_t>(st.a.index)];
      g.seg.offsets = seg.offsets.data();
      g.seg.members = seg.members.data();
      g.seg.coefs = seg.coefs.data();
      if (affine) g.seg.konst = resolve(st.konst);
      sp.seg = &g.seg;
      set_fn([&](int v, bool ind) { return seg_rows_kernel(v, affine, ind); });
      name << "(seg" << (affine ? ";" + kind_name(g.seg.konst) : "") << ")[per-row]";
      break;
    }
    default:
      throw std::invalid_argument(std::string("exec: unsupported op ") + to_string(st.op));
  }
  sp.name = name.str();
}

// Steps k and k + 1 as one fused pair (plan.hpp: StepPlan, kernels_impl.hpp: fused pairs) when
// step k is Add / Sub / Mul / Div over literals, columns and gathers with at least one gather
// (or Neg of a gather), step k + 1 is Add / Sub / Mul / Div of step k and one literal / column /
// gather (or Neg of step k), and nothing else reads step k. A commutative first op has its
// operands ordered scalar first; a commutative second op keeps the pair's value on the left —
// both are bit-identical reorderings of IEEE-commutative operations.
bool Interpreter::Impl::build_pair_step(const ir::Group& grp, const std::vector<int>& uses, std::size_t k, StepPlan& sp) {
  const ir::Step& s0 = grp.steps[k];
  const ir::Step& s1 = grp.steps[k + 1];
  if (uses[k] != 1) return false;
  auto is_arith = [](Op op) { return op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::Div; };
  auto is_commutative = [](Op op) { return op == Op::Add || op == Op::Mul; };
  auto pk_of = [](const ir::Slot& sl) {
    switch (sl.kind) {
      case ir::SlotKind::Literal:
      case ir::SlotKind::Column: return PK::S;
      case ir::SlotKind::Gather: return PK::G;
      default: return PK::None;
    }
  };
  // First step.
  Operand a, b;
  PK ka, kb;
  if (s0.op == Op::Neg) {
    if (pk_of(s0.a) != PK::G) return false;
    a = resolve(s0.a);
    ka = PK::G;
    kb = PK::None;
  } else if (is_arith(s0.op)) {
    ka = pk_of(s0.a);
    kb = pk_of(s0.b);
    if (ka == PK::None || kb == PK::None || (ka == PK::S && kb == PK::S)) return false;
    a = resolve(s0.a);
    b = resolve(s0.b);
    if (is_commutative(s0.op) && ka == PK::G && kb == PK::S) {
      std::swap(a, b);
      std::swap(ka, kb);
    }
  } else {
    return false;
  }
  // Second step.
  const ir::Slot prev{ir::SlotKind::Step, static_cast<std::int32_t>(k)};
  Operand c;
  PK kc = PK::None;
  bool prev_right = false;
  if (s1.op == Op::Neg) {
    if (!(s1.a == prev)) return false;
  } else if (is_arith(s1.op)) {
    const bool left = s1.a == prev, right = s1.b == prev;
    if (left == right) return false;  // neither, or both (t op t)
    const ir::Slot& other = left ? s1.b : s1.a;
    kc = pk_of(other);
    if (kc == PK::None) return false;
    c = resolve(other);
    prev_right = right && !is_commutative(s1.op);
  } else {
    return false;
  }
  sp.op = s0.op;
  sp.op2 = s1.op;
  sp.a = a;
  sp.b = b;
  sp.c = c;
  sp.prev_right = prev_right;
  sp.ir_first = static_cast<int>(k);
  sp.ir_last = static_cast<int>(k) + 1;
  for (int v = 0; v < n_lane_variants; ++v) {
    sp.fn[row_contiguous][v] = pair_kernel(v, s0.op, ka, kb, s1.op, kc, prev_right, false);
    sp.fn[row_indirect][v] = pair_kernel(v, s0.op, ka, kb, s1.op, kc, prev_right, true);
    sp.acc_fn[0][v] = pair_acc_kernel(v, s0.op, ka, kb, s1.op, kc, prev_right, false);
    sp.acc_fn[1][v] = pair_acc_kernel(v, s0.op, ka, kb, s1.op, kc, prev_right, true);
    if (sp.fn[row_contiguous][v] == nullptr || sp.fn[row_indirect][v] == nullptr || sp.acc_fn[0][v] == nullptr ||
        sp.acc_fn[1][v] == nullptr) {
      return false;  // not a tabled shape
    }
  }
  // Name: the second op around the first, operand kinds as the single-step names print them.
  auto kind_name = [](const Operand& o) { return std::string(to_string(o.kind)); };
  std::ostringstream inner;
  inner << to_string(s0.op) << "(" << kind_name(a);
  if (kb != PK::None) inner << "," << kind_name(b);
  inner << ")";
  std::ostringstream name;
  name << to_string(s1.op) << "(";
  if (kc == PK::None) {
    name << inner.str();
  } else if (prev_right) {
    name << kind_name(c) << "," << inner.str();
  } else {
    name << inner.str() << "," << kind_name(c);
  }
  name << ")";
  sp.name = name.str();
  return true;
}

// Step `st` (the one after IR step `prev`, which is the last step the pair `sp` covers and which
// nothing else reads) appended to the pair's chain tail (plan.hpp: StepPlan::n_tail) when it is
// Exp or Log of step prev: the kernel applies the call in place on each stored row
// (kernels_impl.hpp: apply_tails).
bool Interpreter::Impl::add_tail_step(const ir::Step& st, std::size_t prev, StepPlan& sp) {
  if (sp.n_tail >= StepPlan::max_tail) return false;
  if (st.op != Op::Exp && st.op != Op::Log) return false;
  const ir::Slot p{ir::SlotKind::Step, static_cast<std::int32_t>(prev)};
  if (!(st.a == p)) return false;
  sp.tail_op[sp.n_tail++] = st.op;
  if (st.op == Op::Exp && opt.exp == ExpMode::poly) sp.tail_poly = true;
  // Name: the tail around the value in registers, e.g. "mul(neg(gat),col) => exp(vec)".
  std::ostringstream name;
  name << sp.name << " => " << to_string(st.op) << ((st.op == Op::Exp && sp.tail_poly) ? "_poly" : "") << "(vec)";
  sp.name = name.str();
  return true;
}

void Interpreter::Impl::build_group(std::size_t d, GroupPlan& g) {
  const ir::Domain& dom = p->domains[d];
  const ir::Group& grp = p->groups[d];
  g.domain = static_cast<std::int32_t>(d);
  g.rows = dom.rows;
  g.value_base = dom.value_base;
  g.fused = fused[d] != 0;
  g.keep = keep_rows[d];
  g.inlined_into = inlined_into[d];
  table_bytes += g.keep.size() * sizeof(std::int32_t);
  if (g.fused) {
    g.emit_ordinal.assign(static_cast<std::size_t>(g.rows), -1);
    for (std::size_t o = 0; o < p->outputs.size(); ++o) {
      const ir::value_id v = p->outputs[o];
      if (v >= g.value_base && v < g.value_base + g.rows && emitted[static_cast<std::size_t>(v)]) {
        g.emit_ordinal[static_cast<std::size_t>(v - g.value_base)] = static_cast<std::int32_t>(o);
        ++g.emitted;
      }
    }
    table_bytes += g.emit_ordinal.size() * sizeof(std::int32_t);
  }
  if (dom.recurrent) {
    throw std::invalid_argument("exec: domain " + std::to_string(d) + " (" + dom.name +
                                ") is recurrent; scan domains are not supported by this interpreter");
  }
  if (grp.steps.empty()) throw std::invalid_argument("exec: a group with no steps");
  if (is_whole_segment(grp)) {
    const ir::Step& last = grp.steps.back();
    const bool affine = last.op == Op::Affine;
    build_segment(static_cast<std::int32_t>(d), p->segments[static_cast<std::size_t>(last.a.index)], affine, last.konst,
                  dom.rows, g.seg);
    g.whole_segment = true;
    for (int v = 0; v < n_lane_variants; ++v) g.seg_fn[v] = seg_whole_kernel(v, affine);
    return;
  }
  // Kernel calls: a fused pair where two consecutive steps form a chain nobody else reads
  // (build_pair_step), otherwise one call per step.
  std::vector<int> uses(grp.steps.size(), 0);
  for (const ir::Step& st : grp.steps) {
    for (const ir::Slot* sl : {&st.a, &st.b, &st.c, &st.konst}) {
      if (sl->kind == ir::SlotKind::Step) ++uses[static_cast<std::size_t>(sl->index)];
    }
  }
  // Inlined producers: a temporary per inlined gather; the operands that read that gather are
  // re-pointed at the temporary, addressed by tile row.
  g.tile = opt.tile;
  const std::size_t n_tiles = (static_cast<std::size_t>(g.rows) + static_cast<std::size_t>(opt.tile) - 1) /
                              static_cast<std::size_t>(opt.tile);
  for (std::size_t t = 0; t < inline_refs[d].size(); ++t) {
    const InlineRef& ref = inline_refs[d][t];
    InlinedProducer ip;
    ip.domain = ref.producer;
    ip.gather = ref.gather;
    const std::vector<ir::value_id>& ids = p->gathers[ref.gather].index;
    ip.gather_index = ids.data();
    ip.temp = inline_temp(static_cast<int>(t));
    const GroupPlan& pg = groups[static_cast<std::size_t>(ref.producer)];
    ip.index.assign(ids.begin(), ids.end());
    ip.foreign_start.assign(n_tiles + 1, 0);
    for (std::size_t r = 0; r < ids.size(); ++r) {
      const ir::value_id v = ids[r];
      if (v >= pg.value_base && v < pg.value_base + pg.rows) continue;
      ip.index[r] = pg.value_base;
      ip.foreign_row.push_back(static_cast<std::int32_t>(r));
      ip.foreign_id.push_back(v);
      ++ip.foreign_start[r / static_cast<std::size_t>(opt.tile) + 1];
    }
    for (std::size_t q = 1; q <= n_tiles; ++q) ip.foreign_start[q] += ip.foreign_start[q - 1];
    table_bytes += ip.index.size() * sizeof(std::int32_t) + (ip.foreign_row.size() + ip.foreign_id.size()) * sizeof(std::int32_t) +
                   ip.foreign_start.size() * sizeof(std::size_t);
    if (pg.whole_segment) {
      // The producer's segment tables transposed into this group's row order (decide_inline:
      // every row has the same number of members).
      for (int v = 0; v < n_lane_variants; ++v) ip.seg_fn[v] = seg_list_kernel(v, pg.seg.affine);
      const ir::Segment& seg = p->segments[static_cast<std::size_t>(p->groups[static_cast<std::size_t>(ref.producer)].steps.front().a.index)];
      const std::size_t R = ids.size();
      ip.rows = static_cast<std::int32_t>(R);
      ip.len = pg.seg.min_len;
      const std::size_t lenz = static_cast<std::size_t>(ip.len);
      ip.memT.resize(lenz * R);
      if (pg.seg.affine) ip.coefT.resize(lenz * R);
      for (std::size_t r = 0; r < R; ++r) {
        const std::size_t prow = static_cast<std::size_t>(ip.index[r] - pg.value_base);
        const std::size_t lo = static_cast<std::size_t>(seg.offsets[prow]);
        for (std::size_t k = 0; k < lenz; ++k) {
          ip.memT[k * R + r] = seg.members[lo + k];
          if (pg.seg.affine) ip.coefT[k * R + r] = seg.coefs[lo + k];
        }
      }
      ip.konst = pg.seg.konst;
      if (pg.seg.affine && ip.konst.kind == Kind::Col) {
        std::vector<double> c0(R);
        for (std::size_t r = 0; r < R; ++r) c0[r] = pg.seg.konst.data[ip.index[r] - pg.value_base];
        ip.coefT.insert(ip.coefT.end(), c0.begin(), c0.end());   // kept alive behind the transposed coefficients
        ip.konst.data = ip.coefT.data() + lenz * R;
      }
      table_bytes += ip.memT.size() * sizeof(std::int32_t) + ip.coefT.size() * sizeof(double);
    }
    g.inlined.push_back(std::move(ip));
  }
  if (!g.inlined.empty()) {
    g.tile_index.resize(static_cast<std::size_t>(g.rows));
    for (std::int32_t r = 0; r < g.rows; ++r) g.tile_index[static_cast<std::size_t>(r)] = r % opt.tile;
    table_bytes += g.tile_index.size() * sizeof(std::int32_t);
  }
  auto inline_operand = [&](Operand& o) {
    if (o.kind != Kind::Gat) return;
    for (const InlinedProducer& ip : g.inlined) {
      if (o.index == ip.gather_index) {
        o.data = ip.temp;
        o.index = g.tile_index.data();
        return;
      }
    }
  };
  for (std::size_t k = 0; k < grp.steps.size();) {
    StepPlan sp;
    if (opt.fuse_pairs && k + 1 < grp.steps.size() && build_pair_step(grp, uses, k, sp)) {
      // The chain continues while the last covered step is read only by the next one and that
      // step is a tail shape (a unary, or a binary with a literal / column).
      std::size_t last = k + 1;
      while (last + 1 < grp.steps.size() && uses[last] == 1 && add_tail_step(grp.steps[last + 1], last, sp)) ++last;
      if (sp.n_tail > 0) {
        for (int v = 0; v < n_lane_variants; ++v) sp.acc_fn[0][v] = sp.acc_fn[1][v] = nullptr;  // epilogues apply no tails
      }
      sp.ir_last = static_cast<int>(last);
      sp.out = (last + 1 == grp.steps.size()) ? nullptr : step_buffer(static_cast<int>(last));
      g.steps.push_back(std::move(sp));
      k = last + 1;
    } else {
      build_step(grp.steps[k], k, grp.steps.size(), g, sp);
      sp.ir_first = sp.ir_last = static_cast<int>(k);
      g.steps.push_back(std::move(sp));
      k += 1;
    }
  }
  if (!g.inlined.empty()) {
    for (StepPlan& s : g.steps) {
      inline_operand(s.a);
      inline_operand(s.b);
      inline_operand(s.c);
      inline_operand(s.konst);
      for (int q = 0; q < s.n_pre; ++q) inline_operand(s.pre[q].src);
    }
  }
}

Interpreter::Interpreter(const ir::Program& program, Options options) : impl_(std::make_unique<Impl>()) {
  Impl& im = *impl_;
  im.p = &program;
  im.opt = options;
  ir::validate(program);
  if (options.tile < 1) throw std::invalid_argument("exec: tile must be >= 1");
  if (options.max_batch < 1) throw std::invalid_argument("exec: max_batch must be >= 1");
  if (options.lane_tile < 1) throw std::invalid_argument("exec: lane_tile must be >= 1");
  for (const ir::Group& g : program.groups) {
    for (const ir::Step& s : g.steps) {
      if (!op_is_supported(s.op)) {
        throw std::invalid_argument(std::string("exec: unsupported op ") + to_string(s.op));
      }
    }
  }
  im.Lt = std::min(options.lane_tile, options.max_batch);
  im.max_steps = 0;
  for (const ir::Group& g : program.groups) im.max_steps = std::max(im.max_steps, static_cast<int>(g.steps.size()));
  im.tile_elems = static_cast<std::size_t>(options.tile) * static_cast<std::size_t>(im.Lt);
  im.values.assign(program.num_values() * static_cast<std::size_t>(im.Lt), 0.0);
  im.groups.resize(program.domains.size());
  im.decide_fusion();
  im.decide_inline();
  im.scratch.assign(static_cast<std::size_t>(im.max_steps + 5 + im.n_inline_temps) * im.tile_elems, 0.0);
  // Producers before consumers: a fused or inlined domain's consumers come later in Program
  // order (validate: no forward reads), so building in order lets build_segment record them and
  // lets a consumer find its inlined producers' plans.
  for (std::size_t d = 0; d < program.domains.size(); ++d) im.build_group(d, im.groups[d]);
}

Interpreter::~Interpreter() = default;

void Interpreter::run(const double* state, int B, double* out) const {
  const Impl& im = *impl_;
  if (B < 1 || B > im.opt.max_batch) {
    throw std::invalid_argument("exec: B must be in [1, max_batch] (" + std::to_string(B) + " vs " +
                                std::to_string(im.opt.max_batch) + ")");
  }
  const int tile = im.opt.tile;
  double* values = im.values.data();
  for (int b0 = 0; b0 < B; b0 += im.Lt) {
    const int L = std::min(im.Lt, B - b0);
    RunCtx ctx;
    ctx.values = values;
    ctx.state = state;
    ctx.out = out;
    ctx.B = B;
    ctx.b0 = b0;
    ctx.L = L;
    ctx.v = lane_variant(L);
    ctx.acc = im.acc_buffer();
    ctx.member = im.member_buffer();
    ctx.groups = im.groups.data();
    const std::size_t Ls = static_cast<std::size_t>(L);
    for (const GroupPlan& g : im.groups) {
      double* dom = values + static_cast<std::size_t>(g.value_base) * Ls;
      EPYKOS_EXEC_PROFILE_SCOPE(g.domain);
      if (g.inlined_into >= 0) continue;   // evaluated per tile of its consumer
      if (g.fused) {
        // Evaluated inside its consumers' reductions; only the kept rows are materialised:
        // evaluated in tiles of the row list into the member buffer, then copied to their slots.
        const std::int32_t* keep = g.keep.data();
        const int n_keep = static_cast<int>(g.keep.size());
        for (int k0 = 0; k0 < n_keep; k0 += tile) {
          const int n = std::min(tile, n_keep - k0);
          eval_group(g, ctx, 0, keep + k0, n, ctx.member);
          for (int i = 0; i < n; ++i) {
            const double* src = ctx.member + static_cast<std::size_t>(i) * Ls;
            double* dst = dom + static_cast<std::size_t>(keep[k0 + i]) * Ls;
            for (int l = 0; l < L; ++l) dst[l] = src[l];
          }
        }
        continue;
      }
      if (g.whole_segment) {
        g.seg_fn[ctx.v](g.seg, ctx, dom);
        continue;
      }
      for (int r0 = 0; r0 < g.rows; r0 += tile) {
        const int n = std::min(tile, g.rows - r0);
        eval_group(g, ctx, r0, nullptr, n, dom + static_cast<std::size_t>(r0) * Ls);
      }
    }
    {
      EPYKOS_EXEC_PROFILE_SCOPE(63);
      copy_out(ctx.v, values, im.late_ids.data(), im.late_ords.data(), static_cast<int>(im.late_ids.size()), out, B, b0, L);
    }
  }
  EPYKOS_EXEC_PROFILE_RUN();
}

std::string Interpreter::describe() const {
  const Impl& im = *impl_;
  const ir::Program& p = *im.p;
  std::ostringstream os;
  os << "interpreter plan: tile " << im.opt.tile << " rows, lane_tile " << im.opt.lane_tile << " (buffers hold " << im.Lt
     << " lanes), max_batch " << im.opt.max_batch << ", exp " << to_string(im.opt.exp) << ", fuse_reductions "
     << (im.opt.fuse_reductions ? "on" : "off") << '\n';
  os << "  lane-width kernels: runtime";
  for (int v = 1; v < n_lane_variants; ++v) os << ", " << lane_variants[v];
  os << "; a chunk of L lanes uses the L kernel or the runtime one\n";
  os << "  values: " << p.num_values() << " rows x " << im.Lt << " lanes = " << human_bytes(value_bytes()) << " ("
     << im.fused_values << " rows fused or inlined, never materialised); scratch: " << im.max_steps
     << " step + 3 temporary + accumulator + member + " << im.n_inline_temps << " inlined-producer buffers x "
     << im.opt.tile << " x " << im.Lt << " doubles = " << human_bytes(scratch_bytes()) << "; plan tables "
     << human_bytes(table_bytes()) << '\n';
  std::size_t total_tiles = 0, total_calls = 0;
  for (std::size_t d = 0; d < im.groups.size(); ++d) {
    const GroupPlan& g = im.groups[d];
    const ir::Domain& dom = p.domains[d];
    os << "  d" << d << " " << ir::shape_string(p, static_cast<ir::domain_id>(d)) << " rows " << dom.rows << " level "
       << dom.level << " reads {";
    for (std::size_t k = 0; k < dom.reads.size(); ++k) os << (k ? " " : "") << "d" << dom.reads[k];
    os << "} | ";
    if (g.inlined_into >= 0) {
      os << "inlined into d" << g.inlined_into << " (evaluated per consumer tile for the rows it gathers, not materialised): ";
    }
    if (!g.inlined.empty()) {
      os << "inlines {";
      for (std::size_t k = 0; k < g.inlined.size(); ++k) os << (k ? " " : "") << "d" << g.inlined[k].domain;
      os << "} per tile; ";
    }
    if (g.whole_segment) {
      const SegPlan& sp = g.seg;
      os << "whole-domain " << (sp.affine ? "affine" : "sum") << ": members per row " << sp.min_len << ".." << sp.max_len
         << ", " << sp.memT.size() << " members, " << sp.patterns << " (length, producer) bucket(s) in " << sp.blocks.size()
         << " block(s) of <= " << im.opt.tile << " rows, transposed";
      if (sp.affine) os << ", c_0 " << to_string(sp.konst.kind);
      if (sp.fused_members > 0) {
        os << "; " << sp.fused_members << " members evaluated in the block from {";
        for (std::size_t k = 0; k < sp.producers.size(); ++k) {
          const FusedProducer& fp = sp.producers[k];
          os << (k ? " " : "") << "d" << fp.domain << (fp.block_mode ? " (operands in member order, epilogue)" : " (indirect rows, member buffer)");
        }
        os << "}, " << sp.memT.size() - sp.fused_members << " gathered";
      }
      if (g.inlined_into < 0) {
        ++total_tiles;
        ++total_calls;
      }
    } else {
      if (g.inlined_into >= 0) {
        // counted with the consumer's tiles
      } else if (g.fused) {
        os << "fused into {";
        for (std::size_t k = 0; k < g.consumers.size(); ++k) os << (k ? " " : "") << "d" << g.consumers[k];
        os << "} (evaluated per reduction block, not materialised";
        if (!g.keep.empty()) {
          const std::size_t tiles = (g.keep.size() + static_cast<std::size_t>(im.opt.tile) - 1) /
                                    static_cast<std::size_t>(im.opt.tile);
          os << "; " << g.keep.size() << " rows read elsewhere kept in " << tiles << " tile(s)";
          for (const StepPlan& s : g.steps) total_calls += tiles * static_cast<std::size_t>(1 + s.n_pre);
          total_tiles += tiles;
        }
        if (g.emitted > 0) os << "; " << g.emitted << " output rows written from the reduction blocks";
        os << "): ";
      } else {
        const std::size_t tiles = (static_cast<std::size_t>(dom.rows) + static_cast<std::size_t>(im.opt.tile) - 1) /
                                  static_cast<std::size_t>(im.opt.tile);
        os << tiles << " tile(s): ";
        for (const StepPlan& s : g.steps) total_calls += tiles * static_cast<std::size_t>(1 + s.n_pre);
        for (const InlinedProducer& ip : g.inlined) {
          const GroupPlan& pg = im.groups[static_cast<std::size_t>(ip.domain)];
          std::size_t calls = 1;
          if (!pg.whole_segment) {
            calls = 0;
            for (const StepPlan& s : pg.steps) calls += static_cast<std::size_t>(1 + s.n_pre);
          }
          total_calls += tiles * calls;
        }
        total_tiles += tiles;
      }
      for (std::size_t k = 0; k < g.steps.size(); ++k) {
        const StepPlan& s = g.steps[k];
        os << (k ? "; " : "") << s.name << " -> " << (s.out ? "s" + std::to_string(s.ir_last) : (g.fused ? "member" : "values"));
      }
    }
    os << '\n';
  }
  os << "  per lane chunk: " << total_tiles << " tile passes, " << total_calls
     << " kernel calls outside reductions; outputs " << p.outputs.size() << " (" << im.late_ids.size()
     << " copied from the value buffer, " << p.outputs.size() - im.late_ids.size() << " emitted by reduction blocks), inputs "
     << p.inputs.size() << '\n';
  return os.str();
}

const Options& Interpreter::options() const noexcept { return impl_->opt; }
const ir::Program& Interpreter::program() const noexcept { return *impl_->p; }
int Interpreter::n_inputs() const noexcept { return static_cast<int>(impl_->p->inputs.size()); }
int Interpreter::n_outputs() const noexcept { return static_cast<int>(impl_->p->outputs.size()); }
int Interpreter::max_batch() const noexcept { return impl_->opt.max_batch; }
std::size_t Interpreter::num_values() const noexcept { return impl_->p->num_values(); }
std::size_t Interpreter::num_fused_values() const noexcept { return impl_->fused_values; }
std::size_t Interpreter::value_bytes() const noexcept { return impl_->values.size() * sizeof(double); }
std::size_t Interpreter::scratch_bytes() const noexcept { return impl_->scratch.size() * sizeof(double); }
std::size_t Interpreter::table_bytes() const noexcept { return impl_->table_bytes; }

}  // namespace epykos::exec
