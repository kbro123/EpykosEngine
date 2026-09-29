#include "epykos/catalogue/registry.hpp"

#include <algorithm>

namespace epykos::catalogue {

namespace {
// Sorted once, lazily, on first use — `generated_entries()` returns the generator's raw table
// (whatever order the tool emitted it in; the generator itself sorts by hash for a stable diff,
// but lookup() must not assume it and re-sorting a second time is free at this table's size).
const std::vector<RegistryEntry>& sorted_entries() {
  static const std::vector<RegistryEntry> table = [] {
    std::vector<RegistryEntry> t = generated_entries();
    std::sort(t.begin(), t.end(), [](const RegistryEntry& a, const RegistryEntry& b) { return a.signature.hash() < b.signature.hash(); });
    return t;
  }();
  return table;
}
}  // namespace

Kernel lookup(const Signature& sig) noexcept {
  const std::vector<RegistryEntry>& table = sorted_entries();
  const std::uint64_t h = sig.hash();
  auto it = std::lower_bound(table.begin(), table.end(), h,
                             [](const RegistryEntry& e, std::uint64_t key) { return e.signature.hash() < key; });
  for (; it != table.end() && it->signature.hash() == h; ++it) {
    if (it->signature == sig) return it->kernel;
  }
  return nullptr;
}

std::size_t size() noexcept { return sorted_entries().size(); }

}  // namespace epykos::catalogue
