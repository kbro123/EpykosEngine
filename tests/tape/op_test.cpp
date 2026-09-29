// M1/P2: the opcode enum is stable and self-describing.
#include <gtest/gtest.h>

#include <set>
#include <string>

#include "epykos/tape/op.hpp"

using epykos::Op;

TEST(Op, NamesAreStableAndDistinct) {
  EXPECT_STREQ(epykos::to_string(Op::Const), "const");
  EXPECT_STREQ(epykos::to_string(Op::Input), "input");
  EXPECT_STREQ(epykos::to_string(Op::Add), "add");
  EXPECT_STREQ(epykos::to_string(Op::Sub), "sub");
  EXPECT_STREQ(epykos::to_string(Op::Mul), "mul");
  EXPECT_STREQ(epykos::to_string(Op::Div), "div");
  EXPECT_STREQ(epykos::to_string(Op::Neg), "neg");
  EXPECT_STREQ(epykos::to_string(Op::Exp), "exp");
  EXPECT_STREQ(epykos::to_string(Op::Log), "log");
  EXPECT_STREQ(epykos::to_string(Op::Sqrt), "sqrt");
  EXPECT_STREQ(epykos::to_string(Op::Recip), "recip");
  EXPECT_STREQ(epykos::to_string(Op::Fma), "fma");
  EXPECT_STREQ(epykos::to_string(Op::Select), "select");
  EXPECT_STREQ(epykos::to_string(Op::CmpLt), "cmp_lt");
  EXPECT_STREQ(epykos::to_string(Op::CmpLe), "cmp_le");
  EXPECT_STREQ(epykos::to_string(Op::CmpGt), "cmp_gt");
  EXPECT_STREQ(epykos::to_string(Op::CmpGe), "cmp_ge");
  EXPECT_STREQ(epykos::to_string(Op::CmpEq), "cmp_eq");
  EXPECT_STREQ(epykos::to_string(Op::Sum), "sum");
  EXPECT_STREQ(epykos::to_string(Op::Affine), "affine");
  EXPECT_STREQ(epykos::to_string(Op::Gather), "gather");
  EXPECT_STREQ(epykos::to_string(Op::SegmentSum), "segment_sum");
  EXPECT_STREQ(epykos::to_string(Op::Scan), "scan");
  EXPECT_STREQ(epykos::to_string(Op::Linmap), "linmap");
  EXPECT_STREQ(epykos::to_string(Op::Quot), "quot");
  EXPECT_STREQ(epykos::to_string(Op::Quadform), "quadform");
  EXPECT_STREQ(epykos::to_string(Op::SmoothStep), "smooth_step");
  EXPECT_STREQ(epykos::to_string(Op::Frozen), "frozen");
  EXPECT_STREQ(epykos::to_string(Op::Implicit), "implicit");
  EXPECT_STREQ(epykos::to_string(Op::Pin), "pin");

  std::set<std::string> names;
  for (int i = 0; i < epykos::op_count; ++i) names.insert(epykos::to_string(static_cast<Op>(i)));
  EXPECT_EQ(names.size(), static_cast<std::size_t>(epykos::op_count)) << "names collide";
  EXPECT_EQ(names.count("?"), 0u);
}

TEST(Op, NumericValuesAreFixed) {
  // Appended, never renumbered (serialised tapes depend on it).
  EXPECT_EQ(static_cast<int>(Op::Const), 0);
  EXPECT_EQ(static_cast<int>(Op::Input), 1);
  EXPECT_EQ(static_cast<int>(Op::Fma), 11);
  EXPECT_EQ(static_cast<int>(Op::Select), 12);
  EXPECT_EQ(static_cast<int>(Op::CmpEq), 17);
  EXPECT_EQ(static_cast<int>(Op::Sum), 18);
  EXPECT_EQ(static_cast<int>(Op::Affine), 19);
  EXPECT_EQ(static_cast<int>(Op::Pin), 29);
  EXPECT_EQ(epykos::op_count, 30);
}

TEST(Op, Classification) {
  using namespace epykos;
  EXPECT_EQ(op_arity(Op::Const), 0);
  EXPECT_EQ(op_arity(Op::Input), 0);
  EXPECT_EQ(op_arity(Op::Neg), 1);
  EXPECT_EQ(op_arity(Op::Exp), 1);
  EXPECT_EQ(op_arity(Op::Add), 2);
  EXPECT_EQ(op_arity(Op::CmpLt), 2);
  EXPECT_EQ(op_arity(Op::Fma), 3);
  EXPECT_EQ(op_arity(Op::Select), 3);
  EXPECT_EQ(op_arity(Op::Sum), -1);
  EXPECT_EQ(op_arity(Op::Affine), -1);
  EXPECT_EQ(op_arity(Op::Gather), -1);
  EXPECT_TRUE(op_is_variadic(Op::Sum));
  EXPECT_TRUE(op_is_variadic(Op::Affine));
  EXPECT_FALSE(op_is_variadic(Op::Add));
  EXPECT_TRUE(op_is_leaf(Op::Const));
  EXPECT_TRUE(op_is_leaf(Op::Input));
  EXPECT_FALSE(op_is_leaf(Op::Add));
  EXPECT_TRUE(op_is_comparison(Op::CmpGe));
  EXPECT_FALSE(op_is_comparison(Op::Select));
  EXPECT_TRUE(op_is_commutative(Op::Add));
  EXPECT_TRUE(op_is_commutative(Op::Mul));
  EXPECT_TRUE(op_is_commutative(Op::CmpEq));
  EXPECT_FALSE(op_is_commutative(Op::Sub));
  EXPECT_FALSE(op_is_commutative(Op::Div));
  EXPECT_FALSE(op_is_commutative(Op::CmpLt));
  EXPECT_TRUE(op_is_supported(Op::Affine));
  EXPECT_FALSE(op_is_supported(Op::Gather));
  EXPECT_FALSE(op_is_supported(Op::Pin));
}
