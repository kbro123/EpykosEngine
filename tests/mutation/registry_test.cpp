// M2/Q4: the mutant registry (include/epykos/mutation/mutation.hpp) and the selector.
//
// Runs in every preset. It pins the registry to the documented list (docs/WORKLOADS.md §M2
// "Mutation set"), so a mutant added to the code without being documented, or documented without
// being registered, fails here; it prints the registry one name per line for
// scripts/mutation_test.sh, which is how the harness learns what to run (so it cannot silently
// skip one); it checks that every registered name has exactly one use site under src/ and that
// every `mutant("...")` use site under src/ names a registered mutant (a typo at a use site would
// otherwise be a mutant that can never be selected); and it checks the selector against the
// environment: in the mutation build EPYKOS_MUTANT selects exactly the named mutant and an
// unregistered name throws; in every other build the query is a constexpr false whatever the
// environment says.
//
// This is not a gate: a mutant "caught" only here has not been caught (mutation_test.sh excludes
// tests/mutation/ from the gate set).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "epykos/mutation/mutation.hpp"

namespace mutation = epykos::mutation;

namespace {

// The mutation set as documented in docs/WORKLOADS.md §M2, in the order the harness runs it.
// Editing the registry in the header without editing this list (or the other way round) fails
// ListsEveryMutant. The adjoint mutants (M2/Q4b) follow the pass mutants, then the implicit-node
// mutants (M3/G4), then the scan mutants (M3/G3).
const std::vector<std::string> documented = {
    "cse.merge_nonequal",
    "fold_sum.wrong_order",
    "affine.wrong_coefficient",
    "affine.drop_offset",
    "affine.single_term_unscaled",
    "expander.drop_gather",
    "expander.segment_off_by_one",
    "signature.merge_classes",
    "interpreter.tile_boundary",
    "adjoint.wrong_transpose",
    "adjoint.drop_broadcast",
    "adjoint.affine_not_transposed",
    "adjoint.select_wrong_arm",
    "adjoint.recip_rule_sign",
    "implicit.ift_not_transposed",
    "implicit.ift_drop_fp",
    "implicit.stale_jacobian",
    "expander.scan_carry_from_init",
    "interpreter.scan_drop_last_wave",
    "adjoint.scan_forward_order",
    "r1.ignores_last_row",
    "r1.wrong_slot",
    "r2.wrong_run_boundary",
    "r2.column_slice_uses_wrong_bucket",
    "r3.off_by_one_member",
    "r3.treats_length_two_as_trivial",
    "r6.ignore_reduction_boundary",
    "r6.ignore_fanout_boundary",
    "r7.no_offset_rebase",
    "r7.wrong_block_value_base",
    "r4a.wrong_literal",
    "r4a.wrong_row_map",
    "r4b.wrong_op",
    "r4b.wrong_row_map",
    "r5.wrong_step_index",
    "r5.drop_last_step",
    "fma.wrong_operand",
    "fma.drop_remap",
};

std::vector<std::string> registry_names() {
  return std::vector<std::string>(std::begin(mutation::registry), std::end(mutation::registry));
}

// tests/mutation/registry_test.cpp -> the repository root, from this TU's compile-time path.
std::filesystem::path source_root() {
  return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

// Every `mutant("<name>")` under src/ (the engine's own sources; the selector's header holds
// the registry, not use sites): name -> the files that query it.
std::map<std::string, std::vector<std::string>> use_sites(const std::filesystem::path& src) {
  std::map<std::string, std::vector<std::string>> sites;
  const std::regex query(R"re(\bmutant\(\s*"([^"]*)"\s*\))re");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(src)) {
    if (!entry.is_regular_file()) continue;
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".hpp" && ext != ".h") continue;
    std::ifstream in(entry.path());
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    for (auto it = std::sregex_iterator(text.begin(), text.end(), query); it != std::sregex_iterator(); ++it) {
      sites[(*it)[1].str()].push_back(std::filesystem::relative(entry.path(), src.parent_path()).generic_string());
    }
  }
  return sites;
}

}  // namespace

TEST(MutationRegistry, ListsEveryMutant) {
  // One line per mutant, parsed by scripts/mutation_test.sh; then whether this build carries them.
  for (std::string_view name : mutation::registry) std::cout << "mutant " << name << '\n';
  std::cout << "mutants_compiled_in " << (mutation::compiled_in ? 1 : 0) << '\n';

  EXPECT_EQ(registry_names(), documented) << "the registry and docs/WORKLOADS.md §M2 must list the same mutants in the same order";
  EXPECT_EQ(mutation::registry_size, documented.size());
  EXPECT_GT(mutation::registry_size, 0u);

  // Well-formed and unique: "<pass>.<defect>", lower case.
  const std::regex well_formed("^[a-z][a-z0-9_]*\\.[a-z][a-z0-9_]*$");
  std::vector<std::string> names = registry_names();
  for (const std::string& name : names) {
    EXPECT_TRUE(std::regex_match(name, well_formed)) << name;
    EXPECT_TRUE(mutation::registered(name)) << name;
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::unique(names.begin(), names.end()), names.end()) << "duplicate mutant names";
  EXPECT_FALSE(mutation::registered("no_such.mutant"));
  EXPECT_FALSE(mutation::registered(""));
}

TEST(MutationRegistry, EveryMutantHasOneUseSiteAndEveryUseSiteIsRegistered) {
  const std::filesystem::path src = source_root() / "src";
  if (!std::filesystem::is_directory(src)) {
    GTEST_SKIP() << "source tree not present at " << src << " (relocated build)";
  }
  const std::map<std::string, std::vector<std::string>> sites = use_sites(src);
  for (std::string_view name : mutation::registry) {
    const auto it = sites.find(std::string(name));
    ASSERT_NE(it, sites.end()) << name << " is registered but no source under src/ queries it";
    EXPECT_EQ(it->second.size(), 1u) << name << " is queried from more than one place: one guarded line per mutant";
    std::cout << "[ use site ] " << name << " <- " << it->second.front() << '\n';
  }
  for (const auto& [name, files] : sites) {
    EXPECT_TRUE(mutation::registered(name)) << "src/ queries mutant \"" << name << "\" (in " << files.front()
                                             << "), which is not registered: a typo, or an unregistered mutant";
  }
  EXPECT_EQ(sites.size(), mutation::registry_size);
}

TEST(MutationRegistry, SelectorFollowsTheEnvironment) {
  const char* env = std::getenv("EPYKOS_MUTANT");
  const std::string selected = env != nullptr ? env : "";
  std::cout << "[  select  ] EPYKOS_MUTANT=\"" << selected << "\", compiled_in " << (mutation::compiled_in ? 1 : 0) << '\n';
#if defined(EPYKOS_MUTATIONS) && EPYKOS_MUTATIONS
  static_assert(mutation::compiled_in);
  if (!selected.empty() && !mutation::registered(selected)) {
    EXPECT_THROW((void)mutation::active(), std::invalid_argument) << "an unregistered selection must be loud";
    EXPECT_THROW((void)epykos::mutant("cse.merge_nonequal"), std::invalid_argument);
    return;
  }
  EXPECT_EQ(mutation::active(), selected);
  int active_count = 0;
  for (std::string_view name : mutation::registry) {
    const bool on = epykos::mutant(name);
    EXPECT_EQ(on, name == selected) << name;
    active_count += on ? 1 : 0;
  }
  EXPECT_EQ(active_count, selected.empty() ? 0 : 1);
  EXPECT_THROW((void)epykos::mutant("no_such.mutant"), std::logic_error) << "a use-site typo must be loud";
#else
  static_assert(!mutation::compiled_in);
  static_assert(!epykos::mutant("cse.merge_nonequal"), "outside the mutation build the query is a constexpr false");
  static_assert(mutation::active().empty());
  for (std::string_view name : mutation::registry) EXPECT_FALSE(epykos::mutant(name)) << name << " (EPYKOS_MUTANT=\"" << selected << "\")";
  EXPECT_TRUE(mutation::active().empty());
#endif
}
