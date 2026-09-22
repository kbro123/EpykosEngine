// M1/P1: the oracle. Records the book PV and the 1,000 swap PVs on double at the record point and
// at the 64 batch states (docs/WORKLOADS.md §M1 "Outputs") through m1_reference_values(), the
// in-memory table later packages compare against. This TU is contraction-free (E0, D8).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

#include "epykos/maths/m1/book.hpp"
#include "epykos/maths/m1/price.hpp"
#include "epykos/maths/m1/reference.hpp"
#include "epykos/version.hpp"

#ifndef EPYKOS_FP_CONTRACT_OFF
#error "an _e0_test.cpp TU must see EPYKOS_FP_CONTRACT_OFF"
#endif

namespace m1 = epykos::m1;

namespace {

// FNV-1a over the bytes of a double array: a bit-level digest for the log (never asserted against a
// literal: libm exp differs across platforms in the last bit).
std::uint64_t digest(const std::vector<double>& v) {
  std::uint64_t h = 1469598103934665603ull;
  for (double d : v) {
    std::uint64_t bits;
    std::memcpy(&bits, &d, sizeof bits);
    for (int i = 0; i < 8; ++i) {
      h ^= (bits >> (8 * i)) & 0xffu;
      h *= 1099511628211ull;
    }
  }
  return h;
}

}  // namespace

TEST(M1OracleE0, TableShapeAndOrdering) {
  const m1::ReferenceTable t = m1::m1_reference_values();
  ASSERT_EQ(t.n_swaps, 1000);
  ASSERT_EQ(t.n_states, 64);
  ASSERT_EQ(t.stride(), 1001);
  ASSERT_EQ(t.record.size(), 1001u);
  ASSERT_EQ(t.batch.size(), 64u * 1001u);
  for (double v : t.record) ASSERT_TRUE(std::isfinite(v));
  for (double v : t.batch) ASSERT_TRUE(std::isfinite(v));
  // Per state: 1,000 swap PVs then the book PV, the book PV being the left fold of the swap PVs.
  for (int b = -1; b < t.n_states; ++b) {
    const double* row = b < 0 ? t.record.data() : t.state(b);
    double acc = row[0];
    for (int i = 1; i < t.n_swaps; ++i) acc = acc + row[i];
    EXPECT_EQ(row[t.n_swaps], acc) << "state " << b;
  }
  EXPECT_EQ(t.record_book_pv(), t.record[1000]);
  EXPECT_EQ(t.book_pv(3), t.batch[3 * 1001 + 1000]);
  EXPECT_EQ(t.swap_pv(3, 17), t.batch[3 * 1001 + 17]);
  EXPECT_EQ(t.record_swap_pv(17), t.record[17]);
}

TEST(M1OracleE0, BatchStateZeroIsTheRecordPointBitwise) {
  const m1::ReferenceTable t = m1::m1_reference_values();
  EXPECT_EQ(std::memcmp(t.record.data(), t.state(0), 1001 * sizeof(double)), 0);
  // The other states are genuinely different states.
  for (int b = 1; b < t.n_states; ++b) EXPECT_NE(t.book_pv(b), t.record_book_pv()) << b;
}

TEST(M1OracleE0, TableIsTheTemplatedDoubleMathsInThisTU) {
  const m1::Book book = m1::make_m1_book();
  const m1::Batch batch = m1::make_m1_batch();
  const m1::ReferenceTable t = m1::m1_reference_values(book, batch);
  std::vector<double> out(1001);
  m1::price_book<double>(book, book.z0.data(), out.data(), out.data() + 1000);
  EXPECT_EQ(std::memcmp(out.data(), t.record.data(), 1001 * sizeof(double)), 0);
  double z[12];
  for (int b = 0; b < batch.n_states; ++b) {
    batch.state(b, z);
    m1::price_book<double>(book, z, out.data(), out.data() + 1000);
    EXPECT_EQ(std::memcmp(out.data(), t.state(b), 1001 * sizeof(double)), 0) << b;
    m1::m1_reference_state(book, z, out.data());
    EXPECT_EQ(std::memcmp(out.data(), t.state(b), 1001 * sizeof(double)), 0) << b;
  }
  // The seed overload is the same table.
  const m1::ReferenceTable u = m1::m1_reference_values(m1::default_seed);
  EXPECT_EQ(u.record, t.record);
  EXPECT_EQ(u.batch, t.batch);
}

TEST(M1OracleE0, Deterministic) {
  const m1::ReferenceTable a = m1::m1_reference_values();
  const m1::ReferenceTable b = m1::m1_reference_values();
  EXPECT_EQ(a.record, b.record);
  EXPECT_EQ(a.batch, b.batch);
}

TEST(M1OracleE0, ReportValues) {
  const m1::ReferenceTable t = m1::m1_reference_values();
  std::cout << std::setprecision(17) << "m1 oracle: build " << epykos::build_config() << " ["
            << epykos::build_flags() << "], libepykos fp-contract-off=" << epykos::build_fp_contract_off()
            << ", this TU fp-contract-off=1\n"
            << "  record point: book pv = " << t.record_book_pv() << ", swap pv[0] = " << t.record_swap_pv(0)
            << ", swap pv[999] = " << t.record_swap_pv(999) << "\n"
            << "  batch: book pv[1] = " << t.book_pv(1) << ", book pv[63] = " << t.book_pv(63) << "\n"
            << "  digest record = " << std::hex << digest(t.record) << ", digest batch = " << digest(t.batch)
            << std::dec << "\n";
  SUCCEED();
}
