// EpykosEngine — the tiled vector interpreter: plan construction, the run loop and describe().
// See include/epykos/exec/interpreter.hpp for the design. No floating-point arithmetic happens
// in this TU (the kernels live in kernels_impl.hpp, instantiated per lane width in kernels_l*.cpp
// with contraction off).
#include "epykos/exec/interpreter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
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

OpKernel binary_kernel(int v, Op op, Kind ka, Kind kb) { EPYKOS_EXEC_DISPATCH(v, binary(op, ka, kb)) }
OpKernel unary_kernel(int v, Op op, Kind ka) { EPYKOS_EXEC_DISPATCH(v, unary(op, ka)) }
OpKernel exp_poly_kernel(int v, Kind ka) { EPYKOS_EXEC_DISPATCH(v, exp_poly(ka)) }
OpKernel ternary_kernel(int v, Op op) { EPYKOS_EXEC_DISPATCH(v, ternary(op)) }
OpKernel konst_kernel(int v, Kind kk) { EPYKOS_EXEC_DISPATCH(v, konst(kk)) }
OpKernel input_kernel(int v) { EPYKOS_EXEC_DISPATCH(v, input()) }
OpKernel seg_rows_kernel(int v, bool affine) { EPYKOS_EXEC_DISPATCH(v, seg_rows(affine)) }
LoadKernel load_kernel(int v, Kind k) { EPYKOS_EXEC_DISPATCH(v, load(k)) }
SegKernel seg_whole_kernel(int v, bool affine) { EPYKOS_EXEC_DISPATCH(v, seg_whole(affine)) }

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

}  // namespace

struct Interpreter::Impl {
  const ir::Program* p = nullptr;
  Options opt;
  int Lt = 0;              // lanes the buffers hold (min(lane_tile, max_batch))
  int max_steps = 0;       // longest elementwise group
  std::size_t tile_elems = 0;  // tile · Lt doubles per scratch buffer
  std::size_t table_bytes = 0;

  mutable std::vector<double> values;   // num_values · Lt
  mutable std::vector<double> scratch;  // (max_steps + 3 temporaries + 1 accumulator) · tile_elems
  std::vector<GroupPlan> groups;

  double* step_buffer(int k) const { return scratch.data() + static_cast<std::size_t>(k) * tile_elems; }
  double* tmp_buffer(int j) const { return step_buffer(max_steps + j); }
  double* acc_buffer() const { return step_buffer(max_steps + 3); }

  Operand resolve(const ir::Slot& s) const;
  void build_group(std::size_t d, GroupPlan& g);
  void build_segment(const ir::Segment& seg, bool affine, const ir::Slot& konst, std::int32_t rows, SegPlan& sp);
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

void Interpreter::Impl::build_segment(const ir::Segment& seg, bool affine, const ir::Slot& konst, std::int32_t rows,
                                      SegPlan& sp) {
  sp.affine = affine;
  if (affine) sp.konst = resolve(konst);
  sp.offsets = seg.offsets.data();
  sp.members = seg.members.data();
  sp.coefs = seg.coefs.data();
  if (static_cast<std::int32_t>(seg.offsets.size()) != rows + 1) {
    throw std::invalid_argument("exec: segment offsets do not match the domain's rows");
  }
  // Rows ordered by segment length (stable: ties keep row order), then buckets of equal length
  // split into blocks of at most `tile` rows.
  std::vector<std::int32_t> order(static_cast<std::size_t>(rows));
  std::iota(order.begin(), order.end(), 0);
  auto len_of = [&](std::int32_t r) { return seg.offsets[static_cast<std::size_t>(r) + 1] - seg.offsets[static_cast<std::size_t>(r)]; };
  std::stable_sort(order.begin(), order.end(), [&](std::int32_t x, std::int32_t y) { return len_of(x) < len_of(y); });
  sp.rows.reserve(order.size());
  sp.memT.reserve(seg.members.size());
  if (affine) sp.coefT.reserve(seg.members.size());
  sp.min_len = rows > 0 ? len_of(order.front()) : 0;
  sp.max_len = rows > 0 ? len_of(order.back()) : 0;
  if (!affine && rows > 0 && sp.min_len < 1) throw std::invalid_argument("exec: a Sum row with no members");
  std::size_t pos = 0;
  while (pos < order.size()) {
    const std::int32_t len = len_of(order[pos]);
    std::size_t end = pos;
    while (end < order.size() && len_of(order[end]) == len && end - pos < static_cast<std::size_t>(opt.tile)) ++end;
    SegBlock blk;
    blk.n = static_cast<std::int32_t>(end - pos);
    blk.len = len;
    blk.row0 = sp.rows.size();
    blk.memT = sp.memT.size();
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
    sp.blocks.push_back(blk);
    pos = end;
  }
  table_bytes += sp.rows.size() * sizeof(std::int32_t) + sp.memT.size() * sizeof(std::int32_t) +
                 sp.coefT.size() * sizeof(double) + sp.blocks.size() * sizeof(SegBlock);
}

void Interpreter::Impl::build_step(const ir::Step& st, std::size_t k, std::size_t n_steps, GroupPlan& g, StepPlan& sp) {
  sp.op = st.op;
  sp.out = (k + 1 == n_steps) ? nullptr : step_buffer(static_cast<int>(k));
  std::ostringstream name;
  name << to_string(st.op);
  auto kind_name = [](const Operand& o) { return std::string(to_string(o.kind)); };
  switch (st.op) {
    case Op::Const: {
      sp.konst = resolve(st.konst);
      for (int v = 0; v < n_lane_variants; ++v) sp.fn[v] = konst_kernel(v, sp.konst.kind);
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
      for (int v = 0; v < n_lane_variants; ++v) sp.fn[v] = input_kernel(v);
      break;
    }
    case Op::Neg:
    case Op::Exp:
    case Op::Log:
    case Op::Sqrt:
    case Op::Recip: {
      sp.a = resolve(st.a);
      const bool poly = st.op == Op::Exp && opt.exp == ExpMode::poly;
      for (int v = 0; v < n_lane_variants; ++v) {
        sp.fn[v] = poly ? exp_poly_kernel(v, sp.a.kind) : unary_kernel(v, st.op, sp.a.kind);
      }
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
      for (int v = 0; v < n_lane_variants; ++v) sp.fn[v] = binary_kernel(v, st.op, sp.a.kind, sp.b.kind);
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
          for (int v = 0; v < n_lane_variants; ++v) pre.fn[v] = load_kernel(v, ops[j].kind);
          ops[j] = Operand{Kind::Vec, pre.dst, nullptr};
        }
      }
      name << ")";
      sp.a = ops[0];
      sp.b = ops[1];
      sp.c = ops[2];
      for (int v = 0; v < n_lane_variants; ++v) sp.fn[v] = ternary_kernel(v, st.op);
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
      for (int v = 0; v < n_lane_variants; ++v) sp.fn[v] = seg_rows_kernel(v, affine);
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
  if (dom.recurrent) {
    throw std::invalid_argument("exec: domain " + std::to_string(d) + " (" + dom.name +
                                ") is recurrent; scan domains are not supported by this interpreter");
  }
  if (grp.steps.empty()) throw std::invalid_argument("exec: a group with no steps");
  const ir::Step& last = grp.steps.back();
  if (grp.steps.size() == 1 && (last.op == Op::Sum || last.op == Op::Affine)) {
    if (last.a.kind != ir::SlotKind::Segment) throw std::invalid_argument("exec: Sum/Affine without a segment");
    const bool affine = last.op == Op::Affine;
    build_segment(p->segments[static_cast<std::size_t>(last.a.index)], affine, last.konst, dom.rows, g.seg);
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
  im.scratch.assign(static_cast<std::size_t>(im.max_steps + 4) * im.tile_elems, 0.0);
  im.values.assign(program.num_values() * static_cast<std::size_t>(im.Lt), 0.0);
  im.groups.resize(program.domains.size());
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
    const int v = lane_variant(L);
    RunCtx ctx;
    ctx.values = values;
    ctx.state = state;
    ctx.B = B;
    ctx.b0 = b0;
    ctx.L = L;
    ctx.acc = im.acc_buffer();
    const std::size_t Ls = static_cast<std::size_t>(L);
    for (const GroupPlan& g : im.groups) {
      double* dom = values + static_cast<std::size_t>(g.value_base) * Ls;
      if (g.whole_segment) {
        g.seg_fn[v](g.seg, ctx, dom);
        continue;
      }
      for (int r0 = 0; r0 < g.rows; r0 += tile) {
        const int n = std::min(tile, g.rows - r0);
        for (const StepPlan& s : g.steps) {
          for (int j = 0; j < s.n_pre; ++j) s.pre[j].fn[v](s.pre[j].src, ctx, r0, n, s.pre[j].dst);
          double* o = s.out != nullptr ? s.out : dom + static_cast<std::size_t>(r0) * Ls;
          s.fn[v](s, ctx, r0, n, o);
        }
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
     << " lanes), max_batch " << im.opt.max_batch << ", exp " << to_string(im.opt.exp) << '\n';
  os << "  lane-width kernels: runtime";
  for (int v = 1; v < n_lane_variants; ++v) os << ", " << lane_variants[v];
  os << "; a chunk of L lanes uses the L kernel or the runtime one\n";
  os << "  values: " << p.num_values() << " rows x " << im.Lt << " lanes = " << human_bytes(value_bytes())
     << "; scratch: " << im.max_steps << " step + 3 temporary + 1 accumulator buffers x " << im.opt.tile << " x " << im.Lt
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
      std::size_t buckets = 0;
      std::int32_t prev = -1;
      for (const SegBlock& b : sp.blocks) {
        if (b.len != prev) ++buckets;
        prev = b.len;
      }
      os << "whole-domain " << (sp.affine ? "affine" : "sum") << ": members per row " << sp.min_len << ".." << sp.max_len
         << ", " << sp.memT.size() << " members, " << buckets << " length bucket(s) in " << sp.blocks.size()
         << " block(s) of <= " << im.opt.tile << " rows, transposed";
      if (sp.affine) os << ", c_0 " << to_string(sp.konst.kind);
      ++total_tiles;
      ++total_calls;
    } else {
      const std::size_t tiles = (static_cast<std::size_t>(dom.rows) + static_cast<std::size_t>(im.opt.tile) - 1) /
                                static_cast<std::size_t>(im.opt.tile);
      os << tiles << " tile(s): ";
      for (std::size_t k = 0; k < g.steps.size(); ++k) {
        const StepPlan& s = g.steps[k];
        os << (k ? "; " : "") << s.name << " -> " << (s.out ? "s" + std::to_string(k) : "values");
        total_calls += tiles * static_cast<std::size_t>(1 + s.n_pre);
      }
      total_tiles += tiles;
    }
    os << '\n';
  }
  os << "  per lane chunk: " << total_tiles << " tile passes, " << total_calls << " kernel calls; outputs "
     << p.outputs.size() << ", inputs " << p.inputs.size() << '\n';
  return os.str();
}

const Options& Interpreter::options() const noexcept { return impl_->opt; }
const ir::Program& Interpreter::program() const noexcept { return *impl_->p; }
int Interpreter::n_inputs() const noexcept { return static_cast<int>(impl_->p->inputs.size()); }
int Interpreter::n_outputs() const noexcept { return static_cast<int>(impl_->p->outputs.size()); }
int Interpreter::max_batch() const noexcept { return impl_->opt.max_batch; }
std::size_t Interpreter::num_values() const noexcept { return impl_->p->num_values(); }
std::size_t Interpreter::value_bytes() const noexcept { return impl_->values.size() * sizeof(double); }
std::size_t Interpreter::scratch_bytes() const noexcept { return impl_->scratch.size() * sizeof(double); }
std::size_t Interpreter::table_bytes() const noexcept { return impl_->table_bytes; }

}  // namespace epykos::exec
