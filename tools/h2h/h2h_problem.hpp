// tools/h2h/ — THE PROBLEM SHAPE AS A PARAMETER.
//
// Until 2026-10-09 the head-to-head had exactly one problem in it, and not by choice: the shape was
// implied by the bundle file it was handed and by `fixtures::CompareOisOptions`, which is one USD
// SOFR curve and a book of plain spot-starting OIS. `bench/compare/README.md` §4 item 7 lists what
// that leaves out -- "multi-curve and tenor basis, EUR, ... seasoned trades with realised fixings"
// -- and `blueprints/problems/stage_a.json` is a problem this engine already prices that has all
// of them. So every number the project quoted against the other engine covered about a quarter of
// what it can do.
//
// This header makes the CURVE SET and the PRODUCT MIX come from a NAMED problem instead:
//
//   compare_ois    one USD SOFR OIS curve, N plain OIS trades. D90's shape, retained verbatim and
//                  still driven by --trades / --tenors / --seed / --start-years, so D90's and
//                  D81 §7's numbers stay reproducible.
//   stage_a        blueprints/problems/stage_a_h2h.json: 4 curves (USD-SOFR, EUR-ESTR,
//                  EUR-EURIBOR-3M, EUR-EURIBOR-6M), 2 currencies, 70 calibration instruments
//                  including deposits, 16 futures and 9 tenor-basis quotes, and a 2,000-trade book
//                  over 8 blueprints in 5 families with ~20% seasoned. It is stage_a.json with one
//                  field changed per curve -- the -LOGDF definition -- because that is the only
//                  interpolation family the second engine shares (README §3; see the problem
//                  file's own `meta` for why that is a correctness requirement and not a tuning
//                  choice).
//   <name>         any other blueprints/problems/*.json with the same schema, via --problem-path.
//
// A problem may be restricted to ONE product family (`--family`), which is how the harness answers
// the question it exists for: does the advantage on plain OIS survive contact with basis, averaging
// swaps and seasoned trades? A per-family run is a problem in its own right -- its own tape, its
// own agreement gate, its own timings -- never a slice of an aggregate.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "h2h_bridge.hpp"

#include "epykos/fixtures/compare_ois.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/solver/implicit_program.hpp"

namespace h2h {

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;

struct ProblemOptions {
  std::string name = "compare_ois";    // "compare_ois" | "stage_a" | any problem file's name
  std::string problem_path;            // overrides the blueprints/problems/<name>.json lookup
  std::string family;                  // "" = the whole book; else one blueprint name
  int trades = -1;                     // -1 = the problem's own count (compare_ois: 1000)
  std::vector<std::string> tenors;     // compare_ois only
  std::uint64_t seed = 20260925;       // compare_ois only (stage_a's seed is in its problem file)
  int start_years = 0;                 // compare_ois only
  int max_batch = 64;
  int lane_tile = 8;
  bool book = true;                    // false = the calibration-only twin (no book at all)
};

// Our side of one problem: the programs about to be timed, where their answers are, and the
// `ProblemView` the other engine's objects are built from.
class OurProblem {
 public:
  explicit OurProblem(const ProblemOptions& options);
  ~OurProblem();
  OurProblem(const OurProblem&) = delete;
  OurProblem& operator=(const OurProblem&) = delete;

  const ProblemOptions& options() const noexcept { return options_; }
  const ProblemView& view() const noexcept { return view_; }
  const std::string& label() const noexcept { return view_.label; }

  // The whole program: the calibration AND the book in one tape, warm start + chord.
  solver::ImplicitProgram& full() const { return *full_; }
  // The calibration-only twin, warm start + chord / cold from the block's own start. Present only
  // on a problem built with `book = false`.
  solver::ImplicitProgram& cal_hot() const { return *cal_hot_; }
  solver::ImplicitProgram& cal_cold() const { return *cal_cold_; }
  bool has_cal_twin() const noexcept { return cal_hot_ != nullptr; }

  int n_quotes() const noexcept { return static_cast<int>(view_.quotes.size()); }
  int n_trades() const noexcept { return static_cast<int>(view_.book.size()); }
  int n_knots() const noexcept;
  int book_output() const noexcept { return book_output_; }
  // Output ordinal of trade i's PV in the reporting currency (what their book NPV sums).
  const std::vector<int>& pv_ordinals() const noexcept { return pv_ordinals_; }
  std::size_t nodes_raw() const noexcept { return nodes_raw_; }
  std::size_t nodes_after_passes() const noexcept { return nodes_after_; }
  double seconds_record() const noexcept { return seconds_record_; }

  // rows[r·n_quotes + k] = d out_{ordinals[r]} / d quote_k through the adjoint and the IFT (O3),
  // in OUR quote units (a futures quote per 1.0 of PRICE).
  std::vector<double> ladder(const std::vector<double>& quotes, const std::vector<int>& ordinals,
                            std::vector<double>* out) const;

  // The discount factors of every curve on ITS OWN time grid, off the knot values in `out` (a
  // forward run's output buffer at lane 0). `times[slot]` is curve `slot`'s grid and the result is
  // the same shape -- the same quantity their `sample()` returns, one curve at a time.
  //
  // Per curve and not one shared grid, deliberately: the four Stage A curves' last knots are
  // different adjusted dates, so a single grid reaching the LONGEST of them would sample the others
  // past their own last knot, where the two engines extrapolate differently by construction
  // (README §5) -- a gate failure that says "the engines disagree" when what happened is that the
  // harness asked a question neither side claims to answer.
  std::vector<std::vector<double>> discount_factors(const double* out,
                                                    const std::vector<std::vector<double>>& times) const;

 private:
  ProblemOptions options_;
  ProblemView view_;
  std::unique_ptr<fixtures::CompareOis> ois_;
  std::unique_ptr<fixtures::CompareOisTape> ois_tape_;
  std::unique_ptr<fixtures::StageA> stage_;
  std::unique_ptr<fixtures::StageATape> stage_tape_;
  std::unique_ptr<solver::ImplicitProgram> full_, cal_hot_, cal_cold_;
  const solver::CurveSet* set_ = nullptr;
  std::vector<std::vector<int>> knot_outputs_;
  std::vector<int> pv_ordinals_;
  int book_output_ = -1;
  std::size_t nodes_raw_ = 0, nodes_after_ = 0;
  double seconds_record_ = 0.0;

  void build_compare_ois();
  void build_stage_a();
};

// The product families of a problem, in a stable order: the distinct blueprints its book draws
// from, each with its trade count.
struct Family {
  std::string name;
  int trades = 0;
};
std::vector<Family> families_of(const ProblemView& v);

}  // namespace h2h
