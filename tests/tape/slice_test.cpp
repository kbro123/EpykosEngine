// M3/G4: the backward slice of a tape (tape/slice.hpp): the sub-program that computes a set of
// nodes, Inputs kept as Inputs with their ordinals mapped, replaying bit-identically to the
// original's node values.
#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/replay.hpp"
#include "epykos/tape/slice.hpp"
#include "epykos/tape/tape.hpp"

using epykos::Rec;
using epykos::Replayer;
using epykos::Slice;
using epykos::Tape;
namespace fixtures = epykos::fixtures;

TEST(Slice, KeepsOnlyTheDependenciesAndMapsInputOrdinals) {
  Tape tape;
  Rec y1, y2;
  {
    Tape::Scope scope(tape);
    const Rec a = epykos::make_input(tape, 1.5);
    const Rec b = epykos::make_input(tape, 2.5);
    const Rec c = epykos::make_input(tape, 3.5);
    y1 = exp(a * 2.0) + c;           // reads a and c
    y2 = (b - 1.0) * (b - 1.0) * a;  // reads a and b
    epykos::register_output(tape, y1);
    epykos::register_output(tape, y2);
  }
  const Slice s = epykos::slice(tape, std::vector<epykos::node_id>{y1.node()});
  EXPECT_EQ(s.input_ordinals, (std::vector<int>{0, 2}));
  EXPECT_EQ(s.tape.num_inputs(), 2u);
  EXPECT_EQ(s.tape.num_outputs(), 1u);
  EXPECT_LT(s.tape.size(), tape.size());
  // Every node of the slice is a mapped node of the original; unmapped nodes are the ones y1 does not need.
  std::size_t mapped = 0;
  for (epykos::node_id m : s.node_map) mapped += m != epykos::invalid_node ? 1 : 0;
  EXPECT_EQ(mapped, s.tape.size());
  EXPECT_EQ(s.node_map[static_cast<std::size_t>(y2.node())], epykos::invalid_node);
  // Replay at other inputs: the slice's output equals the original's y1.
  const std::vector<double> in = {0.3, 9.0, -1.25};
  const std::vector<double> full = epykos::replay(tape, in);
  const std::vector<double> sub = epykos::replay(s.tape, std::vector<double>{in[0], in[2]});
  EXPECT_EQ(sub[0], full[0]);
}

TEST(Slice, M1BookSliceOfOneSwapReplaysBitwise) {
  const fixtures::Book book = fixtures::make_m1_book();
  const Tape tape = fixtures::record_m1(book);
  // Outputs 17 and 500 (two swap PVs) and the book PV.
  const std::vector<epykos::node_id> roots = {tape.outputs()[17], tape.outputs()[500]};
  const Slice s = epykos::slice(tape, roots);
  EXPECT_LT(s.tape.size(), tape.size() / 4);
  const fixtures::Batch batch = fixtures::make_m1_batch();
  std::vector<double> z(fixtures::n_knots), zs(s.input_ordinals.size());
  Replayer full(tape), sub(s.tape);
  std::vector<double> out_full(tape.num_outputs()), out_sub(2);
  for (int b = 0; b < 8; ++b) {
    batch.state(b, z.data());
    for (std::size_t k = 0; k < zs.size(); ++k) zs[k] = z[static_cast<std::size_t>(s.input_ordinals[k])];
    full.run(z.data(), out_full.data());
    sub.run(zs.data(), out_sub.data());
    EXPECT_EQ(out_sub[0], out_full[17]) << "state " << b;
    EXPECT_EQ(out_sub[1], out_full[500]) << "state " << b;
  }
}

TEST(Slice, RejectsABadRoot) {
  Tape tape;
  {
    Tape::Scope scope(tape);
    const Rec a = epykos::make_input(tape, 1.0);
    epykos::register_output(tape, a * 2.0);
  }
  EXPECT_THROW(epykos::slice(tape, std::vector<epykos::node_id>{99}), epykos::RecordError);
}
