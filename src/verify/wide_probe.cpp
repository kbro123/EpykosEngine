// EpykosEngine — the contraction-independence probe of verify/oracle.hpp (D72).
//
// An ordinary library TU carrying whatever flags the preset gives it. tests/scalar/wide_test.cpp
// computes the identical expression in ITS TU and compares the two bitwise. D46's finding is that
// contraction is "a per-TU, context-sensitive compiler decision", so two instantiations of the same
// header-only template can be rounded differently even under the same flags; that is the failure
// this pair reproduces, and neither side is pinned.
//
// The expressions below are written in the `a·b + c` and `a·b − c` shapes a compiler will fuse if
// it is allowed to. If any of Wide's arithmetic leaked a contractible bare product-sum, this is
// where it would show up as a bitwise difference under the release preset.
#include "epykos/verify/oracle.hpp"

namespace epykos::verify {

void wide_contraction_probe(double seed, Wide* out) {
  const Wide x(seed);
  const Wide y = x * 1.0000000001 + 0.5;

  // 1. An add/multiply chain: 40 rounds of a fused-looking update.
  Wide a(1.0);
  for (int i = 0; i < 40; ++i) a = a * y + x * 0.25;
  out[0] = a;

  // 2. A division chain, which is where Wide's own correction terms live.
  Wide b(1.0);
  for (int i = 0; i < 20; ++i) b = (b * x + 1.0) / (y + b * 0.125);
  out[1] = b;

  // 3. An exp/log chain over the range the engine evaluates.
  Wide c(0.0);
  for (int i = 0; i < 10; ++i) {
    const Wide t = x * (0.1 * static_cast<double>(i)) - 0.3;
    c = c + exp(t) * 0.5 + log(exp(t) + 1.0) * 0.25;
  }
  out[2] = c;
}

}  // namespace epykos::verify
