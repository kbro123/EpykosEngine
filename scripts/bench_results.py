#!/usr/bin/env python3
"""scripts/bench_results.py — the benchmark results format (`epykos-bench 1`) and its summariser.

Python 3 standard library only (D29). Called by bench/run.sh after a Google Benchmark binary has
run, and imported by scripts/perf_gate.py.

One results file per benchmark run, `bench/results/<fingerprint-id>/<name>.json`:

  format            "epykos-bench 1"
  name              the run name (the benchmark target without its `_bench` suffix, e.g. exec_m1_interp)
  binary, args      what was run (path relative to the repository; the Google Benchmark arguments)
  preset            the CMake preset the binary was built with (release | reference | debug)
  date              ISO 8601, local time with offset
  git               {commit, commit_short, branch, dirty}
  fingerprint       scripts/fingerprint.sh --json (id, cpu, cores, compiler, flags; D13), load1 removed
  load              {before, after, cores_logical, threshold = cores_logical / 2, within_threshold,
                     check_enforced}: the 1-minute load average before and after the run and the
                     docs/WORKLOADS.md rule (cores = logical CPUs); the gate refuses a file whose
                     before or after load exceeds the threshold whatever check_enforced says
  env               EPYKOS_* environment variables seen by the binary (e.g. EPYKOS_TILE)
  google_benchmark  {version, repetitions, min_time, context}
  statistics        how min / median / p90 are defined (below)
  raw_output        where the raw Google Benchmark JSON went (bench/results/<id>/tmp/, gitignored)
  benchmarks        name -> {n, min, median, p90, max, mean, unit, label, params, counters,
                             iterations, evaluations, real_time}
  migrated_from     only on files converted from an older layout (which raw files, by which package)
  note              free text (optional)

Statistics per benchmark, over the n Google Benchmark repetitions (run_type == "iteration") of
real_time in the benchmark's time unit: min, median (mean of the two middle values for even n), p90
(nearest rank: the ceil(0.9 n)-th smallest), max, mean. Each repetition's real_time is the MEAN over
that repetition's iterations (>= min_time worth of evaluations), so these are statistics over n
repetition means, not over single evaluations (docs/WORKLOADS.md M1 "Terms"); the file records n,
the iterations per repetition and the total number of timed evaluations.

`params` are the arguments in the benchmark's name (`BM_X/tile:512/lane_tile:1/exp:0` ->
{tile: 512, lane_tile: 1, exp: 0}; positional arguments become arg0, arg1, ...; Google Benchmark's
own modifiers threads/repeats/min_time/... are left out). `counters` are the user counters that
are constant across the repetitions (B, tile, lane_tile, rows, ...); rate counters, which vary,
are not kept.

Usage:
  scripts/bench_results.py summarise --raw RAW.json --name NAME --binary PATH --preset P
        --fingerprint-before FP.json [--fingerprint-after FP.json | --load-after X]
        [--commit SHA --branch B --dirty 0|1] [--arg A ...] [--date ISO] [--raw-output REL]
        [--load-not-enforced] [--note TEXT] [--migrated-from JSON] --out RESULT.json
  scripts/bench_results.py show RESULT.json            print the summary table of a results file
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

FORMAT = "epykos-bench 1"
BASELINE_FORMAT = "epykos-baseline 1"
TARGETS_FORMAT = "epykos-targets 1"

STATISTICS_TEXT = (
    "min / median / p90 (nearest rank) / max / mean over the n Google Benchmark repetitions of real_time; "
    "each repetition's real_time is the mean of its iterations (>= min_time of evaluations), so these are "
    "statistics of repetition means, not of single evaluations (docs/WORKLOADS.md M1 Terms)"
)

# Google Benchmark time units -> seconds.
TIME_UNITS = {"ns": 1e-9, "us": 1e-6, "ms": 1e-3, "s": 1.0}

# Keys of a Google Benchmark JSON record that are not user counters.
GB_RECORD_KEYS = {
    "name", "family_index", "per_family_instance_index", "run_name", "run_type", "repetitions",
    "repetition_index", "threads", "iterations", "real_time", "cpu_time", "time_unit", "aggregate_name",
    "aggregate_unit", "label", "error_occurred", "error_message", "items_per_second", "bytes_per_second",
    "big_o", "rms", "utest", "cv",
}
# Name components that are Google Benchmark modifiers, not the benchmark's own arguments.
GB_NAME_MODIFIERS = {"threads", "repeats", "min_time", "min_warmup_time", "iterations",
                     "real_time", "manual_time", "process_time"}


class FormatError(ValueError):
    """A file is not in the format this module expects."""


# ---------------------------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------------------------
def load_json(path: str) -> Any:
    with open(path) as f:
        return json.load(f)


def write_json(path: str, obj: Any) -> None:
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "w") as f:
        json.dump(obj, f, indent=1, sort_keys=False)
        f.write("\n")


def now_iso() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


def convert_time(value: float, unit_from: str, unit_to: str) -> float:
    """Convert a time between Google Benchmark units (ns, us, ms, s)."""
    if unit_from not in TIME_UNITS or unit_to not in TIME_UNITS:
        raise FormatError("unknown time unit: %r -> %r" % (unit_from, unit_to))
    return value * TIME_UNITS[unit_from] / TIME_UNITS[unit_to]


def stats(values: List[float]) -> Dict[str, Any]:
    """min / median / p90 (nearest rank) / max / mean over the repetitions (same definitions as M1/P6)."""
    v = sorted(values)
    n = len(v)
    if n == 0:
        raise ValueError("no repetitions")
    median = v[n // 2] if n % 2 else 0.5 * (v[n // 2 - 1] + v[n // 2])
    p90 = v[max(0, int(math.ceil(0.9 * n)) - 1)]
    return {"n": n, "min": v[0], "median": median, "p90": p90, "max": v[-1], "mean": sum(v) / n}


def _scalar(text: str) -> Any:
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def parse_name(name: str) -> Tuple[str, Dict[str, Any]]:
    """'BM_X/tile:512/lane_tile:1/exp:0' -> ('BM_X', {tile: 512, lane_tile: 1, exp: 0});
    'BM_HandEval/2' -> ('BM_HandEval', {arg0: 2}). Google Benchmark modifiers are dropped."""
    parts = name.split("/")
    params: Dict[str, Any] = {}
    positional = 0
    for part in parts[1:]:
        if ":" in part:
            key, _, value = part.partition(":")
            if key in GB_NAME_MODIFIERS:
                continue
            params[key] = _scalar(value)
        elif part in GB_NAME_MODIFIERS:
            continue
        else:
            params["arg%d" % positional] = _scalar(part)
            positional += 1
    return parts[0], params


# ---------------------------------------------------------------------------------------------
# Summarising a raw Google Benchmark JSON file
# ---------------------------------------------------------------------------------------------
def summarise_gbench(raw: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    """name -> summary, from the raw repetitions of a Google Benchmark JSON output
    (--benchmark_out_format=json --benchmark_report_aggregates_only=false)."""
    if "benchmarks" not in raw:
        raise FormatError("not a Google Benchmark JSON file (no 'benchmarks')")
    grouped: Dict[str, List[Dict[str, Any]]] = {}
    order: List[str] = []
    for rec in raw["benchmarks"]:
        if rec.get("run_type", "iteration") != "iteration":
            continue  # aggregates (_mean, _median, ...) are recomputed here from the repetitions
        name = rec["name"]
        if name not in grouped:
            grouped[name] = []
            order.append(name)
        grouped[name].append(rec)

    out: Dict[str, Dict[str, Any]] = {}
    for name in order:
        recs = grouped[name]
        function, params = parse_name(name)
        errors = [r.get("error_message", "error") for r in recs if r.get("error_occurred")]
        if errors:
            out[name] = {"function": function, "params": params, "error": errors[0], "n": len(recs)}
            continue
        times = [float(r["real_time"]) for r in recs]
        units = {r.get("time_unit", "ns") for r in recs}
        if len(units) != 1:
            raise FormatError("benchmark %s mixes time units %s" % (name, sorted(units)))
        s = stats(times)
        # User counters constant across the repetitions (B, tile, ...); rates vary and are dropped.
        counters: Dict[str, Any] = {}
        first = recs[0]
        for k, v in first.items():
            if k in GB_RECORD_KEYS or not isinstance(v, (int, float)) or isinstance(v, bool):
                continue
            if all(r.get(k) == v for r in recs):
                counters[k] = int(v) if float(v).is_integer() else v
        iterations = [int(r.get("iterations", 0)) for r in recs]
        cpu = [float(r["cpu_time"]) for r in recs if "cpu_time" in r]
        s.update({
            "unit": units.pop(),
            "function": function,
            "label": first.get("label", ""),
            "params": params,
            "counters": counters,
            "iterations": iterations,
            "evaluations": sum(iterations),
            "real_time": times,
            "cpu_time_median": stats(cpu)["median"] if cpu else None,
        })
        out[name] = s
    return out


def load_rule(before: Optional[float], after: Optional[float], cores_logical: Optional[int],
              enforced: bool = True) -> Dict[str, Any]:
    """docs/WORKLOADS.md M1 Measurement: discard runs with a 1-minute load above cores/2, cores = logical CPUs."""
    threshold = (cores_logical / 2.0) if cores_logical else None
    loads = [x for x in (before, after) if x is not None]
    within: Optional[bool]
    if threshold is None or not loads:
        within = None
    else:
        within = all(x <= threshold for x in loads)
    return {"before": before, "after": after, "cores_logical": cores_logical, "threshold": threshold,
            "within_threshold": within, "check_enforced": enforced,
            "rule": "docs/WORKLOADS.md M1 Measurement: 1-minute load average <= cores/2 before and after, cores = logical CPUs"}


def gbench_flag(args: List[str], name: str) -> Optional[str]:
    """The last value of --<name>=VALUE in the argument list (the last occurrence wins in Google Benchmark)."""
    value = None
    for a in args:
        if a.startswith("--" + name + "="):
            value = a[len(name) + 3:]
    return value


def build_result(raw: Dict[str, Any], *, name: str, binary: str, preset: str, args: List[str],
                 fp_before: Dict[str, Any], fp_after: Optional[Dict[str, Any]] = None,
                 load_after: Optional[float] = None, git: Optional[Dict[str, Any]] = None,
                 date: Optional[str] = None, raw_output: Optional[str] = None, note: Optional[str] = None,
                 migrated_from: Optional[Dict[str, Any]] = None, load_enforced: bool = True,
                 env: Optional[Dict[str, str]] = None) -> Dict[str, Any]:
    """Assemble one results file from the raw Google Benchmark output and the run's metadata."""
    fingerprint = {k: v for k, v in fp_before.items() if k != "load1"}
    before = fp_before.get("load1")
    after = fp_after.get("load1") if fp_after is not None else load_after
    if fp_after is not None and fp_after.get("id") != fp_before.get("id"):
        raise FormatError("fingerprint changed during the run: %s -> %s" % (fp_before.get("id"), fp_after.get("id")))
    cores = fp_before.get("cores_logical")
    context = raw.get("context", {})
    if env is None:
        env = {k: v for k, v in os.environ.items() if k.startswith("EPYKOS_")}
    result: Dict[str, Any] = {
        "format": FORMAT,
        "name": name,
        "binary": binary,
        "args": list(args),
        "preset": preset,
        "date": date or context.get("date") or now_iso(),
        "git": git or {"commit": None, "commit_short": None, "branch": None, "dirty": None},
        "fingerprint": fingerprint,
        "load": load_rule(before, after, cores, load_enforced),
        "env": env,
        "google_benchmark": {
            "version": context.get("library_version"),
            "repetitions": _scalar(gbench_flag(args, "benchmark_repetitions") or "") or None,
            "min_time": gbench_flag(args, "benchmark_min_time"),
            "context": context,
        },
        "statistics": STATISTICS_TEXT,
        "raw_output": raw_output,
        "benchmarks": summarise_gbench(raw),
    }
    if note:
        result["note"] = note
    if migrated_from:
        result["migrated_from"] = migrated_from
    return result


def check_format(result: Dict[str, Any], path: str = "<result>") -> None:
    if not isinstance(result, dict) or result.get("format") != FORMAT:
        raise FormatError("%s: not an %s file (format = %r)" % (path, FORMAT, (result or {}).get("format") if isinstance(result, dict) else None))
    for key in ("name", "fingerprint", "load", "benchmarks"):
        if key not in result:
            raise FormatError("%s: missing '%s'" % (path, key))
    if not result["fingerprint"].get("id"):
        raise FormatError("%s: fingerprint without an id" % path)


# ---------------------------------------------------------------------------------------------
# Printing
# ---------------------------------------------------------------------------------------------
def fmt_time(x: Optional[float], nd: int = 2) -> str:
    return "-" if x is None else "{:.{nd}f}".format(x, nd=nd)


def summary_lines(result: Dict[str, Any]) -> List[str]:
    fp = result["fingerprint"]
    ld = result["load"]
    git = result.get("git") or {}
    L = []
    L.append("%s: %s (%s preset, commit %s%s, %s)" % (
        result["name"], result.get("binary"), result.get("preset"), git.get("commit_short") or "?",
        " dirty" if git.get("dirty") else "", result.get("date")))
    L.append("  fingerprint %s: %s, %s logical cores, %s, flags %s" % (
        fp.get("id"), fp.get("cpu"), fp.get("cores_logical"), fp.get("compiler"), fp.get("flags")))
    within = ld.get("within_threshold")
    L.append("  load1 before %s, after %s; threshold cores/2 = %s: %s%s" % (
        ld.get("before"), ld.get("after"), ld.get("threshold"),
        "ok" if within else ("EXCEEDED" if within is False else "unknown"),
        "" if ld.get("check_enforced", True) else " (check not enforced at run time)"))
    gb = result.get("google_benchmark") or {}
    L.append("  Google Benchmark %s, repetitions %s, min_time %s; %s" % (
        gb.get("version"), gb.get("repetitions"), gb.get("min_time"),
        "min / median / p90 over the repetition means"))
    names = list(result["benchmarks"].keys())
    w = max([len(n) for n in names] + [10])
    L.append("  %-*s %5s %12s %12s %12s  %s" % (w, "benchmark", "n", "min", "median", "p90", "unit"))
    for n in names:
        b = result["benchmarks"][n]
        if "error" in b:
            L.append("  %-*s %5d  ERROR %s" % (w, n, b.get("n", 0), b["error"]))
            continue
        L.append("  %-*s %5d %12s %12s %12s  %s" % (w, n, b["n"], fmt_time(b["min"], 3), fmt_time(b["median"], 3),
                                                   fmt_time(b["p90"], 3), b["unit"]))
    return L


# ---------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------
def cmd_summarise(a: argparse.Namespace) -> int:
    raw = load_json(a.raw)
    fp_before = load_json(a.fingerprint_before)
    fp_after = load_json(a.fingerprint_after) if a.fingerprint_after else None
    git = {"commit": a.commit, "commit_short": (a.commit[:7] if a.commit else None), "branch": a.branch,
           "dirty": (bool(int(a.dirty)) if a.dirty not in (None, "") else None)}
    migrated = load_json(a.migrated_from) if a.migrated_from else None
    result = build_result(raw, name=a.name, binary=a.binary, preset=a.preset, args=a.arg or [],
                          fp_before=fp_before, fp_after=fp_after, load_after=a.load_after, git=git, date=a.date,
                          raw_output=a.raw_output, note=a.note, migrated_from=migrated,
                          load_enforced=not a.load_not_enforced)
    write_json(a.out, result)
    if not a.quiet:
        print("\n".join(summary_lines(result)))
        print("wrote %s" % a.out)
    return 0


def cmd_show(a: argparse.Namespace) -> int:
    result = load_json(a.result)
    check_format(result, a.result)
    print("\n".join(summary_lines(result)))
    return 0


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(prog="bench_results.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd")
    s = sub.add_parser("summarise", help="write a results file from a raw Google Benchmark JSON output")
    s.add_argument("--raw", required=True)
    s.add_argument("--name", required=True)
    s.add_argument("--binary", required=True)
    s.add_argument("--preset", required=True)
    s.add_argument("--fingerprint-before", required=True)
    s.add_argument("--fingerprint-after")
    s.add_argument("--load-after", type=float)
    s.add_argument("--commit")
    s.add_argument("--branch")
    s.add_argument("--dirty")
    s.add_argument("--arg", action="append")
    s.add_argument("--date")
    s.add_argument("--raw-output")
    s.add_argument("--note")
    s.add_argument("--migrated-from", help="JSON file describing the migrated raw files")
    s.add_argument("--load-not-enforced", action="store_true", help="bench/run.sh ran with --ignore-load")
    s.add_argument("--quiet", action="store_true")
    s.add_argument("--out", required=True)
    s.set_defaults(fn=cmd_summarise)
    w = sub.add_parser("show", help="print the summary table of a results file")
    w.add_argument("result")
    w.set_defaults(fn=cmd_show)
    a = p.parse_args(argv)
    if not a.cmd:
        p.print_help()
        return 2
    try:
        return a.fn(a)
    except (FormatError, OSError, KeyError, ValueError) as e:
        print("bench_results: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
