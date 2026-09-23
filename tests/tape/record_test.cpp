// M1/P2: the recording scalar. Each operator records the expected node; constants are leaves,
// deduplicated by bit pattern; RecBool does not convert to bool; structural_if and .value()
// throw on tainted values in record mode; select records both arms.
#include <gtest/gtest.h>

#include <cmath>
#include <type_traits>
#include <utility>
#include <vector>

#include "epykos/scalar/rec.hpp"
#include "epykos/scalar/select.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"
#include "tape/tape_test_helpers.hpp"

using epykos::invalid_node;
using epykos::Node;
using epykos::Op;
using epykos::Rec;
using epykos::RecBool;
using epykos::RecordError;
using epykos::Tape;
using epykos::test::bits;
using epykos::test::count_op;
using epykos::test::ops_of;
using epykos::test::ops_string;

// ---- compile-time discipline ------------------------------------------------------------------

static_assert(!std::is_convertible_v<Rec, double>, "no implicit Rec -> double");
static_assert(!std::is_convertible_v<RecBool, bool>, "no implicit RecBool -> bool");
static_assert(!std::is_constructible_v<bool, RecBool>, "no explicit RecBool -> bool");
static_assert(std::is_convertible_v<double, Rec>, "double -> Rec is allowed (Scalar x = 0.0)");
static_assert(std::is_trivially_copyable_v<Rec> && std::is_trivially_copyable_v<RecBool>);
static_assert(sizeof(Rec) == 16, "D24: {double v; node_id id; tape_serial tape} is 16 bytes");


// A predicate that is only well-formed when `if (RecBool)` would compile.
template <class T, class = void>
struct usable_as_condition : std::false_type {};
template <class T>
struct usable_as_condition<T, std::void_t<decltype(std::declval<T>() ? 1 : 0)>> : std::true_type {};
static_assert(!usable_as_condition<RecBool>::value, "RecBool must not be usable as a condition");
static_assert(usable_as_condition<bool>::value, "sanity check of the detector");

// Comparisons between Rec values yield RecBool, on either side of a double.
static_assert(std::is_same_v<decltype(std::declval<Rec>() < std::declval<Rec>()), RecBool>);
static_assert(std::is_same_v<decltype(std::declval<Rec>() <= 1.0), RecBool>);
static_assert(std::is_same_v<decltype(1.0 > std::declval<Rec>()), RecBool>);
static_assert(std::is_same_v<decltype(std::declval<Rec>() >= std::declval<Rec>()), RecBool>);
static_assert(std::is_same_v<decltype(std::declval<Rec>() == std::declval<Rec>()), RecBool>);

// ---- leaves -----------------------------------------------------------------------------------

TEST(Record, InputIsATaintedNodeWithAnOrdinal) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(1.5);
  const Rec y = epykos::make_input(t, 2.5);
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[x.id].op, Op::Input);
  EXPECT_TRUE(t[x.id].tainted);
  EXPECT_EQ(t[x.id].a, 0);
  EXPECT_EQ(t[x.id].konst, 1.5);
  EXPECT_EQ(t[y.id].a, 1);
  EXPECT_EQ(t.inputs(), (std::vector<epykos::node_id>{x.id, y.id}));
  EXPECT_EQ(t.input_values(), (std::vector<double>{1.5, 2.5}));
  EXPECT_EQ(x.unchecked_value(), 1.5);
  EXPECT_NO_THROW(t.validate());
}

TEST(Record, ConstantsAreLeavesDeduplicatedByBitPattern) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(2.0);
  const Rec a = x * 0.25;
  const Rec b = 0.25 * x;
  const Rec c = x + 0.25;
  const Rec d = x * -0.25;
  const Rec e = x + 0.0;
  const Rec f = x + (-0.0);
  EXPECT_EQ(count_op(t, Op::Const), 4u) << ops_string(t);  // 0.25, -0.25, +0.0, -0.0
  EXPECT_EQ(t.num_constants(), 4u);
  EXPECT_EQ(t[a.id].b, t[b.id].a) << "0.25 recorded once";
  EXPECT_EQ(t[a.id].b, t[c.id].b);
  EXPECT_NE(t[a.id].b, t[d.id].b) << "-0.25 is a different constant";
  EXPECT_NE(t[e.id].b, t[f.id].b) << "+0.0 and -0.0 are different bit patterns";
  EXPECT_EQ(bits(t[t[f.id].b].konst), bits(-0.0));
  EXPECT_FALSE(t[t[a.id].b].tainted);
  EXPECT_EQ(t.constant(0.25), t[a.id].b) << "Tape::constant returns the existing leaf";
}

TEST(Record, DetachedConstantsRecordNothingUntilUsed) {
  Tape t;
  Tape::Scope scope(t);
  const Rec k = 0.25;
  const Rec unused = 7.0;
  (void)unused;
  EXPECT_EQ(t.size(), 0u);
  EXPECT_FALSE(k.recorded());
  const Rec x = epykos::make_input(1.0);
  const Rec y = x * k;
  EXPECT_EQ(ops_of(t), (std::vector<Op>{Op::Input, Op::Const, Op::Mul}));
  EXPECT_EQ(t[y.id].a, x.id);
  EXPECT_EQ(t[y.id].b, 1);
}

TEST(Record, DoubleTimesRecRecordsConstThenMul) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(3.0);
  const Rec y = 0.5 * x;
  ASSERT_EQ(ops_of(t), (std::vector<Op>{Op::Input, Op::Const, Op::Mul})) << ops_string(t);
  EXPECT_EQ(t[1].konst, 0.5);
  EXPECT_EQ(t[y.id].a, 1) << "left operand is the Const leaf";
  EXPECT_EQ(t[y.id].b, x.id);
  EXPECT_EQ(y.unchecked_value(), 1.5);
  EXPECT_TRUE(t[y.id].tainted);

  const Rec z = x * 0.5;
  EXPECT_EQ(t.size(), 4u) << "0.5 is reused";
  EXPECT_EQ(t[z.id].a, x.id);
  EXPECT_EQ(t[z.id].b, 1);
}

// ---- operators ----------------------------------------------------------------------------------

TEST(Record, EachBinaryOperatorRecordsItsNode) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(1.5);
  const Rec y = epykos::make_input(2.5);
  struct Case {
    Rec r;
    Op op;
    double v;
  };
  const Case cases[] = {
      {x + y, Op::Add, 1.5 + 2.5}, {x - y, Op::Sub, 1.5 - 2.5},
      {x * y, Op::Mul, 1.5 * 2.5}, {x / y, Op::Div, 1.5 / 2.5},
  };
  for (const Case& c : cases) {
    const Node& n = t[c.r.id];
    EXPECT_EQ(n.op, c.op);
    EXPECT_EQ(n.a, x.id);
    EXPECT_EQ(n.b, y.id);
    EXPECT_TRUE(n.tainted);
    EXPECT_EQ(bits(c.r.unchecked_value()), bits(c.v));
  }
  EXPECT_EQ(t.size(), 6u);
}

TEST(Record, MixedDoubleOperandsBecomeConstLeavesOnEitherSide) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(1.5);
  const Rec a = x - 2.0;
  const Rec b = 2.0 - x;
  const Rec c = 2.0 / x;
  const Rec d = x / 2.0;
  const Rec e = 2.0 + x;
  const epykos::node_id two = t.constant(2.0);
  EXPECT_EQ(count_op(t, Op::Const), 1u);
  EXPECT_EQ(t[a.id].op, Op::Sub);
  EXPECT_EQ(t[a.id].a, x.id);
  EXPECT_EQ(t[a.id].b, two);
  EXPECT_EQ(t[b.id].op, Op::Sub);
  EXPECT_EQ(t[b.id].a, two);
  EXPECT_EQ(t[b.id].b, x.id);
  EXPECT_EQ(t[c.id].op, Op::Div);
  EXPECT_EQ(t[c.id].a, two);
  EXPECT_EQ(t[d.id].op, Op::Div);
  EXPECT_EQ(t[d.id].b, two);
  EXPECT_EQ(t[e.id].op, Op::Add);
  EXPECT_EQ(t[e.id].a, two);
  EXPECT_EQ(b.unchecked_value(), 0.5);
}

TEST(Record, CompoundAssignmentRecords) {
  Tape t;
  Tape::Scope scope(t);
  Rec acc = epykos::make_input(1.0);
  const epykos::node_id start = acc.id;
  acc += 2.0;
  EXPECT_EQ(t[acc.id].op, Op::Add);
  EXPECT_EQ(t[acc.id].a, start);
  acc *= epykos::make_input(3.0);
  EXPECT_EQ(t[acc.id].op, Op::Mul);
  acc -= 1.0;
  EXPECT_EQ(t[acc.id].op, Op::Sub);
  acc /= 2.0;
  EXPECT_EQ(t[acc.id].op, Op::Div);
  EXPECT_EQ(acc.unchecked_value(), ((1.0 + 2.0) * 3.0 - 1.0) / 2.0);
}

TEST(Record, UnaryOperatorsRecordTheirNodes) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(1.5);
  const Rec n = -x;
  const Rec e = exp(x);
  const Rec l = log(x);
  const Rec s = sqrt(x);
  const Rec r = recip(x);
  const Rec p = +x;
  EXPECT_EQ(t[n.id].op, Op::Neg);
  EXPECT_EQ(t[e.id].op, Op::Exp);
  EXPECT_EQ(t[l.id].op, Op::Log);
  EXPECT_EQ(t[s.id].op, Op::Sqrt);
  EXPECT_EQ(t[r.id].op, Op::Recip);
  EXPECT_EQ(p.id, x.id) << "unary plus records nothing";
  for (const Rec& v : {n, e, l, s, r}) {
    EXPECT_EQ(t[v.id].a, x.id);
    EXPECT_EQ(t[v.id].b, invalid_node);
    EXPECT_TRUE(t[v.id].tainted);
  }
  EXPECT_EQ(bits(n.unchecked_value()), bits(-1.5));
  EXPECT_EQ(bits(e.unchecked_value()), bits(std::exp(1.5)));
  EXPECT_EQ(bits(l.unchecked_value()), bits(std::log(1.5)));
  EXPECT_EQ(bits(s.unchecked_value()), bits(std::sqrt(1.5)));
  EXPECT_EQ(bits(r.unchecked_value()), bits(1.0 / 1.5));
  EXPECT_EQ(t.size(), 6u);
}

TEST(Record, FmaRecordsOneTernaryNode) {
  Tape t;
  Tape::Scope scope(t);
  const Rec a = epykos::make_input(1.1);
  const Rec b = epykos::make_input(2.2);
  const Rec c = epykos::make_input(3.3);
  const Rec f = fma(a, b, c);
  ASSERT_EQ(t.size(), 4u);
  EXPECT_EQ(t[f.id].op, Op::Fma);
  EXPECT_EQ(t[f.id].a, a.id);
  EXPECT_EQ(t[f.id].b, b.id);
  EXPECT_EQ(t[f.id].c, c.id);
  EXPECT_EQ(bits(f.unchecked_value()), bits(std::fma(1.1, 2.2, 3.3)));
  const Rec g = fma(a, b, 1.0);
  EXPECT_EQ(t[g.id].op, Op::Fma);
  EXPECT_EQ(t[t[g.id].c].op, Op::Const);
}

TEST(Record, ComparisonsRecordCmpNodesAndAreNotBool) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(1.0);
  const Rec y = epykos::make_input(2.0);
  struct Case {
    RecBool r;
    Op op;
    bool v;
  };
  const Case cases[] = {
      {x < y, Op::CmpLt, true},  {x <= y, Op::CmpLe, true}, {x > y, Op::CmpGt, false},
      {x >= y, Op::CmpGe, false}, {x == y, Op::CmpEq, false},
  };
  for (const Case& c : cases) {
    const Node& n = t[c.r.id];
    EXPECT_EQ(n.op, c.op);
    EXPECT_EQ(n.a, x.id);
    EXPECT_EQ(n.b, y.id);
    EXPECT_TRUE(n.tainted);
    EXPECT_EQ(c.r.unchecked_value(), c.v);
  }
  const RecBool m = x < 5.0;
  EXPECT_EQ(t[m.id].op, Op::CmpLt);
  EXPECT_EQ(t[t[m.id].b].op, Op::Const);
  const RecBool n = 5.0 > x;
  EXPECT_EQ(t[n.id].op, Op::CmpGt);
  EXPECT_EQ(t[t[n.id].a].op, Op::Const);
}

// ---- discipline ---------------------------------------------------------------------------------

TEST(Record, StructuralIfOnATaintedPredicateThrows) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(1.0);
  EXPECT_THROW((void)epykos::structural_if(x < 2.0), RecordError);
  // A predicate over constants only is structural: no throw, the bit is returned.
  const Rec a = 1.0;
  const Rec b = 2.0;
  EXPECT_TRUE(epykos::structural_if(a < b));
  EXPECT_FALSE(epykos::structural_if(a > b));
  EXPECT_FALSE(t[(a < b).id].tainted);
  // Comparisons of untainted intermediate values are structural too.
  const Rec c = a * 3.0;
  EXPECT_TRUE(epykos::structural_if(c > b));
  // The double instantiation of the same vocabulary is a plain bool.
  EXPECT_TRUE(epykos::structural_if(1.0 < 2.0));
}

TEST(Record, ValueOnATaintedNodeThrowsInRecordModeOnly) {
  Tape t;
  Rec x;
  Rec k;
  {
    Tape::Scope scope(t);
    x = epykos::make_input(1.0) * 2.0;
    k = Rec(3.0) * 2.0;  // untainted: constants only
    EXPECT_THROW((void)x.value(), RecordError);
    EXPECT_EQ(k.value(), 6.0);
    EXPECT_EQ(x.unchecked_value(), 2.0);
  }
  // Outside the scope the value is readable (the record-point value).
  EXPECT_EQ(x.value(), 2.0);
}

TEST(Record, RecordedValuesCannotBeUsedOutsideTheirScope) {
  Tape t;
  Rec x;
  {
    Tape::Scope scope(t);
    x = epykos::make_input(1.0);
  }
  EXPECT_EQ(Tape::current(), nullptr);
  EXPECT_THROW((void)(x + 1.0), RecordError);
  EXPECT_THROW((void)(x < 1.0), RecordError);
  EXPECT_THROW((void)exp(x), RecordError);
  EXPECT_THROW((void)epykos::select(x < 1.0, x, x), RecordError);
  // Detached values behave as doubles outside a scope.
  const Rec a = 1.5;
  const Rec b = a * 2.0 + 1.0;
  EXPECT_FALSE(b.recorded());
  EXPECT_EQ(b.value(), 4.0);
  EXPECT_EQ(epykos::select(a < 2.0, a, b).value(), 1.5);
  EXPECT_TRUE(epykos::structural_if(a < 2.0));
  EXPECT_THROW((void)epykos::make_input(1.0), RecordError);
  EXPECT_THROW((void)epykos::register_output(a), RecordError);
}

TEST(Record, ScopesNestAndRestore) {
  Tape t1;
  Tape t2;
  EXPECT_EQ(Tape::current(), nullptr);
  {
    Tape::Scope s1(t1);
    EXPECT_EQ(Tape::current(), &t1);
    EXPECT_TRUE(t1.recording());
    EXPECT_FALSE(t2.recording());
    {
      Tape::Scope s2(t2);
      EXPECT_EQ(Tape::current(), &t2);
      (void)epykos::make_input(1.0);
    }
    EXPECT_EQ(Tape::current(), &t1);
    (void)epykos::make_input(2.0);
  }
  EXPECT_EQ(Tape::current(), nullptr);
  EXPECT_EQ(t1.size(), 1u);
  EXPECT_EQ(t2.size(), 1u);
}

// ---- select ------------------------------------------------------------------------------------------

TEST(Record, SelectRecordsBothArms) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(1.0);
  const Rec y = epykos::make_input(2.0);
  const Rec a = x * 2.0;
  const Rec b = y * 3.0;
  const RecBool c = x < y;
  const Rec s = epykos::select(c, a, b);
  const Node& n = t[s.id];
  EXPECT_EQ(n.op, Op::Select);
  EXPECT_EQ(n.a, c.id);
  EXPECT_EQ(n.b, a.id);
  EXPECT_EQ(n.c, b.id);
  EXPECT_TRUE(n.tainted);
  EXPECT_EQ(s.unchecked_value(), 2.0);
  EXPECT_EQ(count_op(t, Op::Mul), 2u) << "both arms are on the tape";
  // Double arms become Const leaves.
  const Rec u = epykos::select(c, 1.0, x);
  EXPECT_EQ(t[t[u.id].b].op, Op::Const);
  EXPECT_EQ(t[u.id].c, x.id);
  EXPECT_EQ(u.unchecked_value(), 1.0);
  // A select over untainted arms with a tainted predicate is tainted.
  const Rec w = epykos::select(c, 1.0, 2.0);
  EXPECT_TRUE(t[w.id].tainted);
  // An untainted predicate with untainted arms is not.
  const Rec v = epykos::select(Rec(1.0) < 2.0, 1.0, 2.0);
  EXPECT_FALSE(t[v.id].tainted);
}

TEST(Record, MaxMinAbsRecordSelect) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(-1.5);
  const Rec y = epykos::make_input(2.0);
  const Rec mx = epykos::max(x, y);
  EXPECT_EQ(t[mx.id].op, Op::Select);
  EXPECT_EQ(t[t[mx.id].a].op, Op::CmpLt);
  EXPECT_EQ(mx.unchecked_value(), 2.0);
  const Rec mn = epykos::min(x, y);
  EXPECT_EQ(t[mn.id].op, Op::Select);
  EXPECT_EQ(mn.unchecked_value(), -1.5);
  const Rec ab = epykos::abs(x);
  EXPECT_EQ(t[ab.id].op, Op::Select);
  EXPECT_EQ(t[t[ab.id].b].op, Op::Neg) << "the negated arm is recorded";
  EXPECT_EQ(t[ab.id].c, x.id);
  EXPECT_EQ(ab.unchecked_value(), 1.5);
  EXPECT_EQ(epykos::max(x, 0.0).unchecked_value(), 0.0);
  EXPECT_EQ(epykos::min(0.0, y).unchecked_value(), 0.0);
  // Same semantics on double.
  EXPECT_EQ(epykos::max(-1.5, 2.0), 2.0);
  EXPECT_EQ(epykos::min(-1.5, 2.0), -1.5);
  EXPECT_EQ(epykos::abs(-1.5), 1.5);
}

// ---- outputs, taint, validation ---------------------------------------------------------------------

TEST(Record, OutputsAreRegisteredExplicitly) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(1.0);
  const Rec y = x * 2.0;
  EXPECT_EQ(t.num_outputs(), 0u);
  EXPECT_EQ(epykos::register_output(y), 0);
  EXPECT_EQ(epykos::register_output(t, x), 1);
  EXPECT_EQ(epykos::register_output(Rec(5.0)), 2) << "a detached constant is materialised";
  EXPECT_EQ(t.outputs(), (std::vector<epykos::node_id>{y.id, x.id, t.constant(5.0)}));
  EXPECT_NO_THROW(t.validate());
}

TEST(Record, TaintIsComputedAtRecordTime) {
  Tape t;
  Tape::Scope scope(t);
  const Rec a = Rec(2.0) * 3.0;
  const Rec x = epykos::make_input(1.0);
  const Rec b = a + 1.0;
  const Rec c = b * x;
  const Rec d = c - c;
  EXPECT_FALSE(t[a.id].tainted);
  EXPECT_FALSE(t[b.id].tainted);
  EXPECT_TRUE(t[c.id].tainted);
  EXPECT_TRUE(t[d.id].tainted) << "taint is structural, not a value";
  EXPECT_NO_THROW(t.validate());
}

TEST(Record, LowLevelApiChecksArguments) {
  Tape t;
  const epykos::node_id x = t.input(1.0);
  EXPECT_THROW((void)t.unary(Op::Add, x), RecordError);
  EXPECT_THROW((void)t.binary(Op::Neg, x, x), RecordError);
  EXPECT_THROW((void)t.binary(Op::Add, x, 7), RecordError);
  EXPECT_THROW((void)t.ternary(Op::Add, x, x, x), RecordError);
  EXPECT_THROW((void)t.output(3), RecordError);
  const epykos::node_id args[] = {x, x};
  const double coefs[] = {1.0};
  EXPECT_THROW((void)t.variadic(Op::Affine, args, coefs, 0.0), RecordError);
  EXPECT_THROW((void)t.variadic(Op::Add, args), RecordError);
  EXPECT_THROW((void)t.variadic(Op::Sum, std::span<const epykos::node_id>{}), RecordError);
  const epykos::node_id s = t.variadic(Op::Sum, args);
  EXPECT_EQ(t[s].nargs, 2);
  EXPECT_EQ(t.args(s).size(), 2u);
  EXPECT_TRUE(t[s].tainted);
  EXPECT_NO_THROW(t.validate());
}

TEST(Record, DumpIsReadable) {
  Tape t;
  Tape::Scope scope(t);
  const Rec x = epykos::make_input(1.0);
  const Rec y = x * 0.5;
  (void)epykos::register_output(y);
  const std::string s = epykos::to_string(t);
  EXPECT_NE(s.find("#0 input @0"), std::string::npos) << s;
  EXPECT_NE(s.find("#1 const 0.5"), std::string::npos) << s;
  EXPECT_NE(s.find("#2 mul #0 #1 [t]"), std::string::npos) << s;
  EXPECT_NE(s.find("outputs: #2"), std::string::npos) << s;
}

// ---- tape identity (D24) ----------------------------------------------------------------------

// A value recorded on tape b used under tape a's scope must throw, not be recorded as a's node
// with the same id (before D24: on_b * 2.0 recorded Mul(#0, Const 2) on a, i.e. 5 * 2 = 10 with
// the wrong taint bit and no exception).
TEST(Record, ValueFromAnotherTapeThrowsInsteadOfBeingReinterpreted) {
  Tape a;
  Tape b;
  Rec on_b;
  {
    Tape::Scope sb(b);
    on_b = epykos::make_input(b, 100.0);  // b's node 0
  }
  EXPECT_EQ(on_b.tape, b.serial());
  EXPECT_NE(a.serial(), b.serial());
  Tape::Scope sa(a);
  EXPECT_EQ(a.constant(5.0), 0) << "a's node 0 is Const 5: the id on_b holds";
  EXPECT_THROW((void)(on_b * 2.0), RecordError);
  EXPECT_THROW((void)exp(on_b), RecordError);
  EXPECT_THROW((void)(on_b < 1.0), RecordError);
  EXPECT_THROW((void)fma(on_b, on_b, 1.0), RecordError);
  EXPECT_THROW((void)epykos::select(Rec(1.0) < 2.0, on_b, 0.0), RecordError);
  EXPECT_THROW((void)epykos::register_output(on_b), RecordError);
  EXPECT_THROW((void)on_b.value(), RecordError);
  EXPECT_EQ(on_b.unchecked_value(), 100.0);
  // On a: Const 5 and the select's predicate (Const 1, Const 2, CmpLt); the binary ops check
  // their first operand before materialising the second, so no Const 2 from on_b * 2.0.
  EXPECT_EQ(a.size(), 4u) << epykos::to_string(a);
  for (const Node& n : a.nodes()) EXPECT_NE(n.op, Op::Mul) << "no op on on_b was recorded";

  // A value recorded on a is fine in a's scope while b still exists.
  const Rec x = epykos::make_input(a, 1.0);
  EXPECT_NO_THROW((void)(x * 2.0));
}

TEST(Record, NestedScopesKeepValuesOnTheirOwnTape) {
  Tape outer;
  Tape inner;
  Tape::Scope so(outer);
  const Rec x = epykos::make_input(outer, 1.0);
  const Rec k = x * 3.0;
  {
    Tape::Scope si(inner);
    EXPECT_THROW((void)(k + 1.0), RecordError);
    EXPECT_THROW((void)k.value(), RecordError);
    EXPECT_THROW((void)epykos::structural_if(k < 5.0), RecordError);
    const Rec y = epykos::make_input(inner, 2.0);
    const Rec z = y * 2.0;
    EXPECT_EQ(z.tape, inner.serial());
    EXPECT_EQ(inner.size(), 3u);
    // A predicate recorded on the outer tape cannot be examined on the inner one either.
    RecBool c;
    {
      Tape::Scope back(outer);
      c = k < 5.0;
    }
    EXPECT_THROW((void)epykos::structural_if(c), RecordError);
  }
  EXPECT_NO_THROW((void)(k + 1.0));
  EXPECT_EQ(outer.size(), 7u);  // input, 3.0, mul, 5.0, cmp, 1.0, add
}

TEST(Record, ForeignNodeIdsThrowRecordErrorNotOutOfRange) {
  Tape a;  // empty
  Tape b;
  Rec on_b;
  {
    Tape::Scope sb(b);
    on_b = epykos::make_input(b, 1.0) * 2.0;  // b's node 2
  }
  Tape::Scope sa(a);
  EXPECT_THROW((void)on_b.value(), RecordError);
  EXPECT_THROW((void)a.tainted(on_b.id), RecordError);
  EXPECT_THROW((void)a.node(7), RecordError);
  EXPECT_THROW((void)a.node(-1), RecordError);
  EXPECT_THROW((void)a.args(3), RecordError);
  EXPECT_THROW((void)a.coefs(3), RecordError);
}

TEST(Record, SerialsFollowTheNodeTable) {
  Tape t;
  const epykos::tape_serial s0 = t.serial();
  EXPECT_NE(s0, epykos::no_tape);
  Rec x;
  {
    Tape::Scope scope(t);
    x = epykos::make_input(t, 1.0) * 2.0;
    epykos::register_output(t, x);
  }
  EXPECT_EQ(x.tape, s0);
  // A copy is a new table: the values of t are not values of the copy.
  Tape copy = t;
  EXPECT_NE(copy.serial(), s0);
  EXPECT_EQ(t.serial(), s0);
  EXPECT_EQ(copy.size(), t.size());
  {
    Tape::Scope sc(copy);
    EXPECT_THROW((void)(x + 1.0), RecordError);
  }
  {
    Tape::Scope st(t);
    EXPECT_NO_THROW((void)(x + 1.0));
  }
  // A move keeps the serial with the nodes; the moved-from tape is empty with a new serial.
  Tape moved = std::move(t);
  EXPECT_EQ(moved.serial(), s0);
  EXPECT_NE(t.serial(), s0);
  EXPECT_EQ(t.size(), 0u);
  {
    Tape::Scope sm(moved);
    EXPECT_NO_THROW((void)(x + 1.0));
  }
  // A pass swaps in a rebuilt table: new serial, stale values throw instead of aliasing.
  epykos::standard_passes(moved);
  EXPECT_NE(moved.serial(), s0);
  {
    Tape::Scope sm(moved);
    EXPECT_THROW((void)(x + 1.0), RecordError);
  }
  // clear() likewise.
  Tape u;
  Rec y;
  {
    Tape::Scope su(u);
    y = epykos::make_input(u, 1.0);
  }
  const epykos::tape_serial su0 = u.serial();
  u.clear();
  EXPECT_NE(u.serial(), su0);
  {
    Tape::Scope su(u);
    EXPECT_THROW((void)(y * 2.0), RecordError);
  }
  // Copy assignment: the target takes a new serial; move assignment: the source's.
  Tape v;
  v = copy;
  EXPECT_NE(v.serial(), copy.serial());
  EXPECT_EQ(v.size(), copy.size());
  Tape w;
  const epykos::tape_serial sv = v.serial();
  w = std::move(v);
  EXPECT_EQ(w.serial(), sv);
  EXPECT_NE(v.serial(), sv);
}

TEST(Record, DestroyingTheCurrentTapeClearsTheScope) {

  auto* t = new Tape;
  Tape::Scope* scope = new Tape::Scope(*t);
  EXPECT_EQ(Tape::current(), t);
  delete t;
  EXPECT_EQ(Tape::current(), nullptr);
  delete scope;  // restores the previous (null) tape
  EXPECT_EQ(Tape::current(), nullptr);
}
