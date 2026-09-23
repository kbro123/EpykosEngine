// EpykosEngine — a set of named curves calibrated together through implicit nodes (M3/G4;
// PROBLEM.md §5 "curves depend on each other through the implicit node").
//
// A CurveSet holds curve definitions (name, knot times, start values and the curve's scheme:
// by default the linear-in-zero-rate one of maths/curve/linear.hpp, or — since M3/G5 — any
// composite of G1's family, maths/curve/composite.hpp, given as its regions) and calibration
// instruments. An instrument belongs to the curve it calibrates and is a residual written once
// on Scalar,
//
//     Scalar residual(const CurveStates<Scalar>& states, const Scalar& quote)
//
// that may read ANY curve's knots through `states` (projection on discounting, basis on both,
// xccy on both OIS curves). Which curves it reads is not declared: it is discovered by recording
// the instrument once on a scratch tape with every curve's knots as inputs and slicing back
// from its residual (the dependency graph is inferred from the maths, as domains are, D4). The
// graph gives the solve order:
//
//   Mode::sequential   one implicit block per strongly connected component of the graph, in
//                      dependency order (a curve per block on a DAG: the discount curve first,
//                      then the projection curves that read it; a genuine cycle becomes one
//                      joint block);
//   Mode::joint        one implicit block for every curve (all unknowns, all residuals).
//
// Both are measured on the two-curve fixture in tests/solver/curve_set_test.cpp and
// bench/solver/m1_implicit_bench.cpp; RESUME.md §5 (M3/G4) states the numbers.
//
// calibrate() records the blocks on the current tape (solver/implicit.hpp) for a vector of
// quote Recs in instrument order and returns every curve's knots as Recs carrying the
// record-time solution; a book priced off those Recs on the same tape shares the instruments'
// discount factors (ONE TAPE; ir/sharing.hpp checks it). The residual maths is stored in both its
// Rec and its double instantiation, so residuals(states, quotes) evaluates the same maths on
// double for oracles.
//
// Schemes (M3/G5). A CurveSpec with no regions is the M1 linear zero-rate curve, statement for
// statement (curve::linear::df; the G4 fixtures and their pinned node counts are unchanged). A
// CurveSpec with regions is a curve::Composite over the same knots (one region = one scheme and
// variable, bitwise Curve<S, V> and, for {linear, zero}, bitwise the M1 curve, D38): the knot
// values are then in each region's variable (zero rate, log DF or forward), the state is
// prepared ONCE per curve and state (CurveStates::set: the MonotoneCubic tangents, the cubic's
// second derivatives) and every df(slot, t) reads that prepared state, so a Hyman limiter's
// Selects are recorded once per block and shared by the residuals and the book through cse.
#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "epykos/maths/curve/composite.hpp"
#include "epykos/maths/curve/linear.hpp"
#include "epykos/scalar/rec.hpp"
#include "epykos/solver/implicit.hpp"
#include "epykos/tape/tape.hpp"

namespace epykos::solver {

struct CurveSpec {
  std::string name;
  std::vector<double> knot_t;   // ascending knot times (years)
  std::vector<double> start;    // one per knot: the start of every solve (record and run time), in the knot's variable
  std::string scheme = "linear";  // descriptive: "linear" (no regions) or the composite's regions (set by add_curve)
  std::vector<curve::RegionSpec> regions;   // empty: the M1 linear zero-rate curve; else a Composite of these regions
  curve::Composite composite;               // built by CurveSet::add_curve when `regions` is given

  bool is_composite() const noexcept { return !regions.empty(); }
};

// Every curve's knot values on one Scalar. A curve of a block not yet solved has no values;
// reading it throws (the dependency order is wrong). Values are written through set(), which
// prepares the composite's state (its coefficients) once for the curve and state.
template <class Scalar>
struct CurveStates {
  const std::vector<CurveSpec>* specs = nullptr;
  std::vector<std::vector<Scalar>> z;                          // per curve, per knot
  std::vector<curve::Composite::State<Scalar>> prepared;       // per curve (composite curves only)

  int n_curves() const noexcept { return static_cast<int>(z.size()); }
  const CurveSpec& spec(int i) const { return (*specs)[static_cast<std::size_t>(i)]; }
  int find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < specs->size(); ++i) {
      if ((*specs)[i].name == name) return static_cast<int>(i);
    }
    return -1;
  }
  void resize(std::size_t n_curves) {
    z.resize(n_curves);
    prepared.resize(n_curves);
  }
  // Sets curve i's knot values (n = its knot count) and prepares its scheme's state.
  void set(int i, const Scalar* v, std::size_t n) {
    const CurveSpec& s = spec(i);
    if (n != s.knot_t.size()) {
      throw std::invalid_argument("CurveStates::set: curve '" + s.name + "': " + std::to_string(n) + " values for " +
                                  std::to_string(s.knot_t.size()) + " knots");
    }
    z.at(static_cast<std::size_t>(i)).assign(v, v + n);
    if (s.is_composite()) prepared.at(static_cast<std::size_t>(i)) = s.composite.template prepare<Scalar>(v);
  }
  void set(int i, const std::vector<Scalar>& v) { set(i, v.data(), v.size()); }
  const std::vector<Scalar>& curve(int i) const {
    const std::vector<Scalar>& v = z.at(static_cast<std::size_t>(i));
    if (v.empty()) {
      throw std::logic_error("CurveStates: curve '" + spec(i).name + "' has no values yet (it is calibrated in a later block)");
    }
    return v;
  }
  const std::vector<Scalar>& curve(std::string_view name) const {
    const int i = find(name);
    if (i < 0) throw std::invalid_argument("CurveStates: no curve named '" + std::string(name) + "'");
    return curve(i);
  }
  // The zero rate of a curve with no regions (linear zero rate with flat extrapolation).
  // Throws std::logic_error for a composite curve (its variable is per region: use log_df).
  Scalar zero_rate(int i, double t) const {
    const CurveSpec& s = spec(i);
    if (s.is_composite()) throw std::logic_error("CurveStates::zero_rate: curve '" + s.name + "' is a composite (use log_df / df)");
    return curve::linear::zero_rate(s.knot_t.data(), curve(i).data(), static_cast<int>(s.knot_t.size()), t);
  }
  // log DF(t) of curve i.
  Scalar log_df(int i, double t) const {
    const CurveSpec& s = spec(i);
    if (s.is_composite()) return s.composite.log_df(prepared.at(static_cast<std::size_t>(i)), t);
    const Scalar zt = curve::linear::zero_rate(s.knot_t.data(), curve(i).data(), static_cast<int>(s.knot_t.size()), t);
    return -zt * t;
  }
  // DF(t) of curve i: exp(−z(t)·t) on a linear zero-rate curve, the composite's df otherwise.
  Scalar df(int i, double t) const {
    const CurveSpec& s = spec(i);
    if (s.is_composite()) {
      (void)curve(i);   // a block not yet solved throws here
      return s.composite.df(prepared.at(static_cast<std::size_t>(i)), t);
    }
    return curve::linear::df(s.knot_t.data(), curve(i).data(), static_cast<int>(s.knot_t.size()), t);
  }
};

struct Instrument {
  int curve = -1;      // the curve it calibrates
  std::string label;
  std::function<Rec(const CurveStates<Rec>&, const Rec&)> record;        // the Rec instantiation
  std::function<double(const CurveStates<double>&, double)> value;       // the double instantiation
};

class CurveSet {
 public:
  enum class Mode : int { sequential = 0, joint = 1 };

  // Throws std::invalid_argument for a duplicate name, non-ascending knots, a start vector
  // of the wrong length or regions the Composite rejects. Returns the curve index.
  int add_curve(CurveSpec spec);

  // f: Scalar(const CurveStates<Scalar>&, const Scalar& quote), instantiable on Rec and double
  // (a generic lambda). Returns the instrument index (its quote's position in calibrate()).
  template <class F>
  int add_instrument(int curve, std::string label, F f) {
    check_curve(curve);
    Instrument in;
    in.curve = curve;
    in.label = std::move(label);
    in.record = [f](const CurveStates<Rec>& s, const Rec& q) -> Rec { return f(s, q); };
    in.value = [f](const CurveStates<double>& s, double q) -> double { return f(s, q); };
    instruments_.push_back(std::move(in));
    return static_cast<int>(instruments_.size()) - 1;
  }

  int n_curves() const noexcept { return static_cast<int>(curves_.size()); }
  int n_instruments() const noexcept { return static_cast<int>(instruments_.size()); }
  int n_knots(int curve) const { check_curve(curve); return static_cast<int>(curves_[static_cast<std::size_t>(curve)].knot_t.size()); }
  int n_knots_total() const noexcept;
  const std::vector<CurveSpec>& curves() const noexcept { return curves_; }
  const CurveSpec& curve(int i) const { check_curve(i); return curves_[static_cast<std::size_t>(i)]; }
  const Instrument& instrument(int i) const { return instruments_.at(static_cast<std::size_t>(i)); }
  int find(std::string_view name) const noexcept;
  // The instruments of `curve`, in registration order.
  std::vector<int> instruments_of(int curve) const;

  // reads[i] = the curves instrument i reads, ascending (discovered by a scratch recording).
  // Throws std::runtime_error when an instrument reads no curve at all.
  std::vector<std::vector<int>> instrument_reads() const;
  // deps[c] = the curves other than c that c's instruments read, ascending.
  std::vector<std::vector<int>> dependencies() const;
  // The blocks of `mode`: each a list of curves, in solve order (every curve a block reads
  // that is not in the block is in an earlier block).
  std::vector<std::vector<int>> blocks(Mode mode) const;

  struct Calibration {
    CurveStates<Rec> states;                          // every curve's knots, record-time solution
    std::vector<ImplicitResult> results;              // per block
    std::vector<std::vector<int>> block_curves;       // per block: its curves (unknown order)
    std::vector<std::vector<int>> block_instruments;  // per block: its instruments (residual order)
  };
  // Records the implicit nodes on `tape` (the current recording tape) for quotes[i] = the quote
  // of instrument i (n_instruments Recs, normally tape inputs). Throws RecordError when the
  // tape is not recording or the quote count is wrong; std::invalid_argument when a block has
  // fewer instruments than knots.
  Calibration calibrate(Tape& tape, ImplicitRegistry& registry, std::span<const Rec> quotes, Mode mode = Mode::sequential,
                        const SolveOptions& options = {}) const;

  // The residuals on double: F[i] = instrument i at `states` and quotes[i].
  std::vector<double> residuals(const CurveStates<double>& states, std::span<const double> quotes) const;
  // z_all = every curve's knots concatenated in curve order (n_knots_total) -> states.
  CurveStates<double> states_at(std::span<const double> z_all) const;
  // The start values concatenated in curve order.
  std::vector<double> start_all() const;

 private:
  void check_curve(int i) const {
    if (i < 0 || static_cast<std::size_t>(i) >= curves_.size()) {
      throw std::invalid_argument("CurveSet: curve index " + std::to_string(i) + " out of range");
    }
  }
  std::vector<CurveSpec> curves_;
  std::vector<Instrument> instruments_;
};

const char* to_string(CurveSet::Mode mode) noexcept;

}  // namespace epykos::solver
