// PRINCIPLES.md §1b / I3's GATE: the pinned registry of host-computed tape inputs.
//
// I3 says: "Maths the ENGINE computes is recorded, not hand-written. Any numeric quantity the
// engine produces that a caller can observe is a recorded expression." Its gate is "a pinned
// registry of host-computed tape inputs — inputs whose values are written in by
// Tape::set_input_value from a quantity computed outside the tape — each with a stated
// justification, and a test that fails when the list grows silently."
//
// I3 DOES NOT HOLD. This file does not fix it; the fix for the one violation waits on I1 (a
// derivative cannot be a tape node yet). This is the RATCHET: the violation list cannot grow
// without someone writing down why. The idiom is tests/mutation/registry_test.cpp's — a pinned
// list in the test, a scan of the sources that must agree with it, and a failure message that
// says what to do.
//
// WHAT COUNTS AS A WRITE. §1b names Tape::set_input_value, which is the record-point writer.
// Three other paths put a host double where a tape input's value is read from, and the registry
// covers all four, because a leaf is a leaf whichever door it came through:
//
//   set_input_value   overwrites an existing input's record value (src/solver/implicit.cpp).
//   make_input / Tape::input   creates an input and gives it its record value.
//   lane[...] = ...   the RUN-TIME equivalent of set_input_value: ImplicitProgram keeps one
//                     double per tape input per lane and the solver writes its results straight
//                     into it (src/solver/implicit_program.cpp). Same three quantities, same
//                     ordinals, no Tape involved. It is the same thing wearing a different hat,
//                     and a registry that pinned only the record-point writer would miss the
//                     path every run actually takes.
//
// WHAT IS OUT OF SCOPE, deliberately:
//   * Fixtures (src/fixtures/, include/epykos/fixtures/) create inputs for the quotes they
//     generate from the seed. A fixture is the CALLER, and a caller's problem data is not a
//     quantity the engine computed. The creation idiom is therefore not scanned there — but
//     set_input_value and the lane write ARE scanned everywhere, including fixtures, because
//     overwriting an input with a computed value is never a caller's job.
//   * The adjoint's accumulation into full_bar: that is a derivative being returned, not an
//     input value being supplied.
//   * Constants. `Rec` folds host doubles into Const leaves by design ("constants are leaves",
//     CLAUDE.md), and that surface is the whole conventions layer. I3's gate is about INPUTS.
//
// The source scan is a regex over the engine's own sources and cannot see a host write spelled
// some fourth way. ThePerLaneWritesAreOnlyTheRegisteredRoles test below is the backstop that
// does not depend on spelling: it runs a real calibration at a moved market and checks, by
// VALUE, that the only tape inputs the engine writes are the registered ones.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "epykos/fixtures/m1_book.hpp"
#include "epykos/fixtures/m1_calibration.hpp"
#include "epykos/solver/implicit_program.hpp"

namespace fixtures = epykos::fixtures;
namespace solver = epykos::solver;

namespace {

// ---- the registry ------------------------------------------------------------------------
//
// One entry per ROLE: a kind of value that reaches a tape input from outside the tape. The
// verdict on I3 is the Kind.
enum class Kind {
  CallerInput,   // the problem's own data. The engine did not compute it; not a violation.
  Rebuild,       // an existing input's record value copied onto a rebuilt node table.
  Scratch,       // a throwaway tape recorded for a STRUCTURAL query; no value is ever observed.
  StopGradient,  // a quantity the engine computed that has no derivative. Declared, not hidden.
  Implicit,      // a quantity the engine computed whose derivative IS supplied, by the IFT.
  Violation,     // a quantity the engine computed whose derivative is lost. I3's violation.
};

const char* to_string(Kind k) {
  switch (k) {
    case Kind::CallerInput: return "caller-input";
    case Kind::Rebuild: return "rebuild";
    case Kind::Scratch: return "scratch";
    case Kind::StopGradient: return "stop-gradient";
    case Kind::Implicit: return "implicit";
    case Kind::Violation: return "VIOLATION";
  }
  return "?";
}

struct Entry {
  std::string_view role;
  Kind kind;
  std::string_view quantity;
  std::string_view justification;
};

// THE REGISTRY. Adding an entry is a decision, not a chore: say what the quantity is and why it
// is allowed to be a leaf, or record what its fix waits on.
const std::vector<Entry> registry = {
    {"unknowns", Kind::Implicit,
     "z, the solution of a recorded residual block F(z, p) = 0, at the record point and per lane",
     "Legitimate, by §1b's own carve-out: a Newton loop is a fixpoint, not an expression. There is "
     "no recorded expression for z, and the maths that DEFINES z is recorded -- ImplicitBlock::residuals "
     "are tape outputs and ResidualProgram runs that recorded slice, so the solver iterates on the "
     "engine's own compiled program, not on hand-written pricing. The derivative is not lost either: "
     "Factors::ift_adjoint supplies dz/dp exactly in reverse and ift_tangent in forward. What IS "
     "hand-written here is the IFT rule itself, in Eigen -- but that is I1 (the IR cannot express a "
     "derivative), not I3."},
    {"diag_jtr_input", Kind::Violation,
     "the optimality diagnostic ||J^T F||_inf at the solver's exit point (SolveReport::jtr_inf)",
     "THE violation D87 found and D94 measured. It is an ordinary mathematical expression -- a "
     "matrix-vector product contracted to an infinity norm -- written in C++ in src/solver/residual.cpp "
     "(ResidualProgram::jt_product into inf_norm, in BlockSolver::solve) and pushed onto the tape as a "
     "leaf, so no pass above the pin can see it and nothing could ever have optimised it. D85's own win "
     "had to be found by a person for exactly that reason. WAITING ON I1: J^T F is a derivative quantity, "
     "and a derivative cannot be a tape node while Op::Linmap has no producer and adjoint::Adjoint is a "
     "compiled artifact rather than a program. It is not fixed by recording something CHEAPER than the "
     "norm: that would be a different quantity, and §5.3 does not license it."},
    {"diag_iterations_input", Kind::StopGradient,
     "the accepted-step count of the record-time or per-lane solve (SolveReport::iterations)",
     "Legitimate. An iteration count is a property of the control flow, not of the mathematics: it is "
     "integer-valued and piecewise constant in the quotes, so it has no derivative anywhere it is "
     "defined. It is declared stop-gradient (implicit_program.hpp's header) and the adjoint drops its "
     "row. Recording it would mean recording the loop, which §1b's carve-out explicitly refuses."},
    {"caller_state", Kind::CallerInput,
     "the caller's state vector, written into each lane's free inputs by ImplicitProgram",
     "Not host-computed: these are the problem's inputs -- market quotes -- which the engine did not "
     "compute. They are the differentiation variables, not results. Listed so that the registry is a "
     "COMPLETE map of every write into a tape input and the boundary between a problem input and a "
     "laundered result is stated rather than assumed."},
    {"rebuild", Kind::Rebuild,
     "an existing Input node's record value, re-materialised when a pass, a slice or the IR expander "
     "rebuilds a node table",
     "Introduces no quantity. The value written is the one the input already had (Node::konst), copied "
     "onto a new table so that input ordinals stay stable across the passes. Nothing is computed. If "
     "one of these ever started deriving a value instead of copying one it would need its own role."},
    {"analysis_scratch", Kind::Scratch,
     "placeholder knot and quote values on CurveSet::instrument_reads's throwaway tape",
     "Legitimate, and not observable. instrument_reads records each calibration instrument onto a "
     "scratch tape solely to slice it and read back WHICH curves it touches (Slice::input_ordinals). No "
     "value on that tape is ever evaluated, returned or differentiated -- the start values and the 0.0 "
     "quote are placeholders for a structural query. I3 is about quantities a caller can observe."},
};

// ---- the pinned writer sites -------------------------------------------------------------
//
// Every line of the engine's own sources that writes a host double into a tape input, as the scan
// below finds it: the repository-relative file, the line with its comments stripped and its
// whitespace collapsed, and the registry role it belongs to. Line NUMBERS are deliberately not
// pinned (they move); the text is, so that editing one of these lines is a conscious act.
struct Site {
  std::string_view file;
  std::string_view line;
  std::string_view role;
};

const std::vector<Site> sites = {
    // The IR expander and the two tape rebuilders: an input's existing record value, copied.
    {"src/ir/expand.cpp",
     "id = t.input(p.input_values[idx(ordinal)]);",
     "rebuild"},
    {"src/tape/passes.cpp",
     "id = out_.input(n.konst);",
     "rebuild"},
    {"src/tape/slice.cpp",
     "id = out.tape.input(nd.konst);",
     "rebuild"},

    // CurveSet::instrument_reads's throwaway tape: a structural query, no value observed.
    {"src/solver/curve_set.cpp",
     "zc.push_back(make_input(scratch, v));",
     "analysis_scratch"},
    {"src/solver/curve_set.cpp",
     "const Rec q = make_input(scratch, 0.0);",
     "analysis_scratch"},

    // Recording an implicit block: the three input ordinals are created here, the unknowns at
    // z0 and the two diagnostics at a placeholder the record-time solve overwrites below.
    {"src/solver/implicit.cpp",
     "r.z.push_back(make_input(tape, v));",
     "unknowns"},
    {"src/solver/implicit.cpp",
     "r.jtr_inf = make_input(tape, 0.0);",
     "diag_jtr_input"},
    {"src/solver/implicit.cpp",
     "r.iterations = make_input(tape, 0.0);",
     "diag_iterations_input"},

    // The record point: the record-time solve's results written back, §1b's named writer.
    {"src/solver/implicit.cpp",
     "tape.set_input_value(block.unknowns[j], z[j]);",
     "unknowns"},
    {"src/solver/implicit.cpp",
     "tape.set_input_value(block.diag_jtr_input, result.report.jtr_inf);",
     "diag_jtr_input"},
    {"src/solver/implicit.cpp",
     "tape.set_input_value(block.diag_iterations_input, static_cast<double>(result.report.iterations));",
     "diag_iterations_input"},

    // Run time, ImplicitProgram::Impl::solve_lanes. The caller's state first, then each lane's
    // solve results -- either copied from an identical earlier lane, or freshly solved.
    {"src/solver/implicit_program.cpp",
     "for (std::size_t k = 0; k < free.size(); ++k) lane[idx(free[k])] = state[k * Bs + b];",
     "caller_state"},
    {"src/solver/implicit_program.cpp",
     "for (int j = 0; j < n_z; ++j) lane[idx(blk.unknowns[idx(j)])] = src[idx(blk.unknowns[idx(j)])];",
     "unknowns"},
    {"src/solver/implicit_program.cpp",
     "lane[idx(blk.diag_jtr_input)] = src[idx(blk.diag_jtr_input)];",
     "diag_jtr_input"},
    {"src/solver/implicit_program.cpp",
     "lane[idx(blk.diag_iterations_input)] = src[idx(blk.diag_iterations_input)];",
     "diag_iterations_input"},
    {"src/solver/implicit_program.cpp",
     "for (int j = 0; j < n_z; ++j) lane[idx(blk.unknowns[idx(j)])] = rt.z[idx(j)];",
     "unknowns"},
    {"src/solver/implicit_program.cpp",
     "lane[idx(blk.diag_jtr_input)] = rep.jtr_inf;",
     "diag_jtr_input"},
    {"src/solver/implicit_program.cpp",
     "lane[idx(blk.diag_iterations_input)] = static_cast<double>(rep.iterations);",
     "diag_iterations_input"},
};

// ---- the scan ------------------------------------------------------------------------------

// tests/invariants/i3_host_inputs_test.cpp -> the repository root, from this TU's compile-time path.
std::filesystem::path source_root() {
  return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

// `//...` and `/*...*/` removed, runs of whitespace collapsed to one space, trimmed.
std::string normalise(const std::string& raw) {
  std::string s = raw;
  const std::size_t slashes = s.find("//");
  if (slashes != std::string::npos) s.erase(slashes);
  for (;;) {
    const std::size_t open = s.find("/*");
    if (open == std::string::npos) break;
    const std::size_t close = s.find("*/", open + 2);
    if (close == std::string::npos) {
      s.erase(open);
      break;
    }
    s.erase(open, close + 2 - open);
  }
  std::string out;
  bool space = false;
  for (const char c : s) {
    const bool ws = c == ' ' || c == '\t' || c == '\r' || c == '\n';
    if (ws) {
      space = true;
      continue;
    }
    if (space && !out.empty()) out.push_back(' ');
    space = false;
    out.push_back(c);
  }
  return out;
}

struct Found {
  std::string file;
  std::string line;
  int number = 0;  // for the failure message only; never pinned
};

bool operator<(const Found& a, const Found& b) {
  return a.file != b.file ? a.file < b.file : a.line < b.line;
}

// The four writer idioms. `create` (make_input / Tape::input) is not scanned under fixtures/:
// a fixture is the caller, and creating an input for a quote it generated is its job.
const std::regex& re_overwrite() {
  static const std::regex r(R"re((\.|->)\s*set_input_value\s*\()re");
  return r;
}
const std::regex& re_lane() {
  static const std::regex r(R"re(\b(lane|full)\s*\[.*\]\s*=[^=])re");
  return r;
}
const std::regex& re_create() {
  static const std::regex r(R"re((\bmake_input\s*\()|((\.|->)\s*input\s*\())re");
  return r;
}

std::vector<Found> scan() {
  const std::filesystem::path root = source_root();
  std::vector<Found> found;
  const std::vector<std::filesystem::path> roots = {root / "src", root / "include" / "epykos"};
  for (const std::filesystem::path& base : roots) {
    if (!std::filesystem::is_directory(base)) continue;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(base)) {
      if (!entry.is_regular_file()) continue;
      const std::string ext = entry.path().extension().string();
      if (ext != ".cpp" && ext != ".hpp" && ext != ".h") continue;
      const std::string rel = std::filesystem::relative(entry.path(), root).generic_string();
      const bool is_fixture = rel.find("/fixtures/") != std::string::npos;
      // include/epykos/scalar/rec.hpp DEFINES make_input; its own signature is not a call site.
      const bool is_writer_definition = rel == "include/epykos/scalar/rec.hpp";
      std::ifstream in(entry.path());
      std::string raw;
      int number = 0;
      while (std::getline(in, raw)) {
        ++number;
        const std::string line = normalise(raw);
        if (line.empty()) continue;
        bool hit = std::regex_search(line, re_overwrite()) || std::regex_search(line, re_lane());
        if (!hit && !is_fixture && !is_writer_definition) hit = std::regex_search(line, re_create());
        if (hit) found.push_back(Found{rel, line, number});
      }
    }
  }
  std::sort(found.begin(), found.end());
  return found;
}

// The pinned list in the order and shape the scan produces, for comparison and for the message.
std::vector<Found> pinned() {
  std::vector<Found> v;
  for (const Site& s : sites) v.push_back(Found{std::string(s.file), std::string(s.line), 0});
  std::sort(v.begin(), v.end());
  return v;
}

std::string as_initialiser(const std::vector<Found>& v) {
  std::ostringstream os;
  os << "\nconst std::vector<Site> sites = {\n";
  for (const Found& f : v) {
    std::string escaped;
    for (const char c : f.line) {
      if (c == '"' || c == '\\') escaped.push_back('\\');
      escaped.push_back(c);
    }
    os << "    {\"" << f.file << "\",\n     \"" << escaped << "\",\n     \"<role>\"},  // line " << f.number
       << '\n';
  }
  os << "};\n";
  return os.str();
}

const char* what_to_do() {
  return "\nWHAT TO DO. This is PRINCIPLES.md §1b's gate for invariant I3, and it has just caught a line"
         "\nthat writes a host-computed double into a tape input."
         "\n"
         "\n  * If the value comes from the CALLER (a market quote, a problem definition), it is not a"
         "\n    violation: add the site with the role \"caller_state\", or give it a new caller-input role."
         "\n  * If the value is one the ENGINE computed, ask whether it could have been a recorded"
         "\n    expression. If it could, DO NOT ADD THE ENTRY -- record the expression instead. That is"
         "\n    the whole point of the invariant; a registry entry is not a licence."
         "\n  * If it could not (it is a fixpoint, an iteration count, or anything else §1b's carve-out"
         "\n    covers), add the site and a registry Entry whose justification says WHY, in the same"
         "\n    detail as the entries already there."
         "\n  * If it is a quantity whose derivative is lost, add it with Kind::Violation and say what the"
         "\n    fix waits on. Then expect ExactlyOneViolationToday to fail, and update it and the §1b"
         "\n    status block together -- the count of violations is the status of I3."
         "\n  * The run-time idiom is matched BY NAME (`lane[...] =`, `full[...] =`). If a line matched only"
         "\n    because it assigns into an unrelated array called `lane` or `full`, rename that variable:"
         "\n    the registry does not carry entries for lines that write no tape input, and an exemption"
         "\n    here would be a hole in the scan."
         "\n"
         "\nPaste the block below into `sites` and fill in each <role>. Removing a line that is still"
         "\nthere, or relaxing the scan, is not a fix.\n";
}

// ---- the fixture for the behavioural backstop ------------------------------------------------

struct Calibrated {
  fixtures::Book book;
  std::vector<double> quotes;
  fixtures::CalibratedM1 cal;
};

const Calibrated& calibrated() {
  static const Calibrated c = [] {
    Calibrated x;
    x.book = fixtures::make_m1_book();
    x.quotes = fixtures::m1_par_quotes(x.book.z0.data());
    const std::vector<double> start(static_cast<std::size_t>(fixtures::n_knots), 0.03);
    x.cal = fixtures::record_m1_calibrated(x.book, x.quotes, start);
    return x;
  }();
  return c;
}

// The tape input ordinals the registry's engine-written roles own, for this tape.
std::set<int> registered_ordinals(const solver::ImplicitRegistry& reg) {
  std::set<int> s;
  for (const solver::ImplicitBlock& b : reg.blocks) {
    for (const int o : b.unknowns) s.insert(o);
    s.insert(b.diag_jtr_input);
    s.insert(b.diag_iterations_input);
  }
  return s;
}

}  // namespace

TEST(I3HostInputs, RegistryIsWellFormedAndStated) {
  // One line per role, so a reader of a CI log sees I3's status without opening the file.
  for (const Entry& e : registry) {
    std::cout << "host_input " << e.role << " [" << to_string(e.kind) << "] " << e.quantity << '\n';
  }

  ASSERT_FALSE(registry.empty());
  std::vector<std::string_view> roles;
  for (const Entry& e : registry) {
    EXPECT_FALSE(e.role.empty());
    EXPECT_FALSE(e.quantity.empty()) << e.role << ": a registry entry says WHAT is written";
    // A justification is the entry's reason to exist; §1b asks for a stated one, not a word.
    EXPECT_GT(e.justification.size(), 80u) << e.role << ": the justification must actually justify";
    roles.push_back(e.role);
  }
  std::sort(roles.begin(), roles.end());
  EXPECT_EQ(std::unique(roles.begin(), roles.end()), roles.end()) << "duplicate role";
}

TEST(I3HostInputs, EveryWriterSiteIsPinnedAndEveryPinnedSiteExists) {
  const std::filesystem::path root = source_root();
  if (!std::filesystem::is_directory(root / "src")) {
    GTEST_SKIP() << "source tree not present at " << root << " (relocated build)";
  }
  const std::vector<Found> found = scan();
  const std::vector<Found> want = pinned();

  std::vector<Found> added, removed;
  std::set_difference(found.begin(), found.end(), want.begin(), want.end(), std::back_inserter(added));
  std::set_difference(want.begin(), want.end(), found.begin(), found.end(), std::back_inserter(removed));

  if (!added.empty()) {
    ADD_FAILURE() << added.size() << " write(s) into a tape input are not in the registry:\n"
                  << as_initialiser(added) << what_to_do();
  }
  if (!removed.empty()) {
    std::ostringstream os;
    for (const Found& f : removed) os << "  " << f.file << ": " << f.line << '\n';
    ADD_FAILURE() << removed.size() << " pinned site(s) no longer exist in the sources:\n"
                  << os.str()
                  << "\nIf a host-computed input became a recorded expression, delete the site here and say"
                     "\nso in PRINCIPLES.md §1b. If the line only moved or was reformatted, re-pin its text.\n";
  }
  EXPECT_EQ(found.size(), want.size());

  // Every pinned site names a role the registry explains, and every role is actually used.
  std::map<std::string_view, int> used;
  for (const Entry& e : registry) used[e.role] = 0;
  for (const Site& s : sites) {
    const auto it = used.find(s.role);
    ASSERT_NE(it, used.end()) << s.file << ": site claims role \"" << s.role << "\", which the registry does not define";
    ++it->second;
  }
  for (const auto& [role, n] : used) {
    EXPECT_GT(n, 0) << "role \"" << role << "\" is registered but no site uses it: a role that explains nothing";
  }
}

TEST(I3HostInputs, ExactlyOneViolationToday) {
  int violations = 0;
  std::string names;
  for (const Entry& e : registry) {
    if (e.kind != Kind::Violation) continue;
    ++violations;
    names += (names.empty() ? "" : ", ") + std::string(e.role);
  }
  std::cout << "host_input violations " << violations << " (" << names << ")\n";
  EXPECT_EQ(violations, 1) << "I3's status is the number of violations here, and it has changed (" << names
                           << ").\nIf one was FIXED -- the quantity is a recorded expression now -- that is the"
                              "\ninvariant moving and PRINCIPLES.md §1b's status must be rewritten in the same"
                              "\ncommit. If one was ADDED, read what_to_do() above: a new violation needs a"
                              "\nledger entry, not a line here.";
  EXPECT_EQ(names, "diag_jtr_input");
}

// The backstop that does not depend on how a write is spelled. At a moved market the only tape
// inputs whose values differ from the record point are the registered ones.
TEST(I3HostInputs, PerLaneWritesAreOnlyTheRegisteredRoles) {
  const Calibrated& c = calibrated();
  const std::vector<double> record = c.cal.tape.input_values();
  const std::set<int> owned = registered_ordinals(c.cal.registry);

  solver::ProgramOptions opts;
  opts.max_batch = 1;
  solver::ImplicitProgram prog(c.cal.tape, c.cal.registry, opts);

  // The caller's inputs are exactly the quotes the fixture recorded: this ties "free" to
  // "supplied by the caller" without going through the registry that is on trial.
  EXPECT_EQ(prog.free_inputs(), c.cal.quote_inputs)
      << "the free inputs are no longer the quotes: some caller input became engine-written, or the "
         "other way round";

  const int n_in = prog.n_inputs();
  std::vector<double> out(static_cast<std::size_t>(prog.n_outputs()));

  // 1. At the record point the lane vector reproduces the tape's own record values, every ordinal.
  std::vector<double> state = prog.record_state();
  prog.run(state.data(), 1, out.data());
  for (int o = 0; o < n_in; ++o) {
    EXPECT_EQ(prog.full_state(0)[o], record[static_cast<std::size_t>(o)])
        << "tape input " << o << " is not at its record value after a run at the record point";
  }

  // 2. At a moved market, only the registered ordinals move.
  for (double& q : state) q += 0.0025;
  prog.run(state.data(), 1, out.data());
  const double* lane = prog.full_state(0);
  std::vector<int> moved;
  for (int o = 0; o < n_in; ++o) {
    if (lane[o] != record[static_cast<std::size_t>(o)]) moved.push_back(o);
  }
  for (const int o : moved) {
    const bool free_input = std::find(prog.free_inputs().begin(), prog.free_inputs().end(), o) != prog.free_inputs().end();
    EXPECT_TRUE(free_input || owned.count(o) == 1)
        << "tape input " << o << " changed when the market moved, and it is neither a caller input nor a"
           " registered host-computed one.\nThe engine is writing a quantity into a leaf that §1b's registry"
           " does not know about. Read what_to_do().";
  }

  // The roles are live, not vestigial: a moved market moves the solution and the diagnostic.
  for (const solver::ImplicitBlock& b : c.cal.registry.blocks) {
    for (const int o : b.unknowns) {
      EXPECT_NE(lane[o], record[static_cast<std::size_t>(o)]) << "unknown " << o << " did not move with the market";
    }
    EXPECT_NE(lane[b.diag_jtr_input], record[static_cast<std::size_t>(b.diag_jtr_input)])
        << "the optimality diagnostic did not move with the market";
  }
  std::cout << "host_input moved_ordinals " << moved.size() << " of " << n_in << " tape inputs\n";
}
