// EpykosEngine — interpreter kernels instantiated for lane width 1.
// Contraction is off in this TU whatever the preset (E0: an Affine fold's product and sum round
// separately, as in tape/replay.hpp; GCC in ISO mode never contracts unless asked, the pragma
// covers clang).
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include "kernels_impl.hpp"

namespace epykos::exec::detail {
template struct KernelTable<1>;
}  // namespace epykos::exec::detail
