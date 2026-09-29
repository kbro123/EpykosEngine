// EpykosEngine — interpreter kernels instantiated for lane width 4.
// An E0 TU (src/**/*_e0.cpp, root CMakeLists.txt): compiled with -ffp-contract=off in every
// preset on every compiler, so an Affine fold's product and sum round separately, as in
// tape/replay.hpp, whatever the compiler's contraction default (GCC contracts in C++ even in
// ISO mode; clang within a statement). D13, D25.
#ifndef EPYKOS_FP_CONTRACT_OFF
#error "kernels_l*_e0.cpp must be compiled with -ffp-contract=off (see the *_e0.cpp rule in CMakeLists.txt)"
#endif

#include "kernels_impl.hpp"

namespace epykos::exec::detail {
template struct KernelTable<4>;
}  // namespace epykos::exec::detail
