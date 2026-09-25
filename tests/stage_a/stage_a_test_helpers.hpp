// Shared helpers for tests/stage_a/*_test.cpp: the Stage A problem and its one recording, built
// once per process (fixtures/stage_a.hpp; the definitions of blueprints/problems/stage_a.json).
#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "epykos/fixtures/stage_a.hpp"

namespace epykos::test {

inline std::uint64_t bits(double v) {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

using clock_type = std::chrono::steady_clock;
inline double seconds_since(clock_type::time_point t0) { return std::chrono::duration<double>(clock_type::now() - t0).count(); }

// The full problem (the blueprint's 2,000 trades and 1,000 scenarios) and its recording.
inline const fixtures::StageA& stage_a() {
  static const fixtures::StageA s = [] {
    const clock_type::time_point t0 = clock_type::now();
    fixtures::StageA x = fixtures::make_stage_a();
    std::cout << "[ stage_a ] built in " << seconds_since(t0) << " s: " << x.n_trades() << " trades, " << x.n_quotes() << " quotes, " << x.n_curves()
              << " curves, " << x.n_scenarios() << " scenario lanes\n";
    return x;
  }();
  return s;
}

inline const fixtures::StageATape& stage_a_tape() {
  static const fixtures::StageATape t = [] {
    fixtures::StageATape x = fixtures::record_stage_a(stage_a());
    std::cout << "[ record  ] " << x.stats.to_string() << '\n';
    for (std::size_t k = 0; k < x.record_reports.size(); ++k) {
      std::cout << "[ block   ] " << k << " curves";
      for (int c : x.block_curves[k]) std::cout << ' ' << stage_a().sets[static_cast<std::size_t>(c)].curve;
      std::cout << ": " << solver::to_string(x.record_reports[k]) << '\n';
    }
    return x;
  }();
  return t;
}

// The knots of lane b of a program's last run, per slot (the layout of StageATape::record_knots).
inline std::vector<std::vector<double>> lane_knots(const solver::ImplicitProgram& program, const fixtures::StageATape& tape, int lane) {
  const double* full = program.full_state(lane);
  std::vector<std::vector<double>> z(tape.record_knots.size());
  std::size_t c = 0;
  for (const solver::ImplicitBlock& b : tape.registry.blocks) {
    // Block k holds the curves tape.block_curves[k] in order, their knots concatenated.
    std::size_t off = 0;
    for (int slot : tape.block_curves[c]) {
      const std::size_t nk = tape.record_knots[static_cast<std::size_t>(slot)].size();
      for (std::size_t j = 0; j < nk; ++j) z[static_cast<std::size_t>(slot)].push_back(full[static_cast<std::size_t>(b.unknowns[off + j])]);
      off += nk;
    }
    ++c;
  }
  return z;
}

}  // namespace epykos::test
