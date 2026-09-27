// EpykosEngine — tools/catalogue: the M4/C1 catalogue generator (PROBLEM.md §7, DESIGN.md §7
// tier 1). Runs the two reference workloads — the Stage A tape (docs/PROBLEM.md §4 Stage A) and
// the M1 book (fixtures/m1_book.hpp) — through record -> ir::infer, collects the canonical
// Signature (include/epykos/catalogue/signature.hpp) of every catalogue-eligible domain either
// one produces, and writes src/catalogue/generated/{kernels_e0.cpp,registry.cpp}. Deterministic:
// both fixtures are seeded (docs/WORKLOADS.md) and a Signature carries no row count or value, so
// re-running against the same source tree reproduces byte-identical output
// (scripts/catalogue_regen.sh's `git diff --exit-code`).
//
// Scope, stated plainly (HARD RULE 10: report honestly, label estimates): this generator walks
// the Program `ir::infer` itself produces — the SAME Program an ordinary
// `exec::Interpreter(program, Options{})` / `adjoint::Adjoint(program, Options{})` construction
// runs today. It does NOT additionally run the M4/R-a..R-c structural rewrites (R1-R7) or an
// M4/EG e-graph extraction first: those are opt-in (a caller passes their own Program to
// exec::Interpreter / adjoint::Adjoint, or the future EG driver's extracted one), not what a
// default construction executes, and layering this generator on either is future work — nothing
// about the Signature / registry format is specific to an un-rewritten Program (a rewritten or
// extracted Program is still an ir::Program; the SAME `is_cataloguable` / `signature_of` apply
// unchanged), so a later regen against a different input Program needs no change here, only a
// different `record_and_infer` producing it. See docs/DESIGN.md §7 tier 1's "as built" note.
//
// A domain counts as "hot" simply by appearing, at least once, as a catalogue-eligible domain of
// one of the two reference workloads: PROBLEM.md §7 names them as the reference workloads
// themselves, so every real group either one evaluates is real work, not a row-count threshold
// this file would otherwise have to invent (HARD RULE 9: no magic instance counts).
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "codegen.hpp"
#include "epykos/catalogue/signature.hpp"
#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/record_m1.hpp"
#include "epykos/fixtures/stage_a.hpp"
#include "epykos/ir/program.hpp"
#include "epykos/ir/signature.hpp"
#include "epykos/tape/tape.hpp"

namespace catalogue = epykos::catalogue;
namespace fixtures = epykos::fixtures;
namespace ir = epykos::ir;

namespace {

// Folds every catalogue-eligible domain of `program` into `entries`, tagging new signatures
// with `workload` and appending to an existing entry's `found_in` the first time a later
// workload repeats a signature the first one already produced.
//
// Keyed on the hash AND checked for full `Signature` equality (D69). Until that fix this map was
// keyed on `Signature::hash()` alone, so two genuinely different signatures that collided on 64
// bits would have silently dropped one of them: lost coverage, never a wrong kernel, because
// `catalogue::lookup` verifies full equality at run time (D55) -- but the same defect class as the
// one D61 fixed in the fingerprint itself, and no more expensive to do right. A collision now
// makes a second entry under the same hash instead of discarding one; the generated table is
// sorted by hash and `lookup` binary-searches then compares the full Signature over the run of
// same-hash entries, which is exactly the case it was already written to handle.
void collect(const ir::Program& program, const std::string& workload,
            std::map<std::uint64_t, std::vector<catalogue::tool::CatalogueEntry>>& by_hash) {
  int seen = 0, eligible = 0, collisions = 0;
  for (std::size_t d = 0; d < program.domains.size(); ++d) {
    ++seen;
    const ir::domain_id did = static_cast<ir::domain_id>(d);
    if (!catalogue::is_cataloguable(program, did)) continue;
    ++eligible;
    catalogue::Signature sig = catalogue::signature_of(program, did);
    const std::uint64_t h = sig.hash();
    std::vector<catalogue::tool::CatalogueEntry>& bucket = by_hash[h];
    catalogue::tool::CatalogueEntry* found = nullptr;
    for (catalogue::tool::CatalogueEntry& e : bucket) {
      if (e.signature == sig) {
        found = &e;
        break;
      }
    }
    if (found == nullptr) {
      if (!bucket.empty()) ++collisions;
      bucket.push_back(catalogue::tool::CatalogueEntry{std::move(sig), workload});
    } else if (found->found_in.find(workload) == std::string::npos) {
      found->found_in += ", " + workload;
    }
  }
  std::fprintf(stderr, "[ catalogue ] %s: %d domains, %d catalogue-eligible, %d hash collision(s)\n",
               workload.c_str(), seen, eligible, collisions);
}

ir::Program m1_program() {
  const fixtures::Book book = fixtures::make_m1_book();
  const epykos::Tape tape = fixtures::record_m1(book);
  return ir::infer(tape);
}

ir::Program stage_a_program(bool small) {
  fixtures::StageAOptions opt;
  if (small) {
    opt.trades = 200;
    opt.scenarios = 8;
  }
  const fixtures::StageA s = fixtures::make_stage_a(opt);
  const fixtures::StageATape tape = fixtures::record_stage_a(s);
  return ir::infer(tape.tape);
}

void write_file(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) throw std::runtime_error("catalogue generator: cannot open " + path + " for writing");
  f << content;
  if (!f) throw std::runtime_error("catalogue generator: write failed: " + path);
}

}  // namespace

int main(int argc, char** argv) {
  std::string out_dir;
  bool small = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--small") {
      small = true;
    } else if (out_dir.empty()) {
      out_dir = arg;
    }
  }
  if (out_dir.empty()) {
    std::fprintf(stderr, "usage: catalogue_generate <output-dir> [--small]\n"
                         "  writes <output-dir>/kernels_e0.cpp and <output-dir>/registry.cpp\n"
                         "  --small: a fast, non-default Stage A size for development iteration only —\n"
                         "           scripts/catalogue_regen.sh never passes this flag\n");
    return 2;
  }

  std::map<std::uint64_t, std::vector<catalogue::tool::CatalogueEntry>> by_hash;
  collect(m1_program(), "m1_book", by_hash);
  collect(stage_a_program(small), small ? "stage_a(small)" : "stage_a", by_hash);

  // Sorted by hash ascending (std::map), and within one hash in first-encounter order, which is
  // deterministic for a given pair of fixtures: the generated table's own ordering contract.
  std::vector<catalogue::tool::CatalogueEntry> entries;
  for (auto& [h, bucket] : by_hash) {
    for (auto& e : bucket) entries.push_back(std::move(e));
  }
  std::fprintf(stderr, "[ catalogue ] %zu distinct signature(s) across both workloads\n", entries.size());

  write_file(out_dir + "/kernels_e0.cpp", catalogue::tool::generate_kernels_file(entries));
  write_file(out_dir + "/registry.cpp", catalogue::tool::generate_registry_file(entries));
  std::fprintf(stderr, "[ catalogue ] wrote %s/{kernels_e0.cpp,registry.cpp}\n", out_dir.c_str());
  return 0;
}
