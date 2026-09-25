# bench/results

Committed benchmark results, one directory per machine+toolchain fingerprint (D9, D13, D29). Numbers are only ever
compared within one fingerprint directory; `<fingerprint-id>` is the 12-hex id printed by `scripts/fingerprint.sh`
(CPU brand, core counts, compiler version, release flags).

```
bench/results/<fingerprint-id>/<name>.json      one benchmark run, written by bench/run.sh (format epykos-bench 1)
bench/results/<fingerprint-id>/baseline.json    the accepted baseline per benchmark, medians (format epykos-baseline 1)
bench/results/<fingerprint-id>/README.md        the narrative of a measurement round (optional; M1/P6 wrote one)
bench/results/<fingerprint-id>/tmp/             scratch: raw Google Benchmark output, fingerprints (gitignored)
bench/targets.json                              the absolute targets of docs/ROADMAP.md (format epykos-targets 1)
```

## Producing a result

```
bench/run.sh build/release/bench/exec_m1_interp_bench            # -> bench/results/<id>/exec_m1_interp.json
bench/run.sh build/release/bench/hand_m1_hand_bench              # -> bench/results/<id>/hand_m1_hand.json
bench/run.sh --repetitions 200 --min-time 0.02s build/release/bench/hand_m1_hand_bench --benchmark_filter=BM_HandEval/2
```

`<name>` is the benchmark target without `_bench` (override with `--name`); a baseline entry is keyed by that
derived name, so measure and seed under it and keep `--name` for exploratory runs that are not gated (D34). `run.sh`
refuses to run (exit 2) when
the 1-minute load average exceeds cores/2 (cores = logical CPUs, `docs/WORKLOADS.md` M1 Measurement); the default
protocol is 20 Google Benchmark repetitions of at least 0.2 s (the M1 rounds); Google Benchmark arguments after the
binary are passed through (the last occurrence of a flag wins). Every result records the fingerprint, the 1-minute
load before and after, the git commit and branch (and whether the tree was dirty), the preset, the `EPYKOS_*`
environment, each benchmark's parameters (from its name: `tile`, `lane_tile`, `exp`, positional `arg0`; and the
constant user counters such as `B`), and min / median / p90 / max / mean over the repetitions with the raw
repetition times, the iterations per repetition and the number of timed evaluations. The statistic is the M1/P6 one:
p90 by nearest rank, over *repetition means* (each Google Benchmark repetition's real_time is the mean of its
iterations), stated in the file. The full field list is in the header of `scripts/bench_results.py`.

## Gating a result

```
scripts/perf_gate.py bench/results/<id>/exec_m1_interp.json [bench/results/<id>/hand_m1_hand.json ...]
scripts/perf_gate.py exec_m1_interp            # a run name resolves through this machine's fingerprint
scripts/perf_gate.py --accept <files>          # move the baseline: perf commits only, before/after in the message
```

The gate (`scripts/perf_gate.py`, header for the options) compares every benchmark of the given runs with
`baseline.json` of the **same fingerprint only**: a fresh median above 1.25x its baseline median fails (exit 1);
below 0.8x is reported as faster (accept it so the baseline follows); benchmarks new to the baseline and baseline
entries not measured this time are reported, not failed. It then checks the absolute targets of `bench/targets.json`
(ROADMAP: M1 interpreter <= 1.3x / 1.1x the hand kernel under the D27 pairing, M3 <= 1.05x, M5 < 1 s): a target is
evaluated when every run it names is available on the fingerprint (given on the command line, else the committed
`<run>.json` of the directory, which must pass the load rule too) and at least one side is a given run; a missed
target fails (exit 1). It refuses (exit 2) a run whose load before or after exceeded cores/2, runs or a baseline of
another fingerprint, a run with no baseline entry (seed it with `--accept`), and malformed input. `--json` writes
the report as JSON as well.

`--accept` rewrites the baseline entries of the given runs (per benchmark: median, unit, n, min, p90, commit, date;
per run: source file, binary, preset, commit, date and the Google Benchmark arguments the run was made with, so a
baseline seeded from a filtered run states its filter and the rows outside it are "new" in a full sweep) and prints
the before -> after table for the commit message. Only perf commits change `baseline.json`. A run without a baseline
entry is refused, and the refusal names any entry that holds the same binary under another name: the baseline is
keyed by the run name, and an entry seeded under an ad hoc `--name` gates nothing the default invocation produces
(D34). `tests/scripts/perf_gate_test.cpp` checks the committed results for that: every baseline run is keyed by the
name derived from its binary, and its committed `<run>.json` is gated, never refused.

## d448afd70180 (Mac Pro, Xeon W-3223, Apple clang 21)

- `baseline.json`: seeded by M2/Q5 from the M1 result (D27: P6 attempt 5, engine `b32182a`, results `097d54b`).
- `hand_m1_hand.json`, `exec_m1_interp.json`: fresh runs at the M2 gate re-run (engine `359f321`, M2/m2-gate-2, quiet
  machine, `m2.md`), gated against the baseline: PASS, 47 rows within 0.945-1.020x, D27 targets b1 1.048 / b64 1.076.
  They replaced the M2 gate round's runs at `8914934` (47 rows within 0.997-1.029x, b1 1.043 / b64 1.066, tabulated
  in `m2.md`), which had replaced the P6 attempt-5 rows Q5 migrated into this format. The baseline itself still
  holds the M1 result at `b32182a`.
- `adjoint_m1_adjoint.json` + `m2.md`: value + adjoint on the M1 book (`adjoint_m1_adjoint_bench` with
  `--benchmark_filter='^BM_AdjointEval/|^BM_AdjointEvalBatch/tile:(128|256)/lane_tile:8$|^BM_AdjointBuild$'`: B=1
  at the D15 tiles, B=64 at lane tile 8, the constructor), informational: 7.38x the interpreter's value-only B=1
  median. The committed file is the M2/m2-gate-2 re-run at `359f321`, written under the derived run name by a plain
  `bench/run.sh build/release/bench/adjoint_m1_adjoint_bench` and gated by `scripts/perf_gate.py` at 1.25x against
  the six rows `baseline.json` holds under `adjoint_m1_adjoint` (the M2 gate round's medians at `8914934`): PASS,
  0.961-1.018x (an unfiltered sweep's other rows are reported as new, not gated). The M2 gate round (M2/m2-gate)
  measured and seeded those rows as run `m2` (`m2.json`, perf commit a72f525); M2/m2-fix re-keyed them to
  `adjoint_m1_adjoint` with the medians unchanged, because a baseline under an ad hoc `--name` gated only a run that
  repeated `--name m2` and the default invocation was refused (D34); M2/m2-gate-2 is the first round gated by that
  entry.
- `m2_adjoint.json`, `m2_adjoint.md`, `m2_adjoint_fingerprint.json`: M2/Q3's first adjoint timing (raw Google
  Benchmark dump), load-tainted (13.7 -> 10.2), informational only; superseded by `m2.json` for numbers.
- `m1.json` + `README.md`: the M1/P6 summary and narrative, generated by `m1_summarise.py` (kept as the M1 evidence)
  from `m1_p6_{fingerprint,hand,interp,gates,notes}.json` and `m1_p6_plan.txt`, the raw evidence of attempt 5.
- `fingerprint.json`, `m1_hand.json`, `m1_interp.json`, `m1_*_fingerprint.json`, `m1_interp_plan.txt`,
  `tape_replay.json`: raw Google Benchmark dumps of the earlier P2/P4/P5 rounds at older engine commits,
  informational (D9), not read by the gate.
