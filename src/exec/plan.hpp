// EpykosEngine — private plan structures of the tiled interpreter (src/exec only).
//
// The plan is the Program re-expressed for execution: per step a kernel per lane-width variant
// and row mode, with operand descriptors whose sources are resolved to pointers; per Sum / Affine
// group the length-bucketed, transposed segment tables. Everything is built once in the
// constructor and read-only during run().
//
// Row modes. A kernel evaluates n tile rows i = 0..n-1; the domain row of tile row i is either
// r0 + i (contiguous: the main loop's tiles) or idx[i] + r0 (indirect: a reduction evaluating a
// producer domain for one member position of a block, where idx is the block's member value ids
// and r0 = -producer.value_base). Operands are read by domain row (columns, gathers, ordinals)
// or by tile row (earlier steps' scratch), so the two modes share one kernel body.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "epykos/tape/op.hpp"

namespace epykos::exec::detail {

// Where a kernel reads an operand from, resolved at construction.
enum class Kind : std::uint8_t {
  Vec = 0,  // a tile-shaped vector: an earlier step's scratch or an operand temporary, [i·L + l]
  Lit = 1,  // one double, broadcast
  Col = 2,  // one double per row of the domain, broadcast across lanes
  Gat = 3,  // a value id per row: read from the value buffer, [id·L + l]
};

const char* to_string(Kind k) noexcept;

struct Operand {
  Kind kind = Kind::Lit;
  const double* data = nullptr;        // Vec: the buffer; Lit: &value; Col: values by row; Gat: the rows' base (the value buffer, or an inlined producer's tile temporary)
  const std::int32_t* index = nullptr; // Gat: row of `data` (a value id, or a tile row) by domain row
  std::int32_t stride = 0;             // Lit: 0, Col: 1 — a per-row scalar is data[row · stride] (pair kernels)
};

// Operand kinds of a fused pair (below): a per-row scalar (Lit or Col through Operand::stride),
// a gathered row, or unused (the second operand of Neg).
enum class PK : std::uint8_t { S = 0, G = 1, None = 2 };

const char* to_string(PK k) noexcept;

// Lane-width variants the kernels are instantiated for. Variant 0 is the runtime-L fallback.
inline constexpr int lane_variants[] = {0, 1, 4, 8, 16, 32, 64};
inline constexpr int n_lane_variants = 7;
inline int lane_variant(int L) noexcept {
  for (int v = 1; v < n_lane_variants; ++v) {
    if (lane_variants[v] == L) return v;
  }
  return 0;
}

// Row modes a kernel is instantiated for: index into the fn[][] tables.
inline constexpr int row_contiguous = 0;
inline constexpr int row_indirect = 1;

// Rows a reduction epilogue keeps in flight in local accumulators for lane width L: 32 lanes
// (eight 4-wide accumulators) at 4 <= L < 32, 16 rows at L = 1, one row at L >= 32. Fused
// blocks are sized so that their rows fill these accumulators (build_segment).
constexpr int acc_rows_in_flight_for(int L) noexcept { return L >= 32 ? 1 : (L == 1 ? 16 : 32 / L); }

struct GroupPlan;
struct StepPlan;
struct SegPlan;
struct RunCtx;

// Kernel entry points (kernels_impl.hpp). Rows: r0 / idx per the row modes above; n tile rows;
// `out` tile-shaped ([i·L + l]).
using OpKernel = void (*)(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out);
using LoadKernel = void (*)(const Operand& src, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* out);
using SegKernel = void (*)(const SegPlan& sp, const RunCtx& ctx, double* domain_values);
struct InlinedProducer;
// A whole-domain Sum / Affine inlined producer evaluated for consumer rows r0 .. r0 + n into
// out[i·L + l] from its consumer-ordered transposed tables (InlinedProducer::memT).
using SegListKernel = void (*)(const InlinedProducer& ip, const RunCtx& ctx, int r0, int n, double* out);
// Reduction epilogue: evaluates a step (or loads an operand) for kr member positions of n rows
// (member row idx[kk·n + i] + r0 of position kk, tile row kk·n + i for Vec operands) and folds
// the values into acc[i·L + l] in position order: Sum acc = acc + v (acc = v at the block's
// first position when `first`); Affine p = coef[kk·n + i]·v, acc = acc + p. Rows are kept in
// flight in local accumulators, so members are never stored.
using AccKernel = void (*)(const StepPlan& s, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, int kr,
                           bool first, const double* coef, double* acc);
using LoadAccKernel = void (*)(const Operand& src, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, int kr,
                               bool first, const double* coef, double* acc);

// Per-run context handed to every kernel.
struct RunCtx {
  double* values = nullptr;         // the value buffer of the current chunk, stride L
  const double* state = nullptr;    // inputs, state[k·B + b]
  double* out = nullptr;            // outputs, out[o·B + b] (reduction blocks emit the outputs of their fused producers)
  int B = 0;                        // batch width of the run
  int b0 = 0;                       // first lane of the chunk
  int L = 0;                        // lanes in the chunk
  int v = 0;                        // lane-width variant of the chunk (lane_variant(L))
  double* acc = nullptr;            // reduction accumulator, tile·Lmax doubles
  double* member = nullptr;         // reduction member buffer, tile·Lmax doubles
  const GroupPlan* groups = nullptr;  // all groups (a reduction evaluates its fused producers)
};

// A bucket of rows of one segment length and one member-producer pattern, stored transposed:
// memT[k·n + i] is member k of the bucket's row i; coefT likewise for Affine. A gathered block
// holds up to `tile` rows; a fused block holds up to `tile` members (n·len <= tile, or one row
// whose members are evaluated `kc` positions at a time), so that a producer run of member
// positions is evaluated by one kernel call over up to `tile` member rows.
struct SegBlock {
  std::int32_t n = 0;       // rows
  std::int32_t len = 0;     // members per row
  std::int32_t kc = 0;      // fused: member positions evaluated per chunk (kc·n <= tile)
  std::size_t row0 = 0;     // offset into SegPlan::rows
  std::size_t memT = 0;     // offset into SegPlan::memT / coefT (len·n entries)
  std::size_t prod = 0;     // offset into SegPlan::prod (len entries)
  bool fused = false;       // some member position is a fused producer (evaluated in the block)
};

struct PreLoad {
  Operand src;
  double* dst = nullptr;
  LoadKernel fn[2][n_lane_variants] = {};   // [row mode][lane variant]
};

// One kernel call of a group: a single IR step, or a fused pair of two consecutive IR steps
// t = op(a, b); v = op2(t, c) (prev_right: v = op2(c, t)) — a chain whose middle value t is
// never stored (kernels_impl.hpp: fused pairs). ir_first..ir_last are the IR steps covered;
// `out` is the scratch of the last one, so later steps resolve it as before.
struct StepPlan {
  Op op = Op::Const;
  Operand a, b, c, konst;
  int n_pre = 0;
  PreLoad pre[3];                        // operand materialisation for Fma / Select
  OpKernel fn[2][n_lane_variants] = {};  // [row mode][lane variant]
  AccKernel acc_fn[2][n_lane_variants] = {};  // [affine][lane variant], contiguous rows; nullptr: no epilogue for this step
  const SegPlan* seg = nullptr;          // Sum / Affine (per-tile fallback)
  const std::int32_t* ordinal = nullptr; // Input: input ordinal by row
  double* out = nullptr;                 // scratch for a non-final step; nullptr = the caller's destination
  Op op2 = Op::Const;                    // fused pair: the second step's op (Const: not a pair)
  bool prev_right = false;               // fused pair: the first step's value is op2's right operand
  // Chain tail (plain pair kernels only, kernels_impl.hpp: apply_tails): libm steps (Exp / Log)
  // of the pair's value that nothing else reads, applied in place on each row (row block at
  // L = 1) right after it is stored, so the argument never round-trips through the scratch.
  // acc_fn is null for a step with a tail.
  static constexpr int max_tail = 2;
  int n_tail = 0;
  Op tail_op[max_tail] = {};
  bool tail_poly = false;                // Exp tails use exp_poly (ExpMode::poly, E1) rather than std::exp
  int ir_first = 0, ir_last = 0;         // IR steps this kernel covers
  std::string name;                      // for describe()
};

// A fused producer as one reduction sees it: its steps with every row-addressed operand (column,
// gather index, input ordinal) re-laid out in this reduction's member order, so that a run of
// member positions is evaluated in the contiguous row mode from offset `off` (SegPlan::prod_off)
// with one contiguous load per operand and one gather per value. A producer with a per-row Sum /
// Affine step cannot be re-laid out (block_mode false): it is evaluated in the indirect row mode
// with its own steps into the member buffer.
struct FusedProducer {
  std::int32_t domain = -1;
  bool block_mode = false;
  bool has_outputs = false;                       // some member rows are outputs: emitted from the member buffer
  std::vector<StepPlan> steps;
  std::vector<std::vector<double>> cols;         // permuted columns, one per Col operand
  std::vector<std::vector<std::int32_t>> gats;   // permuted gather indices, one per Gat operand
  std::vector<std::int32_t> ordinal;              // permuted input ordinals (Input steps)
  std::vector<std::int32_t> out_ordinal;          // permuted output ordinal per member (-1: none), block mode
};

struct SegPlan {
  bool affine = false;
  Operand konst;                       // Affine c_0 (Lit or Col)
  std::vector<std::int32_t> rows;      // destination row per bucket entry
  std::vector<std::int32_t> memT;      // transposed member value ids
  std::vector<double> coefT;           // transposed coefficients (Affine only)
  std::vector<std::int32_t> prod;      // per block and member position: index into `producers`, or -1 (read the value buffer)
  std::vector<std::int32_t> prod_off;  // per block and member position: offset of its first member in the producer's permuted tables
  std::vector<FusedProducer> producers;
  std::vector<SegBlock> blocks;
  std::size_t fused_members = 0;       // member entries computed in the block rather than gathered (describe)
  std::size_t patterns = 0;            // distinct (length, producer pattern) buckets (describe)
  LoadAccKernel load_acc_gat[n_lane_variants] = {};  // fused block: members of a materialised domain (contiguous rows)
  LoadAccKernel load_acc_vec[n_lane_variants] = {};  // fused block: a last step without an epilogue, via the member buffer
  // The untransposed tables, for a Sum / Affine step that is not its group's only step.
  const std::int32_t* offsets = nullptr;
  const std::int32_t* members = nullptr;
  const double* coefs = nullptr;
  std::int32_t min_len = 0, max_len = 0;
};

// A domain read only through the gathers of one elementwise domain (the consumer), evaluated for
// each tile of the consumer instead of as a whole: the rows the tile gathers (`index`, the
// consumer's gather, value ids by consumer row) are computed into `temp` (tile-shaped) and the
// consumer's operand reads temp through GroupPlan::tile_index. Never materialised: no write to
// and no read back from a cold region of the value buffer (the 656 KB of interpolated rates on
// the M1 book at 32 lanes, the same arithmetic either way).
struct InlinedProducer {
  std::int32_t domain = -1;                    // the producer
  std::size_t gather = 0;                      // the consumer's ir gather slot
  const std::int32_t* gather_index = nullptr;  // that gather's value ids by consumer row (operand matching)
  std::vector<std::int32_t> index;             // the ids the producer is evaluated for: the gather's, with a
                                               // foreign id (a row of another domain) replaced by the producer's row 0
  std::vector<std::int32_t> foreign_row;       // consumer rows whose id is foreign (ascending) ...
  std::vector<std::int32_t> foreign_id;        // ... and those ids: copied from the value buffer into the temporary
  std::vector<std::size_t> foreign_start;      // per consumer tile: first entry of foreign_row in it (n_tiles + 1)
  double* temp = nullptr;                      // tile · Lt doubles
  // A whole-domain Sum / Affine producer (every row the same `len` members): its member ids and
  // coefficients transposed in consumer row order, memT[k · rows + r] = member k of the row
  // consumer row r gathers (a foreign row: the producer's row 0, overwritten by the copy), and
  // c_0 per consumer row (Affine with a column c_0) or the literal.
  SegListKernel seg_fn[n_lane_variants] = {};
  std::int32_t len = 0;
  std::int32_t rows = 0;                       // the consumer's rows (the stride of memT / coefT)
  std::vector<std::int32_t> memT;
  std::vector<double> coefT;
  Operand konst;                               // Affine: Lit, or Col in consumer row order
};

struct GroupPlan {
  std::int32_t domain = -1;
  std::int32_t rows = 0;
  std::int32_t value_base = 0;
  std::vector<StepPlan> steps;
  // A group that is one Sum / Affine step runs as a whole-domain pass.
  bool whole_segment = false;
  SegPlan seg;
  SegKernel seg_fn[n_lane_variants] = {};
  std::vector<std::int32_t> ordinal;     // Input domain: ordinal by row
  // An elementwise group read (almost) only as members of whole-domain Sum / Affine groups: not
  // materialised as a whole; each consumer evaluates it for the rows it needs inside its
  // blocks. The rows some gather, output or per-row Sum / Affine reads (`keep`, ascending) are
  // evaluated in the indirect row mode and written to their slots of the value buffer.
  bool fused = false;
  std::vector<std::int32_t> keep;        // rows materialised anyway
  std::vector<std::int32_t> consumers;   // the reduction domains that evaluate it (describe)
  // Output rows of a fused group that are not kept are written to `out` by the reduction blocks
  // that compute them, from the member buffer (emit_ordinal: the row's output ordinal, or -1);
  // every other output is copied from the value buffer after the chunk (Impl::late_ids / late_ords).
  std::vector<std::int32_t> emit_ordinal;
  std::size_t emitted = 0;               // rows emitted this way (describe)
  // Producers evaluated per tile of this group (InlinedProducer), and the tile row of each of
  // this group's rows (r mod tile) their operands gather through. A group that inlines producers
  // is only ever evaluated in the contiguous row mode.
  std::vector<InlinedProducer> inlined;
  std::vector<std::int32_t> tile_index;
  std::int32_t tile = 0;                 // rows per tile (foreign_start is indexed by r0 / tile)
  std::int32_t inlined_into = -1;        // this group is an inlined producer of that domain
  // A scan domain (ir::Program::scans, D41): evaluated wave by wave — wave w is the rows
  // wave_rows[wave_begin[w] .. wave_begin[w+1]) (ascending), the rows whose carry chain has
  // depth w — each wave in tiles of the indirect row mode into the member buffer, then copied
  // to the rows' slots of the value buffer, so that every row reads rows of earlier waves only.
  // Sequential along a chain, parallel across chains and lanes. Never fused, never inlined.
  bool scan = false;
  std::vector<std::int32_t> wave_rows;
  std::vector<std::int32_t> wave_begin;  // waves + 1
};

// Evaluates an elementwise group for n rows (r0, idx: see the row modes above) of lane chunk ctx;
// the last step writes `dest` (tile-shaped, [i·L + l]), earlier steps their own scratch. Inlined
// producers (contiguous rows only) are evaluated first, for the rows this tile gathers.
inline void eval_group(const GroupPlan& g, const RunCtx& ctx, int r0, const std::int32_t* idx, int n, double* dest) {
  const int mode = idx != nullptr ? row_indirect : row_contiguous;
  for (const InlinedProducer& ip : g.inlined) {
    const GroupPlan& pg = ctx.groups[ip.domain];
    const std::int32_t* ids = ip.index.data() + r0;
    if (pg.whole_segment) {
      ip.seg_fn[ctx.v](ip, ctx, r0, n, ip.temp);
    } else {
      eval_group(pg, ctx, -pg.value_base, ids, n, ip.temp);
    }
    if (!ip.foreign_row.empty()) {
      const std::size_t t = static_cast<std::size_t>(r0 / g.tile);
      const std::size_t Ls = static_cast<std::size_t>(ctx.L);
      for (std::size_t f = ip.foreign_start[t]; f < ip.foreign_start[t + 1]; ++f) {
        const double* src = ctx.values + static_cast<std::size_t>(ip.foreign_id[f]) * Ls;
        double* dst = ip.temp + static_cast<std::size_t>(ip.foreign_row[f] - r0) * Ls;
        for (std::size_t l = 0; l < Ls; ++l) dst[l] = src[l];
      }
    }
  }
  for (const StepPlan& s : g.steps) {
    for (int j = 0; j < s.n_pre; ++j) s.pre[j].fn[mode][ctx.v](s.pre[j].src, ctx, r0, idx, n, s.pre[j].dst);
    double* o = s.out != nullptr ? s.out : dest;
    s.fn[mode][ctx.v](s, ctx, r0, idx, n, o);
  }
}

// The kernel table for one lane width (kernels_impl.hpp). `ind` selects the indirect row mode.
template <int L>
struct KernelTable {
  static OpKernel binary(Op op, Kind ka, Kind kb, bool ind);  // Add Sub Mul Div CmpLt..CmpEq
  static OpKernel unary(Op op, Kind ka, bool ind);            // Neg Exp Log Sqrt Recip
  static OpKernel exp_poly(Kind ka, bool ind);                // Exp via exp_poly (E1)
  static OpKernel ternary(Op op);                             // Fma Select over three Vec operands
  static OpKernel konst(Kind kk, bool ind);                   // Const: broadcast konst (Lit / Col)
  static OpKernel input(bool ind);                            // Input: copy from the state
  static OpKernel seg_rows(bool affine, bool ind);            // Sum / Affine per tile (fallback)
  static LoadKernel load(Kind k, bool ind);                   // materialise an operand
  static SegKernel seg_whole(bool affine);                    // Sum / Affine whole-domain, bucketed
  static SegListKernel seg_list(bool affine);                 // Sum / Affine for a list of rows (inlined producer)
  static AccKernel binary_acc(Op op, Kind ka, Kind kb, bool affine);  // reduction epilogues (indirect rows)
  static AccKernel unary_acc(Op op, Kind ka, bool affine);
  static AccKernel konst_acc(Kind kk, bool affine);
  static LoadAccKernel load_acc(Kind k, bool affine, bool ind);
  // Fused pairs (kernels_impl.hpp): op ∈ {Add, Sub, Mul, Div} over (ka, kb) ∈ {(S,G), (G,S), (G,G)}
  // or Neg over G; op2 ∈ {Add, Sub, Mul, Div} with kc ∈ {S, G} (prev_right only for Sub / Div)
  // or Neg. nullptr when the shape is not one of these.
  static OpKernel pair(Op op, PK ka, PK kb, Op op2, PK kc, bool prev_right, bool ind);
  static AccKernel pair_acc(Op op, PK ka, PK kb, Op op2, PK kc, bool prev_right, bool affine);
  // out[ordinals[o]·B + b0 + l] = values[ids[o]·L + l] for l < L (the output copy of a lane chunk).
  static void copy_out(const double* values, const std::int32_t* ids, const std::int32_t* ordinals, int n_out, double* out, int B, int b0, int lanes);
  // out[ordinals[t]·B + b0 + l] = member[t·L + l] for the t < n with ordinals[t] >= 0 (outputs emitted from a reduction block).
  static void emit_out(const double* member, const std::int32_t* ordinals, int n, double* out, int B, int b0, int lanes);
};

extern template struct KernelTable<0>;
extern template struct KernelTable<1>;
extern template struct KernelTable<4>;
extern template struct KernelTable<8>;
extern template struct KernelTable<16>;
extern template struct KernelTable<32>;
extern template struct KernelTable<64>;

}  // namespace epykos::exec::detail
