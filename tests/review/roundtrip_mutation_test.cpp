// M1/P7 review: does roundtrip_identical catch E0-relevant mutations of the program that the
// existing ComparisonDetectsDifferences test does not try? Fold-order changes inside a Sum,
// Affine coefficient permutations, a c_0 sign flip, a gather redirected to an equal-valued row,
// and an Input record value change.
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "epykos/ir/expand.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/maths/m1/book.hpp"
#include "epykos/tape/record_m1.hpp"
#include "epykos/tape/tape.hpp"
#include "ir/ir_test_helpers.hpp"

namespace m1 = epykos::m1;
namespace ir = epykos::ir;
using epykos::Tape;

namespace {

const Tape& small_tape() {
  static const Tape t = [] {
    const m1::Book full = m1::make_m1_book();
    return m1::record_m1(epykos::test::sub_book(full, {0, 1, 300, 301}));
  }();
  return t;
}

bool detected(const ir::Program& bad, std::string* diff) {
  return !ir::roundtrip_identical(small_tape(), ir::expand(bad), diff);
}

}  // namespace

TEST(ReviewRoundtrip, SwappingTwoSumMembersIsDetected) {
  ir::Program p = ir::infer(small_tape());
  bool touched = false;
  for (ir::Segment& s : p.segments) {
    if (!s.coefs.empty()) continue;  // a Sum segment
    for (std::size_t r = 0; r + 1 < s.offsets.size(); ++r) {
      if (s.offsets[r + 1] - s.offsets[r] >= 3) {
        std::swap(s.members[static_cast<std::size_t>(s.offsets[r]) + 1], s.members[static_cast<std::size_t>(s.offsets[r]) + 2]);
        touched = true;
        break;
      }
    }
    if (touched) break;
  }
  ASSERT_TRUE(touched);
  std::string diff;
  EXPECT_TRUE(detected(p, &diff)) << "swapping two Sum members (a fold-order change) was not detected";
}

TEST(ReviewRoundtrip, SwappingTwoAffineMembersWithTheirCoefficientsIsDetected) {
  ir::Program p = ir::infer(small_tape());
  bool touched = false;
  for (ir::Segment& s : p.segments) {
    if (s.coefs.empty()) continue;
    for (std::size_t r = 0; r + 1 < s.offsets.size(); ++r) {
      const std::size_t lo = static_cast<std::size_t>(s.offsets[r]);
      if (s.offsets[r + 1] - s.offsets[r] >= 2 && s.members[lo] != s.members[lo + 1]) {
        std::swap(s.members[lo], s.members[lo + 1]);
        std::swap(s.coefs[lo], s.coefs[lo + 1]);
        touched = true;
        break;
      }
    }
    if (touched) break;
  }
  ASSERT_TRUE(touched);
  std::string diff;
  EXPECT_TRUE(detected(p, &diff)) << "permuting an Affine's (member, coef) pairs (a fold-order change) was not detected";
}

TEST(ReviewRoundtrip, AffineC0SignOfZeroIsDetected) {
  ir::Program p = ir::infer(small_tape());
  bool touched = false;
  for (std::size_t d = 0; d < p.groups.size() && !touched; ++d) {
    for (ir::Step& st : p.groups[d].steps) {
      if (st.op != epykos::Op::Affine) continue;
      if (st.konst.kind == ir::SlotKind::Literal) {
        double& v = p.literals[static_cast<std::size_t>(st.konst.index)];
        if (v == 0.0) { v = -v; touched = true; break; }
      } else if (st.konst.kind == ir::SlotKind::Column) {
        double& v = p.columns[static_cast<std::size_t>(st.konst.index)].values[0];
        if (v == 0.0) { v = -v; touched = true; break; }
      }
    }
  }
  ASSERT_TRUE(touched);
  std::string diff;
  EXPECT_TRUE(detected(p, &diff)) << "flipping the sign of an Affine c_0 zero was not detected";
}

TEST(ReviewRoundtrip, InputRecordValueChangeIsDetected) {
  ir::Program p = ir::infer(small_tape());
  p.input_values[3] += 1e-9;
  std::string diff;
  EXPECT_TRUE(detected(p, &diff));
}

TEST(ReviewRoundtrip, GatherRedirectedToAnotherRowIsDetected) {
  ir::Program p = ir::infer(small_tape());
  bool touched = false;
  for (ir::Gather& g : p.gathers) {
    for (std::size_t r = 0; r + 1 < g.index.size(); ++r) {
      if (g.index[r] != g.index[r + 1]) {
        g.index[r] = g.index[r + 1];
        touched = true;
        break;
      }
    }
    if (touched) break;
  }
  ASSERT_TRUE(touched);
  std::string diff;
  EXPECT_TRUE(detected(p, &diff));
}

// A commutative-operand swap on a Mul step is accepted (a * b == b * a bitwise): not a defect,
// documented here so the accepted equivalence class is explicit.
TEST(ReviewRoundtrip, CommutativeOperandSwapIsAcceptedByDesign) {
  ir::Program p = ir::infer(small_tape());
  bool touched = false;
  for (ir::Group& g : p.groups) {
    for (ir::Step& s : g.steps) {
      if (s.op == epykos::Op::Mul) { std::swap(s.a, s.b); touched = true; break; }
    }
    if (touched) break;
  }
  ASSERT_TRUE(touched);
  std::string diff;
  EXPECT_FALSE(detected(p, &diff)) << diff;
}
