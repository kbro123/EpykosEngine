// Shared helpers for the tape tests (tests/tape/*_test.cpp).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "epykos/tape/op.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::test {

inline std::uint64_t bits(double v) noexcept {
  std::uint64_t u;
  std::memcpy(&u, &v, sizeof u);
  return u;
}

inline std::vector<Op> ops_of(const Tape& t) {
  std::vector<Op> v;
  v.reserve(t.size());
  for (const Node& n : t.nodes()) v.push_back(n.op);
  return v;
}

inline std::string ops_string(const Tape& t) {
  std::string s;
  for (const Node& n : t.nodes()) {
    if (!s.empty()) s += ' ';
    s += to_string(n.op);
  }
  return s;
}

inline std::size_t count_op(const Tape& t, Op op) {
  std::size_t c = 0;
  for (const Node& n : t.nodes()) c += (n.op == op);
  return c;
}

// SplitMix64: a tiny deterministic generator for test inputs (not the engine RNG of D17).
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) : s_(seed) {}
  std::uint64_t next() noexcept {
    std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  // Uniform in [0, 1).
  double uniform() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
  // Uniform in [lo, hi).
  double uniform(double lo, double hi) noexcept { return lo + (hi - lo) * uniform(); }

 private:
  std::uint64_t s_;
};

}  // namespace epykos::test
