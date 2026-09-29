// M1/P0: verifies the *_e0_test.cpp convention. This TU must be compiled with -ffp-contract=off in
// every preset, so a*b - c is rounded twice: no fused multiply-add may be emitted here.
#include <gtest/gtest.h>

#include <cmath>

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF (tests/CMakeLists.txt convention)"
#endif

namespace {

// x = 1 + 2^-27, c = 1 + 2^-26: x*x = c + 2^-54 exactly; rounding x*x to double drops the 2^-54.
// Without contraction the residual is exactly 0; with an FMA it is 2^-54.
double contraction_residual(double x, double c) { return x * x - c; }

}  // namespace

TEST(ScaffoldE0, ThisTUHasNoFusedMultiplyAdd) {
  volatile double xv = 1.0 + std::ldexp(1.0, -27);
  volatile double cv = 1.0 + std::ldexp(1.0, -26);
  const double x = xv;
  const double c = cv;
  EXPECT_EQ(contraction_residual(x, c), 0.0);
}
