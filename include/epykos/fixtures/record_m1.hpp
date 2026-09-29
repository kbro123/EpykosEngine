// EpykosEngine — recording the M1 book (M1/P2 integration; a test-only fixture): the unmodified P1
// maths (fixtures/m1_price.hpp over maths/swap/ois.hpp and maths/curve/linear.hpp) instantiated on
// Rec, then the E0 tape passes.
//
// This is the one place that turns docs/WORKLOADS.md §M1 into a tape; P3 (signature pass), P4
// (interpreter) and P6 (benchmark) start from it rather than recording on their own.
//
// Tape layout (stable across the passes):
//   inputs   ordinal k = knot k, i.e. z[k] for k in [0, n_knots); the record-point values are the
//            book's z0, so tape.input_values() == book.z0
//   outputs  ordinal i in [0, n_swaps) = swap PV i in swap order, ordinal n_swaps = the book PV —
//            the same ordering as the double oracle (fixtures/m1_reference.hpp)
//
// Header-only on purpose: the record-point values that Rec carries are computed under the
// including TU's contraction setting, so an *_e0_test.cpp TU (or the reference preset) gets
// contraction-free record-point values as well as a contraction-free replay.
#pragma once

#include <cstddef>
#include <vector>

#include "epykos/compile.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_price.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/tape/passes.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::fixtures {

// Node counts along the pipeline (informational; the tape itself is the source of truth).
struct RecordM1Stats {
  std::size_t recorded = 0;         // right after recording, before any pass
  std::size_t num_inputs = 0;       // n_knots
  std::size_t num_outputs = 0;      // n_swaps + 1
  std::size_t after_compile = 0;    // the pinned tape == the returned tape's size()
};

struct RecordM1Options {
  // Run everything above the pin (`epykos::compile`, PRINCIPLES.md §1) after recording. When
  // false the raw recording is returned.
  bool run_passes = true;
  FoldSumOptions fold = {};  // min_terms = 2: every Add becomes a Sum
};

// The raw recording: 12 inputs, price_book<Rec> on the book exactly as written, 1,001 outputs.
// Validated before it is returned. Throws RecordError if the maths violates the recording
// discipline (it does not: M1 branches only on structure).
inline Tape record_m1_raw(const Book& book) {
  Tape tape;
  {
    Tape::Scope scope(tape);
    std::vector<Rec> z(static_cast<std::size_t>(n_knots));
    for (int k = 0; k < n_knots; ++k) {
      z[static_cast<std::size_t>(k)] = make_input(tape, book.z0[static_cast<std::size_t>(k)]);
    }
    std::vector<Rec> swap_pv(static_cast<std::size_t>(book.n_swaps));
    Rec book_pv;
    price_book<Rec>(book, z.data(), swap_pv.data(), &book_pv);
    for (const Rec& pv : swap_pv) register_output(tape, pv);
    register_output(tape, book_pv);
  }
  tape.validate();
  return tape;
}

// Recording plus the E0 passes. `stats`, when given, receives the node counts of every stage.
inline Tape record_m1(const Book& book, RecordM1Stats* stats = nullptr, RecordM1Options options = {}) {
  Tape tape = record_m1_raw(book);
  RecordM1Stats s;
  s.recorded = tape.size();
  s.num_inputs = tape.num_inputs();
  s.num_outputs = tape.num_outputs();
  if (options.run_passes) {
    // One call, so this fixture exercises the SAME pipeline the engine runs (PRINCIPLES.md §10
    // step 2). It used to open-code cse/dce/fold_sum/affine_collapse/dce and report a node count
    // after each; those four intermediate counts were informational only (the M1 record test
    // printed them) and are not worth a second pipeline to keep. Per-pass behaviour is tested
    // where it belongs, on the passes, in tests/tape/passes_test.cpp.
    compile(tape);
    s.after_compile = tape.size();
    tape.validate();
  } else {
    s.after_compile = s.recorded;
  }
  if (stats != nullptr) *stats = s;
  return tape;
}

}  // namespace epykos::fixtures
