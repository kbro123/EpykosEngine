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

Usage: tools/costmodel/calibrate.py [--skip-build] [--reps-scale X] [--only-paired] [--out-suffix S]
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


def load_1min() -> float:
    """The 1-minute load average, the same quantity bench/run.sh gates on (D29).

    D63: every grid point records it before and after its capture. A profile capture is a timing
    measurement like any other, and the first extended-grid run of this script was taken while
    other work was on the machine -- which showed up as a whole Stage A config being uniformly
    0.57-0.86x faster in EVERY domain including slot 4095, the output copy, which no interpreter
    option can touch. A ratio between two captures is only meaningful when both were taken under
    a comparable load, so the load is now part of the record rather than something to reconstruct
    afterwards.
    """
    return __import__("os").getloadavg()[0]


def fingerprint_id() -> str:
    out = subprocess.run([str(root() / "scripts" / "fingerprint.sh"), "--id"], check=True, capture_output=True, text=True)
    return out.stdout.strip()


# (case, B, tile, lane_tile, reps, fuse_pairs). reps chosen so one-time construction cost (Stage
# A's record-time solves; negligible for the M1 book) is a small fraction of the accumulated total.
# Both workloads get their own tile sweep AND their own B sweep: fitting dispatch_ns and op_ns
# from only one workload's tile variation (the first cut of this grid varied tile on the M1 book
# alone) leaves them under-determined for the other workload's very different domain sizes.
#
# D63 added the `fuse_pairs` column and the six points that set it False. Every point of the
# original 15 ran with exec::Options at their defaults, so step pairing was CONSTANT across the
# whole grid -- and the cost model's two pairing-sensitive coefficients (`dispatch_ns`, and the
# `byte_ns` ladder that prices the per-step scratch a fused pair removes) were therefore nearly
# collinear with the per-op rates inside every domain, all three being proportional to rows x
# lanes. That is visible in the D48 fit: byte_ns[L1] came out at 2.8e-4 ns/byte, i.e. the fit had
# no way to attribute anything to memory traffic and put it all in op_ns. The fuse_pairs=False
# points hold rows, lanes and the op mix fixed and change ONLY the number of kernel calls and of
# stored intermediates, which is what makes those coefficients identifiable at all.
#
# ORDER MATTERS, and this was learned the hard way (D63). The first extended run put every
# fuse_pairs=True point first and every False point last, so the ~12 minutes of load drift across
# the run mapped straight onto the contrast the grid exists to measure: Stage A came out 0.65x
# FASTER with pairing off, uniformly across every domain including slot 4095, the output copy,
# which no interpreter option touches. Each False point is therefore now run IMMEDIATELY after its
# True twin, and every point records the 1-minute load before and after itself (D9: state the load
# alongside the number).
GRID = [
    # M1 book: the tile sweep at B=1 (D15's own 128/256/512), lane_tile 8 (the default), each
    # immediately followed by its fuse_pairs=False twin (D63; see the ORDER note above).
    ("m1", 1, 128, 8, 150000, True),
    ("m1", 1, 128, 8, 150000, False),
    ("m1", 1, 256, 8, 150000, True),
    ("m1", 1, 256, 8, 150000, False),
    ("m1", 1, 512, 8, 150000, True),
    ("m1", 1, 512, 8, 150000, False),
    ("m1", 8, 256, 8, 60000, True),
    # M1 book: the lane-tile sweep at B=64, tile 256.
    ("m1", 64, 256, 1, 20000, True),
    ("m1", 64, 256, 8, 20000, True),
    ("m1", 64, 256, 8, 20000, False),
    ("m1", 64, 256, 16, 20000, True),
    ("m1", 64, 256, 32, 20000, True),
    ("m1", 64, 256, 64, 20000, True),
    # Stage A: bench/stage_a/stage_a_bench.cpp's BM_Evaluate points (1,8), (64,8), (64,32), plus
    # a tile sweep of its own at B=1 and an intermediate B=8 point.
    ("stage_a", 1, 128, 8, 3000, True),
    ("stage_a", 1, 256, 8, 3000, True),
    ("stage_a", 1, 256, 8, 3000, False),
    ("stage_a", 1, 512, 8, 3000, True),
    ("stage_a", 8, 256, 8, 1200, True),
    ("stage_a", 64, 256, 8, 200, True),
    ("stage_a", 64, 256, 8, 200, False),
    ("stage_a", 64, 256, 32, 200, True),
]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--skip-build", action="store_true", help="reuse an already-built build/profile tree")
    ap.add_argument("--reps-scale", type=float, default=1.0, help="multiply every grid point's reps (a quick smoke run: --reps-scale 0.01)")
    ap.add_argument("--only-paired", action="store_true",
                    help="run only the 15 fuse_pairs=True points (D48's original grid), for an apples-to-apples refit against it")
    ap.add_argument("--index-only", action="store_true",
                    help="skip the captures and refit from the raw captures already in bench/results/<fp>/costmodel_raw/")
    a = ap.parse_args(argv)

    r = root()
    if not a.skip_build and not a.index_only:
        run(["cmake", "--preset", "profile"], cwd=r)
        run(["cmake", "--build", "--preset", "profile", "--target", "costmodel_collect", "costmodel_fit"], cwd=r)

    fp = fingerprint_id()
    print(f"fingerprint: {fp}", file=sys.stderr)
    raw_dir = r / "bench" / "results" / fp / "costmodel_raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    collect_bin = r / "build" / "profile" / "tools" / "costmodel" / "costmodel_collect"
    fit_bin = r / "build" / "profile" / "tools" / "costmodel" / "costmodel_fit"

    runs = []
    grid = [g for g in GRID if g[5]] if a.only_paired else GRID
    for case, b, tile, lane_tile, reps, fuse_pairs in grid:
        reps = max(1, int(reps * a.reps_scale))
        suffix = "" if fuse_pairs else "_nopair"
        name = f"{case}_B{b}_T{tile}_L{lane_tile}{suffix}.txt"
        out_path = raw_dir / name
        load_before = load_1min()
        if a.index_only:
            if not out_path.exists():
                print(f"--index-only: {out_path} is missing, skipping that point", file=sys.stderr)
                continue
        else:
            cmd = [str(collect_bin), "--case", case, "--B", str(b), "--tile", str(tile), "--lane-tile", str(lane_tile), "--reps", str(reps),
                   "--fuse-pairs", "1" if fuse_pairs else "0"]
            print("+ " + " ".join(cmd) + f"  > {out_path}", file=sys.stderr)
            with open(out_path, "w") as f:
                subprocess.run(cmd, cwd=r, check=True, stdout=f, stderr=subprocess.STDOUT)
        runs.append({"case": case, "B": b, "tile": tile, "lane_tile": lane_tile, "fuse_pairs": fuse_pairs, "path": name,
                     "load_1min_before": round(load_before, 2), "load_1min_after": round(load_1min(), 2)})

    loads = [r["load_1min_before"] for r in runs] + [r["load_1min_after"] for r in runs]
    index = {"format": "epykos-costmodel-index 1", "fingerprint": fp, "cores": __import__("os").cpu_count(),
             "load_1min_min": min(loads) if loads else None, "load_1min_max": max(loads) if loads else None,
             "runs": runs}
    index_path = raw_dir / "index.json"
    with open(index_path, "w") as f:
        json.dump(index, f, indent=1)
    print(f"wrote {index_path} ({len(runs)} runs)", file=sys.stderr)

    report_path = r / "bench" / "results" / fp / "cost_model_validation.md"
    run([str(fit_bin), "--index", str(index_path), "--out-dir", str(r / "bench" / "results"), "--report", str(report_path)], cwd=r)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
