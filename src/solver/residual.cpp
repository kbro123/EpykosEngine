// The residual sub-program, the factorised Jacobian (Eigen, double side only: D12, D14) and the
// per-lane Gauss–Newton / Levenberg–Marquardt solve. See include/epykos/solver/residual.hpp.
#include "epykos/solver/residual.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/ir/signature.hpp"
#include "epykos/mutation/mutation.hpp"
#include "epykos/tape/passes.hpp"

namespace epykos::solver {

namespace {
std::size_t idx(int i) noexcept { return static_cast<std::size_t>(i); }
using RowMajor = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
using MapRow = Eigen::Map<const RowMajor>;
using VecMap = Eigen::Map<const Eigen::VectorXd>;
using VecMapMut = Eigen::Map<Eigen::VectorXd>;
}  // namespace

// ---------------------------------------------------------------------------------------------
// ResidualProgram
// ---------------------------------------------------------------------------------------------

ResidualProgram::ResidualProgram(const Tape& tape, const ImplicitBlock& block, bool passes, int max_batch) {
  n_z_ = block.n_unknowns();
  n_r_ = block.n_residuals();
  if (n_z_ < 1) throw std::invalid_argument("ResidualProgram '" + block.name + "': no unknowns");
  if (n_r_ < n_z_) {
    throw std::invalid_argument("ResidualProgram '" + block.name + "': " + std::to_string(n_r_) + " residuals for " +
                                std::to_string(n_z_) + " unknowns");
  }
  if (max_batch < 1) throw std::invalid_argument("ResidualProgram: max_batch < 1");
  std::vector<node_id> roots;
  roots.reserve(idx(n_r_));
  for (int o : block.residuals) roots.push_back(tape.outputs().at(idx(o)));
  slice_ = epykos::slice(tape, roots);
  if (passes) standard_passes(slice_.tape);
  const std::vector<int>& ords = slice_.input_ordinals;  // sub ordinal -> tape ordinal
  const int n_sub = static_cast<int>(ords.size());
  z_slot_.assign(idx(n_z_), -1);
  unknown_ordinals_ = block.unknowns;
  for (int k = 0; k < n_sub; ++k) {
    const int t = ords[idx(k)];
    bool is_unknown = false;
    for (int j = 0; j < n_z_; ++j) {
      if (block.unknowns[idx(j)] == t) {
        z_slot_[idx(j)] = k;
        is_unknown = true;
        break;
      }
    }
    if (!is_unknown) {
      p_slot_.push_back(k);
      param_ordinals_.push_back(t);
    }
  }
  for (int j = 0; j < n_z_; ++j) {
    if (z_slot_[idx(j)] < 0) {
      throw std::invalid_argument("ResidualProgram '" + block.name + "': unknown " + std::to_string(j) + " (tape input " +
                                  std::to_string(block.unknowns[idx(j)]) + ") is read by no residual: a zero Jacobian column");
    }
  }
  n_p_ = static_cast<int>(p_slot_.size());
  program_ = ir::infer(slice_.tape);
  exec::Options io;
  io.max_batch = 1;
  io.lane_tile = 1;
  interp_ = std::make_unique<exec::Interpreter>(program_, io);
  adj_batch_ = std::min(max_batch, n_r_);
  adjoint::Options ao;
  ao.max_batch = adj_batch_;
  ao.lane_tile = std::min(ao.lane_tile, adj_batch_);
  adj_ = std::make_unique<adjoint::Adjoint>(program_, ao);
  state_.assign(idx(n_sub) * idx(adj_batch_), 0.0);
  out_.assign(idx(n_r_) * idx(adj_batch_), 0.0);
  out_bar_.assign(idx(n_r_) * idx(adj_batch_), 0.0);
  state_bar_.assign(idx(n_sub) * idx(adj_batch_), 0.0);
}

ResidualProgram::~ResidualProgram() = default;

void ResidualProgram::fill_state(const double* z, const double* p, int B) {
  const std::size_t Bs = idx(B);
  for (int j = 0; j < n_z_; ++j) {
    double* row = state_.data() + idx(z_slot_[idx(j)]) * Bs;
    for (std::size_t b = 0; b < Bs; ++b) row[b] = z[idx(j)];
  }
  for (int m = 0; m < n_p_; ++m) {
    double* row = state_.data() + idx(p_slot_[idx(m)]) * Bs;
    for (std::size_t b = 0; b < Bs; ++b) row[b] = p[idx(m)];
  }
}

void ResidualProgram::evaluate(const double* z, const double* p, double* F) {
  fill_state(z, p, 1);
  interp_->run(state_.data(), 1, F);
  ++evaluations_;
}

void ResidualProgram::jacobian(const double* z, const double* p, double* F, double* Jz, double* Jp) {
  for (int i0 = 0; i0 < n_r_; i0 += adj_batch_) {
    const int B = std::min(adj_batch_, n_r_ - i0);
    const std::size_t Bs = idx(B);
    fill_state(z, p, B);
    std::fill(out_bar_.begin(), out_bar_.begin() + static_cast<std::ptrdiff_t>(idx(n_r_) * Bs), 0.0);
    for (std::size_t b = 0; b < Bs; ++b) out_bar_[(idx(i0) + b) * Bs + b] = 1.0;
    adj_->run(state_.data(), B, out_bar_.data(), out_.data(), state_bar_.data());
    for (std::size_t b = 0; b < Bs; ++b) {
      const std::size_t i = idx(i0) + b;
      F[i] = out_[i * Bs + b];
      for (int j = 0; j < n_z_; ++j) Jz[i * idx(n_z_) + idx(j)] = state_bar_[idx(z_slot_[idx(j)]) * Bs + b];
      if (Jp != nullptr) {
        for (int m = 0; m < n_p_; ++m) Jp[i * idx(n_p_) + idx(m)] = state_bar_[idx(p_slot_[idx(m)]) * Bs + b];
      }
    }
  }
  ++jacobians_;
}

void ResidualProgram::jt_product(const double* z, const double* p, const double* v, double* F, double* jtz, double* jtp) {
  fill_state(z, p, 1);
  for (int i = 0; i < n_r_; ++i) out_bar_[idx(i)] = v[idx(i)];
  adj_->run(state_.data(), 1, out_bar_.data(), F, state_bar_.data());
  for (int j = 0; j < n_z_; ++j) jtz[idx(j)] = state_bar_[idx(z_slot_[idx(j)])];
  if (jtp != nullptr) {
    for (int m = 0; m < n_p_; ++m) jtp[idx(m)] = state_bar_[idx(p_slot_[idx(m)])];
  }
  ++evaluations_;
}

std::string ResidualProgram::describe() const {
  std::ostringstream os;
  os << "residual program: " << n_r_ << " residuals, " << n_z_ << " unknowns (tape inputs";
  for (int o : unknown_ordinals_) os << ' ' << o;
  os << "), " << n_p_ << " parameters (tape inputs";
  for (int o : param_ordinals_) os << ' ' << o;
  os << "); slice " << slice_.tape.size() << " nodes; IR " << program_.domains.size() << " domains, " << program_.num_values()
     << " values; adjoint batch " << adj_batch_ << "; evaluations " << evaluations_ << ", jacobians " << jacobians_ << '\n';
  os << ir::to_string(program_);
  return os.str();
}

// ---------------------------------------------------------------------------------------------
// Factors
// ---------------------------------------------------------------------------------------------

struct Factors::Impl {
  int n_r = 0, n_z = 0, n_p = 0;
  bool square = true;
  bool valid = false;
  Eigen::MatrixXd Jz;   // n_r × n_z
  Eigen::MatrixXd Jp;   // n_r × n_p
  Eigen::PartialPivLU<Eigen::MatrixXd> lu;   // square: LU of Jz
  Eigen::LDLT<Eigen::MatrixXd> ldlt;         // over-determined: Jzᵀ Jz
};

Factors::Factors() : impl_(std::make_unique<Impl>()) {}
Factors::~Factors() = default;
Factors::Factors(Factors&&) noexcept = default;
Factors& Factors::operator=(Factors&&) noexcept = default;
Factors::Factors(const Factors& o) : impl_(std::make_unique<Impl>(*o.impl_)) {}
Factors& Factors::operator=(const Factors& o) {
  if (this != &o) *impl_ = *o.impl_;
  return *this;
}

void Factors::compute(int n_r, int n_z, int n_p, const double* Jz, const double* Jp) {
  Impl& im = *impl_;
  im.n_r = n_r;
  im.n_z = n_z;
  im.n_p = n_p;
  im.square = n_r == n_z;
  im.Jz = MapRow(Jz, n_r, n_z);
  if (n_p > 0 && Jp != nullptr) {
    im.Jp = MapRow(Jp, n_r, n_p);
  } else {
    im.Jp.resize(n_r, 0);
  }
  if (im.square) {
    im.lu.compute(im.Jz);
  } else {
    im.ldlt.compute(im.Jz.transpose() * im.Jz);
  }
  im.valid = true;
}

bool Factors::valid() const noexcept { return impl_->valid; }
int Factors::n_residuals() const noexcept { return impl_->n_r; }
int Factors::n_unknowns() const noexcept { return impl_->n_z; }
int Factors::n_params() const noexcept { return impl_->n_p; }
bool Factors::square() const noexcept { return impl_->square; }

void Factors::solve(const double* rhs, double* dz) const {
  const Impl& im = *impl_;
  VecMap b(rhs, im.n_r);
  VecMapMut x(dz, im.n_z);
  if (im.square) {
    x = im.lu.solve(b);
  } else {
    x = im.ldlt.solve(im.Jz.transpose() * b);
  }
}

void Factors::solve_transposed(const double* z_bar, double* lambda) const {
  const Impl& im = *impl_;
  VecMap zb(z_bar, im.n_z);
  VecMapMut l(lambda, im.n_r);
  if (im.square) {
    // Mutant implicit.ift_not_transposed: F_z lambda = z_bar instead of F_z^T lambda = z_bar.
    if (mutant("implicit.ift_not_transposed")) {
      l = im.lu.solve(zb);
      return;
    }
    l = im.lu.transpose().solve(zb);
  } else {
    l = im.Jz * im.ldlt.solve(zb);
  }
}

void Factors::jp_transposed(const double* lambda, double* out) const {
  const Impl& im = *impl_;
  if (im.n_p == 0) return;
  VecMap l(lambda, im.n_r);
  VecMapMut o(out, im.n_p);
  o = im.Jp.transpose() * l;
}

void Factors::jp_times(const double* dp, double* out) const {
  const Impl& im = *impl_;
  VecMapMut o(out, im.n_r);
  if (im.n_p == 0) {
    o.setZero();
    return;
  }
  VecMap d(dp, im.n_p);
  o = im.Jp * d;
}

void Factors::ift_adjoint(const double* z_bar, double* lambda, double* p_bar) const {
  const Impl& im = *impl_;
  solve_transposed(z_bar, lambda);
  // Mutant implicit.ift_drop_fp: the parameters receive no pull (p_bar -= F_p^T lambda skipped).
  if (im.n_p == 0 || mutant("implicit.ift_drop_fp")) return;
  VecMap l(lambda, im.n_r);
  VecMapMut pb(p_bar, im.n_p);
  pb -= im.Jp.transpose() * l;
}

void Factors::ift_tangent(const double* dp, double* scratch, double* dz) const {
  const Impl& im = *impl_;
  jp_times(dp, scratch);
  for (int i = 0; i < im.n_r; ++i) scratch[idx(i)] = -scratch[idx(i)];
  solve(scratch, dz);
}

std::size_t Factors::bytes() const noexcept {
  const Impl& im = *impl_;
  return sizeof(double) * (idx(im.n_r) * idx(im.n_z) * 2 + idx(im.n_r) * idx(im.n_p) + (im.square ? 0 : idx(im.n_z) * idx(im.n_z)));
}

// ---------------------------------------------------------------------------------------------
// BlockSolver
// ---------------------------------------------------------------------------------------------

struct BlockSolver::Impl {
  Eigen::MatrixXd JtJ;
  Eigen::VectorXd JtF;
  Eigen::LDLT<Eigen::MatrixXd> ldlt;
};

BlockSolver::BlockSolver(ResidualProgram& rp, const SolveOptions& options)
    : rp_(rp), options_(options), impl_(std::make_unique<Impl>()) {
  const auto R = idx(rp.n_residuals());
  const auto Z = idx(rp.n_unknowns());
  const auto P = idx(rp.n_params());
  F_.assign(R, 0.0);
  F_try_.assign(R, 0.0);
  z_try_.assign(Z, 0.0);
  delta_.assign(Z, 0.0);
  Jz_.assign(R * Z, 0.0);
  Jp_.assign(R * P, 0.0);
  jtf_.assign(Z, 0.0);
  impl_->JtJ.resize(static_cast<Eigen::Index>(Z), static_cast<Eigen::Index>(Z));
  impl_->JtF.resize(static_cast<Eigen::Index>(Z));
}

BlockSolver::~BlockSolver() = default;

namespace {
double inf_norm(const std::vector<double>& v) noexcept {
  double m = 0.0;
  for (double x : v) m = std::fmax(m, std::fabs(x));
  return m;
}
}  // namespace

SolveReport BlockSolver::solve(const double* p, double* z, Factors& factors, const Factors* shared) {
  const int n_z = rp_.n_unknowns();
  const int n_r = rp_.n_residuals();
  const int n_p = rp_.n_params();
  SolveReport rep;
  rp_.evaluate(z, p, F_.data());
  ++rep.residual_evaluations;
  double r_inf = inf_norm(F_);
  double mu = options_.mu0;
  bool have_own = false;
  bool use_shared = options_.jacobian == JacobianPolicy::chord && shared != nullptr && shared->valid();
  rep.shared = use_shared;
  int stalls = 0;
  int it = 0;
  while (it < options_.max_iterations && !(r_inf < options_.tol)) {
    const Factors* fac = nullptr;
    if (use_shared) {
      fac = shared;
    } else {
      rp_.jacobian(z, p, F_.data(), Jz_.data(), n_p > 0 ? Jp_.data() : nullptr);
      ++rep.jacobians;
      own_.compute(n_r, n_z, n_p, Jz_.data(), n_p > 0 ? Jp_.data() : nullptr);
      have_own = true;
      fac = &own_;
    }
    bool accepted = false;
    bool refresh = false;
    for (int t = 0; t < options_.max_damping_tries; ++t) {
      if (mu == 0.0 || use_shared) {
        for (int i = 0; i < n_r; ++i) F_try_[idx(i)] = -F_[idx(i)];
        fac->solve(F_try_.data(), delta_.data());
      } else {
        // Levenberg–Marquardt: (JᵀJ + μ·diag(JᵀJ)) δ = −Jᵀ F on the lane's own Jacobian.
        MapRow J(Jz_.data(), n_r, n_z);
        VecMap F(F_.data(), n_r);
        impl_->JtJ.noalias() = J.transpose() * J;
        impl_->JtF.noalias() = J.transpose() * F;
        for (int j = 0; j < n_z; ++j) impl_->JtJ(j, j) *= (1.0 + mu);
        impl_->ldlt.compute(impl_->JtJ);
        VecMapMut d(delta_.data(), n_z);
        d = -impl_->ldlt.solve(impl_->JtF);
      }
      for (int j = 0; j < n_z; ++j) z_try_[idx(j)] = z[idx(j)] + delta_[idx(j)];
      rp_.evaluate(z_try_.data(), p, F_try_.data());
      ++rep.residual_evaluations;
      const double r_try = inf_norm(F_try_);
      if (r_try < r_inf) {
        for (int j = 0; j < n_z; ++j) z[idx(j)] = z_try_[idx(j)];
        F_.swap(F_try_);
        const double contraction = r_try / r_inf;
        r_inf = r_try;
        accepted = true;
        mu *= options_.mu_down;
        if (mu < 1e-300) mu = 0.0;
        if (use_shared) {
          if (contraction > options_.chord_min_contraction) {
            if (++stalls >= options_.chord_max_stalls) refresh = true;
          } else {
            stalls = 0;
          }
        }
        break;
      }
      ++rep.damping_retries;
      if (use_shared) {
        refresh = true;  // a rejected chord step: the lane takes its own Jacobian instead of damping
        break;
      }
      mu = mu == 0.0 ? 1e-3 : mu * options_.mu_up;
    }
    if (refresh) {
      use_shared = false;
      rep.refreshed = true;
      stalls = 0;
    }
    if (accepted) {
      ++it;
      continue;
    }
    if (!refresh) break;  // damping exhausted: no progress
  }
  rep.iterations = it;
  rep.converged = r_inf < options_.tol;
  rep.residual_inf = r_inf;
  // Mutant implicit.stale_jacobian: the last iterate's Jacobian stands in for the solution's
  // (a 1e-9 relative error in the IFT: below the bump gate, above the forward-mode gate).
  const bool final_jacobian = options_.final_jacobian && !mutant("implicit.stale_jacobian");
  if (final_jacobian || !have_own) {
    if (final_jacobian || shared == nullptr) {
      rp_.jacobian(z, p, F_.data(), Jz_.data(), n_p > 0 ? Jp_.data() : nullptr);
      ++rep.jacobians;
      own_.compute(n_r, n_z, n_p, Jz_.data(), n_p > 0 ? Jp_.data() : nullptr);
      have_own = true;
    }
  }
  if (have_own) {
    factors = own_;
    // ‖Jᵀ F‖∞ with the Jacobian held in own_ (at the solution when final_jacobian).
    MapRow J(Jz_.data(), n_r, n_z);
    VecMap F(F_.data(), n_r);
    VecMapMut jtf(jtf_.data(), n_z);
    jtf.noalias() = J.transpose() * F;
    rep.jtr_inf = inf_norm(jtf_);
  } else {
    factors = *shared;
    rep.jtr_inf = std::nan("");
  }
  return rep;
}

}  // namespace epykos::solver
