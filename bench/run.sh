#!/usr/bin/env bash
# bench/run.sh — run one Google Benchmark binary and write its results file (D9, D13, D29).
#
#   bench/run.sh [options] <bench-binary> [google-benchmark arguments...]
#
# Writes bench/results/<fingerprint-id>/<name>.json in the `epykos-bench 1` format
# (scripts/bench_results.py): the fingerprint (scripts/fingerprint.sh), the 1-minute load average
# before and after the run, the git commit, the preset, every benchmark's parameters (tile, B,
# lane_tile, ...) and min / median / p90 over the repetitions. The raw Google Benchmark output goes
# to bench/results/<fingerprint-id>/tmp/<name>.gbench.json (gitignored).
#
# Options (before the binary):
#   --name NAME          results file name; default: the binary's basename without "_bench"
#                        (build/release/bench/exec_m1_interp_bench -> exec_m1_interp)
#   --preset P           the CMake preset the binary was built with; default: taken from a
#                        build/<preset>/ component of the binary's path
#   --results-root DIR   default bench/results (tests use a scratch directory)
#   --repetitions N      --benchmark_repetitions, default 20 (the M1 protocol)
#   --min-time T         --benchmark_min_time, default 0.2s
#   --note TEXT          free text recorded in the results file
#   --ignore-load        run although the 1-minute load is above cores/2. The loads are recorded
#                        and scripts/perf_gate.py still refuses such a file: for tooling tests and
#                        exploratory runs only, never for a number that is reported.
#
# Refuses to run (exit 2) when the 1-minute load average exceeds cores/2 (cores = logical CPUs,
# docs/WORKLOADS.md M1 Measurement). Google Benchmark arguments given after the binary are passed
# through after the defaults, so --benchmark_filter=..., --benchmark_repetitions=..., etc. work
# (the last occurrence of a flag wins). Do not pass --benchmark_out: the raw output path is fixed.
#
# Then: scripts/perf_gate.py bench/results/<id>/<name>.json    (see that script's header)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
NAME=""
PRESET=""
RESULTS_ROOT="$ROOT/bench/results"
REPS=20
MIN_TIME=0.2s
NOTE=""
IGNORE_LOAD=0

usage() { sed -n '2,32p' "$0"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --name) shift; NAME="$1" ;;
    --preset) shift; PRESET="$1" ;;
    --results-root) shift; RESULTS_ROOT="$1" ;;
    --repetitions) shift; REPS="$1" ;;
    --min-time) shift; MIN_TIME="$1" ;;
    --note) shift; NOTE="$1" ;;
    --ignore-load) IGNORE_LOAD=1 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) echo "run.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    *) break ;;
  esac
  shift
done

if [[ $# -lt 1 ]]; then
  echo "run.sh: no benchmark binary given" >&2
  usage >&2
  exit 2
fi
BIN="$1"; shift
if [[ ! -x "$BIN" ]]; then
  echo "run.sh: '$BIN' is not an executable" >&2
  exit 2
fi
BIN_ABS="$(cd "$(dirname "$BIN")" && pwd -P)/$(basename "$BIN")"
case "$BIN_ABS" in
  "$ROOT"/*) BIN_REL="${BIN_ABS#"$ROOT"/}" ;;
  *) BIN_REL="$BIN_ABS" ;;
esac

command -v python3 >/dev/null 2>&1 || { echo "run.sh: python3 is required (D29)" >&2; exit 2; }

base="$(basename "$BIN")"
[[ -n "$NAME" ]] || NAME="${base%_bench}"
if [[ ! "$NAME" =~ ^[A-Za-z0-9_.-]+$ ]]; then
  echo "run.sh: results name '$NAME' must match [A-Za-z0-9_.-]+" >&2
  exit 2
fi

if [[ -z "$PRESET" ]]; then
  if [[ "$BIN_ABS" =~ /build/([^/]+)/ ]]; then
    PRESET="${BASH_REMATCH[1]}"
  else
    echo "run.sh: cannot infer the preset from '$BIN' (no build/<preset>/ in its path): pass --preset" >&2
    exit 2
  fi
fi
BUILD_DIR="$ROOT/build/$PRESET"
if [[ ! -f "$BUILD_DIR/epykos_flags.txt" ]]; then
  echo "run.sh: $BUILD_DIR/epykos_flags.txt not found: the preset '$PRESET' is not configured here" >&2
  exit 2
fi

# ---- fingerprint and load before ------------------------------------------------------------
FP_BEFORE="$("$ROOT/scripts/fingerprint.sh" --json --build-dir "$BUILD_DIR")"
read -r FP_ID LOAD1 CORES <<< "$(printf '%s' "$FP_BEFORE" | python3 -c '
import json, sys
fp = json.load(sys.stdin)
print(fp["id"], fp.get("load1", 0), fp.get("cores_logical", 0))')"
THRESHOLD="$(python3 -c "print($CORES / 2.0)")"
if python3 -c "import sys; sys.exit(0 if float('$LOAD1') > float('$THRESHOLD') else 1)"; then
  if [[ $IGNORE_LOAD -eq 1 ]]; then
    echo "run.sh: WARNING: 1-minute load $LOAD1 exceeds cores/2 = $THRESHOLD ($CORES logical cores); running anyway (--ignore-load): the gate will refuse this file" >&2
  else
    echo "run.sh: REFUSED (exit 2): 1-minute load $LOAD1 exceeds cores/2 = $THRESHOLD ($CORES logical cores; docs/WORKLOADS.md M1 Measurement). Wait for the machine to go idle, or --ignore-load for a run that is not reported." >&2
    exit 2
  fi
fi

OUT_DIR="$RESULTS_ROOT/$FP_ID"
TMP_DIR="$OUT_DIR/tmp"
mkdir -p "$TMP_DIR"
RAW="$TMP_DIR/$NAME.gbench.json"
FP_BEFORE_FILE="$TMP_DIR/$NAME.fingerprint_before.json"
FP_AFTER_FILE="$TMP_DIR/$NAME.fingerprint_after.json"
printf '%s\n' "$FP_BEFORE" > "$FP_BEFORE_FILE"

# ---- git ----------------------------------------------------------------------------------------
COMMIT=""; BRANCH=""; DIRTY=""
if git -C "$ROOT" rev-parse --verify -q HEAD >/dev/null 2>&1; then
  COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
  BRANCH="$(git -C "$ROOT" rev-parse --abbrev-ref HEAD)"
  if [[ -n "$(git -C "$ROOT" status --porcelain --untracked-files=no 2>/dev/null)" ]]; then DIRTY=1; else DIRTY=0; fi
fi

# ---- run ------------------------------------------------------------------------------------------
GB_ARGS=("--benchmark_repetitions=$REPS" "--benchmark_min_time=$MIN_TIME"
         "--benchmark_report_aggregates_only=false" "--benchmark_out_format=json" "--benchmark_out=$RAW" "$@")
echo "run.sh: $BIN_REL ${GB_ARGS[*]}"
echo "run.sh: fingerprint $FP_ID, preset $PRESET, commit ${COMMIT:0:7}${DIRTY:+$( [[ "$DIRTY" == 1 ]] && echo ' (dirty)' )}, load1 $LOAD1 (threshold $THRESHOLD)"
rm -f "$RAW"
"$BIN" "${GB_ARGS[@]}"
if [[ ! -s "$RAW" ]]; then
  echo "run.sh: the benchmark wrote no output to $RAW" >&2
  exit 1
fi

# ---- load after, summarise ---------------------------------------------------------------------
"$ROOT/scripts/fingerprint.sh" --json --build-dir "$BUILD_DIR" > "$FP_AFTER_FILE"

ARG_FLAGS=()
for a in "${GB_ARGS[@]}"; do
  case "$a" in
    --benchmark_out=*) continue ;;  # a scratch path, not part of the protocol
  esac
  ARG_FLAGS+=("--arg=$a")  # values start with "--": the = form keeps argparse from reading them as options
done
EXTRA=()
[[ -n "$NOTE" ]] && EXTRA+=(--note "$NOTE")
[[ $IGNORE_LOAD -eq 1 ]] && EXTRA+=(--load-not-enforced)
[[ -n "$COMMIT" ]] && EXTRA+=(--commit "$COMMIT" --branch "$BRANCH" --dirty "$DIRTY")

python3 "$ROOT/scripts/bench_results.py" summarise \
  --raw "$RAW" --name "$NAME" --binary "$BIN_REL" --preset "$PRESET" \
  --fingerprint-before "$FP_BEFORE_FILE" --fingerprint-after "$FP_AFTER_FILE" \
  --raw-output "tmp/$NAME.gbench.json" "${ARG_FLAGS[@]}" "${EXTRA[@]}" \
  --out "$OUT_DIR/$NAME.json"
