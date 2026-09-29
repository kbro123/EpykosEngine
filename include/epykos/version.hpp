// EpykosEngine — version and build information.
#pragma once

namespace epykos {

inline constexpr int version_major = 0;
inline constexpr int version_minor = 1;
inline constexpr int version_patch = 0;

// "0.1.0"
const char* version() noexcept;

// CMAKE_BUILD_TYPE of the build that produced libepykos: "Release" (release and reference presets)
// or "Debug".
const char* build_config() noexcept;

// The effective compiler flags of that build as one line, e.g. "-O3 -march=x86-64-v3 -fno-math-errno"
// (plus " -ffp-contract=off" under the reference preset). scripts/fingerprint.sh reports the same
// line; benchmark results should record it (D13).
const char* build_flags() noexcept;

// True when libepykos itself was compiled with -ffp-contract=off (the reference preset).
// A *_e0_test.cpp TU is always contraction-free regardless of this value.
bool build_fp_contract_off() noexcept;

}  // namespace epykos
