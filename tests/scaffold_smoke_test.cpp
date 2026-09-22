// M1/P0 smoke test: the library links, the build information is populated.
#include <gtest/gtest.h>

#include <cmath>
#include <string_view>

#include "epykos/version.hpp"

namespace {

// x = 1 + 2^-27, c = 1 + 2^-26: x*x = c + 2^-54 exactly. Rounded to double, x*x == c, so the
// residual is 0 without contraction and 2^-54 with a fused multiply-add.
double contraction_residual() {
  volatile double xv = 1.0 + std::ldexp(1.0, -27);
  volatile double cv = 1.0 + std::ldexp(1.0, -26);
  const double x = xv;
  const double c = cv;
  return x * x - c;
}

}  // namespace

TEST(Scaffold, VersionString) {
  EXPECT_EQ(std::string_view(epykos::version()), "0.1.0");
  EXPECT_EQ(epykos::version_major, 0);
  EXPECT_EQ(epykos::version_minor, 1);
  EXPECT_EQ(epykos::version_patch, 0);
}

TEST(Scaffold, BuildInfoIsPopulated) {
  const std::string_view config(epykos::build_config());
  const std::string_view flags(epykos::build_flags());
  EXPECT_TRUE(config == "Release" || config == "Debug") << config;
  EXPECT_FALSE(flags.empty());
  EXPECT_NE(flags, "unknown");
  EXPECT_EQ(flags.find("-ffast-math"), std::string_view::npos) << "no -ffast-math, ever (D8)";
  if (config == "Release") {
    EXPECT_NE(flags.find("-O3"), std::string_view::npos) << flags;
    EXPECT_NE(flags.find("-fno-math-errno"), std::string_view::npos) << flags;
  }
}

TEST(Scaffold, ReferenceBuildHasNoContraction) {
  // Under the reference preset every TU, this one included, is compiled with -ffp-contract=off.
  // Under other presets this TU may or may not contract, so nothing is asserted there.
  if (epykos::build_fp_contract_off()) {
    EXPECT_EQ(contraction_residual(), 0.0);
    EXPECT_NE(std::string_view(epykos::build_flags()).find("-ffp-contract=off"),
              std::string_view::npos);
  }
}
