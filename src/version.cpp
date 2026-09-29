#include "epykos/version.hpp"

#ifndef EPYKOS_BUILD_CONFIG
#define EPYKOS_BUILD_CONFIG "unknown"
#endif
#ifndef EPYKOS_BUILD_FLAGS
#define EPYKOS_BUILD_FLAGS "unknown"
#endif

namespace epykos {

const char* version() noexcept { return "0.1.0"; }

const char* build_config() noexcept { return EPYKOS_BUILD_CONFIG; }

const char* build_flags() noexcept { return EPYKOS_BUILD_FLAGS; }

bool build_fp_contract_off() noexcept {
#ifdef EPYKOS_FP_CONTRACT_OFF
  return true;
#else
  return false;
#endif
}

}  // namespace epykos
