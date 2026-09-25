// EpykosEngine — the `double` side of the Scalar vocabulary.
//
// Templated maths is written once on `Scalar` and must use this vocabulary instead of raw C++
// branches (CLAUDE.md recording discipline):
//   select(c, a, b)      value branch, both arms computed        (Rec: records Select)
//   structural_if(c)     a branch that must not depend on inputs (Rec: throws if tainted)
//   max/min/abs          via select                              (Rec: records Select)
//   exp/log/sqrt/fma     unqualified, found by ADL or `using std::exp;`
// These are the `double` instantiations. scalar/rec.hpp provides the `Rec` ones and scalar/dual.hpp
// the `Dual<N>` ones.
#pragma once

#include <cmath>

namespace epykos {

inline double select(bool c, double a, double b) noexcept { return c ? a : b; }
inline bool structural_if(bool c) noexcept { return c; }
// Same value semantics as the Rec overloads: max(a,b) = a < b ? b : a, min(a,b) = b < a ? b : a,
// abs(a) = a < 0 ? -a : a (abs(-0.0) is -0.0, unlike std::fabs).
inline double max(double a, double b) noexcept { return a < b ? b : a; }
inline double min(double a, double b) noexcept { return b < a ? b : a; }
inline double abs(double a) noexcept { return a < 0.0 ? -a : a; }
inline double recip(double a) noexcept { return 1.0 / a; }

using std::exp;
using std::fma;
using std::log;
using std::sqrt;

}  // namespace epykos
