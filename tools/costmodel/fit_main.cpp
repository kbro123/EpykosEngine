// EpykosEngine — tools/costmodel/fit_main.cpp (M4/CM, docs/PROBLEM.md §7).
//
// Reads one or more costmodel_collect raw profile captures (named by an index JSON), rebuilds
// the same programs (an ordinary, non-profiled link: both workloads are seeded and deterministic,
// D16 / D44), fits optimise::CostCoefficients by linear least squares (Eigen, D12) and writes
// bench/results/<fingerprint>/cost_model.json plus a validation report.
//
// The model is LINEAR in every coefficient (include/epykos/optimise/cost.hpp: a sum of
// rows x lanes x op_ns, bytes x byte_ns, gathers x gather_ns, tiles x dispatch_ns, members x
// reduction_epilogue_ns), so this tool never re-derives cost.cpp's formula: for each unknown
// coefficient j it calls optimise::estimate_program with EVERY coefficient zero except
// coefficient j set to 1, and reads `per_domain_ns` off as coefficient j's contribution to every
// domain's predicted time in that run — feature extraction by unit impulse, calling the exact
// function the engine will use to predict, so a fit can never silently drift from cost.cpp.
//
// Usage: costmodel_fit --index bench/results/<fp>/costmodel_raw/index.json
//                       [--out-dir bench/results] [--report bench/results/<fp>/cost_model_validation.md]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/optimise/cost.hpp"
#include "epykos/tape/tape.hpp"
#include "epykos/util/json.hpp"

namespace {

namespace fs = std::filesystem;
namespace ir = epykos::ir;
namespace fixtures = epykos::fixtures;
namespace optimise = epykos::optimise;
namespace json = epykos::json;

struct RunSpec {
  std::string case_name;
  int B = 1, tile = 256, lane_tile = 8;
  fs::path path;
};

std::vector<RunSpec> read_index(const fs::path& index_path) {
  const json::Value root = json::parse_file(index_path.string());
  std::vector<RunSpec> specs;
  for (const json::Value& r : root.at("runs").as_array()) {
    RunSpec s;
    s.case_name = r.at("case").as_string();
    s.B = static_cast<int>(r.at("B").as_int());
    s.tile = static_cast<int>(r.at("tile").as_int());
    s.lane_tile = static_cast<int>(r.at("lane_tile").as_int());
    s.path = index_path.parent_path() / r.at("path").as_string();
    specs.push_back(std::move(s));
  }
  return specs;
}

struct MeasuredDomain {
  int domain;
  double us;
};

// Parses a costmodel_collect capture: "exec profile over N runs (us per run):" and
// "  slot D:   US us ( P%)" lines (src/exec/interpreter.cpp's ProfileTable — the same format
// scripts/exec_coverage.py reads). Slot 4095 is the output copy, not a domain, and is skipped.
struct ParsedProfile {
  long runs = 0;
  std::vector<MeasuredDomain> slots;
};

ParsedProfile parse_profile(const fs::path& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open " + path.string());
  static const std::regex runs_re(R"(exec profile over (\d+) runs)");
  static const std::regex slot_re(R"(^\s*slot\s+(\d+):\s+([0-9.]+) us)");
  ParsedProfile p;
  std::string line;
  while (std::getline(f, line)) {
    std::smatch m;
    if (std::regex_search(line, m, runs_re)) p.runs = std::stol(m[1]);
    if (std::regex_search(line, m, slot_re)) {
      const int slot = std::stoi(m[1]);
      if (slot == 4095) continue;
      p.slots.push_back({slot, std::stod(m[2])});
    }
  }
  if (p.runs == 0) {
    throw std::runtime_error(path.string() + " has no 'exec profile over N runs' line (was costmodel_collect built with the `profile` preset?)");
  }
  return p;
}

// One Program per case, built once and reused across every run of that case in the index (tile /
// lane_tile are Interpreter::Options, not part of the recorded Program: ir::infer's output is the
// same regardless — solver::ImplicitProgram's own construction confirms this, src/solver/implicit_program.cpp).
const ir::Program& program_for(std::map<std::string, ir::Program>& cache, const std::string& case_name) {
  auto it = cache.find(case_name);
  if (it != cache.end()) return it->second;
  ir::Program program;
  if (case_name == "m1") {
    const fixtures::Book book = fixtures::make_m1_book();
    const epykos::Tape tape = fixtures::record_m1(book);
    program = ir::infer(tape);
  } else if (case_name == "stage_a") {
    const fixtures::StageA s = fixtures::make_stage_a();
    const fixtures::StageATape t = fixtures::record_stage_a(s);
    program = ir::infer(t.tape);
  } else {
    throw std::runtime_error("unknown case '" + case_name + "'");
  }
  return cache.emplace(case_name, std::move(program)).first->second;
}

// Flat coefficient indexing: [0, kOpNsCount) = op_ns[v][op] at v*op_count+op; then byte_ns[0..3];
// then gather_ns, dispatch_ns, reduction_epilogue_ns. kNumCoeffs total unknowns.
constexpr int kOpNsCount = optimise::n_lane_variants * epykos::op_count;
constexpr int kByteNsBase = kOpNsCount;
constexpr int kGatherNs = kByteNsBase + 4;
constexpr int kDispatchNs = kGatherNs + 1;
constexpr int kReductionNs = kDispatchNs + 1;
constexpr int kNumCoeffs = kReductionNs + 1;

optimise::CostCoefficients coeffs_from_flat(const std::vector<double>& x) {
  optimise::CostCoefficients c{};
  for (int v = 0; v < optimise::n_lane_variants; ++v) {
    for (int o = 0; o < epykos::op_count; ++o) {
      c.op_ns[static_cast<std::size_t>(v)][static_cast<std::size_t>(o)] = x[static_cast<std::size_t>(v * epykos::op_count + o)];
    }
  }
  for (int t = 0; t < 4; ++t) c.byte_ns[t] = x[static_cast<std::size_t>(kByteNsBase + t)];
  c.gather_ns = x[kGatherNs];
  c.dispatch_ns = x[kDispatchNs];
  c.reduction_epilogue_ns = x[kReductionNs];
  return c;
}

std::vector<double> flat_from_coeffs(const optimise::CostCoefficients& c) {
  std::vector<double> x(static_cast<std::size_t>(kNumCoeffs), 0.0);
  for (int v = 0; v < optimise::n_lane_variants; ++v) {
    for (int o = 0; o < epykos::op_count; ++o) {
      x[static_cast<std::size_t>(v * epykos::op_count + o)] = c.op_ns[static_cast<std::size_t>(v)][static_cast<std::size_t>(o)];
    }
  }
  for (int t = 0; t < 4; ++t) x[static_cast<std::size_t>(kByteNsBase + t)] = c.byte_ns[t];
  x[static_cast<std::size_t>(kGatherNs)] = c.gather_ns;
  x[static_cast<std::size_t>(kDispatchNs)] = c.dispatch_ns;
  x[static_cast<std::size_t>(kReductionNs)] = c.reduction_epilogue_ns;
  return x;
}

struct Row {
  std::string case_name;
  int B = 0, tile = 0, lane_tile = 0, domain = 0;
  double measured_ns = 0.0;
  std::vector<double> features;  // length kNumCoeffs
};

}  // namespace

int main(int argc, char** argv) {
  std::string index_path, out_dir = "bench/results", report_path, fingerprint;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
      return argv[++i];
    };
    try {
      if (arg == "--index") index_path = next();
      else if (arg == "--out-dir") out_dir = next();
      else if (arg == "--report") report_path = next();
      else if (arg == "--fingerprint") fingerprint = next();
      else {
        std::cerr << "costmodel_fit: unknown argument '" << arg << "'\n";
        return 2;
      }
    } catch (const std::exception& e) {
      std::cerr << "costmodel_fit: " << e.what() << "\n";
      return 2;
    }
  }
  if (index_path.empty()) {
    std::cerr << "costmodel_fit: --index is required\n"
                 "usage: costmodel_fit --index INDEX.json [--out-dir bench/results] [--report FILE.md] [--fingerprint ID]\n";
    return 2;
  }

  try {
    const std::vector<RunSpec> specs = read_index(index_path);
    if (fingerprint.empty()) {
      const json::Value root = json::parse_file(index_path);
      if (const json::Value* fp = root.find("fingerprint")) fingerprint = fp->as_string();
    }
    if (fingerprint.empty()) {
      std::cerr << "costmodel_fit: no --fingerprint given and the index has none\n";
      return 2;
    }

    std::map<std::string, ir::Program> program_cache;
    std::vector<Row> rows;

    for (const RunSpec& spec : specs) {
      const ir::Program& program = program_for(program_cache, spec.case_name);
      const ParsedProfile prof = parse_profile(spec.path);
      const optimise::Plan plan = optimise::infer_plan(program, spec.tile, spec.lane_tile);
      const std::size_t nd = program.domains.size();

      std::vector<std::vector<double>> feature_by_domain(nd, std::vector<double>(static_cast<std::size_t>(kNumCoeffs), 0.0));
      std::vector<double> unit(static_cast<std::size_t>(kNumCoeffs), 0.0);
      for (int j = 0; j < kNumCoeffs; ++j) {
        std::fill(unit.begin(), unit.end(), 0.0);
        unit[static_cast<std::size_t>(j)] = 1.0;
        optimise::CostModel m;
        m.coeffs = coeffs_from_flat(unit);
        const optimise::ProgramCost pc = optimise::estimate_program(program, plan, spec.B, m);
        for (std::size_t d = 0; d < nd; ++d) feature_by_domain[d][static_cast<std::size_t>(j)] = pc.per_domain_ns[d];
      }
      for (const MeasuredDomain& md : prof.slots) {
        if (md.domain < 0 || static_cast<std::size_t>(md.domain) >= nd) continue;  // the >4094-domain overflow fold: not one domain
        Row row;
        row.case_name = spec.case_name;
        row.B = spec.B;
        row.tile = spec.tile;
        row.lane_tile = spec.lane_tile;
        row.domain = md.domain;
        row.measured_ns = md.us * 1000.0;
        row.features = feature_by_domain[static_cast<std::size_t>(md.domain)];
        rows.push_back(std::move(row));
      }
    }
    if (rows.empty()) {
      std::cerr << "costmodel_fit: no measured domain rows found across " << specs.size() << " run(s)\n";
      return 1;
    }

    // Active columns: those with at least one nonzero feature somewhere. An op neither workload
    // ever uses (Sqrt, Recip, every comparison, Fma, Select, every reserved op) has an all-zero
    // column and stays at CostCoefficients::defaults() — no data supports fitting it, and an
    // all-zero column would make the design matrix column-rank-deficient there anyway.
    std::vector<int> active;
    for (int j = 0; j < kNumCoeffs; ++j) {
      const bool any = std::any_of(rows.begin(), rows.end(), [j](const Row& r) { return r.features[static_cast<std::size_t>(j)] != 0.0; });
      if (any) active.push_back(j);
    }

    const auto n_rows = static_cast<Eigen::Index>(rows.size());
    const auto n_cols = static_cast<Eigen::Index>(active.size());
    Eigen::MatrixXd A(n_rows, n_cols);
    Eigen::VectorXd b(n_rows);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      for (std::size_t k = 0; k < active.size(); ++k) {
        A(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(k)) = rows[i].features[static_cast<std::size_t>(active[k])];
      }
      b(static_cast<Eigen::Index>(i)) = rows[i].measured_ns;
    }

    // Row-weight by 1/measured_ns: the two workloads' measured times span five decades (tens of
    // ns to milliseconds), and the package's own gate (docs/PROBLEM.md §7) is a MEAN RELATIVE
    // error, not a mean absolute one — plain least squares on absolute ns is dominated entirely
    // by the largest Stage A domains and is nearly blind to every M1-book-sized one. Dividing
    // row i by max(1, measured_i) turns "minimise sum (pred - measured)^2" into "minimise sum
    // ((pred - measured)/measured)^2" = minimise summed squared RELATIVE error, i.e. every
    // row's target becomes 1 (pred/measured == 1 is a perfect fit) — still an ordinary least
    // squares system, just reweighted to match what is actually being validated.
    Eigen::MatrixXd Aw(n_rows, n_cols);
    Eigen::VectorXd bw(n_rows);
    for (Eigen::Index i = 0; i < n_rows; ++i) {
      const double w = 1.0 / std::max(1.0, b(i));
      Aw.row(i) = A.row(i) * w;
      bw(i) = 1.0;
    }

    // Column-normalise, then ridge-regularise TOWARD CostCoefficients::defaults() (not toward
    // zero: a coefficient with little data support should stay at its documented, physically
    // reasonable estimate rather than collapse to 0 or explode). Each column j is scaled by its
    // own max absolute entry in the WEIGHTED matrix so every scaled column has range [-1, 1]; a
    // penalty on (x - defaults) in those normalised units is added by augmenting the system with
    // sqrt(lambda)*I rows and target sqrt(lambda)*(defaults/scale) — still one ordinary least
    // squares solve (Eigen::bdcSvd), not a different method. Coefficients are scaled back after.
    Eigen::VectorXd col_scale(n_cols);
    for (Eigen::Index k = 0; k < n_cols; ++k) {
      const double m = Aw.col(k).cwiseAbs().maxCoeff();
      col_scale(k) = m > 0.0 ? m : 1.0;
    }
    Eigen::MatrixXd Aw_scaled = Aw;
    for (Eigen::Index k = 0; k < n_cols; ++k) Aw_scaled.col(k) /= col_scale(k);

    const std::vector<double> defaults_flat = flat_from_coeffs(optimise::CostCoefficients::defaults());
    Eigen::VectorXd x0_scaled(n_cols);
    for (Eigen::Index k = 0; k < n_cols; ++k) x0_scaled(k) = defaults_flat[static_cast<std::size_t>(active[static_cast<std::size_t>(k)])] / col_scale(k);

    constexpr double kRidgeLambda = 0.01;  // in the normalised, relative-error units
    Eigen::MatrixXd augmented(n_rows + n_cols, n_cols);
    augmented.topRows(n_rows) = Aw_scaled;
    augmented.bottomRows(n_cols) = std::sqrt(kRidgeLambda) * Eigen::MatrixXd::Identity(n_cols, n_cols);
    Eigen::VectorXd b_augmented(n_rows + n_cols);
    b_augmented.topRows(n_rows) = bw;
    b_augmented.bottomRows(n_cols) = std::sqrt(kRidgeLambda) * x0_scaled;

    const Eigen::VectorXd solved_scaled = augmented.bdcSvd(Eigen::ComputeThinU | Eigen::ComputeThinV).solve(b_augmented);

    std::vector<double> flat = defaults_flat;
    for (Eigen::Index k = 0; k < n_cols; ++k) {
      const double coeff = solved_scaled(k) / col_scale(k);
      flat[static_cast<std::size_t>(active[static_cast<std::size_t>(k)])] = std::max(0.0, coeff);  // a cost is never negative
    }

    optimise::CostModel fitted;
    fitted.fingerprint = fingerprint;
    fitted.coeffs = coeffs_from_flat(flat);
    fitted.save(out_dir);

    // Validation: predicted (using the SAME fitted, clamped coefficients) vs measured, per row.
    // A domain's config key groups it with its (case, B, tile, lane_tile) siblings so a
    // "significant" share (>= 1% of that config's total measured time) can be told apart from a
    // domain the profiler's own std::chrono::steady_clock::now() bracketing (two calls per
    // domain per run, ProfileScope in src/exec/interpreter.cpp) already dominates: many Stage A
    // domains measure under half a microsecond total, where a few tens of ns of instrumentation
    // overhead is itself a three-figure relative error no cost formula can fit away. Both means
    // are reported so neither the gate number nor "the model is fine really" is one-sided.
    auto config_key = [](const Row& r) {
      std::ostringstream os;
      os << r.case_name << '|' << r.B << '|' << r.tile << '|' << r.lane_tile;
      return os.str();
    };
    std::map<std::string, double> config_total_ns;
    for (const Row& r : rows) config_total_ns[config_key(r)] += r.measured_ns;

    double sum_rel = 0.0, worst_rel = -1.0;
    double sum_rel_significant = 0.0;
    int n_significant = 0;
    std::string worst_desc;
    std::map<std::string, std::pair<double, int>> per_case, per_case_significant;
    for (const Row& r : rows) {
      double pred = 0.0;
      for (int j = 0; j < kNumCoeffs; ++j) pred += flat[static_cast<std::size_t>(j)] * r.features[static_cast<std::size_t>(j)];
      const double denom = std::max(1.0, std::fabs(r.measured_ns));
      const double rel = std::fabs(pred - r.measured_ns) / denom;
      sum_rel += rel;
      std::pair<double, int>& pc = per_case[r.case_name];
      pc.first += rel;
      pc.second += 1;
      const double share = r.measured_ns / std::max(1.0, config_total_ns[config_key(r)]);
      const bool significant = share >= 0.01;
      if (significant) {
        sum_rel_significant += rel;
        ++n_significant;
        std::pair<double, int>& pcs = per_case_significant[r.case_name];
        pcs.first += rel;
        pcs.second += 1;
      }
      if (rel > worst_rel) {
        worst_rel = rel;
        std::ostringstream os;
        os << r.case_name << " B=" << r.B << " tile=" << r.tile << " lane_tile=" << r.lane_tile << " domain " << r.domain << " (measured "
           << (r.measured_ns / 1000.0) << " us, predicted " << (pred / 1000.0) << " us, " << (100.0 * share) << "% of that config's time)";
        worst_desc = os.str();
      }
    }
    const double mean_rel = sum_rel / static_cast<double>(rows.size());
    const double mean_rel_significant = n_significant > 0 ? sum_rel_significant / static_cast<double>(n_significant) : 0.0;

    std::ostringstream report;
    report << "# M4/CM cost model validation\n\n";
    report << "Fingerprint `" << fingerprint << "`; " << rows.size() << " measured (case, config, domain) points from " << specs.size()
           << " capture(s); " << active.size() << " of " << kNumCoeffs
           << " coefficients fitted (the rest kept at CostCoefficients::defaults(), unsupported by either workload).\n\n";
    report << "**Mean absolute relative error, every measured domain: " << (100.0 * mean_rel) << "%** (target < 25%, docs/PROBLEM.md §7 / M4/CM's own gate).\n\n";
    report << "**Mean absolute relative error, domains >= 1% of their config's time (" << n_significant << " of " << rows.size()
           << " points — the ones an optimisation decision actually turns on): " << (100.0 * mean_rel_significant) << "%.**\n\n";
    report << "Worst domain (every measured domain, including negligible ones): " << worst_desc << ", relative error " << (100.0 * worst_rel) << "%.\n\n";
    report << "Per case, every domain:\n\n";
    for (const std::pair<const std::string, std::pair<double, int>>& kv : per_case) {
      report << "  * `" << kv.first << "`: mean " << (100.0 * kv.second.first / static_cast<double>(kv.second.second)) << "% over " << kv.second.second
             << " points\n";
    }
    report << "\nPer case, domains >= 1% of their config's time:\n\n";
    for (const std::pair<const std::string, std::pair<double, int>>& kv : per_case_significant) {
      report << "  * `" << kv.first << "`: mean " << (100.0 * kv.second.first / static_cast<double>(kv.second.second)) << "% over " << kv.second.second
             << " points\n";
    }
    report << "\nWhat this cannot capture (include/epykos/optimise/cost.hpp's header comment): libm `std::exp` vs "
              "`exp_poly` share one Exp coefficient (fitted for whichever ExpMode the capture used); cache effects "
              "across domains live at once, not per domain; fused pairs / chain tails are priced as two ops and two "
              "dispatches; a fused-into-reduction domain's `kept_rows` and an inlined domain's exact re-fetch count "
              "are approximated from IR structure (infer_plan), not from a live interpreter plan; a domain measuring "
              "a few tens of nanoseconds is measuring the profiler's own std::chrono calls as much as its own work.\n";

    std::cout << report.str();
    if (!report_path.empty()) {
      fs::create_directories(fs::path(report_path).parent_path());
      std::ofstream f(report_path);
      f << report.str();
    }
    std::cerr << "costmodel_fit: wrote " << (fs::path(out_dir) / fingerprint / "cost_model.json").string() << "\n";
  } catch (const std::exception& e) {
    std::cerr << "costmodel_fit: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
