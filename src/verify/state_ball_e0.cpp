// The state ball's draws (verify/differential.hpp): z + ρ·u is a multiply-add, so this is an E0
// TU (src/**/*_e0.cpp, root CMakeLists.txt, D25): -ffp-contract=off in every preset on every
// compiler, and the same bits for a draw whatever built it. The uniform draw itself comes from
// src/rng/philox_e0.cpp, pinned the same way.
#ifndef EPYKOS_FP_CONTRACT_OFF
#error "state_ball_e0.cpp must be compiled with -ffp-contract=off (see the *_e0.cpp rule in CMakeLists.txt)"
#endif

#include <cstddef>
#include <stdexcept>

#include "epykos/rng/philox.hpp"
#include "epykos/verify/differential.hpp"

namespace epykos::verify {

namespace {
std::size_t idx(int i) { return static_cast<std::size_t>(i); }
}  // namespace

void StateBall::soa(int first, int B, double* out) const {
  for (int k = 0; k < n_inputs; ++k) {
    for (int b = 0; b < B; ++b) out[idx(k) * idx(B) + idx(b)] = state(first + b)[k];
  }
}

StateBall make_state_ball(const double* centre, int n_inputs, const BallOptions& options) {
  if (n_inputs < 1) throw std::invalid_argument("make_state_ball: n_inputs < 1");
  if (options.draws < 1) throw std::invalid_argument("make_state_ball: draws < 1");
  if (centre == nullptr) throw std::invalid_argument("make_state_ball: centre is null");
  StateBall ball;
  ball.n_inputs = n_inputs;
  ball.n_draws = options.draws;
  ball.options = options;
  ball.centre.assign(centre, centre + n_inputs);
  ball.states.assign(idx(n_inputs) * idx(options.draws), 0.0);
  for (int r = 0; r < options.draws; ++r) {
    rng::Philox g(options.seed, options.substream_base + static_cast<std::uint64_t>(r));
    double* z = ball.states.data() + idx(r) * idx(n_inputs);
    for (int k = 0; k < n_inputs; ++k) {
      const double u = g.uniform_range(-1.0, 1.0);  // draw k: u in [−1, 1)
      z[k] = centre[k] + options.rho * u;
    }
  }
  return ball;
}

}  // namespace epykos::verify
