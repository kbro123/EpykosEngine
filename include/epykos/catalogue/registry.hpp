// EpykosEngine — the catalogue's kernel registry (M4/C1; PROBLEM.md §7).
//
// `src/catalogue/generated/registry.cpp` is the generator's own output (tools/catalogue/,
// scripts/catalogue_regen.sh): one RegistryEntry per distinct Signature the reference workloads
// (the Stage A tape and the M1 book, run through record -> ir::infer -> a rewrite pipeline) hit,
// sorted by Signature::hash() ascending, so lookup() is a binary search followed by a full
// Signature comparison over the (practically always singleton) run of same-hash entries — a hash
// collision never dispatches the wrong kernel (catalogue.hash_collision_returns_wrong_kernel).
#pragma once

#include <cstddef>
#include <vector>

#include "epykos/catalogue/kernel.hpp"
#include "epykos/catalogue/signature.hpp"

namespace epykos::catalogue {

struct RegistryEntry {
  Signature signature;
  const char* name;  // Signature::to_string() at generation time, for coverage reports and logs
  Kernel kernel;
};

// The generated table, sorted by `signature.hash()` ascending. Built once (a function-local
// static over the generated array — no static-initialisation-order hazard across translation
// units).
const std::vector<RegistryEntry>& generated_entries();

// The kernel catalogued for `sig`, or nullptr when no generated entry matches it exactly.
Kernel lookup(const Signature& sig) noexcept;

// Number of distinct signatures the catalogue carries.
std::size_t size() noexcept;

}  // namespace epykos::catalogue
