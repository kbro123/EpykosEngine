#include "epykos/solver/implicit.hpp"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/solver/residual.hpp"

namespace epykos::solver {

namespace {
std::size_t idx(int i) noexcept { return static_cast<std::size_t>(i); }
}  // namespace

bool ImplicitRegistry::is_solved_input(int ordinal) const noexcept {
  for (const ImplicitBlock& b : blocks) {
    if (ordinal == b.diag_jtr_input || ordinal == b.diag_iterations_input) return true;
    if (std::find(b.unknowns.begin(), b.unknowns.end(), ordinal) != b.unknowns.end()) return true;
  }
  return false;
}

bool ImplicitRegistry::is_block_output(int ordinal) const noexcept {
  for (const ImplicitBlock& b : blocks) {
    if (ordinal == b.diag_jtr_output || ordinal == b.diag_iterations_output) return true;
    if (std::find(b.residuals.begin(), b.residuals.end(), ordinal) != b.residuals.end()) return true;
  }
  return false;
}

std::vector<int> ImplicitRegistry::free_inputs(int n_tape_inputs) const {
  std::vector<int> free;
  for (int o = 0; o < n_tape_inputs; ++o) {
    if (!is_solved_input(o)) free.push_back(o);
  }
  return free;
}

namespace detail {

ImplicitResult implicit_begin(Tape& tape, ImplicitRegistry& registry, std::string name, std::span<const double> z0,
                              int n_residuals, const SolveOptions& options) {
  if (!tape.recording()) throw RecordError("implicit: the tape is not the current recording tape");
  if (z0.empty()) throw RecordError("implicit '" + name + "': no unknowns");
  if (n_residuals < static_cast<int>(z0.size())) {
    throw RecordError("implicit '" + name + "': " + std::to_string(n_residuals) + " residuals for " +
                      std::to_string(z0.size()) + " unknowns");
  }
  ImplicitBlock block;
  block.name = std::move(name);
  block.start.assign(z0.begin(), z0.end());
  block.options = options;
  ImplicitResult r;
  r.block = static_cast<int>(registry.blocks.size());
  r.z.reserve(z0.size());
  for (double v : z0) {
    block.unknowns.push_back(static_cast<int>(tape.num_inputs()));
    r.z.push_back(make_input(tape, v));
  }
  block.diag_jtr_input = static_cast<int>(tape.num_inputs());
  r.jtr_inf = make_input(tape, 0.0);
  block.diag_jtr_output = register_output(tape, r.jtr_inf);
  block.diag_iterations_input = static_cast<int>(tape.num_inputs());
  r.iterations = make_input(tape, 0.0);
  block.diag_iterations_output = register_output(tape, r.iterations);
  registry.blocks.push_back(std::move(block));
  return r;
}

void implicit_end(Tape& tape, ImplicitRegistry& registry, ImplicitResult& result, std::span<const Rec> F) {
  ImplicitBlock& block = registry.blocks.at(idx(result.block));
  if (!tape.recording()) throw RecordError("implicit '" + block.name + "': the tape is not the current recording tape");
  for (std::size_t i = 0; i < F.size(); ++i) {
    const Rec& f = F[i];
    if (!f.recorded()) {
      throw RecordError("implicit '" + block.name + "': residual " + std::to_string(i) + " is a constant (not recorded)");
    }
    if (f.tape != tape.serial()) {
      throw RecordError("implicit '" + block.name + "': residual " + std::to_string(i) + " was recorded on another tape");
    }
    if (!tape.tainted(f.id)) {
      throw RecordError("implicit '" + block.name + "': residual " + std::to_string(i) + " depends on no input");
    }
    block.residuals.push_back(register_output(tape, f));
  }
  // The record-time solve on the recorded residual sub-program, the other inputs at their
  // record-point values (quotes as recorded; earlier blocks' unknowns as solved).
  ResidualProgram rp(tape, block, /*passes=*/true);
  const std::vector<double> all = tape.input_values();
  std::vector<double> p(idx(rp.n_params()));
  for (int m = 0; m < rp.n_params(); ++m) p[idx(m)] = all[idx(rp.param_ordinals()[idx(m)])];
  std::vector<double> z = block.start;
  BlockSolver solver(rp, block.options);
  Factors factors;
  result.report = solver.solve(p.data(), z.data(), factors, nullptr);
  for (std::size_t j = 0; j < z.size(); ++j) {
    tape.set_input_value(block.unknowns[j], z[j]);
    result.z[j].v = z[j];
  }
  tape.set_input_value(block.diag_jtr_input, result.report.jtr_inf);
  result.jtr_inf.v = result.report.jtr_inf;
  tape.set_input_value(block.diag_iterations_input, static_cast<double>(result.report.iterations));
  result.iterations.v = static_cast<double>(result.report.iterations);
  if (!result.report.converged && block.options.throw_on_failure) {
    throw std::runtime_error("implicit '" + block.name + "': the record-time solve did not converge: " + to_string(result.report));
  }
}

}  // namespace detail

const char* to_string(JacobianPolicy policy) noexcept {
  switch (policy) {
    case JacobianPolicy::per_iteration: return "per_iteration";
    case JacobianPolicy::chord: return "chord";
  }
  return "?";
}

std::string to_string(const SolveReport& r) {
  std::ostringstream os;
  os << (r.converged ? "converged" : "NOT converged") << " in " << r.iterations << " iterations, " << r.jacobians
     << " jacobians, " << r.residual_evaluations << " residual evaluations, " << r.damping_retries
     << " damping retries, |F|_inf " << r.residual_inf << ", |J^T r|_inf " << r.jtr_inf
     << (r.shared ? ", shared factorisation" : "") << (r.refreshed ? ", refreshed" : "");
  return os.str();
}

}  // namespace epykos::solver
