// M1/P7 review: taint and scope holes in the recorder.
#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "epykos/scalar/rec.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/tape.hpp"
#include "tape/tape_test_helpers.hpp"

using epykos::Op;
using epykos::Rec;
using epykos::RecordError;
using epykos::Tape;

// A Rec carries a node id but not the tape it belongs to (D14). A value made on tape B while
// tape A's scope is current is silently interpreted as a node of A: the recorded graph is wrong
// and nothing throws when A happens to have a node with that id.
TEST(ReviewRecTaint, NodeIdFromAnotherTapeIsSilentlyReinterpreted) {
  Tape a, b;
  Rec on_b;
  {
    Tape::Scope sb(b);
    on_b = epykos::make_input(b, 100.0);  // node #0 of b (Input, value 100)
  }
  Tape::Scope sa(a);
  const Rec ka = Rec(5.0) * Rec(1.0);          // node #0..#2 of a: Const 5, Const 1, Mul (untainted)
  const Rec y = on_b * 2.0;                     // records on a with operand id 0 == a's Const 5 (!)
  epykos::register_output(a, y);
  EXPECT_EQ(y.unchecked_value(), 200.0);        // the value side says 100 * 2
  EXPECT_FALSE(a.tainted(y.node()));            // the tape side says "does not depend on an input"
  const std::vector<double> out = epykos::replay(a, {});
  EXPECT_EQ(out[0], 10.0) << "the replay computes 5 * 2: the recorded graph disagrees with the value";
  EXPECT_NE(out[0], y.unchecked_value());
  // No RecordError anywhere: the hole is silent.
  (void)ka;
}

// .value() on a tainted node throws only while a scope is active. After the scope closes the
// same Rec hands back its record-point value: a caller can branch on it between recordings.
TEST(ReviewRecTaint, ValueOnTaintedNodeIsReadableAfterTheScopeCloses) {
  Tape t;
  Rec pv;
  {
    Tape::Scope s(t);
    const Rec x = epykos::make_input(t, 1.0);
    pv = x * 3.0;
    EXPECT_THROW((void)pv.value(), RecordError);
  }
  EXPECT_NO_THROW((void)pv.value());
  EXPECT_EQ(pv.value(), 3.0);
  // The public data member is readable at any time, tainted or not.
  Tape::Scope s2(t);
  EXPECT_EQ(pv.v, 3.0);
}

// value() on a Rec whose id exceeds the current tape's size throws std::out_of_range (from
// vector::at), not RecordError.
TEST(ReviewRecTaint, ValueOnForeignIdThrowsOutOfRangeNotRecordError) {
  Tape big, small;
  Rec r;
  {
    Tape::Scope s(big);
    Rec x = epykos::make_input(big, 1.0);
    for (int k = 0; k < 10; ++k) x = x + 1.0;
    r = x;
  }
  Tape::Scope s(small);
  EXPECT_THROW((void)r.value(), std::out_of_range);
}
