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
AccKernel binary_acc_kernel(int v, Op op, Kind ka, Kind kb, bool affine) { EPYKOS_EXEC_DISPATCH(v, binary_acc(op, ka, kb, affine)) }
AccKernel unary_acc_kernel(int v, Op op, Kind ka, bool affine) { EPYKOS_EXEC_DISPATCH(v, unary_acc(op, ka, affine)) }
AccKernel konst_acc_kernel(int v, Kind kk, bool affine) { EPYKOS_EXEC_DISPATCH(v, konst_acc(kk, affine)) }
LoadAccKernel load_acc_kernel(int v, Kind k, bool affine, bool ind) { EPYKOS_EXEC_DISPATCH(v, load_acc(k, affine, ind)) }

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

  double* step_buffer(int k) const { return scratch.data() + static_cast<std::size_t>(k) * tile_elems; }
  double* tmp_buffer(int j) const { return step_buffer(max_steps + j); }
  double* acc_buffer() const { return step_buffer(max_steps + 3); }
  double* member_buffer() const { return step_buffer(max_steps + 4); }

  Operand resolve(const ir::Slot& s) const;
  void decide_fusion();
  void build_group(std::size_t d, GroupPlan& g);
  void build_segment(std::int32_t consumer, const ir::Segment& seg, bool affine, const ir::Slot& konst, std::int32_t rows,
                     SegPlan& sp);
  void build_step(const ir::Step& st, std::size_t k, std::size_t n_steps, GroupPlan& g, StepPlan& sp);
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
      break;
    case ir::SlotKind::Column:
      o.kind = Kind::Col;
      o.data = p->columns[static_cast<std::size_t>(s.index)].values.data();
      break;
    case ir::SlotKind::Gather:
      o.kind = Kind::Gat;
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
// (gather, output, per-row Sum / Affine) — those rows are kept: evaluated separately and
// written to the value buffer. A structure-only decision; the arithmetic is the same either way.
void Interpreter::Impl::decide_fusion() {
  const ir::Program& prog = *p;
  const std::size_t nd = prog.domains.size();
  fused.assign(nd, 0);
  keep_rows.assign(nd, {});
  if (!opt.fuse_reductions) return;
  std::vector<char> marked(prog.num_values(), 0);   // value read other than as a whole-domain member
  std::vector<std::size_t> member_refs(nd, 0);
  auto dom_of = [&](ir::value_id v) { return static_cast<std::size_t>(prog.domain_of(v)); };
  for (const ir::Gather& g : prog.gathers) {
    for (ir::value_id v : g.index) marked[static_cast<std::size_t>(v)] = 1;
  }
  for (ir::value_id v : prog.outputs) marked[static_cast<std::size_t>(v)] = 1;
  for (const ir::Segment& seg : prog.segments) {
    const bool whole = is_whole_segment(prog.groups[static_cast<std::size_t>(seg.domain)]);
    for (ir::value_id v : seg.members) {
      if (whole) {
        ++member_refs[dom_of(v)];
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
    std::vector<std::int32_t> keep;
    for (std::int32_t r = 0; r < dom.rows; ++r) {
      if (marked[static_cast<std::size_t>(dom.value_base + r)]) keep.push_back(r);
    }
    if (static_cast<double>(keep.size()) > fuse_max_kept_fraction * static_cast<double>(dom.rows)) continue;
    fused[d] = 1;
    fused_values += static_cast<std::size_t>(dom.rows) - keep.size();
    keep_rows[d] = std::move(keep);
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
    if (!fp.block_mode) continue;
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

void Interpreter::Impl::build_group(std::size_t d, GroupPlan& g) {
  const ir::Domain& dom = p->domains[d];
  const ir::Group& grp = p->groups[d];
  g.domain = static_cast<std::int32_t>(d);
  g.rows = dom.rows;
  g.value_base = dom.value_base;
  g.fused = fused[d] != 0;
  g.keep = keep_rows[d];
  table_bytes += g.keep.size() * sizeof(std::int32_t);
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
  g.steps.resize(grp.steps.size());
  for (std::size_t k = 0; k < grp.steps.size(); ++k) build_step(grp.steps[k], k, grp.steps.size(), g, g.steps[k]);
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
  im.scratch.assign(static_cast<std::size_t>(im.max_steps + 5) * im.tile_elems, 0.0);
  im.values.assign(program.num_values() * static_cast<std::size_t>(im.Lt), 0.0);
  im.groups.resize(program.domains.size());
  im.decide_fusion();
  // Producers before consumers: a fused domain's consumers come later in Program order
  // (validate: no forward reads), so building in order lets build_segment record them.
  for (std::size_t d = 0; d < program.domains.size(); ++d) im.build_group(d, im.groups[d]);
}

Interpreter::~Interpreter() = default;

void Interpreter::run(const double* state, int B, double* out) const {
  const Impl& im = *impl_;
  if (B < 1 || B > im.opt.max_batch) {
    throw std::invalid_argument("exec: B must be in [1, max_batch] (" + std::to_string(B) + " vs " +
                                std::to_string(im.opt.max_batch) + ")");
  }
  const ir::Program& p = *im.p;
  const int tile = im.opt.tile;
  double* values = im.values.data();
  for (int b0 = 0; b0 < B; b0 += im.Lt) {
    const int L = std::min(im.Lt, B - b0);
    RunCtx ctx;
    ctx.values = values;
    ctx.state = state;
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
    const std::size_t Bs = static_cast<std::size_t>(B);
    for (std::size_t o = 0; o < p.outputs.size(); ++o) {
      const double* src = values + static_cast<std::size_t>(p.outputs[o]) * Ls;
      double* dst = out + o * Bs + static_cast<std::size_t>(b0);
      for (int l = 0; l < L; ++l) dst[l] = src[l];
    }
  }
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
     << im.fused_values << " rows fused, never materialised); scratch: " << im.max_steps
     << " step + 3 temporary + accumulator + member buffers x " << im.opt.tile << " x " << im.Lt
     << " doubles = " << human_bytes(scratch_bytes()) << "; plan tables " << human_bytes(table_bytes()) << '\n';
  std::size_t total_tiles = 0, total_calls = 0;
  for (std::size_t d = 0; d < im.groups.size(); ++d) {
    const GroupPlan& g = im.groups[d];
    const ir::Domain& dom = p.domains[d];
    os << "  d" << d << " " << ir::shape_string(p, static_cast<ir::domain_id>(d)) << " rows " << dom.rows << " level "
       << dom.level << " reads {";
    for (std::size_t k = 0; k < dom.reads.size(); ++k) os << (k ? " " : "") << "d" << dom.reads[k];
    os << "} | ";
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
      ++total_tiles;
      ++total_calls;
    } else {
      if (g.fused) {
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
        os << "): ";
      } else {
        const std::size_t tiles = (static_cast<std::size_t>(dom.rows) + static_cast<std::size_t>(im.opt.tile) - 1) /
                                  static_cast<std::size_t>(im.opt.tile);
        os << tiles << " tile(s): ";
        for (const StepPlan& s : g.steps) total_calls += tiles * static_cast<std::size_t>(1 + s.n_pre);
        total_tiles += tiles;
      }
      for (std::size_t k = 0; k < g.steps.size(); ++k) {
        const StepPlan& s = g.steps[k];
        os << (k ? "; " : "") << s.name << " -> " << (s.out ? "s" + std::to_string(k) : (g.fused ? "member" : "values"));
      }
    }
    os << '\n';
  }
  os << "  per lane chunk: " << total_tiles << " tile passes, " << total_calls
     << " kernel calls outside reductions; outputs " << p.outputs.size() << ", inputs " << p.inputs.size() << '\n';
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
