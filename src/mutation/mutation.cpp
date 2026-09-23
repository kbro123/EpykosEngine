// EpykosEngine — the mutation selector's run-time side (include/epykos/mutation/mutation.hpp).
// Compiled into libepykos in every build; it has a body only under EPYKOS_MUTATIONS=ON (the
// mutation preset). Everywhere else the selector is constexpr false and there is nothing to do.
#include "epykos/mutation/mutation.hpp"

#if defined(EPYKOS_MUTATIONS) && EPYKOS_MUTATIONS

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace epykos::mutation {

namespace {

std::string registered_names() {
  std::string s;
  for (std::string_view r : registry) {
    if (!s.empty()) s += ' ';
    s += r;
  }
  return s;
}

// EPYKOS_MUTANT, read once. An unregistered name throws on every call (the static is not
// initialised while its initialiser throws), so a typo in the environment fails every test that
// runs a pass instead of quietly testing nothing.
std::string read_selection() {
  const char* v = std::getenv("EPYKOS_MUTANT");
  std::string name = v != nullptr ? v : "";
  if (!name.empty() && !registered(name)) {
    throw std::invalid_argument("EPYKOS_MUTANT='" + name + "' is not a registered mutant (registered: " +
                                registered_names() + ")");
  }
  return name;
}

}  // namespace

std::string_view active() {
  static const std::string selection = read_selection();
  return selection;
}

bool query(std::string_view name) {
  if (!registered(name)) {
    throw std::logic_error("epykos::mutant(\"" + std::string(name) + "\"): not a registered mutant (registered: " +
                           registered_names() + ")");
  }
  return active() == name;
}

}  // namespace epykos::mutation

#endif  // EPYKOS_MUTATIONS
