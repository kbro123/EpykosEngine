// M1/P5: the structural check. In reference-arithmetic mode the hand kernel performs the oracle's
// own operations in the oracle's order over its prebuilt tables (unique times, gathers, segments),
// so against price_book<double> compiled without contraction it must be bitwise identical (E0) in
// every preset: any table error (a wrong time, a dropped row, a misplaced realised fixing, a
// segment off by one) shows as a bit difference. The fast kernel is gated E1 in m1_hand_test.cpp.
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "epykos/hand/m1_hand_kernel.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/maths/m1/reference.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace m1 = epykos::m1;
using epykos::hand::HandArith;
using epykos::hand::HandExp;
using epykos::hand::M1HandKernel;
using epykos::hand::M1HandOptions;

namespace {

const m1::Book& book() {
  static const m1::Book b = m1::make_m1_book();
  return b;
}
const m1::Batch& batch() {
  static const m1::Batch b = m1::make_m1_batch();
  return b;
}
const m1::ReferenceTable& oracle() {
  static const m1::ReferenceTable t = m1::m1_reference_values(book(), batch());
  return t;
}

M1HandOptions reference_options() {
  M1HandOptions o;
  o.arith = HandArith::reference;
  return o;
}

}  // namespace

TEST(M1HandE0, ReferenceModeForcesReferenceArithmetic) {
  M1HandOptions o = reference_options();
  o.shared_reciprocal = true;  // must be overridden
  o.exp = HandExp::poly;       // must be overridden
  const M1HandKernel k(book(), o);
  EXPECT_EQ(k.options().arith, HandArith::reference);
  EXPECT_FALSE(k.options().shared_reciprocal);
  EXPECT_EQ(k.options().exp, HandExp::std_exp);
}

TEST(M1HandE0, RecordPointBitwise) {
  const M1HandKernel k(book(), reference_options());
  std::vector<double> pv(1000);
  double bpv = 0.0;
  k.eval(book().z0.data(), pv.data(), &bpv);
  const m1::ReferenceTable& t = oracle();
  int diff = 0;
  for (int i = 0; i < 1000; ++i) {
    if (std::memcmp(&pv[static_cast<std::size_t>(i)], &t.record[static_cast<std::size_t>(i)], sizeof(double)) != 0) {
      ++diff;
      ADD_FAILURE() << "swap " << i << ": hand " << pv[static_cast<std::size_t>(i)] << " oracle "
                    << t.record[static_cast<std::size_t>(i)];
      if (diff > 5) break;
    }
  }
  EXPECT_EQ(std::memcmp(&bpv, &t.record[1000], sizeof(double)), 0) << "book pv " << bpv << " vs " << t.record[1000];
}

TEST(M1HandE0, AllStatesBitwise) {
  const M1HandKernel k(book(), reference_options());
  const m1::ReferenceTable& t = oracle();
  std::vector<double> pv(1001);
  double z[12];
  int bad_states = 0;
  for (int b = 0; b < 64; ++b) {
    batch().state(b, z);
    k.eval(z, pv.data(), pv.data() + 1000);
    if (std::memcmp(pv.data(), t.state(b), 1001 * sizeof(double)) != 0) {
      ++bad_states;
      int first = -1;
      for (int i = 0; i < 1001 && first < 0; ++i) {
        if (std::memcmp(&pv[static_cast<std::size_t>(i)], t.state(b) + i, sizeof(double)) != 0) first = i;
      }
      ADD_FAILURE() << "state " << b << " differs first at index " << first;
    }
  }
  EXPECT_EQ(bad_states, 0);
}

// eval_batch on the 64 states is bitwise the oracle table (and hence bitwise 64 evals).
TEST(M1HandE0, BatchBitwise) {
  const M1HandKernel k(book(), reference_options());
  const m1::ReferenceTable& t = oracle();
  std::vector<double> pv(1000 * 64), bpv(64);
  k.eval_batch(batch().z.data(), 64, pv.data(), bpv.data());
  int bad = 0;
  for (int b = 0; b < 64; ++b) {
    for (int i = 0; i < 1000; ++i) {
      if (std::memcmp(&pv[static_cast<std::size_t>(i) * 64 + static_cast<std::size_t>(b)], t.state(b) + i,
                      sizeof(double)) != 0) {
        ++bad;
      }
    }
    if (std::memcmp(&bpv[static_cast<std::size_t>(b)], t.state(b) + 1000, sizeof(double)) != 0) ++bad;
  }
  EXPECT_EQ(bad, 0);
}
