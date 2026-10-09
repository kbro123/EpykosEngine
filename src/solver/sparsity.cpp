// The structural sparsity of a declared block and the structural gate. See
// include/epykos/solver/sparsity.hpp.
#include "epykos/solver/sparsity.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/mutation/mutation.hpp"

namespace epykos::solver {

namespace {
std::size_t idx(int i) noexcept { return static_cast<std::size_t>(i); }
}  // namespace

std::size_t SparsityPattern::nonzeros() const noexcept {
  std::size_t n = 0;
  for (std::uint64_t w : bits) n += static_cast<std::size_t>(std::popcount(w));
  return n;
}

double SparsityPattern::density() const noexcept {
  const std::size_t cells = idx(n_residuals) * idx(n_unknowns);
  if (cells == 0) return 0.0;
  return static_cast<double>(nonzeros()) / static_cast<double>(cells);
}

int SparsityPattern::row_count(int i) const noexcept {
  int n = 0;
  const std::size_t base = idx(i) * idx(words());
  for (int w = 0; w < words(); ++w) n += std::popcount(bits[base + idx(w)]);
  return n;
}

int SparsityPattern::column_count(int j) const noexcept {
  int n = 0;
  for (int i = 0; i < n_residuals; ++i) {
    if (reads(i, j)) ++n;
  }
  return n;
}

std::vector<int> SparsityPattern::dead_unknowns() const {
  std::vector<int> dead;
  for (int j = 0; j < n_unknowns; ++j) {
    if (column_count(j) == 0) dead.push_back(j);
  }
  return dead;
}

std::vector<int> SparsityPattern::dead_residuals() const {
  std::vector<int> dead;
  for (int i = 0; i < n_residuals; ++i) {
    if (row_count(i) == 0) dead.push_back(i);
  }
  return dead;
}

std::string SparsityPattern::to_string() const {
  std::ostringstream os;
  os << n_residuals << " residuals x " << n_unknowns << " unknowns, " << nonzeros() << " structural nonzeros ("
     << (density() * 100.0) << "% dense)";
  const std::vector<int> du = dead_unknowns();
  const std::vector<int> dr = dead_residuals();
  if (!du.empty()) {
    os << ", " << du.size() << " dead unknown" << (du.size() == 1 ? "" : "s");
  }
  if (!dr.empty()) {
    os << ", " << dr.size() << " dead residual" << (dr.size() == 1 ? "" : "s");
  }
  return os.str();
}

SparsityPattern structural_pattern(const Tape& tape, const ImplicitBlock& block) {
  SparsityPattern pat;
  pat.n_residuals = block.n_residuals();
  pat.n_unknowns = block.n_unknowns();
  if (pat.n_residuals < 1 || pat.n_unknowns < 1) {
    throw std::invalid_argument("structural_pattern '" + block.name + "': a block has at least one unknown and one residual");
  }
  // One bitset per residual, carried backwards along the operand edges. Residual i's bit arrives
  // at every node its recorded expression reads, transitively; the Input nodes it arrives at are
  // row i of the pattern. Operands are always EARLIER nodes (the tape is in evaluation order), so
  // one reverse pass is the full transitive closure.
  const int r_words = (pat.n_residuals + 63) / 64;
  const std::size_t n_nodes = tape.size();
  std::vector<std::uint64_t> mark(n_nodes * idx(r_words), 0);
  // The sweep starts at the LAST residual node rather than at the end of the tape: nothing after a
  // residual output can be an ancestor of it, so those nodes can never carry a mark. It matters for
  // a multi-block problem recorded on one tape -- block 0's residuals sit near the front of a tape
  // that later holds three more calibrations and a 2,000-trade book, and without this bound every
  // block would sweep the whole of it.
  //
  // The walk has to run over the TAPE and not over the slice `ResidualProgram` builds immediately
  // afterwards, even though the slice is far smaller: the slice copies only the Inputs the
  // residuals reach, so a dead unknown is absent from it and the dead-column question cannot be
  // asked there at all. That absence is exactly what the old backstop detects, and it is why this
  // analysis cannot be folded into the cheaper object.
  std::size_t last = 0;
  for (int i = 0; i < pat.n_residuals; ++i) {
    const int o = block.residuals[idx(i)];
    if (o < 0 || static_cast<std::size_t>(o) >= tape.num_outputs()) {
      throw std::invalid_argument("structural_pattern '" + block.name + "': residual output ordinal " + std::to_string(o) +
                                  " is not on this tape");
    }
    const node_id id = tape.outputs()[idx(o)];
    mark[idx(static_cast<int>(id)) * idx(r_words) + idx(i / 64)] |= (std::uint64_t{1} << (i % 64));
    last = std::max(last, static_cast<std::size_t>(id));
  }
  const std::vector<Node>& nodes = tape.nodes();
  auto propagate = [&](const std::uint64_t* from, node_id to) {
    std::uint64_t* dst = mark.data() + static_cast<std::size_t>(to) * idx(r_words);
    for (int w = 0; w < r_words; ++w) dst[idx(w)] |= from[idx(w)];
  };
  // Mutant sparsity.closure_forward_order: visit the nodes ASCENDING. Operands are earlier nodes,
  // so a mark propagated downwards lands on a node the sweep has already passed and goes no
  // further: the closure stops one edge deep. That is a FALSE POSITIVE defect -- almost every
  // unknown then looks dead and the gate fires on correct maths -- which is the opposite failure
  // to sparsity.gate_never_fires and has different catchers. Every gate that records an implicit
  // block sees it, compare_ois included. It is deliberately not a variadic-skipping mutant: the
  // Rec operators never emit Sum or Affine (only the passes do, and the gate runs before them),
  // so a variadic mutant would be unreachable here and could not be caught.
  const bool forward = mutant("sparsity.closure_forward_order");
  const std::size_t span = last + 1;
  for (std::size_t k = 0; k < span; ++k) {
    const std::size_t n = forward ? k : last - k;
    const std::uint64_t* row = mark.data() + n * idx(r_words);
    bool any = false;
    for (int w = 0; w < r_words && !any; ++w) any = row[idx(w)] != 0;
    if (!any) continue;
    const Node& nd = nodes[n];
    if (op_is_variadic(nd.op)) {
      for (node_id a : tape.args(nd)) propagate(row, a);
    } else {
      const int arity = op_arity(nd.op);
      if (arity >= 1) propagate(row, nd.a);
      if (arity >= 2) propagate(row, nd.b);
      if (arity >= 3) propagate(row, nd.c);
    }
  }
  // Read the rows off the unknowns' Input nodes.
  pat.bits.assign(idx(pat.n_residuals) * idx(pat.words()), 0);
  for (int j = 0; j < pat.n_unknowns; ++j) {
    const int ord = block.unknowns[idx(j)];
    if (ord < 0 || static_cast<std::size_t>(ord) >= tape.num_inputs()) {
      throw std::invalid_argument("structural_pattern '" + block.name + "': unknown input ordinal " + std::to_string(ord) +
                                  " is not on this tape");
    }
    const node_id id = tape.inputs()[idx(ord)];
    const std::uint64_t* row = mark.data() + static_cast<std::size_t>(id) * idx(r_words);
    for (int i = 0; i < pat.n_residuals; ++i) {
      if (((row[idx(i / 64)] >> (i % 64)) & 1u) != 0) {
        pat.bits[idx(i) * idx(pat.words()) + idx(j / 64)] |= (std::uint64_t{1} << (j % 64));
      }
    }
  }
  return pat;
}

void check_structure(const SparsityPattern& pattern, const ImplicitBlock& block) {
  // Mutant sparsity.gate_never_fires: compute the pattern and then do not read it. A gate that
  // cannot see its own violation is the failure four stages of this programme shipped.
  if (!block.options.require_structural_coupling || mutant("sparsity.gate_never_fires")) return;
  const std::vector<int> du = pattern.dead_unknowns();
  const std::vector<int> dr = pattern.dead_residuals();
  if (du.empty() && dr.empty()) return;
  std::ostringstream os;
  os << "implicit '" << block.name << "': the declared solve is structurally singular (" << pattern.to_string() << ").";
  if (!du.empty()) {
    os << " Unknown" << (du.size() == 1 ? "" : "s") << " no residual reads, so nothing constrains "
       << (du.size() == 1 ? "it" : "them") << " (exact zero Jacobian column" << (du.size() == 1 ? "" : "s") << "):";
    for (std::size_t k = 0; k < du.size(); ++k) {
      os << (k ? "," : "") << " z[" << du[k] << "] (tape input " << block.unknowns[idx(du[k])] << ")";
    }
    os << ".";
  }
  if (!dr.empty()) {
    os << " Residual" << (dr.size() == 1 ? "" : "s") << " that read no unknown, so no step can move "
       << (dr.size() == 1 ? "it" : "them") << " (exact zero Jacobian row" << (dr.size() == 1 ? "" : "s") << "):";
    for (std::size_t k = 0; k < dr.size(); ++k) {
      os << (k ? "," : "") << " F[" << dr[k] << "] (tape output " << block.residuals[idx(dr[k])] << ")";
    }
    os << ".";
  }
  os << " This is a property of the recorded maths, not of the market: it is the same on every"
        " quote. Set SolveOptions::require_structural_coupling = false for a deliberately"
        " under-determined solve with a regulariser.";
  throw std::invalid_argument(os.str());
}

double Colouring::lane_reduction(int n_residuals) const noexcept {
  if (n_colours < 1) return 0.0;
  return static_cast<double>(n_residuals) / static_cast<double>(n_colours);
}

Colouring colour_rows(const SparsityPattern& pattern) {
  const int n = pattern.n_residuals;
  Colouring c;
  c.colour.assign(idx(n), -1);
  if (n < 1) return c;
  // Largest-first: the densest rows conflict with the most others, so colouring them first is the
  // standard greedy order and gives the tighter upper bound.
  std::vector<int> order(idx(n));
  std::iota(order.begin(), order.end(), 0);
  std::vector<int> deg(idx(n));
  for (int i = 0; i < n; ++i) deg[idx(i)] = pattern.row_count(i);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return deg[idx(a)] > deg[idx(b)]; });
  const int w = pattern.words();
  auto conflicts = [&](int a, int b) {
    const std::uint64_t* ra = pattern.bits.data() + idx(a) * idx(w);
    const std::uint64_t* rb = pattern.bits.data() + idx(b) * idx(w);
    for (int k = 0; k < w; ++k) {
      if ((ra[idx(k)] & rb[idx(k)]) != 0) return true;
    }
    return false;
  };
  std::vector<char> taken;
  for (int i : order) {
    taken.assign(idx(c.n_colours) + 1, 0);
    for (int k = 0; k < n; ++k) {
      if (c.colour[idx(k)] < 0 || k == i) continue;
      if (conflicts(i, k)) taken[idx(c.colour[idx(k)])] = 1;
    }
    int pick = 0;
    while (pick < static_cast<int>(taken.size()) && taken[idx(pick)]) ++pick;
    c.colour[idx(i)] = pick;
    if (pick + 1 > c.n_colours) c.n_colours = pick + 1;
  }
  return c;
}

std::string shape_of(const SparsityPattern& pattern) {
  const int nr = pattern.n_residuals;
  const int nz = pattern.n_unknowns;
  if (nr < 1 || nz < 1) return "empty";
  std::ostringstream os;
  if (pattern.nonzeros() == idx(nr) * idx(nz)) return "DENSE (every residual reads every unknown)";
  // Triangularity and bandwidth over the declared ordering, which for a curve block is knot order
  // against instrument order: the only ordering the block states, so the only one to describe.
  bool lower = true, upper = true;
  int band_lo = 0, band_hi = 0;
  for (int i = 0; i < nr; ++i) {
    for (int j = 0; j < nz; ++j) {
      if (!pattern.reads(i, j)) continue;
      if (j > i) lower = false;
      if (j < i) upper = false;
      band_lo = std::max(band_lo, i - j);
      band_hi = std::max(band_hi, j - i);
    }
  }
  // Block diagonal: connected components of the bipartite graph, as the row sets they partition.
  std::vector<int> comp(idx(nz), -1);
  int n_comp = 0;
  std::vector<int> stack;
  for (int j0 = 0; j0 < nz; ++j0) {
    if (comp[idx(j0)] >= 0) continue;
    comp[idx(j0)] = n_comp;
    stack.assign(1, j0);
    while (!stack.empty()) {
      const int j = stack.back();
      stack.pop_back();
      for (int i = 0; i < nr; ++i) {
        if (!pattern.reads(i, j)) continue;
        for (int k = 0; k < nz; ++k) {
          if (pattern.reads(i, k) && comp[idx(k)] < 0) {
            comp[idx(k)] = n_comp;
            stack.push_back(k);
          }
        }
      }
    }
    ++n_comp;
  }
  if (lower && nr == nz) {
    os << "LOWER TRIANGULAR";
  } else if (upper && nr == nz) {
    os << "UPPER TRIANGULAR";
  } else if (band_lo + band_hi + 1 < nz) {
    os << "BANDED (bandwidth " << band_lo << " below, " << band_hi << " above)";
  } else {
    os << "unstructured";
  }
  if (n_comp > 1) {
    std::vector<int> sizes(idx(n_comp), 0);
    for (int j = 0; j < nz; ++j) ++sizes[idx(comp[idx(j)])];
    os << ", BLOCK DIAGONAL in " << n_comp << " components of sizes";
    for (int k = 0; k < n_comp; ++k) os << (k ? "," : "") << " " << sizes[idx(k)];
  } else {
    os << ", fully coupled (one component)";
  }
  return os.str();
}

}  // namespace epykos::solver
