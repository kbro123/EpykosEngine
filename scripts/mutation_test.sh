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
#   4. runs the WHOLE gate set with no mutant selected (they must all pass, or the harness is
#      meaningless). This step is never shortened: it is what makes a later single-gate failure
#      attributable to the mutant and to nothing else;
#   5. FAST PATH (D70): catching a mutant needs exactly ONE failing gate, so for every mutant that
#      scripts/mutation_catchers.tsv records a catching gate for, runs just that gate with
#      EPYKOS_MUTANT=<name>. Several mutants run at once (--outer), each its own process with its
#      own EPYKOS_MUTANT;
#   6. FALLBACK: every mutant the fast path did not catch — no recorded gate, a recorded gate that
#      is not a gate test any more, or a recorded gate that no longer fails — then gets the ENTIRE
#      gate set, exactly as before the fast path existed. A stale or missing map therefore costs
#      time, never correctness: no mutant is called uncaught until every gate has been run at it;
#   7. prints the table mutant -> caught by, says how many mutants the recorded gate caught and how
#      many needed the fallback, and exits non-zero if any mutant survived.
#
# When to run --full (the whole cross-product: every gate against every mutant)
#   * after adding a mutant — that is how the new mutant finds its catching gate;
#   * after adding, renaming or deleting a gate test, or changing EPYKOS_GATE_REGEX;
#   * whenever a run reports a non-zero fallback count: the map has drifted and is costing time;
#   * periodically, to notice a gate that catches nothing and a mutant whose ONLY catcher is a
#     single test. --full is the only run that can see either, because it is the only run that asks
#     every gate about every mutant. It rewrites scripts/mutation_catchers.tsv; commit the result.
#   A --full run costs what this script cost before the fast path existed (of the order of an hour
#   on a CI runner). An ordinary run does not, which is the whole point of the map.
#
# Usage: scripts/mutation_test.sh [--no-build] [--jobs N] [--outer N] [--timeout SECONDS]
#                                 [--full] [--no-map] [--map FILE] [mutant ...]
#   --no-build        skip configure + build (the mutation preset must already be built)
#   --jobs N          ctest parallelism WITHIN one whole-gate-set run (EPYKOS_MUTATION_JOBS, else 4)
#   --outer N         how many mutants the fast path runs at once, each at -j 1 (default: --jobs, so
#                     the fast path uses the same core budget the caller already asked for)
#   --timeout S       per-test timeout in seconds (default 900; a mutant may loop)
#   --full            no fast path: run every gate against every mutant and REWRITE the map file
#   --no-map          ignore the map this run (every mutant takes the fallback); writes nothing
#   --map FILE        use FILE instead of scripts/mutation_catchers.tsv
#   mutant ...        run only these mutants (they must be registered); default: all
# Environment: EPYKOS_GATE_REGEX overrides the gate selection (an extended regex on ctest names).
# Exit status: 0 every mutant caught; 1 a mutant survived or the baseline failed; 2 setup error.
# Logs: build/mutation/mutation/<mutant>.log (ctest output of a whole-gate-set round),
#       fast.<mutant>.log (the single recorded gate), baseline.log.
set -euo pipefail

# ---- 0. the fast-path worker ------------------------------------------------------------------------
# A re-exec of this script, one per mutant, run concurrently by xargs in step 5. Handled before every
# other thing so a worker does no setup: it looks its own gate up in the plan the parent wrote, runs
# that one gate under its own EPYKOS_MUTANT, and writes its own log.
# It always exits 0. The parent judges the mutant by PARSING THE LOG and never by this exit status,
# because ctest exits non-zero both for "the gate failed" (the mutant is caught) and for "no such
# test" (a stale map entry) — reading the exit status would turn the second into a false pass.
if [[ "${1:-}" == "--gate-worker" ]]; then
  shift
  w_build_dir="$1"; w_log_dir="$2"; w_timeout="$3"; w_plan="$4"; w_mutant="$5"
  w_gate="$(awk -F'\t' -v m="$w_mutant" '$1 == m {print $2; exit}' "$w_plan")"
  [[ -n "$w_gate" ]] || exit 0   # no plan line: the parent sees no log and falls back
  EPYKOS_MUTANT="$w_mutant" ctest --test-dir "$w_build_dir" -R "^${w_gate}\$" -j 1 \
    --timeout "$w_timeout" --output-on-failure >"$w_log_dir/fast.$w_mutant.log" 2>&1 || true
  exit 0
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
SELF="$ROOT/scripts/$(basename "${BASH_SOURCE[0]}")"
PRESET=mutation
BUILD_DIR="$ROOT/build/$PRESET"
LOG_DIR="$BUILD_DIR/mutation"
GATE_REGEX="${EPYKOS_GATE_REGEX:-roundtrip|differential|verify|_adjoint_test$|_vs_dual_test$|_e0_test$}"
JOBS="${EPYKOS_MUTATION_JOBS:-4}"
OUTER=""
TIMEOUT=900
BUILD=1
FULL=0
USE_MAP=1
MAP_FILE="$ROOT/scripts/mutation_catchers.tsv"
declare -a ONLY=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) BUILD=0 ;;
    --jobs) JOBS="$2"; shift ;;
    --outer) OUTER="$2"; shift ;;
    --timeout) TIMEOUT="$2"; shift ;;
    --full) FULL=1 ;;
    --no-map) USE_MAP=0 ;;
    --map) MAP_FILE="$2"; shift ;;
    -h|--help) sed -n '2,56p' "${BASH_SOURCE[0]}"; exit 0 ;;
    --*) echo "mutation_test: unknown option $1" >&2; exit 2 ;;
    *) ONLY+=("$1") ;;
  esac
  shift
done
[[ -n "$OUTER" ]] || OUTER="$JOBS"
if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [[ "$JOBS" -lt 1 ]]; then echo "mutation_test: --jobs must be a positive integer" >&2; exit 2; fi
if ! [[ "$OUTER" =~ ^[0-9]+$ ]] || [[ "$OUTER" -lt 1 ]]; then echo "mutation_test: --outer must be a positive integer" >&2; exit 2; fi
[[ $FULL -eq 1 ]] && USE_MAP=0

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

# is_gate <name>: true when <name> is one of the gate tests selected above.
is_gate() {
  local want="$1" g
  for g in "${GATES[@]}"; do [[ "$g" == "$want" ]] && return 0; done
  return 1
}

# parse_ctest_log <logfile>: prints "name<TAB>status<TAB>seconds" per test ctest reported (status
# "Passed", or ctest's failure label: "Failed", "Exception: SegFault", "Timeout", "Not Run", ...).
# Prints NOTHING when ctest reported no test at all — which is exactly how a stale map entry naming
# a gate that no longer exists reads, and is why the fast path treats "no line" as "not caught".
parse_ctest_log() {
  [[ -r "$1" ]] || return 0
  awk '
    /^ *[0-9]+\/[0-9]+ Test +#[0-9]+:/ {
      name = $0; sub(/^ *[0-9]+\/[0-9]+ Test +#[0-9]+: +/, "", name); sub(/ .*/, "", name);
      st = $0; sub(/^[^.]*\.\.+ */, "", st); sub(/ +[0-9.]+ sec.*$/, "", st); sub(/^\*+/, "", st);
      dur = $(NF - 1); if (dur !~ /^[0-9]+(\.[0-9]+)?$/) dur = "999999";
      print name "\t" st "\t" dur
    }' "$1"
}

# run_gates <logfile> <mutant|"">: runs the WHOLE gate set with EPYKOS_MUTANT set to the mutant
# (unset for ""), writes the ctest output to the log and prints "name<TAB>status<TAB>seconds".
run_gates() {
  local log="$1" selected="$2"
  if [[ -n "$selected" ]]; then export EPYKOS_MUTANT="$selected"; else unset EPYKOS_MUTANT; fi
  ctest --test-dir "$BUILD_DIR" -R "$gate_ctest_regex" -j "$JOBS" --timeout "$TIMEOUT" --output-on-failure >"$log" 2>&1 || true
  unset EPYKOS_MUTANT
  parse_ctest_log "$log"
}

# caught_list <results>: "gate (Status), gate (Status), ..." over every non-Passed gate, or empty.
caught_list() {
  awk -F'\t' '$2 != "Passed" {printf "%s%s (%s)", (n++ ? ", " : ""), $1, $2}' <<<"$1"
}

# ---- 4. baseline: no mutant -------------------------------------------------------------------------
# Always the whole gate set. The fast path below trusts that a gate failing under a mutant fails
# BECAUSE of that mutant; that is only sound if every gate passes here first.
echo
echo "== baseline (no mutant selected)"
baseline="$(run_gates "$LOG_DIR/baseline.log" "")"
baseline_failed="$(awk -F'\t' '$2 != "Passed" {print $1 " (" $2 ")"}' <<<"$baseline")"
n_baseline="$(grep -c . <<<"$baseline" || true)"
if [[ -n "$baseline_failed" || "$n_baseline" -ne ${#GATES[@]} ]]; then
  echo "mutation_test: the gates do not pass with no mutant selected; the harness cannot judge mutants:" >&2
  echo "$baseline_failed" >&2
  echo "(see $LOG_DIR/baseline.log)" >&2
  exit 1
fi
echo "   all ${#GATES[@]} gates pass"

# ---- 5. fast path: one recorded gate per mutant -----------------------------------------------------
# Parallel arrays indexed together — bash 3.2 (macOS) has no associative arrays.
declare -a ROW_MUTANT=() ROW_RESULT=() ROW_VIA=() ROW_CAUGHT=()
declare -a PENDING=() PENDING_WHY=()
declare -a FAST_MUTANT=() FAST_GATE=()
fast_hits=0
fb_no_record=0
fb_stale_gate=0
fb_missed=0

if [[ $USE_MAP -eq 1 && -r "$MAP_FILE" ]]; then
  for m in "${MUTANTS[@]}"; do
    g="$(awk -F'\t' -v m="$m" '$1 !~ /^#/ && $1 == m {print $2; exit}' "$MAP_FILE")"
    if [[ -z "$g" ]]; then
      PENDING+=("$m"); PENDING_WHY+=("no recorded gate"); fb_no_record=$((fb_no_record + 1))
    elif ! is_gate "$g"; then
      PENDING+=("$m"); PENDING_WHY+=("recorded gate '$g' is not a gate test any more")
      fb_stale_gate=$((fb_stale_gate + 1))
    else
      FAST_MUTANT+=("$m"); FAST_GATE+=("$g")
    fi
  done
else
  why="no map file at $MAP_FILE"
  [[ $FULL -eq 1 ]] && why="--full: the whole cross-product"
  [[ $FULL -eq 0 && $USE_MAP -eq 0 ]] && why="--no-map"
  for m in "${MUTANTS[@]}"; do
    PENDING+=("$m"); PENDING_WHY+=("$why")
    [[ $FULL -eq 1 ]] || fb_no_record=$((fb_no_record + 1))
  done
fi

declare -a FAST_CAUGHT_M=() FAST_CAUGHT_TXT=()
if [[ ${#FAST_MUTANT[@]} -gt 0 ]]; then
  echo
  echo "== fast path: ${#FAST_MUTANT[@]} mutant(s) with a recorded catching gate, --outer $OUTER at -j 1 each"
  rm -f "$LOG_DIR"/fast.*.log
  : > "$LOG_DIR/fast_plan.tsv"
  i=0
  while [[ $i -lt ${#FAST_MUTANT[@]} ]]; do
    printf '%s\t%s\n' "${FAST_MUTANT[$i]}" "${FAST_GATE[$i]}" >> "$LOG_DIR/fast_plan.tsv"
    i=$((i + 1))
  done
  # One worker process per mutant, OUTER of them at a time. The worker looks its own gate up in the
  # plan, so xargs only has to substitute the mutant name (registry names are [a-z0-9_.]: no blanks,
  # no quotes, nothing xargs can mangle).
  # `|| true`: if xargs itself fails, every mutant simply has no fast-path log, so all of them fall
  # through to the whole gate set below. A broken fast path must cost time, never correctness.
  cut -f1 "$LOG_DIR/fast_plan.tsv" \
    | xargs -P "$OUTER" -I MUTANT "$SELF" --gate-worker "$BUILD_DIR" "$LOG_DIR" "$TIMEOUT" \
        "$LOG_DIR/fast_plan.tsv" MUTANT || true

  i=0
  while [[ $i -lt ${#FAST_MUTANT[@]} ]]; do
    m="${FAST_MUTANT[$i]}"; g="${FAST_GATE[$i]}"
    res="$(parse_ctest_log "$LOG_DIR/fast.$m.log")"
    hit="$(awk -F'\t' -v g="$g" '$1 == g && $2 != "Passed" {print $2; exit}' <<<"$res")"
    if [[ -n "$hit" ]]; then
      FAST_CAUGHT_M+=("$m"); FAST_CAUGHT_TXT+=("$g ($hit)")
      fast_hits=$((fast_hits + 1))
    else
      PENDING+=("$m"); PENDING_WHY+=("recorded gate '$g' did not fail")
      fb_missed=$((fb_missed + 1))
    fi
    i=$((i + 1))
  done
  echo "   ${fast_hits} caught by the recorded gate, ${fb_missed} fell through to the full gate set"
fi

# ---- 6. fallback: the whole gate set, for every mutant the fast path did not settle -----------------
declare -a FULLSET_M=() FULLSET_TXT=()
survivors=0
if [[ ${#PENDING[@]} -gt 0 ]]; then
  echo
  echo "== full gate set: ${#PENDING[@]} mutant(s) against all ${#GATES[@]} gates"
fi
i=0
while [[ $i -lt ${#PENDING[@]} ]]; do
  m="${PENDING[$i]}"
  echo
  echo "== mutant $m (${PENDING_WHY[$i]})"
  results="$(run_gates "$LOG_DIR/$m.log" "$m")"
  caught="$(caught_list "$results")"
  n_results="$(grep -c . <<<"$results" || true)"
  if [[ "$n_results" -ne ${#GATES[@]} ]]; then
    caught="${caught:+$caught, }[ctest reported $n_results of ${#GATES[@]} gates: see $LOG_DIR/$m.log]"
  fi
  if [[ -z "$caught" ]]; then
    echo "   SURVIVED: no gate failed"
    FULLSET_M+=("$m"); FULLSET_TXT+=("")
    survivors=$((survivors + 1))
  else
    echo "   caught by: $caught"
    FULLSET_M+=("$m"); FULLSET_TXT+=("$caught")
  fi
  i=$((i + 1))
done

# ---- 7. the table ------------------------------------------------------------------------------------
# Rebuilt in registry order, whichever phase settled each mutant.
for m in "${MUTANTS[@]}"; do
  txt=""; via=""
  i=0
  while [[ $i -lt ${#FAST_CAUGHT_M[@]} ]]; do
    [[ "${FAST_CAUGHT_M[$i]}" == "$m" ]] && { txt="${FAST_CAUGHT_TXT[$i]}"; via="recorded"; break; }
    i=$((i + 1))
  done
  if [[ -z "$via" ]]; then
    i=0
    while [[ $i -lt ${#FULLSET_M[@]} ]]; do
      [[ "${FULLSET_M[$i]}" == "$m" ]] && { txt="${FULLSET_TXT[$i]}"; via="full set"; break; }
      i=$((i + 1))
    done
  fi
  ROW_MUTANT+=("$m"); ROW_VIA+=("$via")
  if [[ -n "$txt" ]]; then ROW_RESULT+=("caught"); ROW_CAUGHT+=("$txt")
  else ROW_RESULT+=("SURVIVED"); ROW_CAUGHT+=("-"); fi
done

echo
echo "== mutation gate: ${#MUTANTS[@]} mutant(s) over ${#GATES[@]} gate test(s)"
printf '   %-44s %-9s %-9s %s\n' "mutant" "result" "via" "caught by"
i=0
while [[ $i -lt ${#ROW_MUTANT[@]} ]]; do
  printf '   %-44s %-9s %-9s %s\n' "${ROW_MUTANT[$i]}" "${ROW_RESULT[$i]}" "${ROW_VIA[$i]}" "${ROW_CAUGHT[$i]}"
  i=$((i + 1))
done

# ---- 8. the map: report drift, and rewrite it under --full -------------------------------------------
fb_total=$((fb_no_record + fb_stale_gate + fb_missed))
echo
if [[ $FULL -eq 1 ]]; then
  echo "== --full: every one of the ${#GATES[@]} gates was run against every one of the ${#MUTANTS[@]} mutants; no fast path was used"
elif [[ $USE_MAP -eq 0 ]]; then
  echo "== --no-map: the recorded catching gates were ignored; all ${#MUTANTS[@]} mutant(s) took the full ${#GATES[@]}-gate set"
else
  echo "== fast path: ${fast_hits} mutant(s) caught by their recorded gate, ${fb_total} needed the full ${#GATES[@]}-gate fallback"
  if [[ $fb_total -gt 0 ]]; then
    echo "   of those ${fb_total}: ${fb_no_record} had no recorded gate, ${fb_stale_gate} named a gate that is no longer a gate test, ${fb_missed} named a gate that no longer fails"
    echo "   a rising fallback count means $MAP_FILE is drifting: re-run with --full and commit the result"
  fi
fi

if [[ $FULL -eq 1 && $survivors -eq 0 ]]; then
  if [[ ${#ONLY[@]} -gt 0 ]]; then
    echo "== --full was given a mutant list, so $MAP_FILE is left alone (it would lose every other mutant's line)"
  else
    # The cheapest catching gate per mutant, ties broken by name, so the file is stable run to run.
    # Durations come from this machine and this compiler; another toolchain may pick a different
    # (equally valid) catcher, which costs a regenerated file and never correctness.
    tmp="$LOG_DIR/catchers.tsv.new"
    {
      echo "# scripts/mutation_catchers.tsv — the recorded catching gate per mutant (D70)."
      echo "#"
      echo "# GENERATED by \`scripts/mutation_test.sh --full\`; do not hand-maintain the counts."
      echo "# An ordinary mutation_test.sh run tries a mutant's recorded gate FIRST and stops as soon"
      echo "# as that gate fails, instead of running the whole gate set at every mutant. A line that is"
      echo "# missing, stale or simply wrong only costs time: the mutant then gets the entire gate set"
      echo "# before the harness will call it uncaught, so this file can never turn a survivor into a"
      echo "# pass. It is harness metadata, not a fixture and not an input to the engine."
      echo "#"
      echo "# catchers = how many gates failed against that mutant in the --full run that wrote this"
      echo "# line. A 1 means that gate is the mutant's single point of failure: if it is ever weakened"
      echo "# or deleted, the mutant is not covered by anything else."
      echo "#"
      echo "# mutant<TAB>gate<TAB>catchers"
      for m in "${MUTANTS[@]}"; do
        log="$LOG_DIR/$m.log"
        [[ -r "$log" ]] || continue
        res="$(parse_ctest_log "$log")"
        n_catch="$(awk -F'\t' '$2 != "Passed"' <<<"$res" | grep -c . || true)"
        # `|| true`: head closes the pipe, sort takes SIGPIPE, and pipefail would abort the run.
        best="$(awk -F'\t' '$2 != "Passed" {printf "%012.3f\t%s\n", $3, $1}' <<<"$res" | sort | head -1 | cut -f2 || true)"
        [[ -n "$best" ]] && printf '%s\t%s\t%s\n' "$m" "$best" "$n_catch"
      done
    } > "$tmp"
    mv "$tmp" "$MAP_FILE"
    echo "== wrote $MAP_FILE with $(grep -cv '^#' "$MAP_FILE" || true) mutant line(s); commit it"
    singles="$(awk -F'\t' '$1 !~ /^#/ && $3 == 1 {print "   " $1 "  ->  " $2}' "$MAP_FILE")"
    n_singles="$(grep -c . <<<"$singles" || true)"
    if [[ -n "$singles" ]]; then
      echo "== ${n_singles} mutant(s) of the ${#MUTANTS[@]}-mutant registry are caught by exactly ONE gate;"
      echo "   that gate is the mutant's single point of failure — weaken or delete it and nothing covers the mutant:"
      echo "$singles"
    fi
    # A gate that catches NO mutant is not a defect, but --full is the only run that can see it:
    # every mutant's whole-gate-set log is on disk here, so collect the gates that failed at least
    # once (one pass over the logs) and report the gates that never appear in it.
    ever="$LOG_DIR/gates_that_caught_something.txt"
    for m in "${MUTANTS[@]}"; do
      [[ -r "$LOG_DIR/$m.log" ]] || continue
      parse_ctest_log "$LOG_DIR/$m.log" | awk -F'\t' '$2 != "Passed" {print $1}'
    done | sort -u > "$ever"
    never=""
    for g in "${GATES[@]}"; do
      grep -qx -- "$g" "$ever" || never="$never   $g"$'\n'
    done
    if [[ -n "$never" ]]; then
      n_never="$(grep -c . <<<"$never" || true)"
      echo "== ${n_never} gate test(s) of the ${#GATES[@]} in the gate set caught no mutant in this run."
      echo "   Not a defect on its own — a gate may guard a property no registered mutant breaks — but"
      echo "   it is also what a gate that has quietly stopped testing anything looks like:"
      printf '%s' "$never"
    fi
  fi
fi

if [[ $survivors -gt 0 ]]; then
  echo
  echo "mutation_test: $survivors mutant(s) SURVIVED the gates — a gap in the gates, not a job for a mutant-specific test" >&2
  exit 1
fi
echo "   every mutant caught"
