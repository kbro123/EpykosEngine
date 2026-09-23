// Executing a tape with implicit nodes: per-lane solves, the whole-program interpreter and
// adjoint, the IFT reverse rule. See include/epykos/solver/implicit_program.hpp.
#include "epykos/solver/implicit_program.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/ir/signature.hpp"

namespace epykos::solver {

namespace {
std::size_t idx(int i) noexcept { return static_cast<std::size_t>(i); }
}  // namespace

struct ImplicitProgram::Impl {
  const Tape* tape = nullptr;
  ImplicitRegistry registry;
  ProgramOptions options;
  ir::Program program;
  std::unique_ptr<exec::Interpreter> interp;
  std::unique_ptr<adjoint::Adjoint> adj;
  int n_in = 0, n_out = 0, max_batch = 0;
  std::vector<int> free;
  std::vector<double> record_state;

  struct BlockRt {
    std::unique_ptr<ResidualProgram> rp;
    std::unique_ptr<BlockSolver> solver;
    SolveOptions options;
    Factors record_factors;              // J at the tape's record point (the chord policy's shared one)
    std::vector<double> p, z;            // one lane's parameters / unknowns
    std::vector<double> record_z;        // the tape's record-point solution (the warm start)
    std::vector<double> lane_params;     // n_p × max_batch, lane-major: for lane de-duplication
    std::vector<Factors> lane_factors;   // per lane: J at that lane's solution
    std::vector<int> lane_source;        // per lane: the lane whose solve it shares (itself otherwise)
    std::vector<SolveReport> reports;    // per lane
    std::vector<double> z_bar, lambda, p_bar;
  };
  std::vector<BlockRt> blocks;

  std::vector<double> full;       // n_in × max_batch, lane-major: full[b·n_in + o]
  std::vector<double> full_soa;   // n_in × B, batch innermost
  std::vector<double> full_bar;   // n_in × B, batch innermost
  RunStats stats;
  int last_B = 0;

  void solve_lanes(const double* state, int B);
  void to_soa(int B);
};

ImplicitProgram::ImplicitProgram(const Tape& tape, const ImplicitRegistry& registry, ProgramOptions options)
    : impl_(std::make_unique<Impl>()) {
  Impl& im = *impl_;
  im.tape = &tape;
  im.registry = registry;
  im.options = options;
  if (options.max_batch < 1) throw std::invalid_argument("ImplicitProgram: max_batch < 1");
  im.n_in = static_cast<int>(tape.num_inputs());
  im.n_out = static_cast<int>(tape.num_outputs());
  im.max_batch = options.max_batch;
  for (const ImplicitBlock& b : registry.blocks) {
    for (int o : b.unknowns) {
      if (o < 0 || o >= im.n_in) throw std::invalid_argument("ImplicitProgram: block '" + b.name + "' unknown ordinal out of range");
    }
    for (int o : b.residuals) {
      if (o < 0 || o >= im.n_out) throw std::invalid_argument("ImplicitProgram: block '" + b.name + "' residual ordinal out of range");
    }
    if (b.diag_jtr_input < 0 || b.diag_jtr_input >= im.n_in || b.diag_iterations_input < 0 || b.diag_iterations_input >= im.n_in) {
      throw std::invalid_argument("ImplicitProgram: block '" + b.name + "' diagnostic ordinal out of range");
    }
    if (static_cast<int>(b.start.size()) != b.n_unknowns()) {
      throw std::invalid_argument("ImplicitProgram: block '" + b.name + "' start vector has the wrong length");
    }
  }
  im.program = ir::infer(tape);
  exec::Options io = options.interpreter;
  io.max_batch = std::max(io.max_batch, options.max_batch);
  im.interp = std::make_unique<exec::Interpreter>(im.program, io);
  adjoint::Options ao = options.adjoint;
  ao.max_batch = std::max(ao.max_batch, options.max_batch);
  im.adj = std::make_unique<adjoint::Adjoint>(im.program, ao);
  im.free = registry.free_inputs(im.n_in);
  const std::vector<double> all = tape.input_values();
  for (int o : im.free) im.record_state.push_back(all[idx(o)]);

  im.blocks.resize(registry.blocks.size());
  for (std::size_t k = 0; k < registry.blocks.size(); ++k) {
    const ImplicitBlock& b = registry.blocks[k];
    Impl::BlockRt& rt = im.blocks[k];
    rt.rp = std::make_unique<ResidualProgram>(tape, b, options.residual_passes, 64);
    rt.options = b.options;
    if (options.jacobian >= 0) rt.options.jacobian = static_cast<JacobianPolicy>(options.jacobian);
    rt.solver = std::make_unique<BlockSolver>(*rt.rp, rt.options);
    const int n_p = rt.rp->n_params();
    const int n_z = rt.rp->n_unknowns();
    const int n_r = rt.rp->n_residuals();
    rt.p.assign(idx(n_p), 0.0);
    rt.z.assign(idx(n_z), 0.0);
    rt.lane_params.assign(idx(n_p) * idx(im.max_batch), 0.0);
    rt.lane_factors.resize(idx(im.max_batch));
    rt.lane_source.assign(idx(im.max_batch), -1);
    rt.reports.resize(idx(im.max_batch));
    rt.z_bar.assign(idx(n_z), 0.0);
    rt.lambda.assign(idx(n_r), 0.0);
    rt.p_bar.assign(idx(n_p), 0.0);
    // The record-point Jacobian: the calibrated record point (Tape::set_input_value) and its
    // parameters at their record values.
    for (int j = 0; j < n_z; ++j) rt.z[idx(j)] = all[idx(b.unknowns[idx(j)])];
    rt.record_z = rt.z;
    for (int m = 0; m < n_p; ++m) rt.p[idx(m)] = all[idx(rt.rp->param_ordinals()[idx(m)])];
    std::vector<double> F(idx(n_r)), Jz(idx(n_r) * idx(n_z)), Jp(idx(n_r) * idx(n_p));
    rt.rp->jacobian(rt.z.data(), rt.p.data(), F.data(), Jz.data(), n_p > 0 ? Jp.data() : nullptr);
    rt.record_factors.compute(n_r, n_z, n_p, Jz.data(), n_p > 0 ? Jp.data() : nullptr);
  }
  im.full.assign(idx(im.n_in) * idx(im.max_batch), 0.0);
  im.full_soa.assign(idx(im.n_in) * idx(im.max_batch), 0.0);
  im.full_bar.assign(idx(im.n_in) * idx(im.max_batch), 0.0);
}

ImplicitProgram::~ImplicitProgram() = default;

void ImplicitProgram::Impl::solve_lanes(const double* state, int B) {
  if (B < 1 || B > max_batch) throw std::invalid_argument("ImplicitProgram: B out of range");
  const std::size_t Bs = idx(B);
  const std::size_t N = idx(n_in);
  stats = RunStats{};
  stats.lanes = B;
  last_B = B;
  for (std::size_t b = 0; b < Bs; ++b) {
    double* lane = full.data() + b * N;
    for (std::size_t k = 0; k < free.size(); ++k) lane[idx(free[k])] = state[k * Bs + b];
  }
  for (std::size_t k = 0; k < blocks.size(); ++k) {
    BlockRt& rt = blocks[k];
    const ImplicitBlock& blk = registry.blocks[k];
    const int n_p = rt.rp->n_params();
    const int n_z = rt.rp->n_unknowns();
    const std::vector<int>& po = rt.rp->param_ordinals();
    const Factors* shared = rt.options.jacobian == JacobianPolicy::chord ? &rt.record_factors : nullptr;
    for (std::size_t b = 0; b < Bs; ++b) {
      double* lane = full.data() + b * N;
      double* params = rt.lane_params.data() + b * idx(n_p);
      for (int m = 0; m < n_p; ++m) params[idx(m)] = lane[idx(po[idx(m)])];
      rt.lane_source[b] = static_cast<int>(b);
      if (options.dedup_lanes) {
        for (std::size_t a = 0; a < b; ++a) {
          if (rt.lane_source[a] != static_cast<int>(a)) continue;
          if (n_p == 0 || std::memcmp(rt.lane_params.data() + a * idx(n_p), params, idx(n_p) * sizeof(double)) == 0) {
            rt.lane_source[b] = static_cast<int>(a);
            break;
          }
        }
      }
      if (rt.lane_source[b] != static_cast<int>(b)) {
        const std::size_t a = idx(rt.lane_source[b]);
        const double* src = full.data() + a * N;
        for (int j = 0; j < n_z; ++j) lane[idx(blk.unknowns[idx(j)])] = src[idx(blk.unknowns[idx(j)])];
        lane[idx(blk.diag_jtr_input)] = src[idx(blk.diag_jtr_input)];
        lane[idx(blk.diag_iterations_input)] = src[idx(blk.diag_iterations_input)];
        rt.reports[b] = rt.reports[a];
        ++stats.shared_lanes;
        continue;
      }
      for (int j = 0; j < n_z; ++j) rt.z[idx(j)] = options.warm_start ? rt.record_z[idx(j)] : blk.start[idx(j)];
      const std::size_t ev0 = rt.rp->evaluations();
      const std::size_t jac0 = rt.rp->jacobians();
      SolveReport rep = rt.solver->solve(params, rt.z.data(), rt.lane_factors[b], shared);
      stats.residual_evaluations += rt.rp->evaluations() - ev0;
      stats.jacobians += rt.rp->jacobians() - jac0;
      ++stats.solves;
      if (rep.refreshed) ++stats.refreshed;
      if (!rep.converged) {
        ++stats.not_converged;
        if (options.throw_on_failure || rt.options.throw_on_failure) {
          throw std::runtime_error("ImplicitProgram: block '" + blk.name + "' lane " + std::to_string(b) +
                                   " did not converge: " + to_string(rep));
        }
      }
      for (int j = 0; j < n_z; ++j) lane[idx(blk.unknowns[idx(j)])] = rt.z[idx(j)];
      lane[idx(blk.diag_jtr_input)] = rep.jtr_inf;
      lane[idx(blk.diag_iterations_input)] = static_cast<double>(rep.iterations);
      rt.reports[b] = rep;
    }
  }
}

void ImplicitProgram::Impl::to_soa(int B) {
  const std::size_t Bs = idx(B);
  const std::size_t N = idx(n_in);
  for (std::size_t b = 0; b < Bs; ++b) {
    const double* lane = full.data() + b * N;
    for (std::size_t o = 0; o < N; ++o) full_soa[o * Bs + b] = lane[o];
  }
}

void ImplicitProgram::run(const double* state, int B, double* out) {
  Impl& im = *impl_;
  im.solve_lanes(state, B);
  im.to_soa(B);
  im.interp->run(im.full_soa.data(), B, out);
}

void ImplicitProgram::adjoint(const double* state, int B, const double* out_bar, double* out, double* state_bar) {
  Impl& im = *impl_;
  im.solve_lanes(state, B);
  im.to_soa(B);
  im.adj->run(im.full_soa.data(), B, out_bar, out, im.full_bar.data());
  const std::size_t Bs = idx(B);
  // The implicit-function rule, blocks in reverse order: an earlier block's unknowns are a
  // later block's parameters, so their adjoint is complete once every later block has pulled.
  for (std::size_t k = im.blocks.size(); k-- > 0;) {
    Impl::BlockRt& rt = im.blocks[k];
    const ImplicitBlock& blk = im.registry.blocks[k];
    const int n_p = rt.rp->n_params();
    const int n_z = rt.rp->n_unknowns();
    const std::vector<int>& po = rt.rp->param_ordinals();
    for (std::size_t b = 0; b < Bs; ++b) {
      for (int j = 0; j < n_z; ++j) rt.z_bar[idx(j)] = im.full_bar[idx(blk.unknowns[idx(j)]) * Bs + b];
      std::fill(rt.p_bar.begin(), rt.p_bar.end(), 0.0);
      const Factors& f = rt.lane_factors[idx(rt.lane_source[b])];
      f.ift_adjoint(rt.z_bar.data(), rt.lambda.data(), rt.p_bar.data());
      for (int m = 0; m < n_p; ++m) im.full_bar[idx(po[idx(m)]) * Bs + b] += rt.p_bar[idx(m)];
    }
  }
  for (std::size_t k = 0; k < im.free.size(); ++k) {
    for (std::size_t b = 0; b < Bs; ++b) state_bar[k * Bs + b] = im.full_bar[idx(im.free[k]) * Bs + b];
  }
}

int ImplicitProgram::n_state() const noexcept { return static_cast<int>(impl_->free.size()); }
int ImplicitProgram::n_inputs() const noexcept { return impl_->n_in; }
int ImplicitProgram::n_outputs() const noexcept { return impl_->n_out; }
int ImplicitProgram::n_blocks() const noexcept { return static_cast<int>(impl_->blocks.size()); }
int ImplicitProgram::max_batch() const noexcept { return impl_->max_batch; }
const std::vector<int>& ImplicitProgram::free_inputs() const noexcept { return impl_->free; }
const std::vector<double>& ImplicitProgram::record_state() const noexcept { return impl_->record_state; }

const double* ImplicitProgram::full_state(int lane) const {
  if (lane < 0 || lane >= impl_->last_B) throw std::invalid_argument("ImplicitProgram::full_state: no such lane in the last run");
  return impl_->full.data() + idx(lane) * idx(impl_->n_in);
}

const SolveReport& ImplicitProgram::report(int block, int lane) const {
  if (lane < 0 || lane >= impl_->last_B) throw std::invalid_argument("ImplicitProgram::report: no such lane in the last run");
  return impl_->blocks.at(idx(block)).reports[idx(lane)];
}

const ImplicitProgram::RunStats& ImplicitProgram::last_run() const noexcept { return impl_->stats; }
const ir::Program& ImplicitProgram::program() const noexcept { return impl_->program; }
const exec::Interpreter& ImplicitProgram::interpreter() const noexcept { return *impl_->interp; }
const adjoint::Adjoint& ImplicitProgram::whole_adjoint() const noexcept { return *impl_->adj; }
const ImplicitBlock& ImplicitProgram::block(int k) const { return impl_->registry.blocks.at(idx(k)); }
const ResidualProgram& ImplicitProgram::residual_program(int k) const { return *impl_->blocks.at(idx(k)).rp; }
const ImplicitRegistry& ImplicitProgram::registry() const noexcept { return impl_->registry; }
const ProgramOptions& ImplicitProgram::options() const noexcept { return impl_->options; }

std::string ImplicitProgram::RunStats::to_string() const {
  std::ostringstream os;
  os << lanes << " lanes, " << solves << " solves, " << shared_lanes << " shared by identical lanes, " << refreshed
     << " refreshed, " << not_converged << " not converged, " << residual_evaluations << " residual evaluations, " << jacobians
     << " jacobians";
  return os.str();
}

std::string ImplicitProgram::describe() const {
  const Impl& im = *impl_;
  std::ostringstream os;
  os << "implicit program: " << im.n_in << " tape inputs (" << im.free.size() << " free), " << im.n_out << " outputs, "
     << im.blocks.size() << " implicit blocks, max_batch " << im.max_batch << ", dedup_lanes " << (im.options.dedup_lanes ? "on" : "off")
     << "; whole program " << im.program.domains.size() << " domains, " << im.program.num_values() << " values\n";
  for (std::size_t k = 0; k < im.blocks.size(); ++k) {
    const ImplicitBlock& b = im.registry.blocks[k];
    const Impl::BlockRt& rt = im.blocks[k];
    os << "  block " << k << " '" << b.name << "': " << b.n_unknowns() << " unknowns, " << b.n_residuals() << " residuals, "
       << rt.rp->n_params() << " parameters, jacobian " << solver::to_string(rt.options.jacobian) << ", tol " << rt.options.tol
       << ", max " << rt.options.max_iterations << " iterations; residual IR " << rt.rp->program().domains.size() << " domains, "
       << rt.rp->program().num_values() << " values, slice " << rt.rp->tape().size() << " nodes\n";
  }
  return os.str();
}

}  // namespace epykos::solver
