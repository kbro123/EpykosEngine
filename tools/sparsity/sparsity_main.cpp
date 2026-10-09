// tools/sparsity/ — the structural sparsity of ∂F/∂z on the real fixtures, and the greedy colour
// count, measured BEFORE anyone builds a colouring (D68's method; D92's restatement of it).
//
// See bench/compare/SPARSITY.md, whose §1 predictions were committed before this file existed.
//
// NO TIMING. Every number is a count, a density or a colour, which is what makes it quotable on a
// loaded box — the position tools/coverage/ and tools/revcollapse/ take for the same reason.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/sharing.hpp"
#include "epykos/solver/residual.hpp"
#include "epykos/solver/sparsity.hpp"

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;

namespace {

// The pattern as a picture, for the shape claims. Capped so a 70-knot block stays readable.
void print_pattern(const solver::SparsityPattern& pat, int max_rows = 30, int max_cols = 76) {
  const int nr = std::min(pat.n_residuals, max_rows);
  const int nz = std::min(pat.n_unknowns, max_cols);
  std::printf("      pattern (%d of %d rows x %d of %d columns, '#' = structurally present):\n", nr,
              pat.n_residuals, nz, pat.n_unknowns);
  for (int i = 0; i < nr; ++i) {
    std::printf("        %3d |", i);
    for (int j = 0; j < nz; ++j) std::printf("%c", pat.reads(i, j) ? '#' : '.');
    std::printf("| %d\n", pat.row_count(i));
  }
  if (nr < pat.n_residuals) std::printf("        ... %d further rows\n", pat.n_residuals - nr);
}

// The numerical nonzero count of ∂F/∂z at the record point, through the engine's own batched
// adjoint: the measurement that says whether the structural superset is TIGHT or merely sound.
// Returns -1 when the block cannot be built (which the structural gate would already have refused).
int numerical_nonzeros(const epykos::Tape& tape, const solver::ImplicitBlock& block) {
  try {
    solver::ResidualProgram rp(tape, block, /*passes=*/true);
    const std::vector<double> all = tape.input_values();
    std::vector<double> p(static_cast<std::size_t>(rp.n_params() > 0 ? rp.n_params() : 1), 0.0);
    for (int m = 0; m < rp.n_params(); ++m) p[static_cast<std::size_t>(m)] = all[static_cast<std::size_t>(rp.param_ordinals()[static_cast<std::size_t>(m)])];
    std::vector<double> z(static_cast<std::size_t>(rp.n_unknowns()));
    for (int j = 0; j < rp.n_unknowns(); ++j) z[static_cast<std::size_t>(j)] = all[static_cast<std::size_t>(rp.unknown_ordinals()[static_cast<std::size_t>(j)])];
    std::vector<double> F(static_cast<std::size_t>(rp.n_residuals()));
    std::vector<double> Jz(static_cast<std::size_t>(rp.n_residuals()) * static_cast<std::size_t>(rp.n_unknowns()));
    std::vector<double> Jp(static_cast<std::size_t>(rp.n_residuals()) * static_cast<std::size_t>(rp.n_params() > 0 ? rp.n_params() : 1));
    rp.jacobian(z.data(), p.data(), F.data(), Jz.data(), rp.n_params() > 0 ? Jp.data() : nullptr);
    int n = 0;
    for (double v : Jz) n += (v != 0.0) ? 1 : 0;
    return n;
  } catch (const std::exception& e) {
    std::printf("      (numerical Jacobian unavailable: %s)\n", e.what());
    return -1;
  }
}

// The SECOND route the brief offered — `ir::sharing::reach(p, groups)` with the residuals as
// groups — measured rather than reasoned about. `ir/program.hpp`: "inputs: value id of every Input
// ordinal (they are rows of the Input domain)", so every unknown is a ROW of ONE domain and a
// per-domain mask cannot separate unknown j from unknown k. This prints what that route would
// actually return, so the rejection is a measurement.
void report_reach_route(const epykos::Tape& tape, const solver::ImplicitBlock& block) {
  try {
    solver::ResidualProgram rp(tape, block, /*passes=*/true);
    const epykos::ir::Program& p = rp.program();
    std::vector<std::vector<int>> groups;
    for (int i = 0; i < rp.n_residuals(); ++i) groups.push_back({i});
    const std::vector<std::uint32_t> mask = epykos::ir::reach(p, groups);
    // How many distinct domains do the unknowns occupy?
    std::vector<epykos::ir::domain_id> doms;
    for (std::size_t k = 0; k < p.inputs.size(); ++k) doms.push_back(p.domain_of(p.inputs[k]));
    std::sort(doms.begin(), doms.end());
    doms.erase(std::unique(doms.begin(), doms.end()), doms.end());
    std::printf("      ir::reach route: %zu slice inputs occupy %zu domain(s)", p.inputs.size(), doms.size());
    if (doms.size() == 1 && !p.inputs.empty()) {
      const std::uint32_t m = mask[static_cast<std::size_t>(doms[0])];
      int bits = 0;
      for (int i = 0; i < 32; ++i) bits += (m >> i) & 1u;
      std::printf("; that one domain's mask has %d of %d residual bits set, so the route reports a"
                  " DENSE pattern and cannot separate one unknown from another: UNSOUND here\n",
                  bits, rp.n_residuals());
    } else {
      std::printf("; per-unknown separation may be possible -- check before using this route\n");
    }
  } catch (const std::exception& e) {
    std::printf("      ir::reach route: unavailable (%s)\n", e.what());
  }
}

void report_block(const epykos::Tape& tape, const solver::ImplicitBlock& block, bool picture) {
  const solver::SparsityPattern pat = solver::structural_pattern(tape, block);
  const solver::Colouring col = solver::colour_rows(pat);
  std::printf("    block '%s'\n", block.name.c_str());
  std::printf("      n_r %d, n_z %d, structural nonzeros %zu, density %.2f%%\n", pat.n_residuals,
              pat.n_unknowns, pat.nonzeros(), pat.density() * 100.0);
  std::printf("      shape: %s\n", solver::shape_of(pat).c_str());
  std::printf("      greedy colours %d of %d residuals -> lane reduction %.3fx\n", col.n_colours,
              pat.n_residuals, col.lane_reduction(pat.n_residuals));
  const std::vector<int> du = pat.dead_unknowns();
  const std::vector<int> dr = pat.dead_residuals();
  std::printf("      dead unknowns %zu, dead residuals %zu%s\n", du.size(), dr.size(),
              (du.empty() && dr.empty()) ? "  (the structural gate passes)" : "  (THE STRUCTURAL GATE WOULD FIRE)");
  const int num = numerical_nonzeros(tape, block);
  if (num >= 0) {
    const std::size_t str = pat.nonzeros();
    std::printf("      numerical nonzeros at the record point %d vs structural %zu: %s\n", num, str,
                (static_cast<std::size_t>(num) == str)
                    ? "TIGHT"
                    : "structural is a strict superset (sound, loose)");
  }
  report_reach_route(tape, block);
  if (picture) print_pattern(pat);
}

void run_compare_ois(const std::vector<std::string>& tenors, const char* label) {
  std::printf("\n== compare_ois %s (one USD SOFR curve, (logdf, linear), knots AT the maturities) ==\n", label);
  fixtures::CompareOisOptions o;
  o.trades = 8;  // the book is irrelevant to dF/dz; keep the recording cheap
  o.tenors = tenors;
  const fixtures::CompareOis s = fixtures::make_compare_ois(o);
  fixtures::CompareOisTape t = fixtures::record_compare_ois(s);
  std::printf("  %d quotes, %zu blocks\n", s.n_quotes(), t.registry.blocks.size());
  for (const solver::ImplicitBlock& b : t.registry.blocks) report_block(t.tape, b, /*picture=*/true);
}

// The 25-tenor list `bench/compare/README.md` uses to reproduce D90's shape, which is the "25
// knots" every head-to-head number in the ledger is quoted at. The fixture's own default is 16.
const std::vector<std::string>& h2h_tenors() {
  static const std::vector<std::string> t = {"1Y",  "2Y",  "3Y",  "4Y",  "5Y",  "6Y",  "7Y",  "8Y",  "9Y",
                                             "10Y", "11Y", "12Y", "13Y", "14Y", "15Y", "16Y", "17Y", "18Y",
                                             "19Y", "20Y", "25Y", "30Y", "35Y", "40Y", "50Y"};
  return t;
}

void run_stage_a(solver::CurveSet::Mode mode, const char* problem) {
  std::printf("\n== %s, Mode::%s (4 curves, 2 currencies) ==\n", problem, solver::to_string(mode));
  fixtures::StageAOptions o;
  o.path = epykos::instrument::Blueprints::default_root() + "/problems/" + problem + ".json";
  o.trades = 8;  // likewise: the book does not enter dF/dz
  o.scenarios = 0;
  o.mode = mode;
  fixtures::StageA s = fixtures::make_stage_a(o);
  fixtures::StageATape t = fixtures::record_stage_a(s);
  std::printf("  %d quotes over %d curves, %zu blocks\n", s.n_quotes(), s.n_curves(), t.registry.blocks.size());
  int total_r = 0, total_colours = 0;
  for (std::size_t i = 0; i < t.registry.blocks.size(); ++i) {
    const solver::ImplicitBlock& b = t.registry.blocks[i];
    report_block(t.tape, b, /*picture=*/true);
    const solver::SparsityPattern pat = solver::structural_pattern(t.tape, b);
    total_r += pat.n_residuals;
    total_colours += solver::colour_rows(pat).n_colours;
  }
  std::printf("  ALL BLOCKS: %d residuals, %d colours -> %.3fx lanes over the whole calibration\n",
              total_r, total_colours,
              total_colours > 0 ? static_cast<double>(total_r) / static_cast<double>(total_colours) : 0.0);
}

}  // namespace

int main(int argc, char** argv) {
  std::string which = argc > 1 ? argv[1] : "all";
  std::printf("tools/sparsity — the structural pattern of dF/dz and the greedy colour count.\n");
  std::printf("Counts only; no timing, so no load gate (bench/compare/SPARSITY.md).\n");
  try {
    if (which == "all" || which == "compare_ois") {
      run_compare_ois({}, "at its default 16 tenors");
      run_compare_ois(h2h_tenors(), "at the 25 tenors the head-to-head quotes");
    }
    if (which == "all" || which == "stage_a") run_stage_a(solver::CurveSet::Mode::sequential, "stage_a_h2h");
    if (which == "all" || which == "stage_a_joint") run_stage_a(solver::CurveSet::Mode::joint, "stage_a_h2h");
    // The default problem: the same four curves on the linear-ZERO parameterisation rather than
    // -LOGDF, so it says whether the pattern is a property of the instruments or of the variable.
    if (which == "all" || which == "stage_a_default") run_stage_a(solver::CurveSet::Mode::sequential, "stage_a");
  } catch (const std::exception& e) {
    std::printf("\nFAILED: %s\n", e.what());
    return 1;
  }
  return 0;
}
