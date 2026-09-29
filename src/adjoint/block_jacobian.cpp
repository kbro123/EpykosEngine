#include "epykos/adjoint/block_jacobian.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "epykos/mutation/mutation.hpp"
#include "epykos/rewrite/r7_block_linmap.hpp"

namespace epykos::adjoint {

namespace {

std::size_t idx(std::size_t i) noexcept { return i; }

ir::AdMode mode_of(const ir::Program& program, const std::string& block_name) {
  const auto it = program.plan.jacobian.mode.find(block_name);
  return it == program.plan.jacobian.mode.end() ? ir::AdMode::Reverse : it->second;
}

void check_ordinals(const std::vector<int>& ordinals, int n, const char* what) {
  for (int o : ordinals) {
    if (o < 0 || o >= n) throw std::invalid_argument(std::string("block_jacobian: ") + what + " ordinal out of range");
  }
}

void reverse_jacobian(const ir::Program& program, const Adjoint& adj, const double* state, const BlockJacobianRequest& req,
                      double* jacobian) {
  const std::size_t n_in = static_cast<std::size_t>(program.inputs.size());
  const std::size_t n_out = static_cast<std::size_t>(program.outputs.size());
  const std::size_t ni = req.input_ordinals.size();
  std::vector<double> out_bar(n_out, 0.0), out(n_out, 0.0), state_bar(n_in, 0.0);
  for (std::size_t oi = 0; oi < req.output_ordinals.size(); ++oi) {
    std::fill(out_bar.begin(), out_bar.end(), 0.0);
    out_bar[idx(static_cast<std::size_t>(req.output_ordinals[oi]))] = 1.0;
    adj.run(state, 1, out_bar.data(), out.data(), state_bar.data());
    for (std::size_t j = 0; j < ni; ++j) {
      jacobian[oi * ni + j] = state_bar[idx(static_cast<std::size_t>(req.input_ordinals[j]))];
    }
  }
}

void forward_fd_jacobian(const exec::Interpreter& interp, const double* state, const BlockJacobianRequest& req, double* jacobian) {
  const std::size_t n_in = static_cast<std::size_t>(interp.n_inputs());
  const std::size_t n_out = static_cast<std::size_t>(interp.n_outputs());
  const std::size_t ni = req.input_ordinals.size();
  const std::size_t no = req.output_ordinals.size();
  std::vector<double> base_state(state, state + n_in);
  std::vector<double> out0(n_out, 0.0), out1(n_out, 0.0);
  interp.run(base_state.data(), 1, out0.data());
  for (std::size_t j = 0; j < ni; ++j) {
    const std::size_t col_input = idx(static_cast<std::size_t>(req.input_ordinals[j]));
    const double saved = base_state[col_input];
    base_state[col_input] = saved + req.fd_step;
    interp.run(base_state.data(), 1, out1.data());
    base_state[col_input] = saved;
    for (std::size_t oi = 0; oi < no; ++oi) {
      const double a = out0[idx(static_cast<std::size_t>(req.output_ordinals[oi]))];
      const double b = out1[idx(static_cast<std::size_t>(req.output_ordinals[oi]))];
      jacobian[oi * ni + j] = (b - a) / req.fd_step;
    }
  }
}

void closed_form_affine_jacobian(const ir::Program& program, const BlockJacobianRequest& req, double* jacobian) {
  const std::unordered_set<ir::domain_id> allowed(req.output_domains.begin(), req.output_domains.end());
  for (ir::domain_id d : req.output_domains) {
    if (!rewrite::is_linmap_domain(program, d)) {
      throw std::invalid_argument("block_jacobian: ClosedFormAffine requires every output_domain to be a linmap domain");
    }
  }
  std::unordered_map<ir::value_id, int> input_ordinal_of;
  input_ordinal_of.reserve(program.inputs.size() * 2);
  for (std::size_t i = 0; i < program.inputs.size(); ++i) input_ordinal_of[program.inputs[i]] = static_cast<int>(i);

  std::vector<int> col_of_input(program.inputs.size(), -1);
  for (std::size_t j = 0; j < req.input_ordinals.size(); ++j) col_of_input[idx(static_cast<std::size_t>(req.input_ordinals[j]))] = static_cast<int>(j);

  const std::size_t ni = req.input_ordinals.size();
  std::fill(jacobian, jacobian + req.output_ordinals.size() * ni, 0.0);

  for (std::size_t oi = 0; oi < req.output_ordinals.size(); ++oi) {
    const ir::value_id v = program.outputs[idx(static_cast<std::size_t>(req.output_ordinals[oi]))];
    const ir::domain_id d = program.domain_of(v);
    if (allowed.find(d) == allowed.end()) {
      throw std::invalid_argument("block_jacobian: a requested output does not belong to any of request.output_domains");
    }
    const ir::row_id row = program.row_of(v);
    const ir::Step& step = program.groups[idx(static_cast<std::size_t>(d))].steps.front();  // is_linmap_domain: exactly one, Affine
    const ir::Segment& seg = program.segments[idx(static_cast<std::size_t>(step.a.index))];
    const std::int32_t lo = seg.offsets[idx(static_cast<std::size_t>(row))];
    const std::int32_t hi = seg.offsets[idx(static_cast<std::size_t>(row)) + 1];
    for (std::int32_t k = lo; k < hi; ++k) {
      const ir::value_id member = seg.members[idx(static_cast<std::size_t>(k))];
      const auto it = input_ordinal_of.find(member);
      if (it == input_ordinal_of.end()) {
        throw std::invalid_argument("block_jacobian: ClosedFormAffine's narrower scope requires every member to be a Program Input "
                                    "directly (see this file's header); this member is a computed value of another domain");
      }
      // Mutant eg.block_jacobian_wrong_coefficient_index: the coefficient read at position (k -
      // lo + 1) instead of (k - lo) -- one entry off within the row, off-by-one against `coefs`
      // (caught by comparing a linmap block's ClosedFormAffine Jacobian against Reverse on the
      // SAME domain: tests/rewrite/ad_mode_rule_e0_test.cpp's BlockJacobian tests).
      const std::int32_t coef_index = k + (epykos::mutant("eg.block_jacobian_wrong_coefficient_index") ? 1 : 0);
      if (coef_index < lo || coef_index >= hi) continue;  // guards the mutant's own out-of-range tail entry
      const double coef = seg.coefs[idx(static_cast<std::size_t>(coef_index))];
      const int col = col_of_input[idx(static_cast<std::size_t>(it->second))];
      if (col < 0) continue;  // an Input the caller did not ask for as a Jacobian column
      jacobian[oi * ni + idx(static_cast<std::size_t>(col))] += coef;
    }
  }
}

}  // namespace

void block_jacobian(const ir::Program& program, const exec::Interpreter& interp, const Adjoint& adj, const double* state,
                    const BlockJacobianRequest& req, double* jacobian) {
  check_ordinals(req.input_ordinals, static_cast<int>(program.inputs.size()), "input");
  check_ordinals(req.output_ordinals, static_cast<int>(program.outputs.size()), "output");
  const ir::AdMode mode = mode_of(program, req.block_name);
  switch (mode) {
    case ir::AdMode::Reverse:
      reverse_jacobian(program, adj, state, req, jacobian);
      return;
    case ir::AdMode::Forward:
      forward_fd_jacobian(interp, state, req, jacobian);
      return;
    case ir::AdMode::ClosedFormAffine:
      closed_form_affine_jacobian(program, req, jacobian);
      return;
  }
}

}  // namespace epykos::adjoint
