// tools/curveid/ — THE CURVE-IDENTITY PROBE: same variable, same scheme, same knots, same
// instruments => the same curve, to solver tolerance. Not "agrees within a tolerance".
//
// Pre-registered in bench/compare/CURVEID.md in a SEPARATE, EARLIER commit (the D106 standard):
// that file states, cell by cell, what this binary was expected to find before it existed, and
// §6 of it holds the measurement. Read it first; this file is the instrument, not the result.
//
// D107 (2026-10-09) lifted D11's prohibition on READING the other checkout. The prohibition on
// copying, porting or adapting is untouched and nothing here is copied from it: this file calls
// `swaps::curve::ModularCurve<double>`'s public surface (`add`, `set_forwards`, `forward`,
// `integral`, `discount`, `max_time`) and nothing else, exactly as tools/h2h/ calls their facade
// (D78). No test case of theirs is lifted either, so D108's cases-yes / implementations-no line is
// not exercised here; every number below is generated from this file's own profiles and from the
// generating curve of docs/WORKLOADS.md's seed shape.
//
// Three layers, in increasing strength, because they localise differently:
//
//   LAYER A  scheme-interpolant identity. Same knot times, same knot VALUES, our
//            `curve::SchemeVariant::value` against their single leading region's `forward(t)`.
//            Variable-free: it asks only whether the two interpolants through the same points are
//            the same function, so it is the layer that answers "do the six shared scheme names
//            share a CONSTRUCTION".
//   LAYER B  whole-curve identity. DF and the instantaneous forward on a dense grid. This is where
//            the interpolation VARIABLE enters, and where most of the matrix turns out not to
//            exist: their x is always knot forwards, and our Variable::forward is Flat/Linear only.
//            Includes the one cross-variable BRIDGE that is an exact identity (our logdf-linear
//            against their Flat).
//   LAYER C  calibration identity. One instrument per knot, maturities AT the knots, so the system
//            is square and the root unique. The residual and the damped Newton that solves it are
//            written ONCE HERE and run against each engine's own curve object, so the solver is a
//            control and not a variable: if the curve function is the same, the root must be.
//
// Nothing here relaxes a tolerance. A deviation is reported with its LOCALISATION — the pattern of
// the deviation over t decides between units, time measure, scheme construction and extrapolation
// (see `classify`) — and a convention mismatch found is a success of the probe.
//
// No timings and no performance claims: this is a correctness/alignment probe, it refuses no box.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// The other engine. Header-only; no library of theirs is linked (tools/curveid/CMakeLists.txt).
#include "swaps/curve/curve_module.hpp"

#include "epykos/maths/curve/composite.hpp"
#include "epykos/maths/curve/scheme.hpp"
#include "epykos/version.hpp"

namespace ec = epykos::curve;
namespace sc = swaps::curve;

// ==============================================================================================
// The shared configuration — printed in full, so "identically" is a statement and not a claim
// ==============================================================================================

namespace {

// The six scheme names the two engines share. Their `Scheme::Tension` and `Scheme::MonotoneConvex`
// have no counterpart here; our side has no scheme theirs lacks.
struct SchemePair {
  const char* name;
  ec::SchemeKind ours;
  sc::Scheme theirs;
};
const SchemePair kShared[] = {
    {"flat", ec::SchemeKind::flat, sc::Scheme::Flat},
    {"linear", ec::SchemeKind::linear, sc::Scheme::Linear},
    {"hermite", ec::SchemeKind::hermite, sc::Scheme::Hermite},
    {"natural_cubic", ec::SchemeKind::natural_cubic, sc::Scheme::NaturalCubic},
    {"monotone_cubic", ec::SchemeKind::monotone_cubic, sc::Scheme::MonotoneCubic},
    {"bspline", ec::SchemeKind::bspline, sc::Scheme::BSpline},
};
const ec::Variable kVariables[] = {ec::Variable::zero, ec::Variable::logdf, ec::Variable::forward};

// Knot sets. `nonuniform` is a desk-shaped grid (the spacing ratio runs 2:1 to 10:1, which is what
// separates the end-tangent and interior-knot rules); `uniform` is the control that tells a
// spacing-driven difference apart from a construction-driven one.
struct KnotSet {
  const char* name;
  std::vector<double> t;
};
const std::vector<KnotSet>& knot_sets() {
  static const std::vector<KnotSet> s = {
      {"nonuniform", {0.25, 0.5, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0}},
      {"uniform", {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0}},
  };
  return s;
}

// Knot-value profiles. `linear` is affine in t, which is the profile that must expose
// monotone_cubic's tangent difference as ZERO (both reduce to the line) and bspline's as nonzero.
enum class Profile { linear, hump, step, oscillate };
const char* name_of(Profile p) {
  switch (p) {
    case Profile::linear: return "linear";
    case Profile::hump: return "hump";
    case Profile::step: return "step";
    case Profile::oscillate: return "oscillate";
  }
  return "?";
}
double profile_at(Profile p, int k, double t) {
  switch (p) {
    case Profile::linear: return 0.020 + 0.0005 * t;
    case Profile::hump: return 0.030 + 0.015 * std::exp(-((t - 5.0) / 3.0) * ((t - 5.0) / 3.0));
    case Profile::step: return t <= 3.0 ? 0.020 : 0.050;
    case Profile::oscillate: return 0.030 + ((k % 2) == 0 ? 0.010 : -0.010);
  }
  return 0.0;
}
std::vector<double> profile_values(Profile p, const std::vector<double>& t) {
  std::vector<double> v(t.size());
  for (std::size_t k = 0; k < t.size(); ++k) v[k] = profile_at(p, static_cast<int>(k), t[k]);
  return v;
}

// The dense evaluation grid: before the first knot, several points strictly inside every knot
// interval, the knots themselves and just either side of them, and well beyond the last knot.
// Knots test the solve; the interior tests the interpolation; the two tails test extrapolation.
std::vector<double> dense_grid(const std::vector<double>& knots) {
  std::vector<double> g;
  const double t0 = knots.front(), tn = knots.back();
  for (int i = 0; i <= 8; ++i) g.push_back(t0 * static_cast<double>(i) / 9.0);  // [0, t0)
  for (std::size_t k = 0; k + 1 < knots.size(); ++k) {
    const double a = knots[k], b = knots[k + 1];
    g.push_back(a);
    for (int i = 1; i <= 16; ++i) g.push_back(a + (b - a) * static_cast<double>(i) / 17.0);
    g.push_back(std::nextafter(b, a));
  }
  g.push_back(tn);
  for (int i = 1; i <= 12; ++i) g.push_back(tn + static_cast<double>(i) * (tn / 6.0));  // beyond
  std::sort(g.begin(), g.end());
  g.erase(std::unique(g.begin(), g.end()), g.end());
  return g;
}

// ==============================================================================================
// Deviation, and its LOCALISATION
// ==============================================================================================

// What the shape of a deviation over t says about its cause. This is the brief's point 5 made
// mechanical: a number alone does not localise anything.
enum class Cause {
  identical,      // everywhere at or below the roundoff floor
  units,          // constant across the whole grid: a units or compounding difference
  time_measure,   // ~0 at small t and growing with t: a day-count or time-measure difference
  construction,   // confined to the knot range: the interpolation scheme itself
  extrapolation,  // confined to OUTSIDE the knot range
  mixed,          // nonzero both inside and outside, with no constant or growing signature
};
const char* name_of(Cause c) {
  switch (c) {
    case Cause::identical: return "IDENTICAL";
    case Cause::units: return "DEVIATES: units/compounding (constant over t)";
    case Cause::time_measure: return "DEVIATES: time measure (grows with t)";
    case Cause::construction: return "DEVIATES: scheme construction (inside the knot range only)";
    case Cause::extrapolation: return "DEVIATES: extrapolation (outside the knot range only)";
    case Cause::mixed: return "DEVIATES: mixed signature";
  }
  return "?";
}

struct Dev {
  double max_abs = 0.0;
  double max_rel = 0.0;
  double at_abs = 0.0;   // the t where max_abs occurs
  double at_rel = 0.0;
  int interval = -1;     // which knot interval holds max_abs (-1 below t_0, n-1 at/above t_{n-1})
  Cause cause = Cause::identical;
  bool valid = false;
  std::string note;
};

int interval_of(const std::vector<double>& knots, double t) {
  if (t < knots.front()) return -1;
  for (std::size_t k = 0; k + 1 < knots.size(); ++k) {
    if (t < knots[k + 1]) return static_cast<int>(k);
  }
  return static_cast<int>(knots.size()) - 1;
}

// `floor_rel` is the roundoff floor below which a relative deviation is the solve and not a
// finding. It is NEVER widened to make a cell pass: 1e-13 is ~450 ulp of a double, which bounds
// the two sides' different-but-algebraically-equal evaluation orders (a Thomas sweep scaled by 6,
// a Hermite basis against a monomial) and nothing larger.
constexpr double kFloorRel = 1e-13;

Dev compare(const std::vector<double>& knots, const std::vector<double>& grid,
            const std::vector<double>& a, const std::vector<double>& b) {
  Dev d;
  d.valid = true;
  double in_max = 0.0, out_max = 0.0;          // max |dev| inside / outside the knot range
  double rel_lo = 0.0, rel_hi = 0.0;           // the deviation's spread, for the `units` test
  double rel_small_t = 0.0, rel_large_t = 0.0; // for the `time_measure` test
  bool first = true;
  const double tn = knots.back();
  for (std::size_t i = 0; i < grid.size(); ++i) {
    const double t = grid[i];
    const double da = std::abs(a[i] - b[i]);
    const double scale = std::max({std::abs(a[i]), std::abs(b[i]), 1e-12});
    const double dr = da / scale;
    if (da > d.max_abs) { d.max_abs = da; d.at_abs = t; d.interval = interval_of(knots, t); }
    if (dr > d.max_rel) { d.max_rel = dr; d.at_rel = t; }
    if (t >= knots.front() && t <= tn) in_max = std::max(in_max, da); else out_max = std::max(out_max, da);
    const double sdev = a[i] - b[i];
    if (first) { rel_lo = rel_hi = sdev; first = false; }
    rel_lo = std::min(rel_lo, sdev);
    rel_hi = std::max(rel_hi, sdev);
    if (t <= 0.25 * tn) rel_small_t = std::max(rel_small_t, da);
    if (t >= 0.75 * tn) rel_large_t = std::max(rel_large_t, da);
  }
  if (d.max_rel <= kFloorRel) { d.cause = Cause::identical; return d; }
  const double span = rel_hi - rel_lo;
  const double level = std::max(std::abs(rel_hi), std::abs(rel_lo));
  if (level > 0.0 && span <= 1e-9 * level) {
    d.cause = Cause::units;  // the same signed offset everywhere
  } else if (out_max <= kFloorRel * 1e3 * std::max(1.0, in_max) && in_max > 0.0 && out_max < in_max * 1e-6) {
    d.cause = Cause::construction;
  } else if (in_max == 0.0 || in_max < out_max * 1e-6) {
    d.cause = Cause::extrapolation;
  } else if (rel_small_t < rel_large_t * 1e-3) {
    d.cause = Cause::time_measure;
  } else {
    d.cause = Cause::mixed;
  }
  return d;
}

void print_dev(const char* label, const Dev& d) {
  if (!d.valid) { std::printf("    %-26s NOT COMPARABLE — %s\n", label, d.note.c_str()); return; }
  std::printf("    %-26s max|dev| %.3e @ t=%-8.4g (interval %2d)   max rel %.3e @ t=%-8.4g   %s\n",
              label, d.max_abs, d.at_abs, d.interval, d.max_rel, d.at_rel, name_of(d.cause));
}

// ==============================================================================================
// The two engines, each behind the thinnest wrapper that its own public API allows
// ==============================================================================================

// Theirs: ONE region over the given knots, so `set_forwards` marks it LEADING (curve_module.hpp's
// `Boundary<S> b{}` has has_predecessor=false) and it flat-extrapolates its first free value
// backwards — the same convention every scheme of ours uses below its first knot.
class Theirs {
 public:
  Theirs(sc::Scheme s, const std::vector<double>& knots) {
    sc::CurveModule m;
    m.knots = knots;
    m.scheme = s;
    curve_.add(m);
  }
  void set(const std::vector<double>& x) { curve_.set_forwards(x); }
  int n() const { return curve_.n_knots(); }
  double fwd(double t) const { return curve_.forward(t); }
  double df(double t) const { return curve_.discount(t); }

 private:
  sc::ModularCurve<double> curve_;
};

// Ours, Layer A: the scheme as a bare interpolant, no variable attached.
class OurScheme {
 public:
  OurScheme(ec::SchemeKind k, const std::vector<double>& knots)
      : scheme_(ec::make_scheme(k, ec::Grid(knots))) {}
  double value(const std::vector<double>& v, double t) const {
    return std::visit([&](const auto& s) { return s.value(v.data(), t); }, scheme_);
  }

 private:
  ec::SchemeVariant scheme_;
};

// Ours, Layer B: one region [0, inf) with a scheme and a variable. `composite.hpp` documents a
// one-region Composite as the same bits as `Curve<S, V>`, and it is the only one of the two that
// reports the forward-variable restriction as a THROWN message instead of a static_assert, which
// is what lets this probe print the restriction rather than be unable to mention it.
class OurCurve {
 public:
  OurCurve(ec::SchemeKind k, ec::Variable var, const std::vector<double>& knots) {
    ec::RegionSpec sp;
    sp.t_a = 0.0;
    sp.t_b = std::numeric_limits<double>::infinity();
    sp.scheme = k;
    sp.variable = var;
    curve_ = ec::Composite(knots, {sp});
    var_ = var;
  }
  void set(const std::vector<double>& v) { st_ = curve_.prepare(v.data()); }
  double df(double t) const { return curve_.df(st_, t); }
  double log_df(double t) const { return curve_.log_df(st_, t); }
  // The region's own variable function at t (for Variable::forward this IS the instantaneous
  // forward; for logdf it is log DF; for zero the zero rate).
  double value(double t) const { return curve_.value_in_region(st_, 0, t); }
  ec::Variable variable() const { return var_; }

 private:
  ec::Composite curve_;
  ec::Composite::State<double> st_;
  ec::Variable var_ = ec::Variable::zero;
};

// ==============================================================================================
// LAYER A — do the six shared scheme NAMES share a CONSTRUCTION?
// ==============================================================================================

void layer_a() {
  std::printf("\n");
  std::printf("==================================================================================\n");
  std::printf("LAYER A — scheme-interpolant identity (variable-free)\n");
  std::printf("  ours:   epykos::curve::SchemeVariant::value(v, t)\n");
  std::printf("  theirs: swaps::curve::ModularCurve<double> with ONE leading region, forward(t)\n");
  std::printf("  Same knot times, same knot values. This layer answers: same NAME, same CONSTRUCTION?\n");
  std::printf("==================================================================================\n");
  for (const KnotSet& ks : knot_sets()) {
    const std::vector<double> grid = dense_grid(ks.t);
    std::printf("\n  knots = %s (%zu): ", ks.name, ks.t.size());
    for (double t : ks.t) std::printf("%g ", t);
    std::printf("\n");
    for (const SchemePair& sp : kShared) {
      std::printf("  %s\n", sp.name);
      for (Profile p : {Profile::linear, Profile::hump, Profile::step, Profile::oscillate}) {
        const std::vector<double> v = profile_values(p, ks.t);
        std::vector<double> ours(grid.size()), theirs(grid.size());
        try {
          OurScheme a(sp.ours, ks.t);
          Theirs b(sp.theirs, ks.t);
          b.set(v);
          for (std::size_t i = 0; i < grid.size(); ++i) {
            ours[i] = a.value(v, grid[i]);
            theirs[i] = b.fwd(grid[i]);
          }
        } catch (const std::exception& e) {
          Dev d;
          d.note = e.what();
          print_dev(name_of(p), d);
          continue;
        }
        print_dev(name_of(p), compare(ks.t, grid, ours, theirs));
      }
    }
  }
}

// ==============================================================================================
// LAYER B — whole-curve identity, and the (scheme x variable) matrix
// ==============================================================================================

void layer_b() {
  std::printf("\n");
  std::printf("==================================================================================\n");
  std::printf("LAYER B — whole-curve identity: DF and the instantaneous forward on a dense grid\n");
  std::printf("  Their interpolation variable is NOT configurable: x is always knot forwards\n");
  std::printf("  (regions.hpp intro; curve_module.hpp:224). Ours is zero | logdf | forward, and\n");
  std::printf("  Variable::forward is Flat/Linear ONLY (curve.hpp:91, composite.hpp:256).\n");
  std::printf("==================================================================================\n");
  const KnotSet& ks = knot_sets()[0];
  const std::vector<double> grid = dense_grid(ks.t);
  for (ec::Variable var : kVariables) {
    std::printf("\n  variable = %s\n", ec::to_string(var));
    for (const SchemePair& sp : kShared) {
      std::printf("   %-15s", sp.name);
      // Can we even build this cell?
      std::string why;
      try {
        OurCurve probe(sp.ours, var, ks.t);
        (void)probe;
      } catch (const std::exception& e) {
        why = e.what();
      }
      if (!why.empty()) {
        std::printf(" NOT COMPARABLE — not constructible on our side\n                     ours threw: %s\n", why.c_str());
        continue;
      }
      if (var != ec::Variable::forward) {
        std::printf(" NOT COMPARABLE — their curve has no '%s' interpolation variable\n", ec::to_string(var));
        continue;
      }
      std::printf("\n");
      for (Profile p : {Profile::linear, Profile::hump, Profile::step, Profile::oscillate}) {
        const std::vector<double> v = profile_values(p, ks.t);
        OurCurve a(sp.ours, var, ks.t);
        a.set(v);
        Theirs b(sp.theirs, ks.t);
        b.set(v);
        std::vector<double> adf(grid.size()), bdf(grid.size()), af(grid.size()), bf(grid.size());
        for (std::size_t i = 0; i < grid.size(); ++i) {
          adf[i] = a.df(grid[i]);
          bdf[i] = b.df(grid[i]);
          af[i] = a.value(grid[i]);
          bf[i] = b.fwd(grid[i]);
        }
        std::printf("    profile %s\n", name_of(p));
        print_dev("  DF(t)", compare(ks.t, grid, adf, bdf));
        print_dev("  inst. forward f(t)", compare(ks.t, grid, af, bf));
      }
    }
  }
}

// ==============================================================================================
// THE BRIDGE — the one cross-variable cell that is an exact identity
// ==============================================================================================
//
// `logdf`-linear has an instantaneous forward that is constant on (t_{k-1}, t_k] and equal to
// -(y_k - y_{k-1})/(t_k - t_{k-1}), with the structural origin knot y = 0 at t = 0. That is
// EXACTLY the interval their `Flat` anchors its value k to, which is also why their Flat is
// end-anchored. So their Flat with those derived forwards and our Curve<Linear, logdf> are the
// same curve on [0, t_{n-1}] — and must part company beyond it, since ours is flat in logdf (DF
// constant, forward zero) and theirs is flat in the forward.
void bridge() {
  std::printf("\n");
  std::printf("==================================================================================\n");
  std::printf("THE BRIDGE — ours (linear, logdf) against their Flat on the DERIVED knot forwards\n");
  std::printf("  x_k = -(y_k - y_{k-1}) / (t_k - t_{k-1}),  y_{-1} = 0 at t = 0 (our origin knot)\n");
  std::printf("==================================================================================\n");
  const KnotSet& ks = knot_sets()[0];
  const std::vector<double> grid = dense_grid(ks.t);
  for (Profile p : {Profile::linear, Profile::hump, Profile::step, Profile::oscillate}) {
    // Knot log-DFs from a zero-rate profile, so y is a plausible log DF and not an arbitrary level.
    std::vector<double> y(ks.t.size());
    for (std::size_t k = 0; k < ks.t.size(); ++k) y[k] = -profile_at(p, static_cast<int>(k), ks.t[k]) * ks.t[k];
    std::vector<double> x(ks.t.size());
    double yprev = 0.0, tprev = 0.0;
    for (std::size_t k = 0; k < ks.t.size(); ++k) {
      x[k] = -(y[k] - yprev) / (ks.t[k] - tprev);
      yprev = y[k];
      tprev = ks.t[k];
    }
    OurCurve a(ec::SchemeKind::linear, ec::Variable::logdf, ks.t);
    a.set(y);
    Theirs b(sc::Scheme::Flat, ks.t);
    b.set(x);
    std::vector<double> adf(grid.size()), bdf(grid.size());
    std::vector<double> ain, bin, gin;  // the knot range alone, to separate the two regimes
    for (std::size_t i = 0; i < grid.size(); ++i) {
      adf[i] = a.df(grid[i]);
      bdf[i] = b.df(grid[i]);
      if (grid[i] <= ks.t.back()) { ain.push_back(adf[i]); bin.push_back(bdf[i]); gin.push_back(grid[i]); }
    }
    std::printf("  profile %s\n", name_of(p));
    print_dev("DF, whole grid", compare(ks.t, grid, adf, bdf));
    print_dev("DF, [0, t_last] only", compare(ks.t, gin, ain, bin));
  }
}

// ==============================================================================================
// LAYER C — calibration identity
// ==============================================================================================
//
// The instrument set is CONVENTION-FREE on purpose: a curve-identity probe must not be able to
// blame a calendar. Fixed-vs-OIS par swaps on pure year fractions — no calendar, no business-day
// rule, no day-count fraction. The time measure on both sides is the same year fraction t that
// indexes the knots, and nothing else. Single-curve OIS, so the float leg telescopes exactly:
//
//   R_i = (1 - DF(T_i)) / sum_j delta_j DF(T_j),  T_j the annual grid up to T_i, delta_j its gaps.
//
// One instrument per knot, maturities AT the knot times => square, and the intermediate payment
// dates make the residual depend on the curve BETWEEN knots, which is the point of the probe.
//
// The residual and the solver below are written ONCE and run against each engine's own curve, so
// any difference in the root is the curve and not the solve.

struct Instrument {
  double maturity = 0.0;
  std::vector<double> pay;   // payment times, ascending, last == maturity
  std::vector<double> delta; // accruals, delta[j] = pay[j] - pay[j-1], pay[-1] = 0
  double quote = 0.0;
};

std::vector<Instrument> instruments(const std::vector<double>& knots) {
  std::vector<Instrument> ins;
  for (double T : knots) {
    Instrument in;
    in.maturity = T;
    for (double y = 1.0; y < T - 1e-12; y += 1.0) in.pay.push_back(y);
    in.pay.push_back(T);
    double prev = 0.0;
    for (double t : in.pay) { in.delta.push_back(t - prev); prev = t; }
    ins.push_back(in);
  }
  return ins;
}

// The generating curve the quotes come from: the shape docs/WORKLOADS.md's seed uses (a short end
// reverting to a long end over ~3y). Nothing is solved for it; it only makes the quotes realistic
// and the root a non-trivial one that both engines must find.
double generating_df(double t) {
  const double zs = 0.0380, zl = 0.0430, rev = 3.0;
  const double z = zl + (zs - zl) * std::exp(-t / rev);
  return std::exp(-z * t);
}

template <class Curve>
double par_rate(const Curve& c, const Instrument& in) {
  double ann = 0.0;
  for (std::size_t j = 0; j < in.pay.size(); ++j) ann += in.delta[j] * c.df(in.pay[j]);
  return (1.0 - c.df(in.maturity)) / ann;
}

// A damped Newton with a central-difference Jacobian. Identical code for both engines; it is the
// control. Returns ||F||inf at the point returned, and `iters`; it never widens its tolerance.
struct SolveResult {
  std::vector<double> x;
  double resid = 0.0;
  int iters = 0;
  bool converged = false;
  bool singular = false;
  bool stalled = false;             // a Newton step existed but no damping reduced ||F||
  std::vector<int> dead_columns;    // knots NO instrument's residual depends on: d F / d x_j == 0
};

template <class MakeCurve>
SolveResult solve(MakeCurve make, const std::vector<Instrument>& ins, const std::vector<double>& x0,
                  double tol, int max_iter) {
  const int n = static_cast<int>(x0.size());
  SolveResult r;
  r.x = x0;
  auto residual = [&](const std::vector<double>& x) {
    auto c = make(x);
    std::vector<double> F(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) F[static_cast<std::size_t>(i)] = par_rate(c, ins[static_cast<std::size_t>(i)]) - ins[static_cast<std::size_t>(i)].quote;
    return F;
  };
  auto norm = [](const std::vector<double>& F) {
    double m = 0.0;
    for (double f : F) m = std::max(m, std::abs(f));
    return m;
  };
  std::vector<double> F = residual(r.x);
  r.resid = norm(F);
  for (r.iters = 0; r.iters < max_iter && r.resid > tol; ++r.iters) {
    // Central-difference Jacobian, row-major n x n.
    std::vector<double> J(static_cast<std::size_t>(n) * static_cast<std::size_t>(n), 0.0);
    for (int j = 0; j < n; ++j) {
      const double h = 1e-7 * std::max(1.0, std::abs(r.x[static_cast<std::size_t>(j)]));
      std::vector<double> xp = r.x, xm = r.x;
      xp[static_cast<std::size_t>(j)] += h;
      xm[static_cast<std::size_t>(j)] -= h;
      const std::vector<double> Fp = residual(xp), Fm = residual(xm);
      for (int i = 0; i < n; ++i) J[static_cast<std::size_t>(i) * static_cast<std::size_t>(n) + static_cast<std::size_t>(j)] = (Fp[static_cast<std::size_t>(i)] - Fm[static_cast<std::size_t>(i)]) / (2.0 * h);
    }
    // STRUCTURAL singularity: a column of exact zeros means NO instrument's residual depends on
    // that knot at all, so the system is not square in substance however square it looks. This is
    // a property of the curve, not of the solve, which is why the probe reports WHICH knot.
    if (r.iters == 0) {
      for (int j = 0; j < n; ++j) {
        bool dead = true;
        for (int i = 0; i < n && dead; ++i) {
          if (J[static_cast<std::size_t>(i) * static_cast<std::size_t>(n) + static_cast<std::size_t>(j)] != 0.0) dead = false;
        }
        if (dead) r.dead_columns.push_back(j);
      }
    }
    // Gaussian elimination with partial pivoting on J dx = -F.
    std::vector<double> A = J, b(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) b[static_cast<std::size_t>(i)] = -F[static_cast<std::size_t>(i)];
    bool singular = false;
    for (int k = 0; k < n && !singular; ++k) {
      int piv = k;
      for (int i = k + 1; i < n; ++i) {
        if (std::abs(A[static_cast<std::size_t>(i) * n + k]) > std::abs(A[static_cast<std::size_t>(piv) * n + k])) piv = i;
      }
      if (std::abs(A[static_cast<std::size_t>(piv) * n + k]) < 1e-300) { singular = true; break; }
      if (piv != k) {
        for (int j = 0; j < n; ++j) std::swap(A[static_cast<std::size_t>(k) * n + j], A[static_cast<std::size_t>(piv) * n + j]);
        std::swap(b[static_cast<std::size_t>(k)], b[static_cast<std::size_t>(piv)]);
      }
      for (int i = k + 1; i < n; ++i) {
        const double f = A[static_cast<std::size_t>(i) * n + k] / A[static_cast<std::size_t>(k) * n + k];
        if (f == 0.0) continue;
        for (int j = k; j < n; ++j) A[static_cast<std::size_t>(i) * n + j] -= f * A[static_cast<std::size_t>(k) * n + j];
        b[static_cast<std::size_t>(i)] -= f * b[static_cast<std::size_t>(k)];
      }
    }
    if (singular) { r.singular = true; break; }
    std::vector<double> dx(static_cast<std::size_t>(n), 0.0);
    for (int i = n - 1; i >= 0; --i) {
      double s = b[static_cast<std::size_t>(i)];
      for (int j = i + 1; j < n; ++j) s -= A[static_cast<std::size_t>(i) * n + j] * dx[static_cast<std::size_t>(j)];
      dx[static_cast<std::size_t>(i)] = s / A[static_cast<std::size_t>(i) * n + i];
    }
    // Backtracking line search on ||F||inf.
    double step = 1.0;
    bool improved = false;
    for (int t = 0; t < 30; ++t) {
      std::vector<double> xt = r.x;
      for (int i = 0; i < n; ++i) xt[static_cast<std::size_t>(i)] += step * dx[static_cast<std::size_t>(i)];
      const std::vector<double> Ft = residual(xt);
      const double nt = norm(Ft);
      if (nt < r.resid) { r.x = xt; F = Ft; r.resid = nt; improved = true; break; }
      step *= 0.5;
    }
    if (!improved) { r.stalled = true; break; }
  }
  r.converged = r.resid <= tol;
  return r;
}

void report_solve(const char* who, const std::vector<double>& knots, const SolveResult& r) {
  std::printf("    %-6s ||F||inf %.3e after %3d iters  %s%s%s\n", who, r.resid, r.iters,
              r.converged ? "CONVERGED" : "DID NOT CONVERGE", r.singular ? " (SINGULAR Jacobian)" : "",
              (!r.converged && r.stalled) ? " (stalled: no damped step reduced ||F||)" : "");
  if (!r.dead_columns.empty()) {
    std::printf("           STRUCTURALLY SINGULAR: no instrument's residual depends on knot(s)");
    for (int j : r.dead_columns) std::printf(" %d (t=%g)", j, knots[static_cast<std::size_t>(j)]);
    std::printf("\n           -> the system is square by count but not in substance; dF/dx has a zero column.\n");
  }
}

void layer_c() {
  std::printf("\n");
  std::printf("==================================================================================\n");
  std::printf("LAYER C — calibration identity: the same square, convention-free instrument set\n");
  std::printf("  R_i = (1 - DF(T_i)) / sum_j delta_j DF(T_j); annual pays, pure year fractions.\n");
  std::printf("  One instrument per knot, maturities AT the knots => square, unique root.\n");
  std::printf("  The residual AND the damped Newton are written once in this file and run against\n");
  std::printf("  each engine's own curve, so the solver is a CONTROL: a differing root is a\n");
  std::printf("  differing curve.\n");
  std::printf("==================================================================================\n");
  const KnotSet& ks = knot_sets()[0];
  const std::vector<double> grid = dense_grid(ks.t);
  std::vector<Instrument> ins = instruments(ks.t);
  // Quotes from the generating curve, through the same par-rate formula.
  struct GenCurve { double df(double t) const { return generating_df(t); } } gen;
  std::printf("\n  instruments (maturity, pays, quote from the generating curve):\n");
  for (Instrument& in : ins) {
    in.quote = par_rate(gen, in);
    std::printf("    T=%-6.4g  %2zu pays   quote %.10f\n", in.maturity, in.pay.size(), in.quote);
  }
  const double tol = 1e-15;
  const int max_iter = 200;
  const std::vector<double> x0(ks.t.size(), 0.03);  // the same flat start on both sides

  for (const SchemePair& sp : kShared) {
    std::printf("\n  scheme %s on variable forward\n", sp.name);
    std::string why;
    try {
      OurCurve probe(sp.ours, ec::Variable::forward, ks.t);
      (void)probe;
    } catch (const std::exception& e) {
      why = e.what();
    }
    if (!why.empty()) {
      std::printf("    NOT COMPARABLE — ours cannot calibrate this cell at all.\n");
      std::printf("    ours threw: %s\n", why.c_str());
      continue;
    }
    const SolveResult ra = solve(
        [&](const std::vector<double>& x) {
          OurCurve c(sp.ours, ec::Variable::forward, ks.t);
          c.set(x);
          return c;
        },
        ins, x0, tol, max_iter);
    const SolveResult rb = solve(
        [&](const std::vector<double>& x) {
          Theirs c(sp.theirs, ks.t);
          c.set(x);
          return c;
        },
        ins, x0, tol, max_iter);
    report_solve("ours", ks.t, ra);
    report_solve("theirs", ks.t, rb);
    double kmax = 0.0, kmaxrel = 0.0;
    int kat = -1;
    for (std::size_t k = 0; k < ra.x.size(); ++k) {
      const double d = std::abs(ra.x[k] - rb.x[k]);
      const double rel = d / std::max({std::abs(ra.x[k]), std::abs(rb.x[k]), 1e-12});
      if (d > kmax) { kmax = d; kat = static_cast<int>(k); }
      kmaxrel = std::max(kmaxrel, rel);
    }
    std::printf("    calibrated knot values: max|dev| %.3e at knot %d (t=%g), max rel %.3e\n",
                kmax, kat, kat >= 0 ? ks.t[static_cast<std::size_t>(kat)] : 0.0, kmaxrel);
    if (kmaxrel <= kFloorRel) std::printf("      -> IDENTICAL to solver tolerance\n");
    // And the curve at the two roots, which is the real question.
    OurCurve a(sp.ours, ec::Variable::forward, ks.t);
    a.set(ra.x);
    Theirs b(sp.theirs, ks.t);
    b.set(rb.x);
    std::vector<double> adf(grid.size()), bdf(grid.size()), af(grid.size()), bf(grid.size());
    for (std::size_t i = 0; i < grid.size(); ++i) {
      adf[i] = a.df(grid[i]);
      bdf[i] = b.df(grid[i]);
      af[i] = a.value(grid[i]);
      bf[i] = b.fwd(grid[i]);
    }
    print_dev("DF at the two roots", compare(ks.t, grid, adf, bdf));
    print_dev("f(t) at the two roots", compare(ks.t, grid, af, bf));
  }
}

// ==============================================================================================
// What "identically" meant, stated rather than claimed
// ==============================================================================================

void configuration() {
  std::printf("==================================================================================\n");
  std::printf("tools/curveid — THE CURVE-IDENTITY PROBE\n");
  std::printf("  pre-registration: bench/compare/CURVEID.md (committed BEFORE this binary existed)\n");
  std::printf("  epykos %s\n", epykos::version());
  std::printf("==================================================================================\n");
  std::printf("\nWHAT WAS CONFIGURED IDENTICALLY, AND WHAT HAS NO COUNTERPART\n");
  std::printf("  knot times        shared, exactly; both sides take year fractions, ascending,\n");
  std::printf("                    strictly increasing. No conversion on either side.\n");
  std::printf("  time measure      the year fraction t itself, on both sides. The instrument set of\n");
  std::printf("                    Layer C is convention-free by construction: no calendar, no\n");
  std::printf("                    business-day rule, no day-count fraction, so there is no time\n");
  std::printf("                    measure left to differ.\n");
  std::printf("  conventions       NONE, deliberately (see above). The probe is about the curve.\n");
  std::printf("  instruments       Layer C: fixed-vs-OIS par swaps, annual pays, maturities at the\n");
  std::printf("                    knots, one per knot. Identical definition evaluated against both\n");
  std::printf("                    curves by the same code in this file.\n");
  std::printf("  region layout     ONE region covering the whole axis on both sides, so their\n");
  std::printf("                    region is LEADING (no predecessor) and no C0 join is in play.\n");
  std::printf("\n  NO COUNTERPART — the interpolation VARIABLE:\n");
  std::printf("    ours   curve::Variable { zero, logdf, forward }, chosen at construction;\n");
  std::printf("           Variable::forward is Flat/Linear ONLY (curve.hpp:91 static_assert,\n");
  std::printf("           composite.hpp:256 throw) because the closed-form integral is written\n");
  std::printf("           for those two schemes alone.\n");
  std::printf("    theirs NOT CONFIGURABLE. x is always the knot forwards (regions.hpp intro,\n");
  std::printf("           curve_module.hpp:224) and all 8 of their schemes run on it.\n");
  std::printf("    => (zero, *) and (logdf, *) have no counterpart on their side; of the six\n");
  std::printf("       shared names, only flat and linear have a comparable whole-curve cell.\n");
  std::printf("       This is printed per cell in Layer B, never silently substituted.\n");
  std::printf("\n  NO COUNTERPART — the schemes:\n");
  std::printf("    theirs-only  Scheme::Tension (a hyperparameter sigma travels with it) and\n");
  std::printf("                 Scheme::MonotoneConvex (Hagan-West). Neither exists here.\n");
  std::printf("    ours-only    none; our six are a subset of their eight by NAME.\n");
  std::printf("\n  NO COUNTERPART — settings their CurveModule carries that ours has no field for:\n");
  std::printf("    sigma        the Tension hyperparameter (unused by the six shared schemes).\n");
  std::printf("    reg_lambda   a per-region curvature-penalty weight. Our calibration has no\n");
  std::printf("                 regulariser at all; Layer C is square, so none is needed and none\n");
  std::printf("                 is configured on their side either (left at its default).\n");
  std::printf("\n  ROUNDOFF FLOOR  relative deviation <= %.0e is reported IDENTICAL. It is never\n", kFloorRel);
  std::printf("                  widened to make a cell pass; a deviation above it is reported with\n");
  std::printf("                  its localisation.\n");
}

}  // namespace

int main(int argc, char** argv) {
  bool only_a = false, only_b = false, only_c = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--layer-a") only_a = true;
    else if (a == "--layer-b") only_b = true;
    else if (a == "--layer-c") only_c = true;
    else if (a == "--help") {
      std::printf("usage: curveid [--layer-a] [--layer-b] [--layer-c]   (default: all)\n");
      return 0;
    }
  }
  const bool all = !only_a && !only_b && !only_c;
  configuration();
  if (all || only_a) layer_a();
  if (all || only_b) { layer_b(); bridge(); }
  if (all || only_c) layer_c();
  std::printf("\n");
  return 0;
}
