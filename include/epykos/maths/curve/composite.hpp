// EpykosEngine — the composite (region) curve (docs/WORKLOADS.md §M4 "Composite (region) curve",
// docs/PROBLEM.md §3; M3/G1): an ordered list of regions [t_a, t_b), each with its own scheme and
// variable, the knots partitioned by region, DF value-continuous where regions meet. Region lookup
// is structure (class A of DESIGN.md §8): which region prices a time is decided on double t at
// record time and folded away; the runtime scheme choice (a std::variant visited per region) is
// structure too — a curve definition of blueprints/ (D36) maps onto RegionSpecs.
//
// A single-scheme curve is a Composite with one region [0, ∞): the same bits as Curve<S, V>
// (curve.hpp; tests/curve/ asserts it for every scheme and variable).
//
// Continuity mechanism ("the knot on the boundary owns the boundary value"). At the boundary
// t_b between regions r and r + 1:
//   * if region r + 1's first knot sits at t_b (and r + 1 is not a forward region), region r
//     gets a RIGHT ANCHOR: an extra knot at t_b whose value is that first knot converted from
//     r + 1's variable into r's (curve.hpp detail::convert: identity when the variables agree,
//     otherwise the affine conversion at the fixed time t_b with a structural coefficient). Region
//     r then interpolates from its last knot to the anchor and its scheme returns the anchor
//     value exactly at t_b, so DF(t_b⁻) = exp(to_logdf(anchor)) and DF(t_b) = exp(to_logdf(knot))
//     agree bitwise when the variables agree and to one or two roundings otherwise;
//   * otherwise region r extrapolates flat to t_b and region r + 1 gets a LEFT ANCHOR at t_b: its
//     own variable's value for region r's log DF at t_b (a Scalar of region r's state), an extra
//     first knot it interpolates from;
//   * a FORWARD region carries no level of its own: its log DF is region r − 1's log DF at t_a
//     minus ∫_{t_a}^t f (flat beyond its last knot). A forward region is therefore continuous on
//     the left by construction and is accepted as the LAST region only (a following region would
//     have to inherit its level too, which would take the meaning out of that region's knots) —
//     a stated restriction of this milestone; and its scheme must be Flat or Linear;
//   * the first region starts at t_a = 0 with the level DF(0) = 1 (zero: exp(−z·0); logdf: the
//     origin knot (0, 0) of curve.hpp when its first knot is at t > 0; forward: ∫_0^0 = 0).
// Regions must be contiguous ([0, t_1), [t_1, t_2), ..., [t_m, ∞): the last region's t_b is
// taken as +∞ whatever is given) and every region must own at least one knot; a zero/logdf
// region's left anchor needs t_a > 0 (true for r >= 1).
//
// Interface (Scalar = double | Rec | Dual<N>):
//   Composite c(knot_t, regions);   auto st = c.prepare(v);   c.log_df(st, t);   c.df(st, t);
//   c.df(v, t);   c.region_of(t);   c.log_df_in_region(st, r, t)  (region r's own function, t
//   may equal t_b: the left limit — the continuity tests use it).
#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "epykos/maths/curve/curve.hpp"
#include "epykos/maths/curve/scheme.hpp"

namespace epykos::curve {

enum class SchemeKind : int { flat = 0, linear = 1, hermite = 2, natural_cubic = 3, monotone_cubic = 4, bspline = 5 };

inline const char* to_string(SchemeKind k) noexcept {
  switch (k) {
    case SchemeKind::flat: return Flat::name();
    case SchemeKind::linear: return Linear::name();
    case SchemeKind::hermite: return Hermite::name();
    case SchemeKind::natural_cubic: return NaturalCubic::name();
    case SchemeKind::monotone_cubic: return MonotoneCubic::name();
    case SchemeKind::bspline: return BSpline::name();
  }
  return "?";
}
// The kind named `name` ("flat", "linear", "hermite", "natural_cubic", "monotone_cubic",
// "bspline"); throws std::invalid_argument otherwise.
inline SchemeKind parse_scheme(std::string_view name) {
  for (int k = 0; k <= static_cast<int>(SchemeKind::bspline); ++k) {
    if (name == to_string(static_cast<SchemeKind>(k))) return static_cast<SchemeKind>(k);
  }
  throw std::invalid_argument("curve::parse_scheme: unknown scheme '" + std::string(name) + "'");
}
inline Variable parse_variable(std::string_view name) {
  for (int v = 0; v <= static_cast<int>(Variable::forward); ++v) {
    if (name == to_string(static_cast<Variable>(v))) return static_cast<Variable>(v);
  }
  throw std::invalid_argument("curve::parse_variable: unknown variable '" + std::string(name) + "'");
}

using SchemeVariant = std::variant<Flat, Linear, Hermite, NaturalCubic, MonotoneCubic, BSpline>;

inline SchemeVariant make_scheme(SchemeKind kind, Grid grid) {
  switch (kind) {
    case SchemeKind::flat: return Flat(std::move(grid));
    case SchemeKind::linear: return Linear(std::move(grid));
    case SchemeKind::hermite: return Hermite(std::move(grid));
    case SchemeKind::natural_cubic: return NaturalCubic(std::move(grid));
    case SchemeKind::monotone_cubic: return MonotoneCubic(std::move(grid));
    case SchemeKind::bspline: return BSpline(std::move(grid));
  }
  throw std::invalid_argument("curve::make_scheme: bad kind");
}

struct RegionSpec {
  double t_a = 0.0;
  double t_b = std::numeric_limits<double>::infinity();
  SchemeKind scheme = SchemeKind::linear;
  Variable variable = Variable::zero;
};

class Composite {
 public:
  struct Region {
    RegionSpec spec;
    int first = 0;             // first state knot of the region
    int count = 0;             // number of state knots
    bool origin = false;       // logdf region 0 with its first knot at t > 0: the (0, 0) knot in front
    bool left_anchor = false;  // an extra first knot at t_a from the previous region's log DF
    bool right_anchor = false; // an extra last knot at t_b from the next region's first knot
    std::vector<double> grid;  // the scheme's knot times: [origin | t_a] + state knots + [t_b]
    SchemeVariant scheme;
    bool forward() const noexcept { return spec.variable == Variable::forward; }
  };

  Composite() = default;
  // Throws std::invalid_argument on an invalid definition (see the header comment).
  Composite(std::vector<double> knot_t, std::vector<RegionSpec> regions) : knot_t_(std::move(knot_t)) { build(std::move(regions)); }
  Composite(const double* knot_t, int n, std::vector<RegionSpec> regions)
      : Composite(std::vector<double>(knot_t, knot_t + (n > 0 ? n : 0)), std::move(regions)) {}

  int n() const noexcept { return static_cast<int>(knot_t_.size()); }
  const std::vector<double>& knot_times() const noexcept { return knot_t_; }
  int n_regions() const noexcept { return static_cast<int>(regions_.size()); }
  const Region& region(int r) const noexcept { return regions_[static_cast<std::size_t>(r)]; }
  // The region pricing time t: t_a <= t < t_b; region 0 for t < 0 (flat extrapolation).
  int region_of(double t) const noexcept {
    int r = 0;
    while (r + 1 < n_regions() && t >= regions_[static_cast<std::size_t>(r)].spec.t_b) ++r;
    return r;
  }

  template <class Scalar>
  struct RegionState {
    std::vector<Scalar> v;     // the scheme's knot values (anchors included)
    std::vector<Scalar> coef;  // the scheme's prepared coefficients
    Scalar level = Scalar(0.0);  // forward regions r >= 1: log DF at t_a (inherited)
  };
  template <class Scalar>
  struct State {
    std::vector<RegionState<Scalar>> regions;
  };

  template <class Scalar>
  State<Scalar> prepare(const Scalar* v) const {
    State<Scalar> st;
    st.regions.resize(regions_.size());
    for (int r = 0; r < n_regions(); ++r) {
      const Region& reg = regions_[static_cast<std::size_t>(r)];
      RegionState<Scalar>& rs = st.regions[static_cast<std::size_t>(r)];
      rs.v.reserve(reg.grid.size());
      if (reg.origin) rs.v.push_back(Scalar(0.0));
      if (reg.left_anchor) {
        const Scalar y = log_df_in_region(st, r - 1, reg.spec.t_a);
        rs.v.push_back(detail::from_logdf(reg.spec.variable, y, reg.spec.t_a));
      }
      for (int k = 0; k < reg.count; ++k) rs.v.push_back(v[reg.first + k]);
      if (reg.right_anchor) {
        const Region& next = regions_[static_cast<std::size_t>(r) + 1];
        rs.v.push_back(detail::convert(next.spec.variable, reg.spec.variable, v[next.first], reg.spec.t_b));
      }
      if (reg.forward() && r > 0) rs.level = log_df_in_region(st, r - 1, reg.spec.t_a);
      rs.coef = std::visit([&](const auto& s) { return s.prepare(rs.v.data()); }, reg.scheme);
    }
    return st;
  }

  // Region r's own log DF at t (t in [t_a, t_b]; t = t_b is the left limit).
  template <class Scalar>
  Scalar log_df_in_region(const State<Scalar>& st, int r, double t) const {
    const Region& reg = regions_[static_cast<std::size_t>(r)];
    const RegionState<Scalar>& rs = st.regions[static_cast<std::size_t>(r)];
    switch (reg.spec.variable) {
      case Variable::zero: {
        const Scalar zt = std::visit([&](const auto& s) { return s.value(rs.v.data(), rs.coef, t); }, reg.scheme);
        return -zt * t;
      }
      case Variable::logdf:
        return std::visit([&](const auto& s) { return s.value(rs.v.data(), rs.coef, t); }, reg.scheme);
      case Variable::forward: {
        const Scalar I = std::visit(
            [&](const auto& s) -> Scalar {
              using S = std::decay_t<decltype(s)>;
              if constexpr (scheme_has_integral<S>) {
                return s.integral(rs.v.data(), reg.spec.t_a, t);
              } else {
                throw std::logic_error("Composite: a forward region must use Flat or Linear");
              }
            },
            reg.scheme);
        if (r == 0) return -I;
        return rs.level - I;
      }
    }
    throw std::logic_error("Composite: bad variable");
  }
  // The variable's function of region r at t (its value in the region's own variable).
  template <class Scalar>
  Scalar value_in_region(const State<Scalar>& st, int r, double t) const {
    const Region& reg = regions_[static_cast<std::size_t>(r)];
    const RegionState<Scalar>& rs = st.regions[static_cast<std::size_t>(r)];
    return std::visit([&](const auto& s) { return s.value(rs.v.data(), rs.coef, t); }, reg.scheme);
  }

  template <class Scalar>
  Scalar log_df(const State<Scalar>& st, double t) const {
    return log_df_in_region(st, region_of(t), t);
  }
  template <class Scalar>
  Scalar df(const State<Scalar>& st, double t) const {
    using std::exp;
    const Scalar y = log_df(st, t);
    return exp(y);
  }
  template <class Scalar>
  Scalar df(const Scalar* v, double t) const {
    return df(prepare(v), t);
  }

  std::string describe() const {
    std::string s;
    for (int r = 0; r < n_regions(); ++r) {
      const Region& reg = regions_[static_cast<std::size_t>(r)];
      s += "region " + std::to_string(r) + " [" + std::to_string(reg.spec.t_a) + ", " + std::to_string(reg.spec.t_b) + ") " +
           to_string(reg.spec.scheme) + " on " + to_string(reg.spec.variable) + ": knots " + std::to_string(reg.first) + ".." +
           std::to_string(reg.first + reg.count - 1) + (reg.origin ? ", origin knot" : "") +
           (reg.left_anchor ? ", left anchor" : "") + (reg.right_anchor ? ", right anchor" : "") + "\n";
    }
    return s;
  }

 private:
  void build(std::vector<RegionSpec> specs) {
    const int n = this->n();
    if (n < 1) throw std::invalid_argument("Composite: at least one knot");
    for (int k = 0; k < n; ++k) {
      const double t = knot_t_[static_cast<std::size_t>(k)];
      if (!std::isfinite(t) || t < 0.0) throw std::invalid_argument("Composite: knot times must be finite and >= 0");
      if (k > 0 && !(t > knot_t_[static_cast<std::size_t>(k) - 1])) throw std::invalid_argument("Composite: knot times must be strictly increasing");
    }
    if (specs.empty()) throw std::invalid_argument("Composite: at least one region");
    specs.back().t_b = std::numeric_limits<double>::infinity();
    if (specs.front().t_a != 0.0) throw std::invalid_argument("Composite: the first region must start at t = 0");
    for (std::size_t r = 0; r < specs.size(); ++r) {
      const RegionSpec& sp = specs[r];
      if (!(sp.t_a < sp.t_b)) throw std::invalid_argument("Composite: region " + std::to_string(r) + " must have t_a < t_b");
      if (r + 1 < specs.size() && specs[r + 1].t_a != sp.t_b) {
        throw std::invalid_argument("Composite: regions must be contiguous (region " + std::to_string(r) + ")");
      }
      if (sp.variable == Variable::forward) {
        if (r + 1 != specs.size()) throw std::invalid_argument("Composite: a forward region is accepted as the last region only");
        if (sp.scheme != SchemeKind::flat && sp.scheme != SchemeKind::linear) {
          throw std::invalid_argument("Composite: a forward region must use the flat or the linear scheme");
        }
      }
    }
    regions_.clear();
    int k = 0;
    for (std::size_t r = 0; r < specs.size(); ++r) {
      Region reg;
      reg.spec = specs[r];
      reg.first = k;
      while (k < n && knot_t_[static_cast<std::size_t>(k)] < reg.spec.t_b) ++k;
      reg.count = k - reg.first;
      if (reg.count < 1) throw std::invalid_argument("Composite: region " + std::to_string(r) + " owns no knot");
      regions_.push_back(std::move(reg));
    }
    for (std::size_t r = 0; r < regions_.size(); ++r) {
      Region& reg = regions_[r];
      const double first_t = knot_t_[static_cast<std::size_t>(reg.first)];
      if (r == 0) {
        reg.origin = reg.spec.variable == Variable::logdf && first_t > 0.0;
      } else if (!reg.forward()) {
        reg.left_anchor = first_t > reg.spec.t_a;
      }
      if (r + 1 < regions_.size() && !reg.forward()) {
        const Region& next = regions_[r + 1];
        const double next_first_t = knot_t_[static_cast<std::size_t>(next.first)];
        reg.right_anchor = !next.forward() && next_first_t == reg.spec.t_b;
      }
      reg.grid.clear();
      if (reg.origin) reg.grid.push_back(0.0);
      if (reg.left_anchor) reg.grid.push_back(reg.spec.t_a);
      for (int j = 0; j < reg.count; ++j) reg.grid.push_back(knot_t_[static_cast<std::size_t>(reg.first + j)]);
      if (reg.right_anchor) reg.grid.push_back(reg.spec.t_b);
      reg.scheme = make_scheme(reg.spec.scheme, Grid(reg.grid));
    }
  }

  std::vector<double> knot_t_;
  std::vector<Region> regions_;
};

}  // namespace epykos::curve
