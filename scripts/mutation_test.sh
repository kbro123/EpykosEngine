#!/usr/bin/env bash
# scripts/mutation_test.sh — the mutation gate (DESIGN.md §11, D32): every registered mutant must
# fail at least one GATE test.
#
# What it does
#   1. configures and builds the `mutation` preset once (the reference preset with
#      EPYKOS_MUTATIONS=ON: the mutants of include/epykos/mutation/mutation.hpp compiled into
#      libepykos, one selectable per process through EPYKOS_MUTANT);
#   2. reads the registry from the registry test (tests/mutation/registry_test.cpp prints it),
#      so the list the harness runs is the list the binary carries — nothing can be skipped;
#   3. selects the gate tests: the ctest entries whose name matches EPYKOS_GATE_REGEX, by default
#      the round-trip identity tests, the E0 differential tests, any verify tests and the adjoint
#      tolerance gates (adjoint vs finite differences and linearity, vs forward mode: the tests
#      named *_adjoint_test and *_vs_dual_test)
#      ('roundtrip|differential|verify|_adjoint_test$|_vs_dual_test$|_e0_test$'), never a test
#      under tests/mutation/. The passes' own unit tests are not gates: a mutant caught only by a
#      test written for it has not been caught;
#   4. runs the gates with no mutant selected (they must all pass, or the harness is meaningless);
#   5. for every mutant, runs the gates with EPYKOS_MUTANT=<name> and records which ones fail
#      (a crash, an exception or a timeout counts as a failure, and is labelled as such);
#   6. prints the table mutant -> caught by, and exits non-zero if any mutant survived.
#
# Usage: scripts/mutation_test.sh [--no-build] [--jobs N] [--timeout SECONDS] [mutant ...]
#   --no-build        skip configure + build (the mutation preset must already be built)
#   --jobs N          ctest parallelism (default: EPYKOS_MUTATION_JOBS, else 4)
#   --timeout S       per-test timeout in seconds (default 900; a mutant may loop)
#   mutant ...        run only these mutants (they must be registered); default: all
# Environment: EPYKOS_GATE_REGEX overrides the gate selection (an extended regex on ctest names).
# Exit status: 0 every mutant caught; 1 a mutant survived or the baseline failed; 2 setup error.
# Logs: build/mutation/mutation/<mutant>.log (full ctest output per round), baseline.log.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
PRESET=mutation
BUILD_DIR="$ROOT/build/$PRESET"
LOG_DIR="$BUILD_DIR/mutation"
GATE_REGEX="${EPYKOS_GATE_REGEX:-roundtrip|differential|verify|_adjoint_test$|_vs_dual_test$|_e0_test$}"
JOBS="${EPYKOS_MUTATION_JOBS:-4}"
TIMEOUT=900
BUILD=1
declare -a ONLY=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) BUILD=0 ;;
    --jobs) JOBS="$2"; shift ;;
    --timeout) TIMEOUT="$2"; shift ;;
    -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 0 ;;
    --*) echo "mutation_test: unknown option $1" >&2; exit 2 ;;
    *) ONLY+=("$1") ;;
  esac
  shift
done

cd "$ROOT"
mkdir -p "$LOG_DIR"

# ---- 1. build --------------------------------------------------------------------------------------
if [[ $BUILD -eq 1 ]]; then
  echo "== configure + build preset '$PRESET'"
  cmake --preset "$PRESET"
  cmake --build --preset "$PRESET"
fi
if ! grep -q '^EPYKOS_MUTATIONS:BOOL=ON$' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
  echo "mutation_test: $BUILD_DIR is not a mutation build (EPYKOS_MUTATIONS is not ON)" >&2
  exit 2
fi

# ---- 2. the registry, from the binary -------------------------------------------------------------
# ctest -V prints every line of the test's output prefixed with "<n>: ".
unset EPYKOS_MUTANT
registry_out="$(ctest --test-dir "$BUILD_DIR" -R '^mutation_registry_test$' -V 2>&1 || true)"
if ! grep -qE '^[0-9]+: mutants_compiled_in 1$' <<<"$registry_out"; then
  echo "mutation_test: the registry test does not report a mutation build:" >&2
  echo "$registry_out" >&2
  exit 2
fi
if ! grep -qE 'tests passed, 0 tests failed|100% tests passed' <<<"$registry_out"; then
  echo "mutation_test: the registry test fails with no mutant selected:" >&2
  echo "$registry_out" >&2
  exit 2
fi
declare -a REGISTRY=()
while IFS= read -r line; do REGISTRY+=("$line"); done < <(sed -nE 's/^[0-9]+: mutant ([a-z0-9_.]+)$/\1/p' <<<"$registry_out")
if [[ ${#REGISTRY[@]} -eq 0 ]]; then
  echo "mutation_test: the registry is empty" >&2
  exit 2
fi
declare -a MUTANTS=()
if [[ ${#ONLY[@]} -gt 0 ]]; then
  for m in "${ONLY[@]}"; do
    found=0
    for r in "${REGISTRY[@]}"; do [[ "$r" == "$m" ]] && found=1; done
    if [[ $found -eq 0 ]]; then
      echo "mutation_test: '$m' is not a registered mutant (registry: ${REGISTRY[*]})" >&2
      exit 2
    fi
    MUTANTS+=("$m")
  done
else
  MUTANTS=("${REGISTRY[@]}")
fi

# ---- 3. the gate tests ------------------------------------------------------------------------------
declare -a ALL_TESTS=()
while IFS= read -r line; do ALL_TESTS+=("$line"); done < <(ctest --test-dir "$BUILD_DIR" -N | sed -nE 's/^ *Test +#[0-9]+: +([^ ]+).*/\1/p')
declare -a GATES=()
for t in "${ALL_TESTS[@]}"; do
  [[ "$t" == mutation_* ]] && continue
  if grep -qE -- "$GATE_REGEX" <<<"$t"; then GATES+=("$t"); fi
done
if [[ ${#GATES[@]} -eq 0 ]]; then
  echo "mutation_test: no gate test matches '$GATE_REGEX' among: ${ALL_TESTS[*]}" >&2
  exit 2
fi
gate_ctest_regex="^($(IFS='|'; echo "${GATES[*]}"))$"
echo "== registry: ${#REGISTRY[@]} mutant(s): ${REGISTRY[*]}"
echo "== gates (${#GATES[@]}, regex '$GATE_REGEX'): ${GATES[*]}"
echo "== ctest -j $JOBS, timeout ${TIMEOUT}s per test; logs in $LOG_DIR"

# run_gates <logfile> <mutant|"">: runs the gate set with EPYKOS_MUTANT set to the mutant (unset
# for ""), writes the ctest output to the log and prints "name<TAB>status" per test (status
# "Passed", or ctest's failure label: "Failed", "Exception: SegFault", "Timeout", "Not Run", ...).
run_gates() {
  local log="$1" selected="$2"
  if [[ -n "$selected" ]]; then export EPYKOS_MUTANT="$selected"; else unset EPYKOS_MUTANT; fi
  ctest --test-dir "$BUILD_DIR" -R "$gate_ctest_regex" -j "$JOBS" --timeout "$TIMEOUT" --output-on-failure >"$log" 2>&1 || true
  unset EPYKOS_MUTANT
  awk '
    /^ *[0-9]+\/[0-9]+ Test +#[0-9]+:/ {
      name = $0; sub(/^ *[0-9]+\/[0-9]+ Test +#[0-9]+: +/, "", name); sub(/ .*/, "", name);
      st = $0; sub(/^[^.]*\.\.+ */, "", st); sub(/ +[0-9.]+ sec.*$/, "", st); sub(/^\*+/, "", st);
      print name "\t" st
    }' "$log"
}

# ---- 4. baseline: no mutant -------------------------------------------------------------------------
echo
echo "== baseline (no mutant selected)"
baseline="$(run_gates "$LOG_DIR/baseline.log" "")"
baseline_failed="$(awk -F'\t' '$2 != "Passed" {print $1 " (" $2 ")"}' <<<"$baseline")"
n_baseline="$(wc -l <<<"$baseline" | tr -d ' ')"
if [[ -n "$baseline_failed" || "$n_baseline" -ne ${#GATES[@]} ]]; then
  echo "mutation_test: the gates do not pass with no mutant selected; the harness cannot judge mutants:" >&2
  echo "$baseline_failed" >&2
  echo "(see $LOG_DIR/baseline.log)" >&2
  exit 1
fi
echo "   ${#GATES[@]}/${#GATES[@]} gates pass"

# ---- 5. every mutant --------------------------------------------------------------------------------
declare -a ROW_MUTANT=() ROW_RESULT=() ROW_CAUGHT=()
survivors=0
for m in "${MUTANTS[@]}"; do
  echo
  echo "== mutant $m"
  results="$(run_gates "$LOG_DIR/$m.log" "$m")"
  caught="$(awk -F'\t' '$2 != "Passed" {printf "%s%s (%s)", (n++ ? ", " : ""), $1, $2}' <<<"$results")"
  n_results="$(wc -l <<<"$results" | tr -d ' ')"
  if [[ "$n_results" -ne ${#GATES[@]} ]]; then
    caught="${caught:+$caught, }[ctest reported $n_results of ${#GATES[@]} gates: see $LOG_DIR/$m.log]"
  fi
  if [[ -z "$caught" ]]; then
    echo "   SURVIVED: no gate failed"
    ROW_RESULT+=("SURVIVED")
    ROW_CAUGHT+=("-")
    survivors=$((survivors + 1))
  else
    echo "   caught by: $caught"
    ROW_RESULT+=("caught")
    ROW_CAUGHT+=("$caught")
  fi
  ROW_MUTANT+=("$m")
done

# ---- 6. the table -----------------------------------------------------------------------------------
echo
echo "== mutation gate: ${#MUTANTS[@]} mutant(s), ${#GATES[@]} gate test(s)"
printf '   %-30s %-9s %s\n' "mutant" "result" "caught by"
for i in "${!ROW_MUTANT[@]}"; do
  printf '   %-30s %-9s %s\n' "${ROW_MUTANT[$i]}" "${ROW_RESULT[$i]}" "${ROW_CAUGHT[$i]}"
done
if [[ $survivors -gt 0 ]]; then
  echo
  echo "mutation_test: $survivors mutant(s) SURVIVED the gates — a gap in the gates, not a job for a mutant-specific test" >&2
  exit 1
fi
echo "   every mutant caught"
