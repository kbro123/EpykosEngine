#!/usr/bin/env python3
"""M1/P6: summarise the M1 gate benchmark for one fingerprint directory.

Reads, from bench/results/<id>/:
  m1_p6_fingerprint.json   scripts/fingerprint.sh --json output taken just before measuring (load1 inside)
  m1_p6_hand.json          hand_m1_hand_bench raw output (--benchmark_report_aggregates_only=false)
  m1_p6_interp.json        exec_m1_interp_bench raw output (tile x lane_tile x exp sweep)
  m1_p6_gates.json         {"roundtrip_ok": bool, "e0_ok": bool, "hand_e1_ok": bool, "evidence": {...}}
  m1_p6_notes.json         optional free-form notes merged into m1.json: engine_commit, engine_commit_note,
                           load1_after, load1_after_hand, load1_after_interp, hand_variant2_rerun_after_sweep,
                           ci ({status, commit, run_url, note}), comparison (text), profile (text), caveats (list)

Writes m1.json (everything, machine-readable) and README.md (the short table).

Statistics are computed from the raw Google Benchmark repetitions (run_type == "iteration") of
each benchmark: min, median (mean of the two middle values for even n), p90 (nearest rank: the
ceil(0.9 n)-th smallest), mean, n. Times are Google Benchmark real_time in the benchmark's
time_unit (us). Each Google Benchmark repetition's real_time is the MEAN over that repetition's
iterations (>= min_time worth of evaluations), so these are statistics over n repetition means,
not over single evaluations (docs/WORKLOADS.md "Terms": the protocol's ">= 200 repetitions" counts
evaluations); the README states n and the iteration counts.

Ratios (docs/RESUME.md M1/P6): b1_ratio = interpreter median at B = 1 (std::exp, best tile of the
D15 sweep) / hand eval median (variant 2, fused/shared-recip/std::exp); b64_ratio = interpreter
median at B = 64 (std::exp, best tile x lane_tile) / hand eval_batch(64) median (variant 2).
That pairing (libm std::exp on both sides) was chosen by P4/P6; the docs name "the hand-fused
kernel" without a variant, and the README leads with the ratio under every candidate pairing.
Go criteria are b1 <= 1.3 and b64 <= 1.1; this script reports numbers, it does not judge.

Usage: bench/results/m1_summarise.py bench/results/<id>
"""
import json
import math
import os
import re
import sys
from typing import Dict, List, Optional


def load(path):
    with open(path) as f:
        return json.load(f)


def stats(values: List[float]) -> Dict[str, float]:
    v = sorted(values)
    n = len(v)
    if n == 0:
        raise ValueError("no repetitions")
    median = v[n // 2] if n % 2 else 0.5 * (v[n // 2 - 1] + v[n // 2])
    p90 = v[max(0, int(math.ceil(0.9 * n)) - 1)]
    return {"n": n, "min": v[0], "median": median, "p90": p90, "max": v[-1], "mean": sum(v) / n}


def raw_by_name(bench_json) -> Dict[str, dict]:
    """name -> {"times": [...], "unit": str, "counters": {...}} from the raw repetitions."""
    out: Dict[str, dict] = {}
    for b in bench_json["benchmarks"]:
        if b.get("run_type") != "iteration":
            continue
        e = out.setdefault(b["name"], {"times": [], "iterations": [], "unit": b.get("time_unit", "ns"), "counters": {}, "label": b.get("label", "")})
        e["times"].append(b["real_time"])
        e["iterations"].append(b.get("iterations", 0))
        for k in ("B", "tile", "lane_tile", "values", "domains", "times", "rows"):
            if k in b:
                e["counters"][k] = b[k]
    return out


def summarise(bench_json) -> Dict[str, dict]:
    res = {}
    for name, e in raw_by_name(bench_json).items():
        s = stats(e["times"])
        s["unit"] = e["unit"]
        s["label"] = e["label"]
        s["counters"] = e["counters"]
        s["raw"] = e["times"]
        s["iterations"] = e["iterations"]
        s["evaluations"] = sum(e["iterations"])
        res[name] = s
    return res


INTERP_RE = re.compile(r"^(BM_InterpEval|BM_InterpEvalBatch)/tile:(\d+)/lane_tile:(\d+)/exp:(\d)$")


def interp_table(interp: Dict[str, dict]):
    """Rows of the sweep: (bench, tile, lane_tile, exp) -> stats."""
    rows = []
    for name, s in interp.items():
        m = INTERP_RE.match(name)
        if not m:
            continue
        rows.append({"name": name, "bench": m.group(1), "tile": int(m.group(2)), "lane_tile": int(m.group(3)),
                     "exp": int(m.group(4)), "min": s["min"], "median": s["median"], "p90": s["p90"], "n": s["n"]})
    rows.sort(key=lambda r: (r["bench"], r["exp"], r["tile"], r["lane_tile"]))
    return rows


def best(rows, bench, exp, tile: Optional[int] = None):
    cand = [r for r in rows if r["bench"] == bench and r["exp"] == exp and (tile is None or r["tile"] == tile)]
    if not cand:
        return None
    return min(cand, key=lambda r: r["median"])


def fmt(x, nd=1):
    return "-" if x is None else "{:.{nd}f}".format(x, nd=nd)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    d = argv[1].rstrip("/")
    fp = load(os.path.join(d, "m1_p6_fingerprint.json"))
    hand_json = load(os.path.join(d, "m1_p6_hand.json"))
    interp_json = load(os.path.join(d, "m1_p6_interp.json"))
    gates = load(os.path.join(d, "m1_p6_gates.json"))
    notes_path = os.path.join(d, "m1_p6_notes.json")
    notes = load(notes_path) if os.path.exists(notes_path) else {}

    hand = summarise(hand_json)
    interp = summarise(interp_json)
    rows = interp_table(interp)

    hand_names = {
        "eval": {v: "BM_HandEval/%d" % v for v in range(4)},
        "eval_batch": {v: "BM_HandEvalBatch/%d" % v for v in range(4)},
    }
    variant_label = {0: "fused/shared-recip/exp_poly", 1: "fused/per-row-div/exp_poly",
                     2: "fused/shared-recip/std::exp", 3: "reference-arith/std::exp"}

    # --- like-for-like rows (std::exp both sides) ---------------------------------------------
    b1_best = best(rows, "BM_InterpEval", 0)
    b64_best = best(rows, "BM_InterpEvalBatch", 0)
    tile_best = b1_best["tile"]
    b64_at_tile_best = best(rows, "BM_InterpEvalBatch", 0, tile=tile_best)
    hand_eval2 = hand[hand_names["eval"][2]]
    hand_batch2 = hand[hand_names["eval_batch"][2]]
    b1_ratio = b1_best["median"] / hand_eval2["median"]
    b64_ratio = b64_best["median"] / hand_batch2["median"]
    b64_ratio_at_tile_best = b64_at_tile_best["median"] / hand_batch2["median"]

    # --- informational rows: exp_poly both sides; and the reference-arithmetic hand variant ----
    b1_poly = best(rows, "BM_InterpEval", 1)
    b64_poly = best(rows, "BM_InterpEvalBatch", 1)
    info = {
        "b1_exp_poly_ratio_vs_hand0": b1_poly["median"] / hand[hand_names["eval"][0]]["median"],
        "b64_exp_poly_ratio_vs_hand0": b64_poly["median"] / hand[hand_names["eval_batch"][0]]["median"],
        "b1_ratio_vs_hand3_reference_arith": b1_best["median"] / hand[hand_names["eval"][3]]["median"],
        "b64_ratio_vs_hand3_reference_arith": b64_best["median"] / hand[hand_names["eval_batch"][3]]["median"],
        "b1_exp_poly_best": b1_poly, "b64_exp_poly_best": b64_poly,
    }

    # --- every candidate denominator (the docs do not name a hand variant) ---------------------
    def pairing(label, num_b1, num_b64, hv, exactness):
        r1 = num_b1["median"] / hand[hand_names["eval"][hv]]["median"]
        r64 = num_b64["median"] / hand[hand_names["eval_batch"][hv]]["median"]
        return {"pairing": label, "exactness": exactness, "hand_variant": hv,
                "b1_interp_us": num_b1["median"], "b1_hand_us": hand[hand_names["eval"][hv]]["median"], "b1_ratio": r1,
                "b64_interp_us": num_b64["median"], "b64_hand_us": hand[hand_names["eval_batch"][hv]]["median"], "b64_ratio": r64,
                "b1_meets": r1 <= 1.3, "b64_meets": r64 <= 1.1}
    pairings = [
        pairing("interpreter std::exp vs hand variant 2 (fused/shared-recip/std::exp): the P4/P6 like-for-like pairing, libm exp on both sides",
                b1_best, b64_best, 2, "interpreter E0 vs hand E1"),
        pairing("interpreter exp_poly vs hand variant 0 (default: fused/shared-recip/exp_poly): the same exactness class on both sides",
                b1_poly, b64_poly, 0, "E1 vs E1"),
        pairing("interpreter std::exp (the gated E0 mode) vs hand variant 0 (default, as specified and delivered by P5)",
                b1_best, b64_best, 0, "E0 vs E1"),
        pairing("interpreter std::exp vs hand variant 3 (reference arithmetic: the interpreter's own operation order, std::exp)",
                b1_best, b64_best, 3, "E0 vs E0"),
    ]
    # exp parity at B=64: how much of each side's time is the scalar libm exp (interp exp:0 - exp:1 at the
    # gate point; hand v2 - v0).
    b64_gate_point_poly = next((r for r in rows if r["bench"] == "BM_InterpEvalBatch" and r["exp"] == 1
                                and r["tile"] == b64_best["tile"] and r["lane_tile"] == b64_best["lane_tile"]), None)
    exp_parity = {
        "interp_exp0_minus_exp1_us_b64": (b64_best["median"] - b64_gate_point_poly["median"]) if b64_gate_point_poly else None,
        "hand_v2_minus_v0_us_b64": hand[hand_names["eval_batch"][2]]["median"] - hand[hand_names["eval_batch"][0]]["median"],
    }

    # --- the measurement statistic and the load rule ------------------------------------------
    gate_rows = {"interpreter B=1": b1_best["name"], "hand eval B=1": hand_names["eval"][2],
                 "interpreter B=64": b64_best["name"], "hand eval_batch B=64": hand_names["eval_batch"][2]}
    def its(name):
        s = interp.get(name) or hand.get(name)
        return {"n": s["n"], "iterations_min": min(s["iterations"]), "iterations_max": max(s["iterations"]), "evaluations": s["evaluations"]}
    statistic = {
        "definition": "min / median / p90 over n Google Benchmark repetitions; each repetition's real_time is the mean of its "
                      "iterations (>= min_time of evaluations), so p90 is a p90 of repetition means, not of evaluations",
        "protocol": "docs/WORKLOADS.md M1 Measurement asks for >= 200 repetitions (timed evaluations); this round has n = 20 "
                    "repetition means per row, a stated deviation; the gate uses medians",
        "gate_rows": {k: its(v) for k, v in gate_rows.items()},
    }
    cp, cl = fp.get("cores_physical"), fp.get("cores_logical")
    loads = [x for x in (fp.get("load1"), notes.get("load1_after_hand"), notes.get("load1_after_interp"), notes.get("load1_after")) if x is not None]
    after = notes.get("hand_variant2_rerun_after_sweep")
    load_rule = {
        "definition": "docs/WORKLOADS.md Terms: cores = logical CPUs (hardware threads)",
        "threshold_logical": (cl / 2.0) if cl else None, "threshold_physical": (cp / 2.0) if cp else None,
        "loads_during_measurement": loads,
        "within_logical_threshold": all(x <= cl / 2.0 for x in loads) if cl and loads else None,
        "within_physical_threshold": all(x <= cp / 2.0 for x in loads) if cp and loads else None,
        "physical_reading": None,
    }
    if after and cp and fp.get("load1") is not None and fp["load1"] > cp / 2.0:
        # Only when the before-sweep hand run would actually be discarded under the physical reading.
        load_rule["physical_reading"] = {
            "note": "Under the physical reading the before-sweep hand run (load %s) would be discarded and the after-sweep re-run "
                    "(load %s) used as the denominator" % (fp.get("load1"), notes.get("load1_after_interp")),
            "b1_ratio": after.get("b1_ratio_with_after_denominator"), "b64_ratio": after.get("b64_ratio_with_after_denominator"),
        }

    # --- tile sweep tables (medians) ------------------------------------------------------------
    tiles = sorted({r["tile"] for r in rows})
    lane_tiles = sorted({r["lane_tile"] for r in rows if r["bench"] == "BM_InterpEvalBatch"})
    sweep_b1 = {str(t): {str(e): next((r for r in rows if r["bench"] == "BM_InterpEval" and r["tile"] == t and r["exp"] == e), None)
                         for e in (0, 1)} for t in tiles}
    sweep_b64 = {str(t): {str(l): {str(e): next((r for r in rows if r["bench"] == "BM_InterpEvalBatch" and r["tile"] == t
                                                  and r["lane_tile"] == l and r["exp"] == e), None)
                                    for e in (0, 1)} for l in lane_tiles} for t in tiles}

    out = {
        "package": "M1/P6",
        "engine_commit": notes.get("engine_commit"),
        "fingerprint": fp,
        "load1_before_measurement": fp.get("load1"),
        "load1_after_measurement": notes.get("load1_after"),
        "benchmark_flags": "--benchmark_repetitions=20 --benchmark_min_time=0.2s --benchmark_report_aggregates_only=false",
        "statistics": "per benchmark over the 20 raw repetitions of real_time (us): min, median, p90 (nearest rank), mean",
        "hand": {name: {k: v for k, v in s.items()} for name, s in hand.items()},
        "hand_variants": variant_label,
        "interp": {name: {k: v for k, v in s.items()} for name, s in interp.items()},
        "tile_sweep": {"tiles": tiles, "lane_tiles": lane_tiles, "B1": sweep_b1, "B64": sweep_b64},
        "tile_best": tile_best,
        "gate": {
            "like_for_like": "interpreter exp:0 (std::exp) vs hand variant 2 (fused/shared-recip/std::exp)",
            "b1_interp_us": b1_best["median"], "b1_interp_row": b1_best,
            "b1_hand_us": hand_eval2["median"], "b1_ratio": b1_ratio,
            "b64_interp_us": b64_best["median"], "b64_interp_row": b64_best,
            "b64_hand_us": hand_batch2["median"], "b64_ratio": b64_ratio,
            "b64_at_b1_best_tile": {"row": b64_at_tile_best, "ratio": b64_ratio_at_tile_best},
            "go_criteria": {"b1_ratio_max": 1.3, "b64_ratio_max": 1.1},
            "b1_meets": b1_ratio <= 1.3, "b64_meets": b64_ratio <= 1.1,
        },
        "informational": info,
        "pairings": pairings,
        "exp_parity_us_b64": exp_parity,
        "statistic": statistic,
        "load_rule": load_rule,
        "ci": notes.get("ci"),
        "correctness_gates_reference_preset": gates,
        "notes": notes,
    }
    with open(os.path.join(d, "m1.json"), "w") as f:
        json.dump(out, f, indent=1, sort_keys=False)

    # --- README -----------------------------------------------------------------------------------
    L = []
    L.append("# M1 gate benchmark (P6) — fingerprint `%s`" % fp["id"])
    L.append("")
    L.append("%s, %d physical / %d logical cores, %s, flags `%s`. 1-minute load before measuring: %s%s." % (
        fp["cpu"], fp["cores_physical"], fp["cores_logical"], fp["compiler"], fp["flags"], fp.get("load1"),
        (", after: %s" % notes["load1_after"]) if "load1_after" in notes else ""))
    if notes.get("engine_commit"):
        L.append("Engine commit measured: `%s`%s." % (notes["engine_commit"], (" (%s)" % notes["engine_commit_note"]) if notes.get("engine_commit_note") else ""))
    L.append("Google Benchmark, `%s`; tables prebuilt, state written fresh each iteration; stats over the 20 repetition means "
             "(real time, us; see Measurement statistic). Interpreter and hand kernel compiled with the release preset (D13). Correctness gates run under the reference preset." % out["benchmark_flags"])
    L.append("")
    L.append("## Read first: the outcome depends on the pairing")
    L.append("")
    L.append("ROADMAP.md M1 Go and RESUME.md P6 say \"interpreter within 1.3x of the hand-fused kernel single-state, within 1.1x batched\" "
             "and name no hand variant or exp implementation. The table below gives the ratio under every candidate denominator "
             "(medians, us). The gate table that follows uses the pairing P4/P6 chose (row 1); which pairing is the gate is a "
             "DECISIONS entry the orchestrator owes before the M1 verdict (M1/P7 review).")
    L.append("")
    L.append("| pairing | exactness | B=1 interp / hand | B=1 ratio (<= 1.3) | B=64 interp / hand | B=64 ratio (<= 1.1) |")
    L.append("|---|---|---:|---:|---:|---:|")
    for pr in pairings:
        L.append("| %s | %s | %.2f / %.2f | **%.3f** (%s) | %.1f / %.1f | **%.3f** (%s) |" % (
            pr["pairing"], pr["exactness"], pr["b1_interp_us"], pr["b1_hand_us"], pr["b1_ratio"], "met" if pr["b1_meets"] else "NOT met",
            pr["b64_interp_us"], pr["b64_hand_us"], pr["b64_ratio"], "met" if pr["b64_meets"] else "NOT met"))
    L.append("")
    if exp_parity["interp_exp0_minus_exp1_us_b64"] is not None:
        L.append("Row 1 puts the identical scalar libm exp on both sides: at B=64 it is %.0f us of the interpreter's %.1f "
                 "(exp:0 minus exp:1 at the gate point) and %.0f us of the hand kernel's %.1f (variant 2 minus variant 0), so the "
                 "non-exp remainder ratio is %.3f, the same as the E1-vs-E1 row. Against the hand kernel as specified and delivered "
                 "(variant 0, exp_poly, E1) the interpreter's gated E0 mode is %.3fx batched." % (
                     exp_parity["interp_exp0_minus_exp1_us_b64"], b64_best["median"], exp_parity["hand_v2_minus_v0_us_b64"],
                     hand[hand_names["eval_batch"][2]]["median"],
                     (b64_best["median"] - exp_parity["interp_exp0_minus_exp1_us_b64"]) / hand[hand_names["eval_batch"][0]]["median"],
                     pairings[2]["b64_ratio"]))
        L.append("")
    L.append("## Gate (like for like: interpreter std::exp vs hand fused/shared-recip/std::exp)")
    L.append("")
    L.append("| row | config | min | median | p90 | ratio | go |")
    L.append("|---|---|---:|---:|---:|---:|---|")
    L.append("| interpreter B=1 | tile %d, lane_tile 1, std::exp | %s | %s | %s | **%.3f** | <= 1.3: %s |" % (
        b1_best["tile"], fmt(b1_best["min"]), fmt(b1_best["median"]), fmt(b1_best["p90"]), b1_ratio, "yes" if b1_ratio <= 1.3 else "NO"))
    L.append("| hand eval B=1 | variant 2 | %s | %s | %s | 1 | |" % (fmt(hand_eval2["min"]), fmt(hand_eval2["median"]), fmt(hand_eval2["p90"])))
    L.append("| interpreter B=64 | tile %d, lane_tile %d, std::exp | %s | %s | %s | **%.3f** | <= 1.1: %s |" % (
        b64_best["tile"], b64_best["lane_tile"], fmt(b64_best["min"]), fmt(b64_best["median"]), fmt(b64_best["p90"]), b64_ratio, "yes" if b64_ratio <= 1.1 else "NO"))
    L.append("| hand eval_batch B=64 | variant 2 | %s | %s | %s | 1 | |" % (fmt(hand_batch2["min"]), fmt(hand_batch2["median"]), fmt(hand_batch2["p90"])))
    L.append("")
    L.append("B=64 at the B=1-best tile (%d): best lane_tile %d, median %s us, ratio %.3f." % (
        tile_best, b64_at_tile_best["lane_tile"], fmt(b64_at_tile_best["median"]), b64_ratio_at_tile_best))
    L.append("")
    L.append("## Measurement statistic")
    L.append("")
    L.append("%s. %s. Gate rows:" % (statistic["definition"], statistic["protocol"]))
    L.append("")
    L.append("| row | n (repetition means) | iterations per repetition (min..max) | timed evaluations |")
    L.append("|---|---:|---:|---:|")
    for k, v in statistic["gate_rows"].items():
        L.append("| %s | %d | %d..%d | %d |" % (k, v["n"], v["iterations_min"], v["iterations_max"], v["evaluations"]))
    L.append("")
    L.append("## Load rule")
    L.append("")
    lr = load_rule
    L.append("%s: threshold %s (physical reading: %s). 1-minute loads during measurement: %s; within the logical threshold: %s; within the physical threshold: %s.%s" % (
        lr["definition"], fmt(lr["threshold_logical"], 0), fmt(lr["threshold_physical"], 0),
        ", ".join(str(x) for x in lr["loads_during_measurement"]),
        "yes" if lr["within_logical_threshold"] else "no", "yes" if lr["within_physical_threshold"] else "no",

        (" " + lr["physical_reading"]["note"] + ": ratios %.3f / %.3f." % (lr["physical_reading"]["b1_ratio"], lr["physical_reading"]["b64_ratio"]))
        if lr.get("physical_reading") and lr["physical_reading"].get("b1_ratio") is not None else ""))
    L.append("")
    L.append("## CI")
    L.append("")
    ci = notes.get("ci")
    if ci:
        L.append("GitHub Actions (ubuntu GCC 13 release + reference, macOS Apple clang release): %s at `%s`%s.%s" % (
            ci.get("status"), ci.get("commit"), (" (%s)" % ci["run_url"]) if ci.get("run_url") else "",
            (" " + ci["note"]) if ci.get("note") else ""))
    else:
        L.append("Not recorded for this round.")
    L.append("")
    L.append("## Correctness gates (reference preset, -ffp-contract=off)")
    L.append("")
    L.append("| gate | test executable | result |")
    L.append("|---|---|---|")
    ev = gates.get("evidence", {})
    L.append("| P3 round-trip identity | ir_roundtrip_test | %s |" % ("pass" if gates["roundtrip_ok"] else "FAIL"))
    L.append("| P4 E0 (interpreter vs replay and oracle, B=1 and B=64) | exec_m1_interp_e0_test | %s |" % ("pass" if gates["e0_ok"] else "FAIL"))
    L.append("| P5 E1 (hand kernel vs double maths, 1e-12) | hand_m1_hand_test | %s |" % ("pass" if gates["hand_e1_ok"] else "FAIL"))
    for k, v in ev.items():
        L.append("")
        L.append("`%s`: %s" % (k, v))
    L.append("")
    L.append("## Hand kernel (all variants)")
    L.append("")
    L.append("| benchmark | variant | min | median | p90 | us/state |")
    L.append("|---|---|---:|---:|---:|---:|")
    for kind in ("eval", "eval_batch"):
        for v in range(4):
            s = hand[hand_names[kind][v]]
            B = 64 if kind == "eval_batch" else 1
            L.append("| %s | %d %s | %s | %s | %s | %s |" % (hand_names[kind][v], v, variant_label[v], fmt(s["min"]), fmt(s["median"]), fmt(s["p90"]), fmt(s["median"] / B, 2)))
    if "BM_HandBuild" in hand:
        L.append("| BM_HandBuild | table build | %s | %s | %s | |" % (fmt(hand["BM_HandBuild"]["min"]), fmt(hand["BM_HandBuild"]["median"]), fmt(hand["BM_HandBuild"]["p90"])))
    L.append("")
    L.append("## Interpreter tile sweep (D15), medians in us")
    L.append("")
    L.append("B=1 (lane_tile 1):")
    L.append("")
    L.append("| tile | std::exp min | median | p90 | exp_poly min | median | p90 |")
    L.append("|---:|---:|---:|---:|---:|---:|---:|")
    for t in tiles:
        a = sweep_b1[str(t)]["0"]; b = sweep_b1[str(t)]["1"]
        L.append("| %d | %s | %s | %s | %s | %s | %s |" % (t, fmt(a["min"]), fmt(a["median"]), fmt(a["p90"]), fmt(b["min"]), fmt(b["median"]), fmt(b["p90"])))
    L.append("")
    L.append("B=64, std::exp (median us per call of 64 states; us/state = median/64):")
    L.append("")
    L.append("| tile \\ lane_tile | " + " | ".join(str(l) for l in lane_tiles) + " |")
    L.append("|---:|" + "---:|" * len(lane_tiles))
    for t in tiles:
        L.append("| %d | " % t + " | ".join(fmt(sweep_b64[str(t)][str(l)]["0"]["median"]) for l in lane_tiles) + " |")
    L.append("")
    L.append("B=64, exp_poly (median us):")
    L.append("")
    L.append("| tile \\ lane_tile | " + " | ".join(str(l) for l in lane_tiles) + " |")
    L.append("|---:|" + "---:|" * len(lane_tiles))
    for t in tiles:
        L.append("| %d | " % t + " | ".join(fmt(sweep_b64[str(t)][str(l)]["1"]["median"]) for l in lane_tiles) + " |")
    L.append("")
    L.append("## Informational ratios (D9)")
    L.append("")
    L.append("- exp_poly both sides: B=1 %.3f (interp tile %d %.1f us vs hand variant 0 %.1f us); B=64 %.3f (tile %d lane_tile %d %.1f us vs %.1f us)." % (
        info["b1_exp_poly_ratio_vs_hand0"], b1_poly["tile"], b1_poly["median"], hand[hand_names["eval"][0]]["median"],
        info["b64_exp_poly_ratio_vs_hand0"], b64_poly["tile"], b64_poly["lane_tile"], b64_poly["median"], hand[hand_names["eval_batch"][0]]["median"]))
    L.append("- vs the hand reference-arithmetic variant 3 (the interpreter's own operation order, no fma, division, std::exp): B=1 %.3f, B=64 %.3f." % (
        info["b1_ratio_vs_hand3_reference_arith"], info["b64_ratio_vs_hand3_reference_arith"]))
    for k in ("BM_InterpBuild", "BM_InterpPipeline"):
        if k in interp:
            s = interp[k]
            L.append("- %s: median %.3f %s (min %.3f, p90 %.3f)." % (k, s["median"], s["unit"], s["min"], s["p90"]))
    if notes.get("comparison"):
        L.append("")
        L.append("## Compared with the previous measurement")
        L.append("")
        L.append(notes["comparison"])
    if notes.get("profile"):
        L.append("")
        L.append("## Profile")
        L.append("")
        L.append(notes["profile"])
    if notes.get("caveats"):
        L.append("")
        L.append("## Caveats")
        L.append("")
        for c in notes["caveats"]:
            L.append("- " + c)
    L.append("")
    L.append("Raw data: `m1_p6_hand.json`, `m1_p6_interp.json` (20 Google Benchmark repetitions each), `m1_p6_gates.json`, `m1_p6_fingerprint.json`, `m1_p6_notes.json`; everything above in `m1.json`. Regenerate with `bench/results/m1_summarise.py bench/results/%s`." % fp["id"])

    with open(os.path.join(d, "README.md"), "w") as f:
        f.write("\n".join(L) + "\n")

    print(json.dumps({"tile_best": tile_best, "b1_interp_us": b1_best["median"], "b1_hand_us": hand_eval2["median"], "b1_ratio": b1_ratio,
                      "b64_interp_us": b64_best["median"], "b64_hand_us": hand_batch2["median"], "b64_ratio": b64_ratio,
                      "b64_row": {k: b64_best[k] for k in ("tile", "lane_tile")},
                      "b64_ratio_at_tile_best": b64_ratio_at_tile_best}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
