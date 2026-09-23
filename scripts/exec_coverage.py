#!/usr/bin/env python3
"""scripts/exec_coverage.py — the interpreter planner's fusion coverage on a program (M3/G6).

Joins three inputs into one table per domain, in Markdown:

  --plan FILE      exec::Interpreter::describe() output (the lines "  d<N> <shape> rows <R> level
                   <L> reads {...} | <treatment>: <steps>"), as tests/stage_a/gate_lanes_test.cpp
                   writes it to stage_a_plan.txt;
  --domains FILE   the IR facts per domain (stage_a_domains.csv from the same test: rows, last
                   op, readers through gathers / segments, scan readers, output rows);
  --profile FILE   optional: the stderr of a -DEPYKOS_EXEC_PROFILE run ("  slot <id>: <us> us
                   (<pct>%)" lines; slot 4095 is the output copy), giving each domain's share of
                   the interpreter's run time. With --profile-runs N the per-run microseconds
                   are divided by N instead of the profile's own run count (the record-time
                   solves also run the residual interpreters, so the count in the table is not
                   the number of whole-program runs).

For every domain the table states the planner's treatment (the rule that fired: whole-domain
reduction with fused producers, fused into a reduction, inlined into a consumer's tiles, scan,
plain tiles) and, for a domain that is materialised by plain tiles, the reason the two fusion
rules of include/epykos/exec/interpreter.hpp do not apply, computed from the IR facts:

  fuse_reductions   applies to an elementwise domain read ONLY as members of whole-domain Sum /
                    Affine groups: no gather reader, no output row, not a scan;
  inline_producers  applies to a domain read ONLY through the gathers of ONE elementwise
                    domain (no output row, no segment membership, no other reader), that
                    consumer not a scan and not a whole-domain reduction of mixed member patterns.

Usage: scripts/exec_coverage.py --plan stage_a_plan.txt --domains stage_a_domains.csv [--profile p.txt]
       [--profile-runs N] [--json OUT]
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from typing import Any, Dict, List, Optional

PLAN_RE = re.compile(r"^\s*d(\d+) (\S+) rows (\d+) level (\d+) reads \{([^}]*)\} \| (.*)$")
PROFILE_RE = re.compile(r"^\s*slot\s+(\d+):\s+([0-9.]+) us \(\s*([0-9.]+)%\)")
PROFILE_RUNS_RE = re.compile(r"exec profile over (\d+) runs")
OUTPUT_SLOT = 4095


HEADER_RE = re.compile(r"interpreter plan: tile (\d+) rows, lane_tile (\d+)")
INLINE_MAX_REFS_PER_ROW = 1.25   # exec/interpreter.cpp inline_max_refs_per_row


def row_fusion_pays(lanes: int) -> bool:
    return lanes == 1 or lanes >= 16   # exec/interpreter.cpp row_fusion_pays


def parse_plan(path: str) -> Dict[int, Dict[str, Any]]:
    plan: Dict[int, Dict[str, Any]] = {}
    with open(path) as f:
        for line in f:
            h = HEADER_RE.search(line)
            if h:
                plan[-1] = {"tile": int(h.group(1)), "lane_tile": int(h.group(2))}
                continue
            m = PLAN_RE.match(line.rstrip("\n"))
            if not m:
                continue
            d = int(m.group(1))
            rest = m.group(6)
            treatment, steps = rest, ""
            # The steps follow the last ": " of a plain / fused / scan line; a whole-domain line has none.
            if rest.startswith("whole-domain"):
                treatment = rest
            else:
                idx = rest.rfind(": ")
                if idx >= 0:
                    treatment, steps = rest[:idx], rest[idx + 2:]
            kind = "plain"
            detail = ""
            if treatment.startswith("whole-domain"):
                kind = "reduction"
                fm = re.search(r"(\d+) members evaluated in the block from \{([^}]*)\}, (\d+) gathered", treatment)
                tm = re.search(r"(\d+) members,", treatment)
                if fm:
                    detail = "%s of %s members evaluated in the block from {%s}, %s gathered" % (fm.group(1), tm.group(1) if tm else "?", fm.group(2), fm.group(3))
                else:
                    detail = "%s members, all gathered" % (tm.group(1) if tm else "?")
            elif treatment.startswith("fused into"):
                kind = "fused"
                detail = treatment
            elif treatment.startswith("inlined into"):
                kind = "inlined"
                detail = treatment
            elif treatment.startswith("scan:"):
                kind = "scan"
                detail = treatment
            elif re.match(r"^\d+ tile\(s\)", treatment):
                kind = "plain"
                detail = treatment
            if "inlines {" in treatment and kind == "plain":
                detail += "; " + re.search(r"inlines \{[^}]*\} per tile", treatment).group(0)
            plan[d] = {"domain": d, "shape": m.group(2), "rows": int(m.group(3)), "level": int(m.group(4)),
                       "reads": [int(x[1:]) for x in m.group(5).split()], "kind": kind, "treatment": detail, "steps": steps}
    return plan


def parse_domains(path: str) -> Dict[int, Dict[str, Any]]:
    out: Dict[int, Dict[str, Any]] = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            d = int(row["domain"])
            ints = lambda s: [int(x) for x in s.split()] if s.strip() else []  # noqa: E731
            out[d] = {"shape": row["shape"], "rows": int(row["rows"]), "level": int(row["level"]), "scan": int(row["scan"]) == 1,
                      "chains": int(row["chains"]), "last_op": row["last_op"], "steps": int(row["steps"]), "reads": ints(row["reads"]),
                      "gather_readers": ints(row["gather_readers"]), "segment_readers": ints(row["segment_readers"]),
                      "scan_readers": ints(row["scan_readers"]), "output_rows": int(row["output_rows"]),
                      "gather_refs": int(row.get("gather_refs") or 0)}
    return out


def parse_profile(path: Optional[str], runs_override: Optional[int]) -> Dict[str, Any]:
    if not path:
        return {}
    slots: Dict[int, Dict[str, float]] = {}
    runs = None
    with open(path) as f:
        for line in f:
            r = PROFILE_RUNS_RE.search(line)
            if r:
                runs = int(r.group(1))
            m = PROFILE_RE.match(line)
            if m:
                slots[int(m.group(1))] = {"us": float(m.group(2)), "pct": float(m.group(3))}
    if runs_override and runs:
        for s in slots.values():
            s["us"] = s["us"] * runs / runs_override
    return {"runs": runs, "runs_used": runs_override or runs, "slots": slots}


def reason(d: int, facts: Dict[int, Dict[str, Any]], plan: Dict[int, Dict[str, Any]]) -> str:
    """Why a plain-tile (materialised elementwise) domain is not fused or inlined."""
    f = facts[d]
    if f["last_op"] == "Input":
        return "the Input domain (structural)"
    if f["last_op"] == "Const":
        return "the Const domain (constants that are Sum members or outputs; structural)"
    why: List[str] = []
    gr, sr, scr = f["gather_readers"], f["segment_readers"], f["scan_readers"]
    if f["output_rows"] > 0:
        why.append("%d of its rows are outputs" % f["output_rows"])
    if scr:
        why.append("read by scan domain(s) {%s} (a scan is never fused or inlined into)" % " ".join("d%d" % x for x in scr))
    non_scan_gr = [x for x in gr if x not in scr]
    if len(non_scan_gr) >= 2:
        why.append("gathered by %d domains {%s}: the inliner needs exactly one consumer" % (len(non_scan_gr), " ".join("d%d" % x for x in non_scan_gr)))
    elif len(non_scan_gr) == 1 and sr:
        why.append("gathered by d%d and also a member of reduction(s) {%s}" % (non_scan_gr[0], " ".join("d%d" % x for x in sr)))
    elif len(non_scan_gr) == 1 and not sr and not scr and f["output_rows"] == 0:
        c = non_scan_gr[0]
        ck = plan.get(c, {}).get("kind")
        lt = plan.get(-1, {}).get("lane_tile", 0)
        if ck == "reduction":
            why.append("its only reader d%d is a whole-domain reduction reading it through a gather (not a uniform member pattern)" % c)
        elif ck == "scan":
            why.append("its only reader d%d is a scan" % c)
        elif ck == "fused":
            why.append("its only reader d%d is itself fused into reductions (evaluated per reduction block: no tiles of its own to inline into)" % c)
        elif not row_fusion_pays(lt):
            why.append("inlinable into d%d, but row fusion is off at lane tile %d (row_fusion_pays: L = 1 or L >= 16)" % (c, lt))
        elif f["rows"] > 0 and f["gather_refs"] > INLINE_MAX_REFS_PER_ROW * f["rows"]:
            why.append("gathered %d times for %d rows by d%d: %.2f reads per producer row exceeds the inliner's %.2f (it would recompute the producer)" % (
                f["gather_refs"], f["rows"], c, f["gather_refs"] / f["rows"], INLINE_MAX_REFS_PER_ROW))
        else:
            why.append("its only reader d%d did not inline it (consumer kind %s)" % (c, ck))
    if not gr and sr and not f["output_rows"]:
        why.append("members of reduction(s) {%s} only, yet materialised (per-row Sum or mixed producers)" % " ".join("d%d" % x for x in sr))
    if not gr and not sr and not f["output_rows"]:
        why.append("no reader and no output (dead?)")
    return "; ".join(why) if why else "-"


def render(plan: Dict[int, Dict[str, Any]], facts: Dict[int, Dict[str, Any]], prof: Dict[str, Any]) -> str:
    L: List[str] = []
    slots = prof.get("slots", {})
    total_us = sum(s["us"] for s in slots.values()) if slots else 0.0
    L.append("| d | shape | rows | kind | planner treatment | reason not fused (plain tiles only) | readers (gather / segment) | outputs |%s" % (" time us | share |" if slots else ""))
    L.append("|---|---|---:|---|---|---|---|---:|%s" % ("---:|---:|" if slots else ""))
    kinds: Dict[str, Dict[str, float]] = {}
    for d in sorted(k for k in plan if k >= 0):
        p, f = plan[d], facts[d]
        why = reason(d, facts, plan) if p["kind"] == "plain" else "-"
        readers = "%s / %s" % (" ".join("d%d" % x for x in f["gather_readers"]) or "-", " ".join("d%d" % x for x in f["segment_readers"]) or "-")
        row = "| d%d | `%s` | %d | %s | %s | %s | %s | %d |" % (d, p["shape"], p["rows"], p["kind"], p["treatment"].replace("|", "/"), why, readers, f["output_rows"])
        k = kinds.setdefault(p["kind"], {"domains": 0, "rows": 0, "us": 0.0})
        k["domains"] += 1
        k["rows"] += p["rows"]
        if slots:
            s = slots.get(d)
            us = s["us"] if s else 0.0
            k["us"] += us
            row += " %.1f | %.1f%% |" % (us, 100.0 * us / total_us if total_us else 0.0)
        L.append(row)
    L.append("")
    lt = plan.get(-1, {}).get("lane_tile")
    L.append("Summary by treatment (domains, rows%s; lane tile %s, row fusion %s):" % (
        ", time share" if slots else "", lt, "on" if lt is not None and row_fusion_pays(lt) else "off"))
    for kind in ("reduction", "fused", "inlined", "scan", "plain"):
        if kind not in kinds:
            continue
        k = kinds[kind]
        L.append("  %-9s %3d domains %9d rows%s" % (kind, int(k["domains"]), int(k["rows"]), (" %6.1f%% of the run" % (100.0 * k["us"] / total_us) if slots and total_us else "")))
    if slots:
        oc = slots.get(OUTPUT_SLOT)
        if oc:
            L.append("  output copy: %.1f us (%.1f%%)" % (oc["us"], 100.0 * oc["us"] / total_us))
        L.append("  total per run: %.1f us over %s runs (per-run divisor %s)" % (total_us, prof.get("runs"), prof.get("runs_used")))
    return "\n".join(L)


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(prog="exec_coverage.py", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--domains", required=True)
    ap.add_argument("--profile")
    ap.add_argument("--profile-runs", type=int)
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    plan = parse_plan(a.plan)
    facts = parse_domains(a.domains)
    missing = sorted(set(facts) - set(k for k in plan if k >= 0))
    if missing:
        print("exec_coverage: domains in the facts but not in the plan: %s" % missing, file=sys.stderr)
        return 2
    prof = parse_profile(a.profile, a.profile_runs)
    text = render(plan, facts, prof)
    print(text)
    if a.json:
        rows = []
        for d in sorted(k for k in plan if k >= 0):
            r = dict(plan[d])
            r.update({"facts": facts[d], "reason": reason(d, facts, plan) if plan[d]["kind"] == "plain" else None})
            if prof.get("slots") and d in prof["slots"]:
                r["profile_us"] = prof["slots"][d]["us"]
            rows.append(r)
        with open(a.json, "w") as f:
            json.dump({"format": "epykos-exec-coverage 1", "profile_runs": prof.get("runs"), "profile_runs_used": prof.get("runs_used"), "domains": rows}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
