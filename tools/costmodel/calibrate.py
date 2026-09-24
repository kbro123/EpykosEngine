#!/usr/bin/env python3
"""tools/costmodel/calibrate.py — M4/CM's calibration driver (docs/PROBLEM.md §7).

Configures and builds the `profile` preset (CMakePresets.json: release flags plus
EPYKOS_EXEC_PROFILE=ON), runs tools/costmodel/collect_main.cpp (`costmodel_collect`) once per
(case, B, tile, lane_tile) grid point as its own process — the profile table is a process-lifetime
global (src/exec/interpreter.cpp), so one process per point is required, not a convenience — and
writes each capture plus an index JSON to bench/results/<fingerprint>/costmodel_raw/. Then runs
`costmodel_fit` (built by the same preset) to fit CostCoefficients by linear least squares and
write bench/results/<fingerprint>/cost_model.json and cost_model_validation.md.

The grid reuses the (B, tile, lane_tile) points docs/RESUME.md §5 "M3 result" already measured
with BM_Evaluate (D45's ad hoc EPYKOS_EXEC_PROFILE run: tile 128/256/512 at B=1, lane_tile
1/8/16/32/64 at B=64 for the M1 book; (1,8), (64,8), (64,32) for Stage A) so the fitted model can
be checked against those numbers directly, not just against itself.

Usage: tools/costmodel/calibrate.py [--skip-build] [--reps-scale X]
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


def root() -> Path:
    return Path(__file__).resolve().parents[2]


def run(cmd: list[str], cwd: Path | None = None) -> None:
    print("+ " + " ".join(str(c) for c in cmd), file=sys.stderr)
    subprocess.run(cmd, cwd=cwd, check=True)


def fingerprint_id() -> str:
    out = subprocess.run([str(root() / "scripts" / "fingerprint.sh"), "--id"], check=True, capture_output=True, text=True)
    return out.stdout.strip()


# (case, B, tile, lane_tile, reps). reps chosen so one-time construction cost (Stage A's
# record-time solves; negligible for the M1 book) is a small fraction of the accumulated total.
# Both workloads get their own tile sweep AND their own B sweep: fitting dispatch_ns and op_ns
# from only one workload's tile variation (the first cut of this grid varied tile on the M1 book
# alone) leaves them under-determined for the other workload's very different domain sizes.
GRID = [
    # M1 book: the tile sweep at B=1 (D15's own 128/256/512), lane_tile 8 (the default).
    ("m1", 1, 128, 8, 150000),
    ("m1", 1, 256, 8, 150000),
    ("m1", 1, 512, 8, 150000),
    ("m1", 8, 256, 8, 60000),
    # M1 book: the lane-tile sweep at B=64, tile 256.
    ("m1", 64, 256, 1, 20000),
    ("m1", 64, 256, 8, 20000),
    ("m1", 64, 256, 16, 20000),
    ("m1", 64, 256, 32, 20000),
    ("m1", 64, 256, 64, 20000),
    # Stage A: bench/stage_a/stage_a_bench.cpp's BM_Evaluate points (1,8), (64,8), (64,32), plus
    # a tile sweep of its own at B=1 and an intermediate B=8 point.
    ("stage_a", 1, 128, 8, 3000),
    ("stage_a", 1, 256, 8, 3000),
    ("stage_a", 1, 512, 8, 3000),
    ("stage_a", 8, 256, 8, 1200),
    ("stage_a", 64, 256, 8, 200),
    ("stage_a", 64, 256, 32, 200),
]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--skip-build", action="store_true", help="reuse an already-built build/profile tree")
    ap.add_argument("--reps-scale", type=float, default=1.0, help="multiply every grid point's reps (a quick smoke run: --reps-scale 0.01)")
    a = ap.parse_args(argv)

    r = root()
    if not a.skip_build:
        run(["cmake", "--preset", "profile"], cwd=r)
        run(["cmake", "--build", "--preset", "profile", "--target", "costmodel_collect", "costmodel_fit"], cwd=r)

    fp = fingerprint_id()
    print(f"fingerprint: {fp}", file=sys.stderr)
    raw_dir = r / "bench" / "results" / fp / "costmodel_raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    collect_bin = r / "build" / "profile" / "tools" / "costmodel" / "costmodel_collect"
    fit_bin = r / "build" / "profile" / "tools" / "costmodel" / "costmodel_fit"

    runs = []
    for case, b, tile, lane_tile, reps in GRID:
        reps = max(1, int(reps * a.reps_scale))
        name = f"{case}_B{b}_T{tile}_L{lane_tile}.txt"
        out_path = raw_dir / name
        cmd = [str(collect_bin), "--case", case, "--B", str(b), "--tile", str(tile), "--lane-tile", str(lane_tile), "--reps", str(reps)]
        print("+ " + " ".join(cmd) + f"  > {out_path}", file=sys.stderr)
        with open(out_path, "w") as f:
            subprocess.run(cmd, cwd=r, check=True, stdout=f, stderr=subprocess.STDOUT)
        runs.append({"case": case, "B": b, "tile": tile, "lane_tile": lane_tile, "path": name})

    index = {"format": "epykos-costmodel-index 1", "fingerprint": fp, "runs": runs}
    index_path = raw_dir / "index.json"
    with open(index_path, "w") as f:
        json.dump(index, f, indent=1)
    print(f"wrote {index_path} ({len(runs)} runs)", file=sys.stderr)

    report_path = r / "bench" / "results" / fp / "cost_model_validation.md"
    run([str(fit_bin), "--index", str(index_path), "--out-dir", str(r / "bench" / "results"), "--report", str(report_path)], cwd=r)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
