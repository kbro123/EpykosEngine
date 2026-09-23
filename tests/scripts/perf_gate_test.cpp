// M2/Q5: the performance-gate tooling (scripts/perf_gate.py, scripts/bench_results.py, bench/run.sh)
// exercised on synthetic JSON: a self-regression above 1.25x fails (exit 1), a cross-fingerprint
// comparison is refused (exit 2), a run measured under load > cores/2 is refused (exit 2), a run
// without a baseline is refused until --accept seeds one, absolute targets from a targets file are
// evaluated, and the summariser computes min / median / p90 as documented. The scripts are Python 3
// standard library (D29); the test skips when no python3 is on the PATH.
//
// M2/m2-fix (D34): a baseline entry is keyed by the run name, and bench/run.sh derives that name from
// the binary unless --name overrides it, so an entry seeded under an ad hoc name gates nothing a
// default invocation produces. The committed results are checked for that (every baseline run keyed
// by its derived name, its <run>.json present and gated), the refusal names the entry holding the
// same binary, and --accept records the run's arguments so a filtered baseline states its filter.
//
// The repository root is derived from this file's compile-time path (CMake passes absolute source
// paths), or from EPYKOS_SOURCE_DIR when set.
#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Cmd {
  int exit;
  std::string out;
};

std::string q(const std::string& s) {  // single-quote for the shell
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''"; else r += c;
  }
  return r + "'";
}
std::string q(const fs::path& p) { return q(p.string()); }

Cmd run(const std::string& cmd) {
  const std::string full = cmd + " 2>&1";
  FILE* p = popen(full.c_str(), "r");
  if (p == nullptr) return {-1, "popen failed"};
  std::string out;
  char buf[4096];
  while (std::size_t n = std::fread(buf, 1, sizeof buf, p)) out.append(buf, n);
  const int st = pclose(p);
  return {WIFEXITED(st) ? WEXITSTATUS(st) : -1, out};
}

fs::path repo_root() {
  if (const char* e = std::getenv("EPYKOS_SOURCE_DIR")) return fs::path(e);
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

void write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p);
  f << text;
}
std::string read(const fs::path& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

using Medians = std::vector<std::pair<std::string, double>>;

struct RunSpec {
  std::string name = "run";
  std::string binary;  // default build/release/bench/<name>_bench: the binary run.sh derives <name> from
  std::string fp = "aaaaaaaaaaaa";
  int cores = 16;
  double load_before = 2.0;
  double load_after = 2.5;
  std::string unit = "us";
  std::string commit = "abc1234";
  Medians medians;
};

// A results file in the epykos-bench 1 format (the fields the gate reads; 5 identical repetitions).
std::string run_json(const RunSpec& r) {
  std::ostringstream s;
  s.precision(17);
  const std::string binary = r.binary.empty() ? "build/release/bench/" + r.name + "_bench" : r.binary;
  s << "{\"format\": \"epykos-bench 1\", \"name\": \"" << r.name << "\", \"binary\": \"" << binary
    << "\", \"args\": [\"--benchmark_repetitions=5\", \"--benchmark_min_time=0.01s\"], \"preset\": \"release\",\n"
    << " \"date\": \"2026-09-23T10:00:00+01:00\", \"git\": {\"commit\": \"" << r.commit << "0000000000000000000000000000000000\", \"commit_short\": \""
    << r.commit << "\", \"branch\": \"test\", \"dirty\": false},\n"
    << " \"fingerprint\": {\"id\": \"" << r.fp << "\", \"cpu\": \"synthetic\", \"cores_physical\": " << r.cores / 2 << ", \"cores_logical\": " << r.cores
    << ", \"compiler\": \"cc 0\", \"flags\": \"-O3\", \"os\": \"test\", \"arch\": \"test\"},\n"
    << " \"load\": {\"before\": " << r.load_before << ", \"after\": " << r.load_after << ", \"cores_logical\": " << r.cores
    << ", \"threshold\": " << r.cores / 2.0 << ", \"within_threshold\": true, \"check_enforced\": true},\n"
    << " \"env\": {}, \"google_benchmark\": {\"version\": \"v0\", \"repetitions\": 5, \"min_time\": \"0.01s\", \"context\": {}},\n"
    << " \"statistics\": \"synthetic\", \"raw_output\": null, \"benchmarks\": {";
  bool first = true;
  for (const auto& [name, med] : r.medians) {
    s << (first ? "" : ",") << "\n  \"" << name << "\": {\"n\": 5, \"min\": " << med << ", \"median\": " << med << ", \"p90\": " << med
      << ", \"max\": " << med << ", \"mean\": " << med << ", \"unit\": \"" << r.unit << "\", \"function\": \"" << name.substr(0, name.find('/'))
      << "\", \"label\": \"\", \"params\": {}, \"counters\": {}, \"iterations\": [1,1,1,1,1], \"evaluations\": 5, \"real_time\": ["
      << med << "," << med << "," << med << "," << med << "," << med << "], \"cpu_time_median\": " << med << "}";
    first = false;
  }
  s << "\n }\n}\n";
  return s.str();
}

// A baseline file: fingerprint id + one run with the given medians.
std::string baseline_json(const std::string& fp, const std::string& run, const Medians& medians, const std::string& unit = "us") {
  std::ostringstream s;
  s.precision(17);
  s << "{\"format\": \"epykos-baseline 1\", \"fingerprint\": {\"id\": \"" << fp << "\", \"cores_logical\": 16}, \"updated\": null, \"runs\": {\"" << run
    << "\": {\"commit\": \"base000\", \"benchmarks\": {";
  bool first = true;
  for (const auto& [name, med] : medians) {
    s << (first ? "" : ",") << "\"" << name << "\": {\"median\": " << med << ", \"unit\": \"" << unit << "\", \"n\": 5, \"commit\": \"base000\"}";
    first = false;
  }
  s << "}}}}\n";
  return s.str();
}

class PerfGateTest : public ::testing::Test {
 protected:
  fs::path root, scripts, dir, results;

  void SetUp() override {
    if (run("python3 --version").exit != 0) GTEST_SKIP() << "python3 not on the PATH; the gate scripts need it (D29)";
    root = repo_root();
    scripts = root / "scripts";
    ASSERT_TRUE(fs::exists(scripts / "perf_gate.py")) << "scripts/perf_gate.py not found under " << root;
    ASSERT_TRUE(fs::exists(scripts / "bench_results.py")) << "scripts/bench_results.py not found under " << root;
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir = fs::temp_directory_path() / ("epykos_perf_gate_" + std::to_string(::getpid()) + "_" + info->name());
    fs::remove_all(dir);
    fs::create_directories(dir);
    results = dir / "results";
  }
  void TearDown() override { fs::remove_all(dir); }

  fs::path write_run(const RunSpec& r, const std::string& file = "") {
    const fs::path p = dir / (file.empty() ? r.name + ".json" : file);
    write(p, run_json(r));
    return p;
  }
  fs::path baseline_path(const std::string& fp) { return results / fp / "baseline.json"; }
  void write_baseline(const std::string& fp, const std::string& run, const Medians& medians, const std::string& unit = "us") {
    write(baseline_path(fp), baseline_json(fp, run, medians, unit));
  }
  // Runs the gate with the scratch results root; --no-targets unless targets is given.
  Cmd gate(const std::vector<fs::path>& runs, const std::string& extra = "", const fs::path& targets = "") {
    std::string cmd = "python3 " + q(scripts / "perf_gate.py") + " --results-root " + q(results);
    cmd += targets.empty() ? " --no-targets" : " --targets " + q(targets);
    if (!extra.empty()) cmd += " " + extra;
    for (const auto& r : runs) cmd += " " + q(r);
    return run(cmd);
  }
};

// ---------------------------------------------------------------------------------------------
// Self-regression against the baseline
// ---------------------------------------------------------------------------------------------
TEST_F(PerfGateTest, PassesWithinThreshold) {
  write_baseline("aaaaaaaaaaaa", "run", {{"BM_A", 100.0}, {"BM_B", 200.0}});
  RunSpec r;
  r.medians = {{"BM_A", 110.0}, {"BM_B", 190.0}};
  const Cmd c = gate({write_run(r)});
  EXPECT_EQ(c.exit, 0) << c.out;
  EXPECT_TRUE(has(c.out, "verdict: PASS")) << c.out;
  EXPECT_TRUE(has(c.out, "2 compared: 2 ok, 0 regression")) << c.out;
}

TEST_F(PerfGateTest, RegressionAboveThresholdFails) {
  write_baseline("aaaaaaaaaaaa", "run", {{"BM_A", 100.0}, {"BM_B", 200.0}});
  RunSpec r;
  r.medians = {{"BM_A", 130.0}, {"BM_B", 200.0}};  // 1.30x > 1.25x
  const Cmd c = gate({write_run(r)});
  EXPECT_EQ(c.exit, 1) << c.out;
  EXPECT_TRUE(has(c.out, "REGRESSION")) << c.out;
  EXPECT_TRUE(has(c.out, "verdict: FAIL")) << c.out;
  EXPECT_TRUE(has(c.out, "run BM_A x1.300")) << c.out;
}

TEST_F(PerfGateTest, ThresholdBoundaryAndOverride) {
  write_baseline("aaaaaaaaaaaa", "run", {{"BM_A", 100.0}});
  RunSpec r;
  r.medians = {{"BM_A", 125.0}};  // exactly 1.25: not above the threshold
  EXPECT_EQ(gate({write_run(r)}).exit, 0);
  r.medians = {{"BM_A", 125.5}};
  EXPECT_EQ(gate({write_run(r)}).exit, 1);
  EXPECT_EQ(gate({write_run(r)}, "--threshold 1.3").exit, 0);  // the limit is a parameter
}

TEST_F(PerfGateTest, FasterNewAndNotMeasuredDoNotFail) {
  write_baseline("aaaaaaaaaaaa", "run", {{"BM_A", 100.0}, {"BM_B", 200.0}});
  RunSpec r;
  r.medians = {{"BM_A", 60.0}, {"BM_C", 5.0}};  // faster; new; BM_B not measured
  const Cmd c = gate({write_run(r)});
  EXPECT_EQ(c.exit, 0) << c.out;
  EXPECT_TRUE(has(c.out, "faster")) << c.out;
  EXPECT_TRUE(has(c.out, "new")) << c.out;
  EXPECT_TRUE(has(c.out, "not_measured")) << c.out;
  EXPECT_TRUE(has(c.out, "1 compared: 0 ok, 0 regression, 1 faster; 1 new, 1 not measured")) << c.out;
}

TEST_F(PerfGateTest, BaselineUnitConversion) {
  write_baseline("aaaaaaaaaaaa", "run", {{"BM_A", 100000.0}}, "ns");  // 100 us
  RunSpec r;
  r.medians = {{"BM_A", 130.0}};  // us: 1.30x
  EXPECT_EQ(gate({write_run(r)}).exit, 1);
  r.medians = {{"BM_A", 110.0}};
  EXPECT_EQ(gate({write_run(r)}).exit, 0);
}

// ---------------------------------------------------------------------------------------------
// Refusals (exit 2)
// ---------------------------------------------------------------------------------------------
TEST_F(PerfGateTest, CrossFingerprintBaselineRefused) {
  // The baseline in the run's fingerprint directory carries another fingerprint id.
  write(baseline_path("aaaaaaaaaaaa"), baseline_json("bbbbbbbbbbbb", "run", {{"BM_A", 100.0}}));
  RunSpec r;
  r.medians = {{"BM_A", 100.0}};
  const Cmd c = gate({write_run(r)});
  EXPECT_EQ(c.exit, 2) << c.out;
  EXPECT_TRUE(has(c.out, "REFUSED")) << c.out;
  EXPECT_TRUE(has(c.out, "fingerprint")) << c.out;
}

TEST_F(PerfGateTest, CrossFingerprintRunsRefused) {
  write_baseline("aaaaaaaaaaaa", "run", {{"BM_A", 100.0}});
  RunSpec a, b;
  a.medians = {{"BM_A", 100.0}};
  b.name = "other";
  b.fp = "bbbbbbbbbbbb";
  b.medians = {{"BM_A", 100.0}};
  const Cmd c = gate({write_run(a), write_run(b)});
  EXPECT_EQ(c.exit, 2) << c.out;
  EXPECT_TRUE(has(c.out, "different fingerprints")) << c.out;
}

TEST_F(PerfGateTest, LoadAboveHalfTheCoresRefused) {
  write_baseline("aaaaaaaaaaaa", "run", {{"BM_A", 100.0}});
  RunSpec r;
  r.medians = {{"BM_A", 100.0}};
  r.load_before = 8.5;  // 16 logical cores: threshold 8
  Cmd c = gate({write_run(r)});
  EXPECT_EQ(c.exit, 2) << c.out;
  EXPECT_TRUE(has(c.out, "REFUSED")) << c.out;
  EXPECT_TRUE(has(c.out, "load")) << c.out;
  r.load_before = 2.0;
  r.load_after = 9.0;  // the load after the run counts too
  c = gate({write_run(r)});
  EXPECT_EQ(c.exit, 2) << c.out;
  r.load_after = 8.0;  // at the threshold: allowed
  EXPECT_EQ(gate({write_run(r)}).exit, 0);
  r.cores = 0;  // unknown core count: the rule cannot be evaluated
  EXPECT_EQ(gate({write_run(r)}).exit, 2);
}

TEST_F(PerfGateTest, MalformedInputRefused) {
  write(dir / "bad.json", "{\"format\": \"something else\"}\n");
  EXPECT_EQ(gate({dir / "bad.json"}).exit, 2);
  EXPECT_EQ(gate({dir / "missing.json"}).exit, 2);
}

// ---------------------------------------------------------------------------------------------
// --accept seeds and updates the baseline
// ---------------------------------------------------------------------------------------------
TEST_F(PerfGateTest, NoBaselineRefusedUntilAccepted) {
  RunSpec r;
  r.medians = {{"BM_A", 110.0}, {"BM_B", 5.0}};
  const fs::path p = write_run(r);
  Cmd c = gate({p});
  EXPECT_EQ(c.exit, 2) << c.out;
  EXPECT_TRUE(has(c.out, "seed with --accept")) << c.out;
  EXPECT_FALSE(fs::exists(baseline_path("aaaaaaaaaaaa")));

  c = gate({p}, "--accept");
  EXPECT_EQ(c.exit, 0) << c.out;
  ASSERT_TRUE(fs::exists(baseline_path("aaaaaaaaaaaa")));
  const std::string b = read(baseline_path("aaaaaaaaaaaa"));
  EXPECT_TRUE(has(b, "\"epykos-baseline 1\"")) << b;
  EXPECT_TRUE(has(b, "\"aaaaaaaaaaaa\"")) << b;
  EXPECT_TRUE(has(b, "\"BM_A\"")) << b;
  EXPECT_TRUE(has(b, "\"binary\": \"build/release/bench/run_bench\"")) << b;
  EXPECT_TRUE(has(b, "\"--benchmark_repetitions=5\"")) << b;  // the run's arguments: a filtered baseline states its filter (D34)
  EXPECT_TRUE(has(c.out, "baseline updated")) << c.out;
  EXPECT_TRUE(has(c.out, "(new @abc1234)")) << c.out;

  // Now a plain gate passes, a 1.5x run fails, and accepting it prints before -> after.
  EXPECT_EQ(gate({p}).exit, 0);
  r.medians = {{"BM_A", 165.0}, {"BM_B", 5.0}};
  r.commit = "def5678";
  const fs::path p2 = write_run(r, "run2.json");
  EXPECT_EQ(gate({p2}).exit, 1);
  c = gate({p2}, "--accept");
  EXPECT_EQ(c.exit, 0) << c.out;
  EXPECT_TRUE(has(c.out, "run BM_A: 110.000 -> 165.000 us (abc1234 -> def5678, x1.500)")) << c.out;
  EXPECT_TRUE(has(c.out, "1 regression(s) accepted")) << c.out;
  EXPECT_EQ(gate({p2}).exit, 0);  // the new baseline
  EXPECT_EQ(gate({p}).exit, 0);   // and the old run is now "faster", not a failure
}

// A baseline seeded under an ad hoc `bench/run.sh --name` does not gate the default invocation of the
// same binary: the run under the derived name is refused, and the refusal names the entry (D34; the
// M2 gate round had seeded adjoint_m1_adjoint_bench's rows as run "m2").
TEST_F(PerfGateTest, NoBaselineRefusalNamesTheEntryOfTheSameBinary) {
  RunSpec adhoc;
  adhoc.name = "m2";
  adhoc.binary = "build/release/bench/adjoint_bench";
  adhoc.medians = {{"BM_A", 100.0}};
  ASSERT_EQ(gate({write_run(adhoc)}, "--accept").exit, 0);

  RunSpec derived;  // what `bench/run.sh build/release/bench/adjoint_bench` writes
  derived.name = "adjoint";
  derived.binary = adhoc.binary;
  derived.medians = {{"BM_A", 100.0}};
  const fs::path p = write_run(derived);
  Cmd c = gate({p});
  EXPECT_EQ(c.exit, 2) << c.out;
  EXPECT_TRUE(has(c.out, "no baseline for run(s) adjoint")) << c.out;
  EXPECT_TRUE(has(c.out, "the baseline holds this binary under the run name(s) m2")) << c.out;
  EXPECT_TRUE(has(c.out, "re-seed under adjoint")) << c.out;

  // A different binary under another name is not confused with it.
  RunSpec other;
  other.name = "other";
  other.medians = {{"BM_A", 100.0}};
  c = gate({write_run(other)});
  EXPECT_EQ(c.exit, 2) << c.out;
  EXPECT_FALSE(has(c.out, "holds this binary")) << c.out;

  // Re-seeded under the derived name, the default invocation is gated.
  ASSERT_EQ(gate({p}, "--accept").exit, 0);
  c = gate({p});
  EXPECT_EQ(c.exit, 0) << c.out;
  EXPECT_FALSE(has(c.out, "no baseline entry")) << c.out;
  EXPECT_TRUE(has(c.out, "1 compared: 1 ok")) << c.out;
}

// The committed results (bench/results/<id>/): every run in a baseline.json is keyed by the name
// bench/run.sh derives from its binary (basename without _bench), so the default invocation is what
// the baseline gates; its <run>.json is committed under that name, carries it, and is gated against
// the entry (a PASS or a FAIL, never a refusal). Regression test for the M2/m2-fix finding: the
// adjoint rows had been seeded as run "m2" while run.sh names the run adjoint_m1_adjoint (D34).
TEST_F(PerfGateTest, CommittedBaselinesAreKeyedByTheDerivedRunName) {
  const fs::path committed = root / "bench" / "results";
  ASSERT_TRUE(fs::is_directory(committed)) << committed;
  int baselines = 0, runs_checked = 0;
  for (const auto& e : fs::directory_iterator(committed)) {
    if (!e.is_directory() || !fs::exists(e.path() / "baseline.json")) continue;
    const std::string id = e.path().filename().string();
    ASSERT_EQ(id.size(), 12u) << e.path();  // a fingerprint directory
    ++baselines;
    // One line per run: "<key> <baseline binary> <source>", after the baseline's fingerprint id.
    Cmd c = run("python3 -c " + q(std::string("import json, sys\nb = json.load(open(sys.argv[1]))\nprint(b['fingerprint']['id'])\n"
                                              "for k, v in b['runs'].items(): print(k, v.get('binary'), v.get('source'))\n")) +
                " " + q(e.path() / "baseline.json"));
    ASSERT_EQ(c.exit, 0) << c.out;
    std::istringstream lines(c.out);
    std::string line;
    ASSERT_TRUE(std::getline(lines, line));
    EXPECT_EQ(line, id) << "baseline.json under " << e.path() << " carries fingerprint " << line;
    while (std::getline(lines, line)) {
      std::istringstream f(line);
      std::string key, binary, source;
      f >> key >> binary >> source;
      SCOPED_TRACE(e.path().string() + " run " + key);
      std::string derived = fs::path(binary).filename().string();
      if (derived.size() > 6 && derived.substr(derived.size() - 6) == "_bench") derived.resize(derived.size() - 6);
      EXPECT_EQ(key, derived) << "baseline run '" << key << "' is not the name bench/run.sh derives from " << binary
                              << ": a default `bench/run.sh " << binary << "` would be refused, not gated (D34)";
      EXPECT_EQ(source, key + ".json");
      const fs::path file = e.path() / (key + ".json");
      ASSERT_TRUE(fs::exists(file)) << "the committed results file of baseline run " << key << " is missing";
      c = run("python3 -c " + q(std::string("import json, sys\nd = json.load(open(sys.argv[1]))\nprint(d['name'], d['binary'])\n")) + " " + q(file));
      ASSERT_EQ(c.exit, 0) << c.out;
      EXPECT_EQ(c.out, key + " " + binary + "\n") << file;
      // Gated, never refused: no baseline / load / fingerprint refusal on the committed pair.
      c = run("python3 " + q(scripts / "perf_gate.py") + " --results-root " + q(committed) + " --fingerprint " + id + " --no-targets " + q(file));
      EXPECT_TRUE(c.exit == 0 || c.exit == 1) << c.out;
      EXPECT_FALSE(has(c.out, "REFUSED")) << c.out;
      EXPECT_FALSE(has(c.out, "no baseline entry for this run")) << c.out;
      EXPECT_TRUE(has(c.out, "verdict: PASS") || has(c.out, "verdict: FAIL")) << c.out;
      ++runs_checked;
    }
  }
  EXPECT_GE(baselines, 1) << "no committed baseline under " << committed;  // d448afd70180 is committed
  EXPECT_GE(runs_checked, 1);
}

// ---------------------------------------------------------------------------------------------
// Absolute targets
// ---------------------------------------------------------------------------------------------
TEST_F(PerfGateTest, AbsoluteTargetsRatioAndTime) {
  const fs::path targets = dir / "targets.json";
  write(targets,
        "{\"format\": \"epykos-targets 1\", \"targets\": [\n"
        " {\"id\": \"t_ratio\", \"milestone\": \"M1\", \"description\": \"interp <= 1.1x hand\", \"kind\": \"ratio\", \"max\": 1.1,\n"
        "  \"numerator\": {\"run\": \"interp\", \"match\": \"BM_I/tile:(1|2)\", \"pick\": \"min_median\"},\n"
        "  \"denominator\": {\"run\": \"hand\", \"benchmark\": \"BM_H\"}},\n"
        " {\"id\": \"t_time\", \"milestone\": \"M5\", \"description\": \"grid under 1 s\", \"kind\": \"time\", \"max\": 1.0, \"unit\": \"s\",\n"
        "  \"value\": {\"run\": \"mc\", \"match\": \"BM_E.*\"}}\n"
        "]}\n");
  write_baseline("aaaaaaaaaaaa", "interp", {{"BM_I/tile:1", 120.0}, {"BM_I/tile:2", 100.0}});
  RunSpec interp, hand, mc;
  interp.name = "interp";
  interp.medians = {{"BM_I/tile:1", 120.0}, {"BM_I/tile:2", 100.0}};  // best point 100
  hand.name = "hand";
  hand.unit = "ns";
  hand.medians = {{"BM_H", 95000.0}};  // 95 us -> ratio 1.053
  const fs::path pi = write_run(interp);
  const fs::path ph = write_run(hand);

  // Only the interpreter is given: the hand side is not measured -> not applicable, no failure.
  Cmd c = gate({pi}, "", targets);
  EXPECT_EQ(c.exit, 0) << c.out;
  EXPECT_TRUE(has(c.out, "t_ratio")) << c.out;
  EXPECT_TRUE(has(c.out, "not applicable")) << c.out;

  // Both given (the hand run has no baseline entry: accepted first so the gate is not refused).
  ASSERT_EQ(gate({ph}, "--accept", targets).exit, 0);
  c = gate({pi, ph}, "", targets);
  EXPECT_EQ(c.exit, 0) << c.out;
  EXPECT_TRUE(has(c.out, "= 1.053 (max 1.1): PASS")) << c.out;
  EXPECT_TRUE(has(c.out, "numerator: BM_I/tile:2 fresh")) << c.out;
  EXPECT_TRUE(has(c.out, "t_time")) << c.out;
  EXPECT_TRUE(has(c.out, "run mc not measured")) << c.out;

  // The hand side from the committed file of the fingerprint directory, the interpreter fresh.
  write(results / "aaaaaaaaaaaa" / "hand.json", run_json(hand));
  c = gate({pi}, "", targets);
  EXPECT_EQ(c.exit, 0) << c.out;
  EXPECT_TRUE(has(c.out, "denominator: BM_H committed (hand.json @abc1234)")) << c.out;

  // A slower hand kernel makes the ratio 1.25 > 1.1: the target fails (exit 1) although no self-regression.
  hand.medians = {{"BM_H", 80000.0}};
  write(results / "aaaaaaaaaaaa" / "hand.json", run_json(hand));
  c = gate({pi}, "", targets);
  EXPECT_EQ(c.exit, 1) << c.out;
  EXPECT_TRUE(has(c.out, "= 1.250 (max 1.1): FAIL")) << c.out;
  EXPECT_TRUE(has(c.out, "absolute target(s) missed: t_ratio")) << c.out;

  // A committed file that fails the load rule is not used.
  hand.load_after = 12.0;
  write(results / "aaaaaaaaaaaa" / "hand.json", run_json(hand));
  c = gate({pi}, "", targets);
  EXPECT_EQ(c.exit, 0) << c.out;
  EXPECT_TRUE(has(c.out, "not applicable")) << c.out;

  // Time target: 1200 ms > 1 s fails, 900 ms passes.
  mc.name = "mc";
  mc.unit = "ms";
  mc.medians = {{"BM_E/threads:8", 1200.0}};
  ASSERT_EQ(gate({write_run(mc)}, "--accept", targets).exit, 1);  // accepted, but the target is missed
  c = gate({write_run(mc)}, "", targets);
  EXPECT_EQ(c.exit, 1) << c.out;
  EXPECT_TRUE(has(c.out, "1.2 s (max 1 s): FAIL")) << c.out;
  mc.medians = {{"BM_E/threads:8", 900.0}};
  c = gate({write_run(mc)}, "", targets);
  EXPECT_EQ(c.exit, 0) << c.out;
  EXPECT_TRUE(has(c.out, "0.9 s (max 1 s): PASS")) << c.out;
}

TEST_F(PerfGateTest, RepositoryTargetsFileParses) {
  // bench/targets.json itself: with no runs of its own, every target is "not applicable", never an error.
  write_baseline("aaaaaaaaaaaa", "run", {{"BM_A", 100.0}});
  RunSpec r;
  r.medians = {{"BM_A", 100.0}};
  const Cmd c = gate({write_run(r)}, "", root / "bench" / "targets.json");
  EXPECT_EQ(c.exit, 0) << c.out;
  EXPECT_TRUE(has(c.out, "m1_b1")) << c.out;
  EXPECT_TRUE(has(c.out, "m1_b64")) << c.out;
  EXPECT_TRUE(has(c.out, "m5_exposure")) << c.out;
  EXPECT_FALSE(has(c.out, "malformed")) << c.out;
}

// ---------------------------------------------------------------------------------------------
// The summariser: statistics and name parsing from a raw Google Benchmark JSON
// ---------------------------------------------------------------------------------------------
TEST_F(PerfGateTest, SummariserStatisticsAndParams) {
  std::ostringstream raw;
  raw << "{\"context\": {\"date\": \"2026-09-23T10:00:00+01:00\", \"library_version\": \"v1.9.5\"}, \"benchmarks\": [\n";
  const double times[5] = {5.0, 1.0, 4.0, 2.0, 3.0};
  for (int i = 0; i < 5; ++i) {
    raw << (i ? ",\n" : "") << "{\"name\": \"BM_X/tile:128/lane_tile:1/exp:0\", \"run_name\": \"BM_X/tile:128/lane_tile:1/exp:0\", \"run_type\": \"iteration\","
        << " \"repetitions\": 5, \"repetition_index\": " << i << ", \"threads\": 1, \"iterations\": " << 100 + i << ", \"real_time\": " << times[i]
        << ", \"cpu_time\": " << times[i] << ", \"time_unit\": \"us\", \"B\": 1.0, \"rate\": " << 1.0 / times[i] << ", \"label\": \"tile 128\"}";
  }
  // An aggregate record and a second benchmark with positional arguments.
  raw << ",\n{\"name\": \"BM_X/tile:128/lane_tile:1/exp:0_mean\", \"run_type\": \"aggregate\", \"aggregate_name\": \"mean\", \"real_time\": 3.0, \"time_unit\": \"us\"}";
  raw << ",\n{\"name\": \"BM_Y/2/real_time\", \"run_type\": \"iteration\", \"repetition_index\": 0, \"iterations\": 7, \"real_time\": 10.0, \"cpu_time\": 9.0, \"time_unit\": \"ms\"}";
  raw << "\n]}\n";
  write(dir / "raw.json", raw.str());
  write(dir / "fp.json", "{\"id\": \"aaaaaaaaaaaa\", \"cpu\": \"synthetic\", \"cores_physical\": 8, \"cores_logical\": 16, \"compiler\": \"cc\", \"flags\": \"-O3\", \"load1\": 1.5}\n");

  Cmd c = run("python3 " + q(scripts / "bench_results.py") + " summarise --raw " + q(dir / "raw.json") + " --name x --binary build/release/bench/x_bench --preset release --fingerprint-before " +
              q(dir / "fp.json") + " --load-after 2.5 --commit 0123456789abcdef --branch b --dirty 1 --arg=--benchmark_repetitions=5 --quiet --out " + q(dir / "x.json"));
  ASSERT_EQ(c.exit, 0) << c.out;
  ASSERT_TRUE(fs::exists(dir / "x.json"));

  // Read the numbers back through python (no JSON parser in the test).
  c = run("python3 -c " + q("import json; d = json.load(open(\"" + (dir / "x.json").string() + "\"))\n"
                           "b = d['benchmarks']['BM_X/tile:128/lane_tile:1/exp:0']\n"
                           "print(b['n'], b['min'], b['median'], b['p90'], b['max'], b['mean'], b['unit'], b['evaluations'])\n"
                           "print(sorted(b['params'].items()), sorted(b['counters'].items()), b['label'])\n"
                           "y = d['benchmarks']['BM_Y/2/real_time']\n"
                           "print(y['n'], y['median'], y['unit'], sorted(y['params'].items()))\n"
                           "print(d['load']['before'], d['load']['after'], d['load']['threshold'], d['load']['within_threshold'])\n"
                           "print(d['git']['commit_short'], d['git']['dirty'], d['google_benchmark']['repetitions'], d['date'], len(d['benchmarks']))\n"));
  ASSERT_EQ(c.exit, 0) << c.out;
  // Sorted [1,2,3,4,5]: min 1, median 3, p90 = the ceil(0.9*5) = 5th smallest = 5, mean 3; iterations 100..104 sum 510.
  EXPECT_TRUE(has(c.out, "5 1.0 3.0 5.0 5.0 3.0 us 510\n")) << c.out;
  // Params from the name; the constant counter B kept, the varying rate dropped.
  EXPECT_TRUE(has(c.out, "[('exp', 0), ('lane_tile', 1), ('tile', 128)] [('B', 1)] tile 128\n")) << c.out;
  // Positional argument -> arg0; the real_time modifier is not a parameter; the aggregate record is ignored.
  EXPECT_TRUE(has(c.out, "1 10.0 ms [('arg0', 2)]\n")) << c.out;
  EXPECT_TRUE(has(c.out, "1.5 2.5 8.0 True\n")) << c.out;
  EXPECT_TRUE(has(c.out, "0123456 True 5 2026-09-23T10:00:00+01:00 2\n")) << c.out;

  // Even n: the median is the mean of the two middle values (4 repetitions: 1 2 4 5 -> 3.0); p90 = 4th smallest.
  std::string raw4 = "{\"context\": {}, \"benchmarks\": [";
  const double t4[4] = {5.0, 1.0, 4.0, 2.0};
  for (int i = 0; i < 4; ++i)
    raw4 += std::string(i ? "," : "") + "{\"name\": \"BM_Z\", \"run_type\": \"iteration\", \"iterations\": 1, \"real_time\": " + std::to_string(t4[i]) + ", \"cpu_time\": 1, \"time_unit\": \"us\"}";
  raw4 += "]}";
  write(dir / "raw4.json", raw4);
  c = run("python3 " + q(scripts / "bench_results.py") + " summarise --raw " + q(dir / "raw4.json") + " --name z --binary z --preset release --fingerprint-before " +
          q(dir / "fp.json") + " --out " + q(dir / "z.json"));
  ASSERT_EQ(c.exit, 0) << c.out;
  EXPECT_TRUE(has(c.out, "BM_Z")) << c.out;
  EXPECT_TRUE(has(c.out, "4        1.000        3.000        5.000  us")) << c.out;
}

// ---------------------------------------------------------------------------------------------
// bench/run.sh end to end on the scaffold benchmark (skipped when the binary is not next to the tests)
// ---------------------------------------------------------------------------------------------
TEST_F(PerfGateTest, RunScriptWritesAResultsFile) {
  // ctest runs the tests in build/<preset>/tests; the benchmarks are in build/<preset>/bench.
  const fs::path bin = fs::current_path().parent_path() / "bench" / "scaffold_bench";
  if (!fs::exists(bin)) GTEST_SKIP() << "scaffold_bench not found at " << bin << " (benchmarks not built)";
  if (!fs::exists(fs::current_path().parent_path() / "epykos_flags.txt")) GTEST_SKIP() << "not run from a preset build tree";
  const Cmd c = run(q(root / "bench" / "run.sh") + " --results-root " + q(results) + " --ignore-load --repetitions 3 --min-time 0.01s " + q(bin) +
                    " --benchmark_filter=BM_ScaffoldVersion");
  ASSERT_EQ(c.exit, 0) << c.out;
  // One fingerprint directory with scaffold.json and the raw output under tmp/.
  std::vector<fs::path> files;
  for (const auto& e : fs::recursive_directory_iterator(results))
    if (e.is_regular_file() && e.path().filename() == "scaffold.json") files.push_back(e.path());
  ASSERT_EQ(files.size(), 1u) << c.out;
  const std::string text = read(files[0]);
  EXPECT_TRUE(has(text, "\"format\": \"epykos-bench 1\"")) << text;
  EXPECT_TRUE(has(text, "\"name\": \"scaffold\"")) << text;
  EXPECT_TRUE(has(text, "\"BM_ScaffoldVersion\"")) << text;
  EXPECT_TRUE(has(text, "\"preset\": \"release\"") || has(text, "\"preset\": \"reference\"") || has(text, "\"preset\": \"debug\"")) << text;
  EXPECT_TRUE(has(text, "\"--benchmark_filter=BM_ScaffoldVersion\"")) << text;
  EXPECT_TRUE(fs::exists(files[0].parent_path() / "tmp" / "scaffold.gbench.json")) << c.out;
  EXPECT_EQ(files[0].parent_path().filename().string().size(), 12u) << files[0];  // the fingerprint id
}

}  // namespace
