#!/usr/bin/env python3
"""scripts/perf_gate.py — the performance gate (DESIGN.md §11; D9, D13, D29).

Compares fresh benchmark results (files written by bench/run.sh, format `epykos-bench 1`) with
the accepted baseline of the SAME machine+toolchain fingerprint, and checks the absolute targets
of bench/targets.json (from docs/ROADMAP.md) that apply.

Usage:
  scripts/perf_gate.py [options] RUN [RUN ...]
    RUN               a results file (bench/results/<id>/<name>.json), or a run name resolved to
                      <results-root>/<fingerprint-id>/<name>.json with the fingerprint of this
                      machine (scripts/fingerprint.sh --id, or --fingerprint ID)
    --accept          update <results-root>/<id>/baseline.json with the fresh medians (creating it
                      if absent). For perf commits only, with the before/after it prints in the
                      commit message. Self-regressions are reported, not failed, under --accept.
    --threshold X     self-regression limit on the median, default 1.25 (fresh / baseline)
    --results-root D  default bench/results
    --baseline FILE   the baseline file (default <results-root>/<id>/baseline.json)
    --targets FILE    default bench/targets.json;  --no-targets skips the absolute targets
    --fingerprint ID  the fingerprint id used to resolve run names (default: scripts/fingerprint.sh)
    --build-dir DIR   the build tree fingerprint.sh reads the flags from (default build/release)
    --json OUT        also write the report as JSON
    --quiet           print the verdict line only

Rules:
  * A results file is refused when its 1-minute load before or after the run exceeded
    cores_logical / 2 (docs/WORKLOADS.md M1 Measurement), when the fingerprints of the runs and
    the baseline differ (numbers are never compared across fingerprints, D9/D13), when a run has
    no baseline (seed it with --accept), or when a file is malformed.
  * Self-regression: for every benchmark of a run that has a baseline entry, fresh median /
    baseline median > threshold fails. Benchmarks without a baseline entry are "new"; baseline
    entries not measured this time are "not measured"; neither fails. A ratio below 1/threshold
    is reported as "faster" (accept it in a perf commit so the baseline follows).
  * Absolute targets: each target names the runs and benchmarks it needs; a side is taken from
    the runs given on the command line, else from the committed <results-root>/<id>/<run>.json
    (it must pass the load rule too). A target is evaluated when every side is available and at
    least one comes from a run given on the command line; otherwise it is reported as not
    applicable. Ratio targets compare the best (lowest) median of the benchmarks a side matches.

Exit status: 0 pass; 1 fail (a self-regression above the threshold, or an applicable absolute
target missed); 2 refused (load, cross-fingerprint, no baseline, bad input) — the report says why.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from typing import Any, Dict, List, Optional, Tuple

sys.dont_write_bytecode = True  # no scripts/__pycache__ in the checkout
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_results as br  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_THRESHOLD = 1.25

EXIT_PASS, EXIT_FAIL, EXIT_REFUSED = 0, 1, 2


class Refusal(Exception):
    """The gate cannot be evaluated; the message says why (exit 2)."""


# ---------------------------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------------------------
def machine_fingerprint_id(build_dir: str) -> str:
    script = os.path.join(ROOT, "scripts", "fingerprint.sh")
    try:
        out = subprocess.run([script, "--id", "--build-dir", build_dir], check=True, capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as e:
        raise Refusal("cannot determine this machine's fingerprint (%s); pass --fingerprint ID" % e)
    fid = out.stdout.strip()
    if not re.fullmatch(r"[0-9a-f]{12}", fid):
        raise Refusal("unexpected fingerprint id %r from %s" % (fid, script))
    return fid


def load_run(path: str) -> Dict[str, Any]:
    try:
        run = br.load_json(path)
    except (OSError, ValueError) as e:
        raise Refusal("%s: cannot read (%s)" % (path, e))
    try:
        br.check_format(run, path)
    except br.FormatError as e:
        raise Refusal(str(e))
    run["_path"] = path
    return run


def resolve_runs(names: List[str], results_root: str, fingerprint: Optional[str], build_dir: str) -> List[Dict[str, Any]]:
    runs = []
    fid = fingerprint
    for n in names:
        if os.path.isfile(n):
            runs.append(load_run(n))
            continue
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", n):
            raise Refusal("%s: not a results file and not a run name" % n)
        if fid is None:
            fid = machine_fingerprint_id(build_dir)
        path = os.path.join(results_root, fid, n + ".json")
        if not os.path.isfile(path):
            raise Refusal("run %s: no results file %s for fingerprint %s (run bench/run.sh first)" % (n, path, fid))
        runs.append(load_run(path))
    if not runs:
        raise Refusal("no runs given")
    return runs


def load_check(run: Dict[str, Any]) -> Dict[str, Any]:
    """Re-evaluate the load rule from the recorded numbers; refuse when it is exceeded or unknown."""
    ld = run["load"]
    cores = ld.get("cores_logical") or run["fingerprint"].get("cores_logical")
    rule = br.load_rule(ld.get("before"), ld.get("after"), cores, ld.get("check_enforced", True))
    if rule["within_threshold"] is None:
        raise Refusal("%s: load rule cannot be evaluated (loads %s / %s, logical cores %s)" % (
            run["name"], ld.get("before"), ld.get("after"), cores))
    if not rule["within_threshold"]:
        raise Refusal("%s: 1-minute load %s before / %s after the run exceeds cores/2 = %s (%s logical cores): "
                      "the measurement is discarded (docs/WORKLOADS.md M1 Measurement)" % (
                          run["name"], ld.get("before"), ld.get("after"), rule["threshold"], cores))
    return rule


def load_baseline(path: str) -> Optional[Dict[str, Any]]:
    if not os.path.isfile(path):
        return None
    try:
        b = br.load_json(path)
    except (OSError, ValueError) as e:
        raise Refusal("%s: cannot read the baseline (%s)" % (path, e))
    if not isinstance(b, dict) or b.get("format") != br.BASELINE_FORMAT:
        raise Refusal("%s: not an %s file" % (path, br.BASELINE_FORMAT))
    b.setdefault("runs", {})
    return b


def load_targets(path: str) -> List[Dict[str, Any]]:
    try:
        t = br.load_json(path)
    except (OSError, ValueError) as e:
        raise Refusal("%s: cannot read the targets (%s)" % (path, e))
    if not isinstance(t, dict) or t.get("format") != br.TARGETS_FORMAT:
        raise Refusal("%s: not an %s file" % (path, br.TARGETS_FORMAT))
    return list(t.get("targets", []))


# ---------------------------------------------------------------------------------------------
# Self-regression against the baseline
# ---------------------------------------------------------------------------------------------
def compare_run(run: Dict[str, Any], base_run: Optional[Dict[str, Any]], threshold: float) -> Dict[str, Any]:
    rows = []
    base_b = (base_run or {}).get("benchmarks", {})
    counts = {"ok": 0, "regression": 0, "faster": 0, "new": 0, "error": 0, "not_measured": 0}
    for name, b in run["benchmarks"].items():
        if "error" in b:
            rows.append({"benchmark": name, "status": "error", "message": b["error"]})
            counts["error"] += 1
            continue
        base = base_b.get(name)
        if base is None:
            rows.append({"benchmark": name, "status": "new", "fresh": b["median"], "unit": b["unit"], "n": b["n"]})
            counts["new"] += 1
            continue
        base_median = br.convert_time(base["median"], base.get("unit", b["unit"]), b["unit"])
        ratio = b["median"] / base_median if base_median > 0 else float("inf")
        if ratio > threshold:
            status = "regression"
        elif ratio < 1.0 / threshold:
            status = "faster"
        else:
            status = "ok"
        counts[status] += 1
        rows.append({"benchmark": name, "status": status, "baseline": base_median, "baseline_commit": base.get("commit"),
                     "fresh": b["median"], "unit": b["unit"], "ratio": ratio, "n": b["n"]})
    for name in base_b:
        if name not in run["benchmarks"]:
            rows.append({"benchmark": name, "status": "not_measured", "baseline": base_b[name]["median"],
                         "unit": base_b[name].get("unit"), "baseline_commit": base_b[name].get("commit")})
            counts["not_measured"] += 1
    return {"run": run["name"], "path": run["_path"], "has_baseline": base_run is not None,
            "rows": rows, "counts": counts, "threshold": threshold}


# ---------------------------------------------------------------------------------------------
# Absolute targets
# ---------------------------------------------------------------------------------------------
def _side_spec(side: Dict[str, Any]) -> Tuple[str, str, str]:
    run = side.get("run")
    if not run:
        raise Refusal("targets: a side without a 'run'")
    if "benchmark" in side:
        pattern = re.escape(side["benchmark"])
    else:
        pattern = side.get("match", ".*")
    return run, pattern, side.get("pick", "min_median")


def resolve_side(side: Dict[str, Any], fresh: Dict[str, Dict[str, Any]], results_dir: str, fid: str,
                 cache: Dict[str, Any]) -> Dict[str, Any]:
    """{available, source, run, path, commit, benchmark, median, unit} for one side of a target."""
    run_name, pattern, pick = _side_spec(side)
    run = fresh.get(run_name)
    source = "fresh"
    if run is None:
        source = "committed"
        if run_name in cache:
            run = cache[run_name]
        else:
            path = os.path.join(results_dir, run_name + ".json")
            run = None
            if os.path.isfile(path):
                try:
                    r = load_run(path)
                    if r["fingerprint"].get("id") != fid:
                        return {"available": False, "reason": "%s has fingerprint %s, not %s" % (path, r["fingerprint"].get("id"), fid)}
                    load_check(r)
                    run = r
                except Refusal as e:
                    return {"available": False, "reason": str(e)}
            cache[run_name] = run
        if run is None:
            return {"available": False, "reason": "run %s not measured on fingerprint %s" % (run_name, fid)}
    cands = [(n, b) for n, b in run["benchmarks"].items() if "error" not in b and re.fullmatch(pattern, n)]
    if not cands:
        return {"available": False, "reason": "run %s: no benchmark matches %r" % (run_name, pattern)}
    if pick == "max_median":
        name, b = max(cands, key=lambda nb: nb[1]["median"])
    else:
        name, b = min(cands, key=lambda nb: nb[1]["median"])
    git = run.get("git") or {}
    return {"available": True, "source": source, "run": run_name, "path": run["_path"],
            "commit": git.get("commit_short"), "benchmark": name, "median": b["median"], "unit": b["unit"],
            "candidates": len(cands)}


def evaluate_targets(targets: List[Dict[str, Any]], runs: List[Dict[str, Any]], results_dir: str, fid: str) -> List[Dict[str, Any]]:
    fresh = {r["name"]: r for r in runs}
    cache: Dict[str, Any] = {}
    rows = []
    for t in targets:
        row: Dict[str, Any] = {"id": t.get("id"), "milestone": t.get("milestone"), "kind": t.get("kind"),
                               "description": t.get("description"), "max": t.get("max")}
        kind = t.get("kind")
        try:
            if kind == "ratio":
                sides = {"numerator": resolve_side(t["numerator"], fresh, results_dir, fid, cache),
                         "denominator": resolve_side(t["denominator"], fresh, results_dir, fid, cache)}
            elif kind == "time":
                sides = {"value": resolve_side(t["value"], fresh, results_dir, fid, cache)}
            else:
                row.update({"status": "not_applicable", "reason": "unknown target kind %r" % kind})
                rows.append(row)
                continue
        except (KeyError, Refusal) as e:
            row.update({"status": "not_applicable", "reason": "malformed target: %s" % e})
            rows.append(row)
            continue
        row["sides"] = sides
        missing = [k for k, s in sides.items() if not s["available"]]
        if missing:
            row.update({"status": "not_applicable", "reason": "; ".join(sides[k]["reason"] for k in missing)})
            rows.append(row)
            continue
        if not any(s["source"] == "fresh" for s in sides.values()):
            row.update({"status": "not_evaluated", "reason": "no run given on the command line is involved"})
            rows.append(row)
            continue
        if kind == "ratio":
            num, den = sides["numerator"], sides["denominator"]
            den_in_num_unit = br.convert_time(den["median"], den["unit"], num["unit"])
            value = num["median"] / den_in_num_unit if den_in_num_unit > 0 else float("inf")
            row["value"] = value
            row["unit"] = "x"
        else:
            v = sides["value"]
            unit = t.get("unit", "s")
            row["value"] = br.convert_time(v["median"], v["unit"], unit)
            row["unit"] = unit
        row["status"] = "pass" if row["value"] <= float(t["max"]) else "fail"
        rows.append(row)
    return rows


# ---------------------------------------------------------------------------------------------
# Accepting a baseline
# ---------------------------------------------------------------------------------------------
def accept(baseline: Optional[Dict[str, Any]], runs: List[Dict[str, Any]], fid: str) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
    if baseline is None:
        fp = {k: v for k, v in runs[0]["fingerprint"].items() if k not in ("load1", "flags_source")}
        baseline = {"format": br.BASELINE_FORMAT, "fingerprint": fp, "updated": None,
                    "note": "accepted baseline per benchmark (medians); updated only by scripts/perf_gate.py --accept in perf commits",
                    "runs": {}}
    changes = []
    now = br.now_iso()
    for run in runs:
        git = run.get("git") or {}
        entry = baseline["runs"].setdefault(run["name"], {"benchmarks": {}})
        entry.update({"source": os.path.basename(run["_path"]), "binary": run.get("binary"), "preset": run.get("preset"),
                      "commit": git.get("commit_short"), "commit_full": git.get("commit"), "date": run.get("date"),
                      "accepted": now})
        for name, b in run["benchmarks"].items():
            if "error" in b:
                continue
            old = entry["benchmarks"].get(name)
            new = {"median": b["median"], "unit": b["unit"], "n": b["n"], "min": b["min"], "p90": b["p90"],
                   "commit": git.get("commit_short"), "date": run.get("date")}
            entry["benchmarks"][name] = new
            changes.append({"run": run["name"], "benchmark": name,
                            "before": (br.convert_time(old["median"], old.get("unit", b["unit"]), b["unit"]) if old else None),
                            "before_commit": old.get("commit") if old else None,
                            "after": b["median"], "unit": b["unit"], "after_commit": git.get("commit_short")})
    baseline["updated"] = now
    baseline["fingerprint"]["id"] = fid
    return baseline, changes


# ---------------------------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------------------------
def _f(x: Optional[float], nd: int = 2) -> str:
    return "-" if x is None else "{:.{nd}f}".format(x, nd=nd)


def render(report: Dict[str, Any]) -> str:
    L: List[str] = []
    fp = report.get("fingerprint") or {}
    L.append("perf gate: fingerprint %s (%s, %s logical cores, %s, flags %s)" % (
        fp.get("id"), fp.get("cpu"), fp.get("cores_logical"), fp.get("compiler"), fp.get("flags")))
    if report.get("baseline_path"):
        L.append("baseline: %s%s" % (report["baseline_path"], "" if report.get("baseline_exists") else " (absent)"))
    L.append("threshold: a benchmark fails at fresh median > %.2fx its baseline median; faster below %.3fx" % (
        report["threshold"], 1.0 / report["threshold"]))
    for c in report.get("comparisons", []):
        run = c["run_info"]
        ld = run["load"]
        L.append("")
        L.append("run %s: %s (%s preset, commit %s%s, %s); load1 %s -> %s (threshold %s: %s); %s repetitions x min_time %s" % (
            c["run"], run.get("binary"), run.get("preset"), run.get("commit") or "?", " DIRTY" if run.get("dirty") else "",
            run.get("date"), ld.get("before"), ld.get("after"), _f(ld.get("threshold"), 1),
            "ok" if ld.get("within_threshold") else "EXCEEDED", run.get("repetitions"), run.get("min_time")))
        if not c["has_baseline"]:
            L.append("  no baseline entry for this run: self-regression not checked (seed with --accept)")
        names = [r["benchmark"] for r in c["rows"]]
        w = max([len(n) for n in names] + [10])
        L.append("  %-*s %12s %12s %7s  %s" % (w, "benchmark", "baseline", "fresh", "ratio", "status"))
        for r in c["rows"]:
            st = r["status"].upper() if r["status"] in ("regression", "error") else r["status"]
            if r["status"] == "error":
                L.append("  %-*s %12s %12s %7s  %s %s" % (w, r["benchmark"], "-", "-", "-", st, r.get("message")))
                continue
            L.append("  %-*s %12s %12s %7s  %s%s" % (
                w, r["benchmark"], _f(r.get("baseline"), 3), _f(r.get("fresh"), 3), _f(r.get("ratio"), 3), st,
                (" (baseline @%s)" % r["baseline_commit"]) if r.get("baseline_commit") and r["status"] in ("regression", "faster") else ""))
        k = c["counts"]
        L.append("  %d compared: %d ok, %d regression, %d faster; %d new, %d not measured, %d error" % (
            k["ok"] + k["regression"] + k["faster"], k["ok"], k["regression"], k["faster"], k["new"], k["not_measured"], k["error"]))
    if report.get("targets") is not None:
        L.append("")
        L.append("absolute targets (%s):" % report.get("targets_path"))
        for t in report["targets"]:
            head = "  %-14s %-3s" % (t.get("id"), t.get("milestone") or "")
            if t["status"] in ("pass", "fail"):
                if t["kind"] == "ratio":
                    n, d = t["sides"]["numerator"], t["sides"]["denominator"]
                    L.append("%s %s: %s %s / %s %s = %.3f (max %.3g): %s" % (
                        head, t.get("description"), _f(n["median"], 2), n["unit"], _f(br.convert_time(d["median"], d["unit"], n["unit"]), 2),
                        n["unit"], t["value"], t["max"], t["status"].upper()))
                    for label, s in (("numerator", n), ("denominator", d)):
                        L.append("      %s: %s %s (%s%s)" % (label, s["benchmark"], s["source"], os.path.basename(s["path"]),
                                                             (" @%s" % s["commit"]) if s.get("commit") else ""))
                else:
                    v = t["sides"]["value"]
                    L.append("%s %s: %.4g %s (max %.3g %s): %s" % (head, t.get("description"), t["value"], t["unit"], t["max"], t["unit"], t["status"].upper()))
                    L.append("      value: %s %s (%s%s)" % (v["benchmark"], v["source"], os.path.basename(v["path"]), (" @%s" % v["commit"]) if v.get("commit") else ""))
            else:
                L.append("%s %s: %s (%s)" % (head, t.get("description"), t["status"].replace("_", " "), t.get("reason")))
    if report.get("accepted"):
        L.append("")
        L.append("baseline updated: %s (before -> after, medians)" % report["baseline_path"])
        for ch in report["accepted"]:
            L.append("  %s %s: %s -> %s %s%s" % (
                ch["run"], ch["benchmark"], _f(ch["before"], 3), _f(ch["after"], 3), ch["unit"],
                (" (%s -> %s, x%.3f)" % (ch.get("before_commit"), ch.get("after_commit"), ch["after"] / ch["before"]))
                if ch["before"] else " (new @%s)" % ch.get("after_commit")))
    L.append("")
    L.append("verdict: %s (exit %d)%s" % (report["verdict"], report["exit"], (" - " + report["reason"]) if report.get("reason") else ""))
    return "\n".join(L)


def run_info(run: Dict[str, Any]) -> Dict[str, Any]:
    git = run.get("git") or {}
    gb = run.get("google_benchmark") or {}
    return {"name": run["name"], "path": run["_path"], "binary": run.get("binary"), "preset": run.get("preset"),
            "commit": git.get("commit_short"), "commit_full": git.get("commit"), "dirty": git.get("dirty"),
            "date": run.get("date"), "load": run["load"], "repetitions": gb.get("repetitions"), "min_time": gb.get("min_time")}


# ---------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------
def gate(a: argparse.Namespace) -> Dict[str, Any]:
    report: Dict[str, Any] = {"threshold": a.threshold, "runs": [], "comparisons": [], "targets": None,
                              "verdict": None, "exit": EXIT_REFUSED, "reason": None}
    runs = resolve_runs(a.run, a.results_root, a.fingerprint, a.build_dir)
    fids = sorted({r["fingerprint"]["id"] for r in runs})
    if len(fids) != 1:
        raise Refusal("the runs have different fingerprints %s: numbers are never compared across fingerprints (D9, D13)" % fids)
    fid = fids[0]
    report["fingerprint"] = runs[0]["fingerprint"]
    for r in runs:
        load_check(r)
        report["runs"].append(run_info(r))
    names = [r["name"] for r in runs]
    if len(set(names)) != len(names):
        raise Refusal("the same run name is given twice: %s" % names)

    results_dir = os.path.join(a.results_root, fid)
    baseline_path = a.baseline or os.path.join(results_dir, "baseline.json")
    report["baseline_path"] = baseline_path
    baseline = load_baseline(baseline_path)
    report["baseline_exists"] = baseline is not None
    if baseline is not None:
        bfid = (baseline.get("fingerprint") or {}).get("id")
        if bfid != fid:
            raise Refusal("baseline %s is for fingerprint %s, the runs are %s: numbers are never compared across fingerprints (D9, D13)" % (
                baseline_path, bfid, fid))

    # Self-regression.
    for r in runs:
        base_run = (baseline or {}).get("runs", {}).get(r["name"])
        c = compare_run(r, base_run, a.threshold)
        c["run_info"] = run_info(r)
        report["comparisons"].append(c)
    unbaselined = [c["run"] for c in report["comparisons"] if not c["has_baseline"]]
    regressions = [(c["run"], row) for c in report["comparisons"] for row in c["rows"] if row["status"] == "regression"]

    # Absolute targets.
    target_fail = []
    if not a.no_targets:
        targets = load_targets(a.targets)
        report["targets_path"] = a.targets
        report["targets"] = evaluate_targets(targets, runs, results_dir, fid)
        target_fail = [t["id"] for t in report["targets"] if t["status"] == "fail"]

    # Accept.
    if a.accept:
        baseline, changes = accept(baseline, runs, fid)
        br.write_json(baseline_path, baseline)
        report["accepted"] = changes

    # Verdict.
    reasons = []
    if unbaselined and not a.accept:
        report.update({"verdict": "REFUSED", "exit": EXIT_REFUSED,
                       "reason": "no baseline for run(s) %s on fingerprint %s: seed with --accept in a perf commit" % (", ".join(unbaselined), fid)})
        return report
    if regressions and not a.accept:
        reasons.append("%d self-regression(s) above %.2fx: %s" % (
            len(regressions), a.threshold, ", ".join("%s %s x%.3f" % (rn, row["benchmark"], row["ratio"]) for rn, row in regressions[:6])
            + (" ..." if len(regressions) > 6 else "")))
    if target_fail:
        reasons.append("absolute target(s) missed: %s" % ", ".join(target_fail))
    if reasons:
        report.update({"verdict": "FAIL", "exit": EXIT_FAIL, "reason": "; ".join(reasons)})
    else:
        note = []
        if a.accept:
            note.append("baseline accepted")
            if regressions:
                note.append("%d regression(s) accepted into the baseline" % len(regressions))
        report.update({"verdict": "PASS", "exit": EXIT_PASS, "reason": "; ".join(note) or None})
    return report


def main(argv: List[str]) -> int:
    p = argparse.ArgumentParser(prog="perf_gate.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("run", nargs="+", help="results file(s) or run name(s)")
    p.add_argument("--accept", action="store_true")
    p.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD)
    p.add_argument("--results-root", default=os.path.join(ROOT, "bench", "results"))
    p.add_argument("--baseline")
    p.add_argument("--targets", default=os.path.join(ROOT, "bench", "targets.json"))
    p.add_argument("--no-targets", action="store_true")
    p.add_argument("--fingerprint")
    p.add_argument("--build-dir", default=os.path.join(ROOT, "build", "release"))
    p.add_argument("--json")
    p.add_argument("--quiet", action="store_true")
    a = p.parse_args(argv)
    if a.threshold <= 1.0:
        print("perf_gate: --threshold must be > 1", file=sys.stderr)
        return EXIT_REFUSED
    try:
        report = gate(a)
    except Refusal as e:
        report = {"verdict": "REFUSED", "exit": EXIT_REFUSED, "reason": str(e), "threshold": a.threshold}
        print("perf gate: REFUSED (exit 2) - %s" % e)
        if a.json:
            br.write_json(a.json, report)
        return EXIT_REFUSED
    text = render(report)
    if a.quiet:
        print(text.splitlines()[-1])
    else:
        print(text)
    if a.json:
        br.write_json(a.json, report)
    return int(report["exit"])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
