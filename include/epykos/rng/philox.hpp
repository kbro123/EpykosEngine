// EpykosEngine — counter-based random numbers (D17): Philox-4x32-10 and the Wichura AS241
// inverse normal CDF. Sufficient for fixture generation (docs/WORKLOADS.md); M5/T2 may extend it.
//
// Stream model. A stream is (seed, sub-stream): the seed is the 64-bit Philox key, the sub-stream
// id fills the high 64 bits of the 128-bit counter and the draw index fills the low 64 bits. Draw n
// of a stream is therefore a pure function of (seed, sub-stream, n), independent of the order in
// which anything else was drawn:
//
//   key     = { lo32(seed), hi32(seed) }
//   counter = { lo32(sub), hi32(sub), lo32(n >> 1), hi32(n >> 1) }
//   words   = philox4x32_10(counter, key);  draw n uses words[2·(n & 1)], words[2·(n & 1) + 1]
//   u       = ((w0 << 21) | (w1 >> 11)) · 2^-53          // 53 random bits, u ∈ [0, 1)
//
// The block function is integer-only and lives here inline; the floating-point draws are compiled
// once, in src/rng/philox.cpp, so every TU of a build sees the same fixture bits.
#pragma once

#include <array>
#include <cstdint>

namespace epykos::rng {

// One Philox-4x32-10 block (Salmon, Moraes, Dror, Shaw, "Parallel random numbers: as easy as
// 1, 2, 3", SC'11): 128-bit counter and 64-bit key in, 128 bits out.
inline std::array<std::uint32_t, 4> philox4x32_10(std::array<std::uint32_t, 4> ctr,
                                                  std::array<std::uint32_t, 2> key) noexcept {
  constexpr std::uint32_t m0 = 0xD2511F53u;
  constexpr std::uint32_t m1 = 0xCD9E8D57u;
  constexpr std::uint32_t w0 = 0x9E3779B9u;
  constexpr std::uint32_t w1 = 0xBB67AE85u;
  for (int round = 0; round < 10; ++round) {
    if (round > 0) {
      key[0] += w0;
      key[1] += w1;
    }
    const std::uint64_t p0 = static_cast<std::uint64_t>(m0) * ctr[0];
    const std::uint64_t p1 = static_cast<std::uint64_t>(m1) * ctr[2];
    const std::uint32_t hi0 = static_cast<std::uint32_t>(p0 >> 32);
    const std::uint32_t lo0 = static_cast<std::uint32_t>(p0);
    const std::uint32_t hi1 = static_cast<std::uint32_t>(p1 >> 32);
    const std::uint32_t lo1 = static_cast<std::uint32_t>(p1);
    ctr = {hi1 ^ ctr[1] ^ key[0], lo1, hi0 ^ ctr[3] ^ key[1], lo0};
  }
  return ctr;
}

// Inverse of the standard normal CDF, Wichura's algorithm AS241 (PPND16, Applied Statistics 37,
// 1988), accurate to about 1e-16 relative. p in (0, 1); returns -inf at 0 and +inf at 1, NaN outside.
double inverse_normal_cdf(double p) noexcept;

// A (seed, sub-stream) stream with a draw index. The *_at functions are pure; the others read the
// draw at the current index and advance it by one.
class Philox {
 public:
  Philox(std::uint64_t seed, std::uint64_t substream, std::uint64_t index = 0) noexcept
      : seed_(seed), sub_(substream), idx_(index) {}

  std::uint64_t seed() const noexcept { return seed_; }
  std::uint64_t substream() const noexcept { return sub_; }
  std::uint64_t index() const noexcept { return idx_; }
  void seek(std::uint64_t index) noexcept { idx_ = index; }

  // The 53 random bits of draw n, as an integer in [0, 2^53).
  std::uint64_t bits53_at(std::uint64_t n) const noexcept;
  // Draw n as a double in [0, 1): bits53_at(n) · 2^-53.
  double uniform_at(std::uint64_t n) const noexcept;
  // Draw n as a double in (0, 1): (bits53_at(n) + 0.5) · 2^-53.
  double uniform_open_at(std::uint64_t n) const noexcept;

  double uniform() noexcept { return uniform_at(idx_++); }            // [0, 1)
  double uniform_open() noexcept { return uniform_open_at(idx_++); }  // (0, 1)
  // lo + (hi − lo)·u, u ∈ [0, 1).
  double uniform_range(double lo, double hi) noexcept;
  // exp(log lo + u·(log hi − log lo)), u ∈ [0, 1); lo, hi > 0.
  double log_uniform(double lo, double hi) noexcept;
  // Uniform integer in {lo, ..., hi} (inclusive): lo + floor(u·(hi − lo + 1)).
  std::int64_t uniform_int(std::int64_t lo, std::int64_t hi) noexcept;
  // Standard normal: inverse_normal_cdf(uniform_open()).
  double gaussian() noexcept;

 private:
  std::uint64_t seed_;
  std::uint64_t sub_;
  std::uint64_t idx_;
};

}  // namespace epykos::rng
