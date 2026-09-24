# EpykosEngine — Decision log

Append-only. Superseding a decision adds a new entry that names the one it replaces.

## D1 — No JIT (2026-09-22)
Kernels are pre-compiled engine code driven by data: a build-time catalogue (AOT code generation) plus a tiled
interpreter. Rationale: portability (x86 + ARM), no runtime compiler dependency, deterministic builds.
Revisit only if M1/M3 show the catalogue + interpreter cannot reach the gates.

## D2 — Standalone repository (2026-09-22)
No code or build dependency on SwapEngine. The correctness oracle is the engine's own templated-`double` path;
cross-checks against SwapEngine and QuantLib are added later as test-only dependencies.

## D3 — Maths is written once, templated on `Scalar` (2026-09-22)
The same template is the reference (`double`) and the recording source (`Rec`). No hand-written derivative code and
no hand-written per-product kernels on the hot path.

## D4 — Domains are inferred, not declared (2026-09-22)
The signature pass is authoritative. Scope markers in templates are optional hints that the pass verifies.

## D5 — `select` is the default for value branches (2026-09-22)
Both arms computed; mask, margin and arm gap exported. Guards remain only as a safety net for unconverted branches.
Flips are classified by arm gap, never by the predicate bit alone.

## D6 — Quote-dependent transforms stay outside the pricing graph (2026-09-22)
Bands, log/zero-coupon row maps depend on the live quote: they are `row_map`s at the graph edge, and their active sets
belong to the solver (`pin`).

## D7 — Solvers are `implicit` nodes, never unrolled (2026-09-22)
Tape length must not depend on iteration count; derivatives must not depend on the solver's path.

## D8 — Every rewrite declares an exactness class (2026-09-22)
E0 bit-identical, E1 ≤ 1 ulp per op. Gates apply the matching tolerance. Bit-identity gates build the reference
with `-ffp-contract=off`. No `-ffast-math`.

## D9 — Performance is gated against ourselves (2026-09-22)
Per machine+toolchain fingerprint: fail on > 1.25× self-regression or on an absolute target miss. External references
(QuantLib, hand-fused kernels, other engines) are informational tables, never the gate. No cross-fingerprint
comparisons.

## D10 — Verification precedes the compiler (2026-09-22)
The round-trip identity check and the differential tester land with the first pass that needs them, not after.

## D11 — No code shared with SwapEngine (2026-09-22)
Supersedes the second sentence of D2. EpykosEngine is a fresh take on the problem; SwapEngine continues separately.
No source, header, fixture, build script or generated file moves between the two repositories in either direction,
whether by copying, porting, vendoring, symlinking or `#include`. Agents working in this repository do not open the
SwapEngine checkout. SwapEngine is withdrawn from the future oracle list; QuantLib remains a candidate test-only
oracle. The measured numbers already transcribed into `PRIOR_ART.md` stay as informational tables (D9).

## D12 — Toolchain (2026-09-22)
C++20. CMake (≥ 3.25) + Ninja, installed as build tools via Homebrew. Libraries are vendored under `third_party/`
(gitignored) by `scripts/bootstrap.sh` at pinned versions with checksums: Eigen (header-only; used on the `double`
side only), GoogleTest, Google Benchmark. No Homebrew-installed libraries. Compilers: Apple clang 21 on macOS, GCC 13+
on Linux. GitHub Actions builds and runs the tests on both; performance gates run only locally and are fingerprinted.

## D13 — Flags and ISA (2026-09-22)
Gated build: `-O3 -march=x86-64-v3 -fno-math-errno`, no `-ffast-math`. Reference TUs (the templated-`double`
oracle, the hand-fused reference, the round-trip expander) add `-ffp-contract=off`. Fingerprint = CPU brand, core
count, compiler version, flags. `-march=native` builds may be reported as informational rows under their own
fingerprint (D9), never as the gate.

## D14 — Recorder representation (2026-09-22)
`Rec = {double v; node_id id}` with `node_id` a typedef (32-bit to start). Taint ("depends on an input") lives in the
tape's node table, not in `Rec`. `RecBool = {bool v; node_id id}` comes from comparisons and does not convert to
`bool`. The recorder has no Eigen dependency. An `Eigen::NumTraits<Rec>` specialisation is provided as an opt-in
header: matrix expressions over `Scalar` record elementwise and linear ones collapse to `linmap` in the affine pass;
products are never intercepted. One opcode enum is shared by every pass (record, CSE/DCE/fold-sum, affine collapse,
signature, expander, interpreter, adjoint, catalogue). `SUM` is variadic from the first commit. The domain IR is
plain data and serialisable.

## D15 — Batch-axis layout (2026-09-22)
SoA with the batch axis innermost on every domain. Batch width `B` is a runtime value. Tile size is a runtime
parameter; M1 sweeps 128/256/512 and records the winner with the fingerprint. Compile-time `B` specialisation is a
catalogue (M3) decision, not an interpreter one.

## D16 — The M1 book is fixed (2026-09-22)
The kill-test book is specified exactly in `WORKLOADS.md` §M1 and generated from a fixed seed; there are no data
files. Linear-in-zero-rate curve on 12 knots; 1,000 OIS par swaps; the float leg is written compounded in the
templated maths and telescoped by the fusion pass (no `scan` in M1); one ungated bucket of seasoned swaps with a
realised first fixing. The hand-fused reference and the generic path price exactly this book.

## D17 — Parallelism and random numbers (2026-09-22)
A `std::thread` pool owned by the engine; no OpenMP or TBB. Counter-based RNG (Philox-4x32-10): the uniform for
(path, step, dimension) is a pure function of (seed, path, step, dimension); Gaussians come from the inverse CDF
(Wichura AS241). Every path is therefore bit-identical regardless of tile size or thread count. Reductions across
paths use a fixed pairwise order so aggregates are thread-count independent too.

## D18 — Milestones are gated and land by PR (2026-09-22)
M(n+1) starts only when M(n)'s exit gate passes. M1's kill path follows `ROADMAP.md`: up to three tile/layout
iterations; if still > 2×, M3 moves ahead of M2. Work happens on package branches merged into the integration branch
`integrate/m1-m5` when each milestone's gates pass; one PR from the integration branch to `main`, merged only by the
owner. Docs-only commits that record decisions may go to `main` directly.

## D19 — M5 models (2026-09-22)
Hull–White one-factor and LGM, sharing one affine kernel: `log P(t,T) = A(t,T) − B(t,T)·x_t` as a batched `linmap`,
with the time step an affine `scan`. Hull–White is the gated model; LGM must reproduce Hull–White prices under the
equivalent parametrisation to 1e-12 relative.

## D20 — Licence (2026-09-22)
No licence file yet. The repository is public; with no licence it is all rights reserved by default. Revisit before
accepting any external contribution.

## D21 — SwapEngine as a black-box benchmark, for the G4 comparison only (2026-09-22)
Refines D11 for one purpose. The stretch-goal G4 comparison (`ROADMAP.md` §MX) may build and run SwapEngine from its
own checkout as a black box, and may read its public interface and documentation only as far as needed to feed it the
same bundle. Nothing derived from SwapEngine source enters this repository; no agent developing engine code opens the
checkout; the comparison lives under `bench/compare/` with a README stating exactly what was and was not like-for-like.
Its numbers are informational (D9).

## D22 — Signature-pass rules as implemented on M1 (2026-09-23)
Refines DESIGN.md §5 steps 3–6 (M1/P3, `include/epykos/ir/`). Five rules, the first two adjustments to the text:
1. **References carry no class.** A boundary's signature is its local op tree with "constant slot" and "reference"
   tokens; a reference does not record which domain it reads, and a gather may read several domains (value ids are
   global: domain base + row). Upstream buckets — the three knot-time DFs vs the interpolated ones today, `select`
   and R2 buckets later — therefore never fragment downstream domains.
2. **Sharing is decided per computation, not per node.** Besides Inputs, Sums, Affines, fan-out > 1 and
   member/output uses, every node whose deep expression (the op tree seen through shared nodes, modulo constants,
   stopping at Inputs / Sum / Affine / Const) equals that of a boundary is materialised too. Otherwise a shared
   forward and an unshared one give the float coupon two signatures (forward inlined vs referenced).
3. **Sum and Affine are order- and arity-insensitive**: operands are segment members (CSR, fold order kept), Affine
   with one coefficient per member and c_0 as a constant slot. A Const that is a member or an output is a row of the
   const domain.
4. **Recurrence.** A class whose rows read rows of the same class directly is flagged `recurrent` (a scan candidate;
   unsupported until M5, M1 asserts none). Classes that reach themselves only through other classes
   (legs → swaps → book) are split by dependency level inside their strongly connected component, so every domain is
   evaluated in one pass.
5. **Round-trip identity** compares canonical forms: Inputs by ordinal, then a post-order walk from the outputs;
   commutative operand pairs compared unordered (IEEE add / mul commute bitwise), everything else exactly.
Observed on the M1 book (75,698 nodes → 10 domains, 42,314 values): Inputs(12) → affine(2,561) → DF
`exp(mul(neg(@),$))`(2,564; three rows read a knot directly: t = 0, t = 1 on knot 4, t = 10958/365 beyond the last
knot) → constant-rate coupons `mul($,@)`(16,103 = 15,903 fixed + 200 seasoned-first) and forwards(2,432) → float
coupons(15,703) → legs `sum`(1,938)@L0 → swaps `mul($,sub(@,@))` 969@L1 plus the 31 T = 1 swaps @L0 (no leg Sum) →
book `sum`(1)@L2. The seasoned-first coupon N·τ·R·DF(e) and the fixed coupon N·τ·K·DF(e) are the same op tree with
one constant slot and share a class: hash-consing modulo constants cannot separate them (the WORKLOADS.md
expectation holds against the float class, not the fixed class). Not changed: P2's affine pass; a single-term
absorption of `Neg(Input)` would fold the three knot-time DFs into the affine domain and is left to M3 (R1/R3).

## D23 — Recurrence flags after level splitting (2026-09-23)
Refines D22 rule 4 (M1/P7 review). `Domain::recurrent` means what its name says: the rows of this domain read
(earlier) rows of this same domain, which is what `ir::validate` permits and what `exec::Interpreter` refuses.
`infer()` never sets it: a class whose rows read rows of the same class directly is split by dependency level like
any class in a cycle, so every domain it produces reads only earlier domains and is evaluable in one pass. The
class-level fact is carried separately as `Domain::scan_class`, set on every level-domain of such a class: that is
the M5 scan candidate, and it is what M1 asserts absent on the M1 book (`scan_class_domains(program).empty()`).
Before this entry the class flag was copied into `recurrent`, so the interpreter refused ordinary acyclic programs
such as exp(exp(x)) with the inner exp registered as an output (both level-domains of the exp class were marked
recurrent although neither reads itself) while `ir::Evaluator` evaluated them correctly. The IR text format becomes
`epykos-ir 2` (one more domain field); nothing persisted uses the old one.

## D24 — A recorded value carries its tape's serial (2026-09-23)
Refines D14 (M1/P7 review). `Rec = {double v; node_id id; tape_serial tape}` and `RecBool` likewise (16 bytes; the
32-bit serial sits next to the 32-bit id). Every `Tape` has a process-unique serial: a copy is a new table with a new
serial, a move keeps it, `clear()` and the passes (which swap in a rebuilt table) renew it. Using a recorded value on
a tape other than the one it was recorded on — under a nested `Tape::Scope`, or after a pass or a clear — throws
`RecordError` from `node_on` / `value()` / `structural_if`, and `Tape::node` / `tainted` throw `RecordError` (not
`std::out_of_range`) for an id the tape does not hold. Before this entry a value from tape B used under tape A's
scope was silently recorded as A's node with the same id (wrong graph, wrong taint, no exception); M2+ nests
recordings (implicit nodes), which is where that would have bitten. Taint still lives in the node table.

## D25 — Reference TUs are pinned by build convention, on both compilers (2026-09-23)
Refines D13 (M1/P7 review). The reference TUs of D13 — the fixture generators (`src/maths/m1/book_e0.cpp`,
`src/rng/philox_e0.cpp`), the hand-fused reference (`src/hand/m1_hand_kernel_e0.cpp`) and the interpreter kernels
(`src/exec/kernels_l*_e0.cpp`) — are named `src/**/*_e0.cpp`, and the root CMake gives every such source
`-ffp-contract=off` and `EPYKOS_FP_CONTRACT_OFF=1` in every preset (the TU `#error`s without the define), the same
convention as `tests/**/*_e0_test.cpp`. Until this entry they were pinned with `#pragma clang fp contract(off)`
under `#if defined(__clang__)` and a comment claiming GCC in ISO C++ mode does not contract. That claim is false:
GCC's ISO-mode `-ffp-contract=off` default applies to C only, and `g++ -std=c++20 -O3 -march=x86-64-v3` contracts
across statements. Consequences before the fix: GitHub Actions was red on every push to `integrate/m1-m5` from P2
(2026-09-22T22:13Z) to P6 attempt 4 — under GCC 13 release the E0 gates `exec_m1_interp_e0_test`,
`hand_m1_hand_e0_test` and `exec_interp_test` failed at rounding level (the kernels, the hand kernel and the book
generator contracted), and `tape_replay_e0_test` failed under both GCC presets for an unrelated reason (argument
evaluation order); nothing in this repository's "23/23 under both presets" lines was measured on GCC. Also:
`tests/exec/interp_test.cpp` became `interp_e0_test.cpp` (its replay and evaluator are header templates that must
not contract either), and `tests/maths/m1_fixture_e0_test.cpp` recomputes the book generator's arithmetic in a
contraction-free TU and checks it bitwise, with literal digests for the libm-independent parts of the fixture, so a
fixture that diverges across compilers is caught rather than reported as "bit-identical across presets" from one
machine. libm-dependent bits (exp/log: notionals, par rates, fixed rates, the oracle) are printed as digests, never
asserted against a literal. CI status is now part of the M1 evidence (P6 README, RESUME progress lines).

## D26 — E1 tolerance for a difference of legs (2026-09-23)
Refines D8 (M1/P7 review; the definition M1/P5 implemented in `tests/hand/m1_hand_test.cpp` without recording it).
A swap PV is `side·(fixed − float)`, a difference of two legs that at the record point cancel to `|ε_i|·par·annuity`
and at the batch states to as little as 1e-6 of the leg size; rounding-level (E1, ~1e-15) agreement of the legs is
therefore up to 1e-9 *relative to such a PV* for any kernel that does not reproduce the oracle's operations bit for
bit. The E1 tolerance for a value that is a difference of terms is relative to the scale of the terms:
`|Δpv_i| ≤ 1e-12·(|fixed_i| + |float_i|)` per swap and `|Δbook| ≤ 1e-12·Σ_i |pv_i|` for the book; the literal
`|Δ| ≤ 1e-12·|value|` is asserted in addition wherever `|value| ≥ 1e-2 × scale`. P5 measured on d448afd70180
(reference preset, default variant fused/shared-recip/exp_poly): worst `|Δpv|/(|fixed|+|float|)` 2.75e-15, worst
`|Δbook|/Σ|pv|` 1.35e-15, worst literal `|Δpv|/|pv|` 2.4e-13 on the 39,729 well-conditioned swap-states (581 of the
25,271 ill-conditioned ones exceed the literal bound), worst literal `|Δbook|/|book|` 1.02e-13 on the 58
well-conditioned states and 1.46e-12 over all 65 (7 states have `|book| < 1e-2·Σ|pv|`); std::exp variant 1.54e-13 /
6.15e-13, per-row-division variant 2.4e-13 / 8.1e-13. Read literally, "E1 vs P1 double (≤ 1e-12)" is therefore
violated at those states; the RESUME M1 table now states the scaled gate. If the owner wants the literal bound, P5
is failed at those states and the hand kernel's fused arithmetic would need the oracle's operation order there.

## D27 — The M1 gate pairing and verdict (2026-09-23)
Refines D9 and closes the question the M1/P7 review left open (`bench/results/d448afd70180/README.md`, "Read first").
`ROADMAP.md` §M1 names no hand-kernel variant or exp implementation. The M1 go criterion is read like for like: the
interpreter in its gated E0 mode (scalar libm `std::exp`) against the hand-fused kernel using the same scalar libm
`std::exp` (variant 2: fused, shared reciprocal, `std::exp`), so that the ratio measures the generic program's
arithmetic and data movement and not the choice of exp implementation, which both sides can change identically (a
cross-lane exp is an M3 catalogue decision, E1). Under this pairing M1 passes on d448afd70180: 1.044 single-state
(≤ 1.3) and 1.079 batched (≤ 1.1), measured at b32182a (P6 attempt 5, results 097d54b). The other pairings stay as
informational rows in every M1 result (D9): E1 vs E1 (interpreter exp_poly vs the hand default variant 0) 1.077 /
1.240, the batched row outside 1.1 — the gap is the E0 arithmetic the hand kernel replaces in E1 (two IEEE divisions
per forward element vs one shared reciprocal, three-rounding coupons vs gather → fma → segment_sum) and is M3's R4b and
fma-contraction work, each with its E1 differential test; gated E0 vs the hand default 1.219 / 2.235; vs the hand
reference-arithmetic variant 3 (the interpreter's own operation order) 0.859 / 0.705. Verdict: **go**, recorded by the
orchestrator at M1 close; M2 starts (D18). The owner may supersede this entry; if the E1-vs-E1 pairing is preferred,
M1's batched criterion is not met and the kill path's remaining option is M3 before M2.

## D28 — Layout by library structure: the M1 fixture code consolidated (2026-09-23)
Refines the paths named in D13 and D25 and records the M2/Q0 consolidation of `RESUME.md` §1's layout rule: the
tree is organised by library structure, never by milestone. Engine code lives under `include/epykos/<component>/`
and `src/<component>/`; seeded fixture books, their oracle pricers and record helpers under
`include/epykos/fixtures/` and `src/fixtures/` (namespace `epykos::fixtures`; compiled into libepykos like any
source, but test-only and not engine API: no engine header includes a fixture header); the hand-fused reference
kernel under `bench/hand/` as the static library `epykos_hand` (namespace `epykos::hand`, included as
`"hand/<name>.hpp"`), outside libepykos, linked by `tests/hand/` and `bench/hand/`. Moves, no behaviour change:
`maths/m1/book.hpp` → `fixtures/m1_book.hpp`, `maths/m1/reference.hpp` → `fixtures/m1_reference.hpp`,
`tape/record_m1.hpp` → `fixtures/record_m1.hpp`, `src/maths/m1/book_e0.cpp` → `src/fixtures/m1_book_e0.cpp` (the
`_e0` suffix stays: it is what pins the TU, D25); `maths/m1/curve.hpp` → `maths/curve/linear.hpp`
(`epykos::curve::linear`, the linear-in-zero-rate scheme, function bodies unchanged); `maths/m1/price.hpp` split —
the three coupon formulas, which are engine maths, → `maths/swap/ois.hpp` (`epykos::ois`), and the leg / swap / book
folds, which iterate the fixture `Book`'s row table, → `fixtures/m1_price.hpp` (a schedule-driven leg API that does
not take the fixture is M4's swap/ work, not a consolidation); `hand/exp_poly.hpp` → `maths/exp_poly.hpp`
(`epykos::maths`): it stays in the engine because `exec::Interpreter`'s `ExpMode::poly` (src/exec/kernels_impl.hpp)
uses it, and its test moves with it to `tests/maths/`; `hand/m1_hand_kernel.hpp` and `src/hand/m1_hand_kernel_e0.cpp`
→ `bench/hand/`. The `*_e0.cpp` pin of D25 applies under `bench/hand/` as well (bench/hand/CMakeLists.txt), so the
hand kernel's E0 mode is still bitwise the contraction-free oracle on both compilers. Evidence: 24/24 ctest under
the release and the reference preset before and after (the same executables; `hand_exp_poly_test` is now
`maths_exp_poly_test`), every E0 gate bitwise, the benchmarks build and run, `bench/results/` untouched.

## D29 — Benchmark results format, baselines and the performance gate; Python for scripts (2026-09-23)
Implements DESIGN.md §11's performance gate (M2/Q5) and turns D9/D13's "per fingerprint, never across" into tooling.
(1) **Results.** Every benchmark run is written by `bench/run.sh <binary> [args]` as
`bench/results/<fingerprint-id>/<name>.json` (format `epykos-bench 1`, defined in `scripts/bench_results.py`): the
fingerprint, the 1-minute load before and after, git commit and branch, preset, the `EPYKOS_*` environment, each
benchmark's parameters (tile, lane_tile, exp, B, ...) and min / median / p90 / max / mean over the Google Benchmark
repetitions with the raw repetition times — the M1/P6 definitions (p90 nearest rank; statistics of repetition means,
`WORKLOADS.md` "Terms"). The raw Google Benchmark output stays in `tmp/` (gitignored). `run.sh` refuses to run (exit
2) when the 1-minute load exceeds cores/2 (logical). Its default protocol is the M1 one (20 repetitions x 0.2 s); the
>= 200 timed evaluations WORKLOADS asks for remain owed and are one `--repetitions` away.
(2) **Baseline.** `bench/results/<id>/baseline.json` (format `epykos-baseline 1`) holds the accepted median per
benchmark for that fingerprint, changed only by `scripts/perf_gate.py --accept` in a perf commit whose message states
before/after. Seeded for d448afd70180 from the M1 result (D27: P6 attempt 5, engine b32182a): the raw files P6
committed are converted into the run format (`hand_m1_hand.json`, `exec_m1_interp.json`, marked `migrated_from`,
statistics identical to `m1.json`); the M1 evidence files stay as they are. The hand kernel's rows are in the
baseline too, so a change in the gate's denominator is caught as well.
(3) **Gate.** `scripts/perf_gate.py RUN...` compares fresh runs with the baseline of the same fingerprint only: a
median above 1.25x its baseline fails (exit 1), below 0.8x is reported as faster, new and unmeasured benchmarks are
reported; the absolute targets of `bench/targets.json` (`ROADMAP.md`: M1 <= 1.3x / 1.1x the hand kernel under the
D27 pairing, M3 <= 1.05x, M5 < 1 s; the M3/M5 run and benchmark names are provisional until those benches land)
are checked whenever every side is measured on the fingerprint — a run given on the command line, else the
committed `<run>.json` of the directory, which must pass the load rule too — and a miss fails (exit 1). The gate
refuses (exit 2) a run whose load before or after the measurement exceeded cores/2, runs or a baseline of another
fingerprint, a run without a baseline entry, and malformed input. Reference kernels remain informational rows (D9):
the hand kernel enters the gate only as the denominator of the ROADMAP ratios.
(4) **Python.** Tooling under `scripts/` may be Python 3 standard library (3.8+; this is the decision entry the
CLAUDE.md rule requires): `scripts/bench_results.py`, `scripts/perf_gate.py`; `bench/run.sh` stays bash and calls
them. No third-party Python, no Python in the engine, tests or benches. `bench/results/m1_summarise.py` (M1/P6)
predates this entry and stays as the generator of the M1 evidence tables.
Tests: `tests/scripts/perf_gate_test.cpp` (ctest `scripts_perf_gate_test`) drives the scripts on synthetic JSON —
regression detected, cross-fingerprint refused, load refused, `--accept` seeding and updating, ratio and time targets
including a committed denominator, the summariser's statistics and name parsing, and `run.sh` end to end on the
scaffold benchmark — and skips only when no `python3` is on the PATH.

## D30 — The differential tester's E1 class is D26's bound; 4 ulps is the harness default, not the M1 gate (2026-09-23)
Refines D8 and D26 (M2/Q1, `include/epykos/verify/differential.hpp`, `src/verify/`). The tester compares a
compiled evaluation (`exec::Interpreter`, the tape replay, any `BatchFn`) with the templated maths instantiated on
double at the draws of the `WORKLOADS.md` §M2 ball, z + 0.005·u with u uniform on [−1, 1]^12 from Philox sub-stream
200000 + r (draw index = knot), 256 draws; the draws are computed in an E0 TU (`src/verify/state_ball_e0.cpp`, D25),
so a draw is the same bits on every preset and compiler and a report's worst state is reproducible. Two classes:
E0 is bitwise; E1 is |a − b| ≤ max(ulps·ulp(m), rel·m) with m = max(|a|, |b|, scale), where `ulps` (default 4) counts
spacings of doubles at m, `rel` (default 0) is a relative bound, and `scale` is an optional per-output, per-state
scale supplied by the caller (D26's |fixed_i| + |float_i| per swap and Σ|pv_i| for the book; +inf exempts a value).
Measured on the M1 book under the release preset on Apple clang 21 (`-O3 -march=x86-64-v3 -fno-math-errno`; the
reference `price_book<double>` instantiated in a TU with those flags, the interpreter's kernels pinned E0 by D25):
101,304 of 256,256 values differ; against the leg scale the worst swap is 1.55e-15 (13 ulps; swap 297, draw 243)
and the book 7.8e-16 of Σ|pv| (6.6 ulps); against the values themselves 5.4e-10 (4.2e6 ulps; swap 182, draw 201, a
PV of 0.87 on a notional of millions). 4 ulps of the leg scale is therefore exceeded at 879 values and 4 ulps of the
value at 101,199. The mechanism is the maths as written, not a defect: the compiler contracts the interpolation
(1 − w)·z_k + w·z_{k+1} (≤ 1 ulp of z(t), ≈ 1 ulp of DF(t) at t ≈ 30 y) and the float coupon's
fwd = (DF(s)/DF(e) − 1)/τ amplifies a 1-ulp DF difference by 1/(z·τ) ≈ 25 before the leg sums up to 30 of them (GCC
contracts the leg folds acc + N·τ·K·DF as well, D25). Decision: the M1 E1 differential
(`tests/verify/m1_differential_test.cpp`) asserts D26 as written — 1e-12 of the leg scale for every value, and
1e-12 of the value itself wherever |value| ≥ 1e-2 × scale (65,507 of the 256,256 swap-states are ill-conditioned
and exempt from the literal) — at B = 1 and B = 64, and prints the 4-ulp counts scaled and unscaled for the record;
under the reference preset it is bitwise and asserts so. A bound of a few ulps of the value is the class for a
kernel that reproduces the reference's operations, which the E0 gate asserts bitwise under every preset
(`tests/verify/m1_differential_e0_test.cpp`, B = 1 and B = 64, plus the tape replay); M3's E1 rewrites use the
tolerance each rewrite declares (D8), with `ulps` and `rel` both available and the scale chosen per output.

## D31 — Adjoint: row values kept, intermediates recomputed, pulls in a fixed order (2026-09-23)
Implements DESIGN.md §7 "Adjoints" (M2/Q3, `include/epykos/adjoint/`, `src/adjoint/`). Three choices:
1. **What the reverse pass keeps.** The forward pass stores every *row value* (the last step of every group, all
   domains, batch innermost in one value buffer — the interpreter's layout, but with nothing fused away) and nothing
   else. The reverse of a group recomputes its intermediate steps 0 .. last−1 per tile from the stored operands
   (gathers of earlier domains, columns, literals) and reads the group's own value from the buffer, so a group whose
   value is an `exp` / `log` / `sqrt` (every M1 exp: `exp(mul(neg(@),$))`) never recomputes the libm call; only the
   arithmetic before it is recomputed, with the forward's bits. Storing every step instead would cost
   `max_steps × rows × L` doubles (4–6× the value buffer on M1) written in the forward and read back cold in the
   reverse; recomputing costs one extra pass of cheap arithmetic over operands that the reverse loads anyway for the
   local rules. Not a checkpointing scheme: at MC scale (M6) this changes.
2. **No scatter.** Every (gather, row) and every (segment, row) has an *edge slot*; the reverse of the reading group
   accumulates the operand's adjoint into the slot, and a value's adjoint is later pulled by its own domain as the
   sum over the "who reads me" CSR built once per Program: output seeds (ordinal order), gather slots (gathers in
   Program order, rows ascending), Sum member slots (segments in Program order, rows, members), Affine member slots
   times the member's coefficient — left to right from +0.0. An Affine's reader list is its coefficient table
   transposed: `Wᵀ`, the calibration Jacobian shape, derived. Within a group: steps from the last to the first,
   operands in the order a, b, c, each `+=` into the target's running sum (an earlier step's adjoint or an edge slot).
   The local rules, as evaluated: add `ā += ȳ; b̄ += ȳ`; sub `ā += ȳ; b̄ −= ȳ`; mul `ā += ȳ·b; b̄ += ȳ·a`;
   div `t = ȳ/b; ā += t; b̄ −= t·y`; neg `ā −= ȳ`; exp `ā += ȳ·y`; log `ā += ȳ/a`; sqrt `ā += (0.5·ȳ)/y`;
   recip `ā −= (ȳ·y)·y`; fma `ā += ȳ·b; b̄ += ȳ·a; c̄ += ȳ`; select `b̄ += a ? ȳ : 0; c̄ += a ? 0 : ȳ` (the
   predicate and the comparisons receive nothing); Sum / Affine store `ȳ` in the row's segment slot and the members
   pull it (Affine: times the coefficient); Input writes `state_bar[ordinal]`. These orders need not match any other
   implementation (the forward-mode and FD checks are tolerance gates) but are fixed, so the adjoint is deterministic.
3. **Bits are independent of B, tile and lane_tile by construction**, and the kernel TU is `src/adjoint/adjoint_e0.cpp`
   (the D25 pin), so the E0 gates — forward bitwise vs the replay / oracle / interpreter, B = 64 lane for lane the 64
   B = 1 runs (outputs and state adjoints), every tile in {1, 7, 128, 256, 512, 4096} × lane tile in {1, 3, 4, 8, 16,
   32, 64}, odd B — hold under the release preset as well as the reference one. Every loop is per (row, lane): no
   cross-lane or cross-row reduction, and the pull of a row does not see tile boundaries.
Measured on the M1 book (tests/adjoint/): d(book)/dz vs central FD (h = 1e-6) of `price_book<double>` at the record
point and 8 M2-ball states within 2.9e-9 relative (worst component; gate 1e-6 with floor 1e-9); the 50 swaps of
sub-stream 300000 at 3 states within 1.7e-8 (one lane per swap, out_bar = e_i); linearity 1.3e-14 (gate 1e-13);
zero allocations in run(). Adjoint-vs-Dual (1e-12) lands with Q2. Buffers at lane tile 8: values + adjoints 5.4 MB,
edge slots 3.9 MB (56,937 gather + 4,500 segment slots), tables 1.1 MB.
## D32 — Mutation testing: mutants live in the passes behind a build option; a survivor is a gap in the gates (2026-09-23)
Implements DESIGN.md §11 "mutation testing" for M2/Q4a (CLAUDE.md: every rewrite ships with a mutation test) and
adds a preset to D13's list. A mutant is a deliberate one-line defect written inside the real pass it belongs to,
guarded by `epykos::mutant("<pass>.<defect>")` and listed in the registry of `include/epykos/mutation/mutation.hpp`.
The CMake option `EPYKOS_MUTATIONS` (default OFF) compiles them in; the `mutation` configure preset is the
reference preset plus that option, so the E0 gates run under the reference flags with the mutants present. It is
never a gated or measured build. With the option ON, the environment variable `EPYKOS_MUTANT=<name>` selects exactly
one mutant per process, read once on the first query; an unregistered name throws on that query (a typo is loud, not
a run with no mutant). With the option OFF, `epykos::mutant()` is a constexpr false: the defects compile away, the
environment is never read, and `tests/mutation/registry_test.cpp` asserts this statically. The registry test also
pins the registry to the list in `docs/WORKLOADS.md` §M2 (adding a mutant means editing both), prints it for the
harness, and checks that every registered name has exactly one use site under `src/` and every use site names a
registered mutant. `scripts/mutation_test.sh` builds the preset once and runs the GATE tests once per mutant — the
ctest entries matching `roundtrip|differential|verify|_e0_test$`, never a test under `tests/mutation/` and never a
pass's own unit tests — after checking they all pass with no mutant selected; it prints mutant → caught-by and exits
non-zero if any mutant survives. A mutant caught only by a test written for it has not been caught: a survivor is a
gap in the gates and is closed by a new generic gate, not by a mutant-specific test. CI runs the script on Linux GCC
13 (`.github/workflows/ci.yml`, job `mutation`). The first run (Apple clang 21, reference flags) caught 7 of the 8
registered mutants with the M1 gates alone and left `signature.merge_classes` alive: no gate fixture held two
boundary shapes differing only in where a constant sits, because every class of the M1 book is far from every other
(D22's observed chain), so a signature pass that merges `sub(c, x)` with `sub(x, c)` or `select(p, c, y)` with
`select(p, y, c)` passed every gate; and `affine.drop_offset` was caught by one test only (the M1 book has no affine
chain with a leading constant). The gap is closed by the near-miss shapes fixture (`fixtures/nearmiss_shapes.hpp`,
WORKLOADS.md §M2) and its gates `ir_nearmiss_roundtrip_e0_test` and `exec_nearmiss_interp_e0_test`: 42 templated
shapes that differ from one another in exactly one respect a signature may overlook, five seeded instances each,
round-trip identity raw and after the passes, E0 replay of the expanded tape, the IR evaluator and the tape after
every pass against the double instantiation on a 64-draw ball, and the interpreter at B = 1 and B = 64 across tiles
and lane tiles. Adjoint mutants (wrong transpose, dropped pull) are M2/Q4b's, after Q3 lands; rewrite mutants
(R1–R7) land in M3 with the rewrites (WORKLOADS.md §M2). The harness is bash only (macOS bash 3.2 and Linux); no
Python.

## D33 — Adjoint mutants and the adjoint gates of the mutation harness (2026-09-23)
Extends D32 with the adjoint's mutants (M2/Q4b) and adds the adjoint gates to the harness's default gate set.
(1) **Five mutants**, one-line defects inside `src/adjoint/` (`plan.cpp` for the transposes, `adjoint_e0.cpp` for the
local rules, each queried once per `build_plan` / `run`), registered in `include/epykos/mutation/mutation.hpp` and
tabulated in `WORKLOADS.md` §M2: `adjoint.wrong_transpose` (gather 0's reader slots come from the forward index
array: value v is pulled from the slot of row `index[r] mod rows`, not of the row `r` that read it),
`adjoint.drop_broadcast` (the last member of every Sum row gets no reader entry), `adjoint.affine_not_transposed`
(an Affine reader's coefficient is read from the forward table at the reader's transposed position — `W`'s entries
in `Wᵀ`'s order), `adjoint.select_wrong_arm` (the adjoint goes to the other arm), `adjoint.recip_rule_sign`
(`ā += (ȳ·y)·y` instead of `−=`). In every build but the mutation preset the checks are constexpr false and fold
away; the E0 kernel's arithmetic is untouched (a mutant path is a separate function, never a sign multiplied in).
(2) **The gates.** The E0 adjoint gate (`adjoint_m1_adjoint_e0_test`: forward bitwise, lanes bitwise, tiles bitwise)
cannot see a defect in the reverse that is consistent per lane, so the harness's default gate regex becomes
`roundtrip|differential|verify|_adjoint_test$|_vs_dual_test$|_e0_test$`: the tests named `*_adjoint_test` and
`*_vs_dual_test` are the adjoint tolerance gates — `adjoint_m1_adjoint_test` (M1 book: `d(book)/dz` and the 50
sampled swaps vs central FD at 1e-6, linearity at 1e-13), `adjoint_m1_adjoint_vs_dual_test` (M2/Q3b: the full
1,001 × 12 Jacobian vs forward mode at 1e-12) and `adjoint_nearmiss_adjoint_test` below. `adjoint_rules_test` (the
op rules on small recorded programs) stays a unit test of the pass and is not a gate.
(3) **The near-miss adjoint gate** (`tests/adjoint/nearmiss_adjoint_test.cpp`) closes the gap D32 predicted for the
adjoint: the M1 book has no `select`, `recip`, `fma`, `log` or `sqrt`, so `select_wrong_arm` and `recip_rule_sign`
are *not exercisable on the M1 book* and would survive its gates. On the near-miss shapes fixture (WORKLOADS §M2)
the adjoint's full Jacobian (one lane per output, `out_bar = e_o`) is compared with one forward pass of
`nearmiss_evaluate<Dual<6>>` at 1e-12·max(|J|, 1) and with central FD (h = 1e-6) at 1e-6 rel / 1e-7 abs wherever the
difference does not straddle a kink (an entry whose FD disagrees with the Dual tangent by more than the FD tolerance
is a kink and is counted, not asserted; none at the 16 states), on the raw recording and after the standard passes
at the record point and 15 ball draws (20,256 entries each; worst 1.9e-15 / 4.1e-15 vs Dual, 0.04 of the FD
tolerance), plus linearity in the seed with the seeded adjoint checked against `Jᵀ·seed`. To instantiate the fixture
on `Dual`, its calls to `recip` / `max` / `min` / `abs` became unqualified (`Dual`'s are hidden friends, found only by
ADL; `double`'s and `Rec`'s are namespace-scope functions of `epykos`, found by ordinary lookup from
`epykos::fixtures`): the `double` and `Rec` instantiations pick the same functions as before and the existing
near-miss gates are unchanged bitwise.
(4) **Result** (Apple clang 21, reference flags, `scripts/mutation_test.sh`): 13 registered mutants, all caught;
the adjoint mutants by the adjoint gates only — `wrong_transpose`, `drop_broadcast` and `affine_not_transposed` by
the M1 gates (FD and linearity; vs forward mode) and by the near-miss gate (after the passes; on the raw near-miss
recording gather 0 is the identity index, where `wrong_transpose` is a no-op), `select_wrong_arm` and
`recip_rule_sign` by the near-miss gate alone. The harness table is in `RESUME.md` §5 (M2/Q4b).

## D34 — A baseline is keyed by the run name bench/run.sh derives from the binary (2026-09-23)
Refines D29 (M2/m2-fix, from the M2 review of the gates). `baseline.json` entries are keyed by the run name, and
`bench/run.sh` names a run after its binary (`<target>_bench` → `<target>`) unless `--name` overrides it, so an entry
seeded under an ad hoc name gates only a run that repeats that name: the default invocation is refused (exit 2, no
baseline), not gated. That is what the M2 gate round had done: the adjoint rows were seeded as run `m2`
(`bench/results/d448afd70180/m2.json`, perf commit a72f525) while the binary is `adjoint_m1_adjoint_bench`, so
`bench/run.sh build/release/bench/adjoint_m1_adjoint_bench` followed by the gate was refused and the documented "the
next adjoint measurement is gated at 1.25x" held only for a measurer who remembered `--name m2` (a milestone name as a
results key besides, which the layout rule of D28 — layout by library structure — tolerates in file names only). Rule: every run in a `baseline.json`
is keyed by the derived name of its binary, its committed `<run>.json` carries that name and is gated against the
entry (a pass or a fail, never a refusal); `--name` is for exploratory runs that are not gated. `--accept` records the
run's Google Benchmark arguments in the entry, so a baseline seeded from a filtered run states its filter and the rows
outside it are expected as "new" in a full sweep (reported, never gated, D29). The refusal for a run without a
baseline names any entry that holds the same binary under another name. Applied: `m2.json` → `adjoint_m1_adjoint.json`
(the same measurement; `name` re-keyed, its note says so), the baseline entry re-seeded under `adjoint_m1_adjoint`
with identical medians (perf commit, before → after in the message); `tests/scripts/perf_gate_test.cpp` pins the
rule on the committed results (every baseline run keyed by its derived name, its file present under that name and
gated) and on synthetic JSON (the refusal names the ad hoc entry; `--accept` records the arguments).

## D35 — The desk problem is the fixture; milestones re-sequenced (2026-09-23)
Supersedes the M3–M5 milestone definitions of `ROADMAP.md` as first written (rewrites on the M1 book, curves as a
separate fixture, MC as a separate fixture). Optimising stages in isolation bakes in boundaries that hide the
cross-stage reuse the engine exists to exploit. From M3 onward the fixture is the desk problem of `docs/PROBLEM.md`:
outputs O1–O6 produced by one recording per stage, with real instruments, real conventions and every interpolation
type. Order: M3 groundwork and the Stage A tape (USD + EUR), M4 optimise the totality (rewrites, cost model,
equality saturation, catalogue), M5 Stages B and C with streaming, M6 Monte Carlo, MX the SwapEngine comparison.
Owner choices: Stage A is USD + EUR; futures without convexity, stated; 1,000 fully recalibrated scenarios; O5
includes CVA delta. M2 is unaffected.

## D36 — Definitions are data; mechanics are code (2026-09-23)
Conventions (calendar rule lists, day counts, roll rules, lags, observation windows, index definitions), curve
definitions (scheme, variable, regions, knot tenors, calibration instrument sets) and instrument blueprints (legs as
lists of coupon rows with a coupon-kind tag and parameters) are **data**, in JSON files under `blueprints/`, read by
a small strict in-house JSON reader (no third-party dependency, D12). The rule kinds (a business-day convention, a
day count, a holiday rule type) and the coupon mechanics (a compounded-in-arrears coupon with shift and lockout, an
averaging coupon, a term-rate coupon) are **code**, templated on `Scalar`, because recording runs C++ templates.
A trade of a known kind is a row; a new coupon kind or a new op is code, once. `blueprints/` are definitions, not
fixtures: quotes, fixings and trade populations stay seeded (D16). A payoff language interpreted at record time into
the op set remains a later item; the record cycle makes it performance-neutral by construction.

## D37 — SwapEngine's harvested market conventions may be used as data (2026-09-23)
Refines D11 and D21 for one more purpose. The market conventions already harvested in SwapEngine —
`conventions/conventions.json` and its schema in the SwapEngine checkout (calendars as rule lists with observance
rules, day counts, indices, products including OIS, IRS, tenor basis, futures, FX forwards and MtM cross-currency
swaps, FX pairs, central-bank schedules, each with its cited sources) — may be **read and imported as data** into
`blueprints/`, with provenance recorded (the SwapEngine file, its `meta.sources`, the date). Still no code: nothing
under `include/`, `src/`, `api/`, `tools/` or `tests/` of SwapEngine is opened. Imported conventions are
cross-checked against the independent web research of `docs/G4_BUNDLE.md`; a disagreement is recorded, not
silently resolved either way.

## D38 — The curve family: schemes as templated maths over structural tables, the variable as a template parameter, composites anchored on the boundary knot (2026-09-23)
Implements `PROBLEM.md` §3 "interpolation" and `WORKLOADS.md` §M4 "Schemes / Interpolation variable / Composite"
(M3/G1, `include/epykos/maths/curve/`). Six choices:
1. **A scheme is structure plus a Scalar loop.** `Flat`, `Linear`, `Hermite` (Bessel tangents), `NaturalCubic`,
   `MonotoneCubic` (Fritsch–Carlson three-point tangents through the Hyman filter) and `BSpline` (clamped, degree
   min(3, n − 1)) are built from the knot times alone — bracket tables, Bessel weights, the Thomas pivots and their
   reciprocals, the B-spline knot vector — and evaluate `prepare(v)` (the Scalar coefficients of one state: tangents,
   second derivatives) once per curve and state, then `value(v, coef, t)` per time. Every scheme is flat outside its
   grid and returns the knot value itself at a knot time (structure), so a composite's boundary is bitwise where its
   two regions agree on the variable. `Linear` is the M1 scheme statement for statement; `maths/curve/linear.hpp`
   is untouched and the M1 E0 gates are unchanged.
2. **Division by a structural constant is written as a product by its reciprocal.** `x / c` is not `(1/c)·x` bitwise,
   so a pass cannot absorb a division exactly; the Thomas sweep and the secants therefore multiply by reciprocal pivots
   and reciprocal spacings computed in double at construction (they are structure: knot times only). With D38's
   single-term scaling, every linear-in-values scheme then collapses to Affine nodes: NaturalCubic's sweep becomes a
   chain of one- and two-term Affines (one level-domain per sweep step in the IR), Hermite's tangents two-term
   Affines of one-term Affines, the interpolated value one four-term Affine (measured on the 10-swap book:
   `curve_record_e0_test`, no Select and nothing but Input / Affine / Neg / Const×value in the log-DF subgraph for
   the five linear schemes; 57–67 Selects for MonotoneCubic).
3. **The variable is a template parameter**, `Variable::{zero, logdf, forward}` on `Curve<S, V>`, never a select:
   zero is `log DF = −z(t)·t` (M1's), logdf interpolates log DF itself with the origin knot (0, 0) in front of the
   grid when the first knot is at t > 0 (DF(0) = 1 is structure), forward is `−∫_0^t f` in closed form for Flat and
   Linear only (a compile-time restriction). Extrapolation is flat in the variable for all three; for logdf that is
   a constant DF (a zero forward) beyond the last knot — a stated simplification: a logdf curve definition places its
   last knot at or beyond the last cash flow.
4. **BSpline's state is its de Boor points.** The clamped B-spline interpolates its end points only and reproduces
   linear functions of the Greville abscissae; the knot vector's interior knots are the de Boor averages of the knot
   times (Piegl & Tiller 9.8). An interpolating variant (values on the grid, a structural collocation inverse) was
   not built: the calibrating state is the control points, whose Jacobian is the basis matrix.
5. **Composite continuity by anchoring** (`composite.hpp`): regions `[t_a, t_b)` partition `[0, ∞)` and the knots;
   at a boundary the knot that sits on it owns the boundary value — if region r + 1's first knot is at t_b, region r
   gets a right anchor there (that knot converted into r's variable by an affine map with a structural coefficient:
   identity when the variables agree, `y = −z·t_b` / `z = y·(−1/t_b)` otherwise) and interpolates to it; otherwise
   region r extrapolates flat to t_b and region r + 1 takes a left anchor at t_b from region r's log DF. Forward
   regions carry no level: they inherit region r − 1's log DF at t_a and are accepted as the last region only
   (a following region would have to inherit its level too, and its knots would lose their meaning) — a stated
   restriction. Region lookup is a comparison on double t at record time (class A, folded). Measured on the
   `WORKLOADS.md` §M4 composite: bitwise at 1Y (zero / zero), within 2e-16 relative at 10Y (zero / logdf, two
   roundings of the conversion), round-trip identity raw and after the passes, E0 through the replay, the evaluator
   and the interpreter at B = 1 and B = 13, select buckets only for the monotone region (every Select row reads
   knots 4–9: the region's five knots and the 10Y knot its anchor is converted from), the adjoint's full Jacobian
   vs `Dual<12>` within 3.9e-15 of the row scale at 24 ball states, the record point skipped as a limiter tie.
6. **The runtime scheme choice is a `std::variant` visited per region** (`SchemeKind`, `RegionSpec`), the shape a
   curve definition of `blueprints/` (D36) maps onto; a single-scheme curve is a one-region composite and is bitwise
   the template (`curve_record_e0_test`).

## D39 — Select exports are outputs; affine_collapse absorbs a scaled atom that is scaled again (2026-09-23)
Two mechanisms of M3/G1 that other packages build on.
1. **Select exports** (`tape/select_export.hpp`). `export_selects(tape)` appends three outputs per Select node —
   the predicate as the mask (1.0 / 0.0), a Sub over the comparison's operands as the signed margin (`y − x` for
   `<`, `<=`; `x − y` for `>`, `>=`, `==`; the mask itself for a constant predicate), a Sub of the arms as the arm
   gap (`a − b`) — and returns the ordinal table. Nothing else is needed: the tape passes keep outputs, the
   signature pass materialises them, `exec::Interpreter::run` writes them at `out[o·B + b]`, and `adjoint::Adjoint`
   seeds them with `out_bar` (zero for a diagnostic: the pricing outputs and adjoints are unchanged, asserted).
   The task's "export flag on the IR / Interpreter / Adjoint" is therefore not a flag: the export is the program's
   output list, which every consumer already honours, and the IR format is untouched (the Scan work of G3 changes
   it concurrently). `classify_flip` applies DESIGN.md §6: a flip is significant only if the arm gap exceeds a
   threshold; the Hyman limiter's flips are ties (measured: a bisected flip at the record point's `|d_4| = |d_5|`
   tie has a gap of 1e-16 and moves no DF by more than 1e-16 relative), a select whose arms differ at the boundary
   is a jump.
2. **Single-term scaling in `affine_collapse`** (E0). A tainted product `Const × atom` that another
   `Mul(Const, ·)` reads becomes the one-term Affine `−0.0 + c·x` (exact for every double, `−0.0 + p = p`), so a
   chain that scales a scaled affine — a Thomas sweep step `(r − a·d'_{i−1})·(1/p_i)`, a Bessel tangent
   `w_a·d_{k−1} + w_b·d_k` of scaled secants, a back-substitution `d'_i − c'_i·M_{i+1}` — reads it as an atom and
   collapses. Every other product is left as it was: one read only by chains is taken by them as a term (the M1
   tape is unchanged: one Affine per interpolated time, the pinned counts of `tape_m1_record_e0_test` and
   `ir_domain_chain_test`), one read by an Exp, a Div, a Mul by a value or an output stays a Mul (D22's
   single-term question for the knot-time DFs remains R1/R3's). A division by a constant is not absorbed (D38.2).
   Mutant `affine.single_term_unscaled` (the coefficient dropped) is registered; no such product exists on the M1
   book, so it is caught by the curve E0 gates alone (`curve_record_e0_test`, `curve_composite_e0_test`), the
   mechanism of D32/D33 for an op the M1 book does not contain.

## D40 — The implicit node is a block over solved inputs and residual outputs; the IFT rule; solve order and factorisation sharing measured (2026-09-23)
Implements DESIGN.md §3.2 `implicit(F)` and D7 for M3/G4 (`include/epykos/solver/`, `src/solver/`; PROBLEM.md §5).
1. **Representation.** An implicit node is recorded, on the one tape, as a *block*: its unknowns are tape Inputs the
   solver layer fills in (solved inputs), its residuals are tape outputs the solver drives to zero (residual outputs),
   and its two O1 diagnostics — `‖Jᵀr‖∞` and the iteration count — are solved inputs registered as outputs. The block
   (`solver::ImplicitBlock`: those ordinals, the start point, the solver options) lives in a `solver::ImplicitRegistry`
   beside the tape; ordinals are stable across every pass, so the registry survives cse / dce / fold_sum /
   affine_collapse untouched, and every landed consumer of a tape — validate, the passes, infer, expand, the
   interpreter, the adjoint — sees the straight-line program over Inputs that the residual sub-program and the book
   are once z is known. The reserved opcode `Op::Implicit` stays reserved: promoting the block into the node table
   and the IR would touch every pass and both engines for no semantic gain while G3 rewrites the same files for
   `Scan`; it can be done later without changing the recording API. `Tape::set_input_value` (the one Tape addition)
   lets the record-time solve write the calibrated values into the record point.
2. **One tape, one residual program.** `solver::implicit()` records the residual in place on the current tape
   (the calibration instruments' discount factors are ordinary nodes of that tape and cse merges them with the
   book's), then lifts the residual sub-program out by the backward slice from the residual outputs
   (`tape/slice.hpp`: Inputs kept as Inputs, no arithmetic, E0 by construction), infers its IR and builds its
   interpreter and adjoint (`solver::ResidualProgram`). The untaped solve — Gauss–Newton with Levenberg–Marquardt
   damping on rejected steps, stop at `‖F‖∞ < 1e-14` or 50 iterations — uses that program for every value and its
   batched adjoint for every Jacobian (one lane per residual, `out_bar = e_i`: rows of J from `state_bar`), never a
   second copy of the maths. The tape's length is independent of the iteration count (three starting points give the
   same node count and the same IR up to the record-point values; measured: 4 / 3 / 3 iterations, 75,881 nodes each).
3. **Backward: the implicit-function rule.** With every lane's Jacobian factorised at its solution (Eigen
   `PartialPivLU` for a square block, the Gauss–Newton normal equations for an over-determined one, double side only,
   hidden behind the headers), `solver::ImplicitProgram::adjoint` runs the whole-program adjoint over every tape input
   and then, per lane and blocks in reverse recording order, `λ = F_z⁻ᵀ z̄`, `p̄ −= F_pᵀ λ`, the pull landing on
   free inputs (quotes) or on earlier blocks' unknowns, which their own rule then propagates (the multi-curve chain).
   The diagnostics' adjoints are dropped (stop-gradient). Forward mode: `Factors::ift_tangent` for a recorded block,
   `solver::implicit_dual<N>` (`solver/tangent.hpp`) for the templated maths on `Dual<N>`, the oracle of the IFT.
4. **Sharing gate.** `ir/sharing.hpp`: per domain, which output groups it feeds (`reach`), who reads it (`readers`),
   the report over the domains ending in a given op (`sharing`, `Op::Exp` = the discount factors) and the
   assertions `assert_all_shared` / `assert_shared(expected)` plus `duplicates(p, op)` (expand the IR, cse it, count
   the merged `op` nodes: a DF computed twice). On the calibrated M1 tape every DF domain feeds both the residuals and
   the book and no DF is computed twice; there are **two** DF domains of disjoint times, the interpolated
   `exp(mul(neg(@),$))` (2,563 rows) and the knot-time `exp(mul(@,$))` (4 rows), because DF(0) and the 7-day
   instrument share `neg(z_0)` and D22's fan-out rule materialises it. D22 already names the fold (absorb a lone
   `Neg` into the affine pass); it is not done here because on the M1 interpreter the reshaped exp group loses its
   fused-pair exp tail (the tail needs a pair) and the ragged affine loses its inlined evaluation (the inliner needs
   uniform member counts) — a cost-model decision for M4's R1/R3. G5/G6 gate on "every DF domain feeds both and no
   DF is duplicated"; the domain count is reported.
5. **Lanes.** A scenario per lane recalibrates: lanes are solved one at a time and never mix, so lane b of a batched
   run is bitwise its B = 1 run (forward, solved curve, diagnostics and adjoint; `tests/solver/m1_implicit_lanes_e0_test`).
   Lanes whose block parameters are bitwise identical share one solve and one factorisation (`dedup_lanes`; the
   risk ladder's one-lane-per-output pattern: 64 lanes, 1 solve). Jacobian policy per block or per program:
   `per_iteration` (Newton / LM, J at every iterate) or `chord` (the record-point factorisation drives every lane's
   steps, Jacobian-free; a lane whose contraction stalls or whose step is rejected refreshes its own J); the
   solution's Jacobian is evaluated once at the end either way (`final_jacobian`, the exact IFT and the `‖Jᵀr‖∞`
   diagnostic). Measured on the M1 fixture at 10 bp quote shocks (`bench/solver/m1_implicit_bench`, d448afd70180,
   load 5.4 → 3.6 of 16, n = 20 × 0.2 s, `bench/results/d448afd70180/solver_m1_implicit.json`; medians, µs):
   forward B = 1 per-iteration 154.4 from the flat start (5 Jacobians, 5 residual evaluations) / 137.6 warm
   (4 / 4), chord 96.1 (1 / 7) / 94.7 (1 / 6); B = 64 per-iteration 6,987 / 6,201, chord 3,594 / 3,515 (64
   Jacobians, 551–593 residual evaluations, no lane refreshed) — the solves cost about 72 µs per lane with the
   Jacobian at every iterate and 30 µs with the shared factorisation (one Jacobian per lane, evaluated at the
   solution for the IFT), on top of the 1,584 µs whole-program run; 64 identical lanes 1,674 (one solve);
   forward + IFT adjoint of the book PV at B = 1 505.8 per-iteration / 461.8 chord (the M2 value + adjoint alone:
   418.5), at B = 8 2,486 / 2,154. `chord` is the default for scenario lanes; `per_iteration` stays the reference.
6. **Solve order.** `solver::CurveSet` discovers each instrument's curve dependencies from the maths (a scratch
   recording sliced back to the curves' inputs), orders the strongly connected components (Tarjan) and records one
   block per component (`Mode::sequential`) or one block for every curve (`Mode::joint`). On the two-curve fixture
   (12-knot discount curve, 6-knot projection curve whose basis swaps read both): sequential 4 + 3 iterations,
   5 + 4 Jacobians (12×12 and 6×6); joint 4 iterations, 5 Jacobians (18×18); the two modes' adjoints agree to
   6e-16 of the row scale. Timed (same run, B = 1, one quote shocked 1 bp): sequential 123.7 µs from the flat
   starts / 62.1 warm, joint 199.0 / 119.4 — a joint Jacobian is one adjoint lane per residual over the union
   program (18 lanes of 286 values) where the sequential blocks cost 12 + 6 lanes of 187 and 99. Sequential is the
   default: fewer flops per Jacobian, and it exposes the chained IFT the xccy curves of Stage B need.
7. **Mutants** (D32, D33): `implicit.ift_not_transposed`, `implicit.ift_drop_fp`, `implicit.stale_jacobian` in
   `src/solver/residual.cpp`, each caught by all three IFT gates `solver_m1_implicit_adjoint_test`,
   `solver_curve_set_adjoint_test` and `solver_m1_implicit_vs_dual_test` (17/17 registered mutants caught against
   29 gates on the tree rebased onto G1, `scripts/mutation_test.sh`); the forward-mode gate at 1e-12 is the one that sees a stale Jacobian
   whatever the start point.
## D41 — Scan domains: a recurrence is a domain, detected as a chain of identical steps (2026-09-23)
Implements DESIGN.md §3.1 `scan` and §5.6 (M3/G3, `include/epykos/ir/`, `src/ir/signature.cpp`, the interpreter and
the adjoint); refines D22 rule 4 and D23. Four choices:
1. **Representation.** A scan is a *domain kind*, not a step op: a recurrent domain (`Domain::recurrent`, rows
   reading earlier rows of the same domain, which `ir::validate` always permitted) whose recurrence
   `Program::scans[domain.scan]` describes — rows chain-major (chain c is rows `[off[c], off[c+1])`), one *carry*
   gather reading `value_base + r − 1` for every row but a chain's first, whose carry reads the chain's initial
   value in an earlier domain (a Const initial value is a row of the const domain, like a Const Sum member). The
   step is the group; the carry is a gather like any other, so the expander, the evaluator, the adjoint's edge
   slots and pulls and the serialisation (`epykos-ir 3`) need no new op. `Op::Scan` stays reserved. A Sum on the
   carry's path (fold_sum's `+` of `a·x + b`) becomes a *fixed-arity Sum step* (`Step{Sum, a, b[, c]}`, no
   segment): the same left fold, emitted back as a variadic Sum node, executed as Add kernels — the only extension
   to the step vocabulary.
2. **Detection, without hints.** Before the partition, a node's expression tree (walked as the extraction walks it:
   through single-use fixed-arity nodes, stopping at boundaries and constants, and additionally through a
   two-or-three-member Sum used once and a single-use member *when they lie on the path to the carry*) is hashed
   with one node cut out and replaced by a carry token; a node whose cut of a node m hashes like m's own cut, at
   the same operand path, extends m's chain or starts one (m's cut is the initial value). Three or more steps make
   a chain (two identical steps are straight-line code: `x·a·b`). A cut directly under an Add or a Sum whose
   target is a single-use value is not a link: an inner running sum is a reduction and fold_sum's, never a scan
   (the raw M1 leg sums). The carry is searched at most four operands deep; a tree beyond 256 entries is not a
   step. The steps become boundaries, the initial value a boundary, the on-path nodes inner steps of the group,
   and a scan step's class carries a scan token, so it never shares a class with a non-scan node of the same shape.
   The sharing rule (D22 rule 2) runs again with the steps as stops of the deep hash, never promoting a step's
   inner nodes. What the author must do: write the loop as the definition states it, one Scalar carried through
   (`acc = acc * (1 + r_i * tau_i)`); realised fixings as plain doubles form a chain of their own feeding the
   projected one; a chain whose steps differ in shape is two chains (`maths/swap/compounding.hpp`).
3. **Layout.** Every Sum of a program is one class, so a scan class normally sits in the level-splitting SCC of
   D22 rule 4 (the compounding steps read the `1 + r·τ` sums, the leg sums read the coupons that read the steps).
   It takes part in the split as a unit: its carry reads are not edges, and every step of a chain gets the chain's
   level — one more than the deepest read of any of its steps — by a fixed-point over the SCC's rows (levels only
   rise). A class whose rows read a later row after the chain-major reordering, or whose chains never settle (a
   step reading a value that depends on an earlier step of its own chain through another class), is retried with
   its steps banned from any chain and is split by level as before (D23); `InferStats::scan_retries` says why.
   The M1 book has no chain of any kind and its program is unchanged (8 classes, 10 domains, 42,314 rows).
4. **Execution.** `exec::Interpreter`: wave by wave — wave w is the rows whose carry chain has depth w, evaluated in
   tiles of the indirect row mode into the member buffer and copied to their slots, so a row reads rows of earlier
   waves only (sequential along a chain, parallel across chains and lanes); never fused, never inlined.
   `adjoint::Adjoint`: forward one row per tile in row order (each load sees the previous row), reverse one row at a
   time backwards — the reverse scan is the same pull, since the edge slot (carry, r + 1) is one of row r's readers
   (D31). Bits are independent of B, tile and lane tile as before. Mutants (D32): `expander.scan_carry_from_init`,
   `interpreter.scan_drop_last_wave`, `adjoint.scan_forward_order`, exercised by the scan fixtures' gates (WORKLOADS
   §M2), since the M1 book has no scan.
Measured on the fixtures (Apple clang 21, release and reference presets): the RFR compounding book (D16-style seed;
`fixtures/rfr_book.hpp`) after the passes is 2 scan domains — 48 projected chains of 73–92 daily steps (CSE shares the
unseasoned swaps' identical coupon periods) and 3 chains of 10–18 fixings — and 156 chains on the raw recording; the
affine scan (`fixtures/affine_scan.hpp`, 4 paths recorded time-outer) is one domain of 4 chains of 100 steps with the
step `sum(mul(@0,^),@1,@2)`; round-trip identity holds raw and after the passes; the interpreter is bitwise the double
maths and the replay at 65 states, B = 64 lane for lane the 64 B = 1 runs over tiles {1, 7, 256, 4096} × lane tiles
{1, 3, 8, 32, 64}; the adjoint's Jacobian is within 3.1e-15 of forward mode (gate 1e-12, relative to the row's scale)
and 2.1e-9 of central FD (gate 1e-6); the batched adjoint is lane for lane the single runs. The reverse of a fused
scan step and the per-row adjoint of a scan are M4's, like every other fusion.

## D42 — The conventions layer: a strict JSON registry of definitions, mechanics in code, disagreements recorded (2026-09-23)
Implements D36 and D37 for Stage A (M3/G0). `blueprints/conventions/*.json` hold currencies, calendars (rule
instances), day-count names, index definitions, instrument conventions and central-bank dates; every file may carry
any section, the registry merges the directory in name order, a name defined twice is an error, and the schema is
strict: an unknown key or a missing required field fails the load with `file:line:column`. Every value carries its
citation (`sources`) matching `docs/G4_BUNDLE.md` and, where imported from SwapEngine's conventions file, a
`provenance` (file, date, key, `cross_check` verdict). The rule kinds (`fixed`, `nth_weekday`, `easter_offset` with
the SIFMA first-Friday exception; observances `none`, `sat_to_fri_sun_to_mon`, `sun_to_mon`, `sat_to_fri`,
`next_weekday`), the business day conventions, the day counts, schedule generation, IMM periods and the RFR
observation windows (plain, lookback, observation shift, lockout) are code under `include/epykos/conventions/`,
structure only (ints and doubles; never a `Scalar`). Adding a currency, index or instrument is a JSON edit; the
registry test proves it with a synthetic extra file. The JSON reader is in-house (`epykos/util/json.hpp`, D12).
Three calendars are kept apart for SOFR because they differ on Good Friday: SIFMA business days (an early close is a
business day), the days SOFR is published for (never Good Friday: NY Fed 2026), and Federal Reserve days; SOFR swap
dates use the joint SIFMA + Fed calendar (ISDA MPN 2022-04-08). Where the sources disagree the registry takes one
value and records the other rather than resolving silently (D37): the EUR EURIBOR IRS fixed leg is 30/360 Bond Basis
(CFTC MAT, Bloomberg SEF, Strata) against SwapEngine's 30E/360; the €STR OIS payment lag is 2 (Strata, SwapEngine)
against the TP ICAP template's and LCH 2019's 1. Futures carry no convexity adjustment (D35, stated). Items the
research could not source are marked UNVERIFIED in both the bundle and the JSON, never presented as standard.

## D43 — Instruments as templated maths over row tables; blueprints and curve definitions as data; the builder's rules (2026-09-23)
Implements `PROBLEM.md` §3 "real instruments" and D36 for Stage A (M3/G2, `include/epykos/maths/instrument/`,
`src/maths/instrument/`, `blueprints/instruments/*.json`, `blueprints/curves/*.json`). Eight choices:
1. **The maths reads plain row tables** (`tables.hpp`: `Coupon`, `ObsDay`, `Leg`, `Instrument`) and a curve callable
   `Scalar df(int slot, double t)`. Every branch of `coupon.hpp` / `instrument.hpp` is on a table field (the coupon
   kind, a realised flag, the instrument kind, the side); the constants are the rows' doubles; a curve is a small
   integer *slot* (the discount curve of a currency is the slot of its discount index, the projection curve of a
   float leg the slot of its index; `builder.hpp` `CurveSlots` maps index names to slots), so a trade never names a
   curve object and the same tables price off `solver::CurveStates::df`, a `Composite` state or a plain array.
   Curve times are ACT/365F from the valuation date, the convention of every curve in `maths/curve/`.
2. **Four coupon kinds, written the natural way.** Fixed `N τ K DF(t_pay)`; RFR compounded in arrears as the product
   loop `acc = realised factor; acc = acc·(1 + f_i·w_i)` over the projected observation days of `conventions/rfr.hpp`
   (plain, lookback, observation shift, lockout), `R = (acc − 1)/τ_obs` on the observation period's calendar days,
   paid on the accrual period's day count through the payment lag (`maths/swap/compounding.hpp`'s `compound_step`
   and `forward_rate`: the loop records as a scan domain, D41 — measured on the instrument sample: one scan domain
   `mul(^,@0)@scan`, 52 chains of 47–259 steps after the passes); RFR arithmetic average as the running sum of the
   daily forwards weighted by calendar days (a reduction, never a scan: it does not telescope); term rate fixing in
   advance as the realised fixing (structure) or the forward over the accrual period with the index's day count. The
   days already fixed enter as ONE double (the realised factor / weighted sum), not as a chain of constant steps as
   in G3's fixture: they are structure, and the projected chain starts from that constant.
3. **The projected rate FOR a day r is the overnight forward over the rate's own span** `[r, r')`, `r'` the next
   fixing business day, applied for the entry's weight `n_i`: for the plain method the span and the weight coincide
   and the product telescopes (the closed form the tests check, `float leg = N (DF(s) − DF(e))` at zero payment lag);
   under lookback, observation shift and lockout they differ, and it does not.
4. **Stated simplifications**, in code and here: a term-rate stub's forward runs over the accrual period with the
   index's day count (not interpolated between index tenors); futures carry no convexity adjustment (D35), a future's
   PV is `(price − traded price)/100 · notional`, undiscounted (variation margin), its calibration residual is in
   rate units (`rate(curves) − (1 − q/100)`) so the solver's `‖F‖∞` tolerance means the same on every instrument; a
   deposit is a one-period swap (the fixed coupon at the deposit rate against the index forward over the same
   period, `par = the forward`); accrued interest is structure (the rate known so far — the fixed rate, the realised
   fixing, the realised compounded / averaged rate — times the accrual to the valuation date); cash flows paid on or
   before the valuation date are dropped; a projected observation day or a term fixing dated before the valuation
   date without a rate in the history is an error, never a projection.
5. **Blueprints** (`blueprint.hpp`, `blueprints/instruments/stage_a.json`): a named registry convention plus the
   coupon kind per leg (with an observation override) and default trade fields; a `Trade` (blueprint, dates or
   tenor, notional, side, rate / spread / traded price, futures contract) plus the registry, a fixings history, the
   valuation date and the curve slots resolve into the tables (`build_instrument`). A trade of a known coupon kind
   needs no C++: `tests/instrument/blueprint_test.cpp` adds a quarterly-fixed SOFR OIS with a 5-day lookback as a
   convention file and a blueprint file and prices it. The strict schema is documented in `blueprint.hpp`; an
   unknown key or a missing field fails with `file:line:column`, as the registry's does (D42).
6. **Curve definitions** (`blueprints/curves/{usd,eur}.json`): the index a curve projects, its scheme and variable
   (or regions with a scheme and variable each, boundaries at instrument tenors), knots (the instruments' maturities
   — the last cash-flow time — or explicit tenors) and the calibration instrument set (blueprints with tenor lists,
   or the front n futures contracts) with quote keys (`"<blueprint> <tenor>"` or the contract code).
   `build_calibration_set` sorts the instruments by maturity, refuses two at one maturity, and returns the knot
   times, the `RegionSpec`s and the instruments; `calibrate.hpp` `add_calibration_set` makes them one curve of a
   `solver::CurveSet` with `instrument::residual` as each residual — the CurveSet's curve indices ARE the slots, and
   the set discovers from the maths which curves an instrument reads (D40.6). Stage A's sets: USD-SOFR (the overnight
   fixing, OIS 1M–3M, eight SR3 quarters, OIS 3Y–30Y: 22 instruments) on four schemes (linear zero, log-DF, monotone
   cubic, a three-region composite); EUR-ESTR (18), EUR-EURIBOR-6M (the 6M deposit and fixed-vs-6M swaps, 12),
   EUR-EURIBOR-3M (the 3M deposit, eight FEU3, 3s6s basis swaps, 18). Measured: the par quotes of a generating curve
   are recovered to 3.4e-14 (USD) and 3e-14 / 2e-16 / 4e-16 (EUR, sequential blocks €STR → 6M → 3M discovered from
   the maths) from a flat start.
7. **The record-time facts of the instrument sample** (`fixtures/instrument_sample.hpp`: 75 seeded trades, five of
   every Stage A blueprint, two of each seasoned, 44 inputs): 44,516 nodes / 33 domains / 29,369 values after the
   passes; the interpreter bitwise the double maths at 17 states and lane for lane at B = 17 over 9 tile / lane-tile
   configurations; the adjoint's Jacobian within 2.2e-15 of `Dual<44>` (gate 1e-12) and 3.7e-8 of central FD (gate
   1e-6); round-trip identity raw (3 scan domains, 110 chains) and after the passes.
8. **What is not here**: the CurveSet still interpolates linearly in the zero rate (D40); a definition on another
   scheme keeps its regions for G5's Composite-aware solve. Cross-currency legs, MtM notionals and FX are Stage B.
   No SwapEngine file was opened for this package: the conventions came from G0's registry (D37 was exercised by G0).

## D44 — The Stage A tape: the problem definition as data, one recording with its output layout, the AD mode of the ladder, the scheme sweep as recorded variants, a scan branch is straight-line code (2026-09-23)
Implements `PROBLEM.md` §4 Stage A and §5 for M3/G5 (`blueprints/problems/stage_a.json`,
`include/epykos/fixtures/stage_a.hpp`, `src/fixtures/stage_a_e0.cpp`, `tests/stage_a/`, `bench/stage_a/`). Nine choices:
1. **The problem definition is data, its numbers are seeded.** `blueprints/problems/stage_a.json` holds definitions
   only (D36): the curves in slot order (their definitions in `blueprints/curves/`) and the SOFR scheme variants, the
   plausible levels the synthetic quotes and fixings are generated around (stated: USD SOFR 3.80 % → 4.30 %, €STR
   1.95 % → 2.65 %, EURIBOR 3M / 6M above €STR by 10 / 18 bp, a Nelson–Siegel level + slope shape with τ = 3 y), the
   quote noise (± 1 bp, ± 0.01 on a futures price), the book's mix (SOFR OIS plain 25 %, observation shift 10 %, shift +
   lockout 10 %, SOFR averaging 10 %, €STR OIS 15 %, EURIBOR 6M IRS 12 %, 3M IRS 8 %, 3s6s 10 %), tenor and notional
   distributions, the seasoned fraction (20 %, ages 30–1,500 days), 40 netting sets, and the scenario families
   (parallel / twist / butterfly / per-curve, 250 lanes each, ± 100 / 100 / 50 / 100 bp). Every quote, fixing, trade and
   lane is generated in code from the seed (Philox sub-streams 800000 + trade, 810000 + quote, 820000 + lane) and is
   labelled synthetic; the schema is strict (an unknown key fails with `file:line:column`). Quotes are the generating
   curves' par quotes plus noise, so O1 is a fit; noise 0 gives the par quotes exactly (the recovery gate).
2. **One tape, one output order.** The 70 quotes are the free inputs (slot order, each set in maturity order:
   22 USD-SOFR, 18 EUR-ESTR, 18 EUR-EURIBOR-3M, 12 EUR-EURIBOR-6M); `solver::CurveSet::calibrate` records one implicit
   block per curve in the order it discovers from the maths (SOFR alone; €STR → 6M → 3M) with the residual outputs and
   the two O1 diagnostics per block registered by `implicit()`; then O1, the knot values of every curve (the unknown
   Inputs registered as outputs, in each region's variable); then O2 in four contiguous vectors of n_trades — pv (trade
   currency), pv in the reporting currency, legs[0], legs[1] — the per-currency totals, the per-netting-set totals and
   the book; then O6, three exports per Select when the tape has any (`StageALayout` names every ordinal; 8,191
   outputs on the base problem). Accrued interest is structure (a double per leg in the tables, dumped, not an
   output). O3 and O4 are runs of `solver::ImplicitProgram` over that tape (`ladder`, `run_lanes`).
3. **The CurveSet calibrates any curve of the family.** A `CurveSpec` with regions is a `curve::Composite` whose state
   is prepared once per curve and state (`CurveStates::set`), so a Hyman limiter's Selects are recorded once per block
   and shared with the book through cse; a spec with no regions is the M1 linear zero-rate path, statement for
   statement (the G4 fixtures and their pinned node counts are unchanged), and a one-region {linear, zero} definition
   is bitwise that path (D38). `add_calibration_set` passes the definition's regions; `start_values` gives a flat start
   in each knot's variable. The unknowns of a log-DF curve are log DFs.
4. **The scheme sweep is a set of recordings, not one tape.** The base problem calibrates SOFR on the linear zero
   definition; the log-DF, monotone cubic and composite variants are the same problem recorded again with slot 0 on
   that definition (`StageAOptions::usd_curve`), each its own tape. A variant curve inside the base tape would be
   calibrated and never read by the book, so the sharing gate ("every DF domain feeds both the residuals and the
   book") would fail by construction. Measured on 300 trades: every variant converges, round-trips and holds the
   sharing gate; the monotone variant exports 126 Selects (18 within 1e-6 of a flip at the record point), the composite
   36, log-DF none.
5. **A record-time discount-factor memo, stated.** The book's `df(slot, t)` memoises the Rec of each (slot, t) it has
   recorded; the tape after cse is the same either way (the merged nodes are the ones the memo returns), the raw
   recording is smaller by the duplicates (27.5 M raw nodes → 517,036 after the passes on the 2,000-trade book; the
   calibration instruments' discount factors are not memoised and are merged by cse). Nothing of the maths is folded.
6. **Solve tolerance 1e-13.** The compounded residuals of a 30-year daily product carry a rounding floor of 3–5e-14
   in rate units, above G4's default 1e-14 (the record-time Levenberg–Marquardt then rejects twelve steps and reports
   "not converged" at the floor). The O1 gate `‖Jᵀr‖∞ < 1e-12` holds on zero-rate unknowns (6.8e-14 / 2.9e-14 /
   6.2e-16 / 7.7e-15 per block); on log-DF unknowns the overnight deposit's Jacobian entry is 1/τ = 360, so the
   diagnostic sits at 2e-11 at the same floor and the variant gate is scaled by that 1/τ (stated).
7. **The AD mode of O3, measured both ways.** The book and aggregate ladders (43 outputs) are the IFT adjoint, one lane
   per output (0.22 s). The per-trade ladder — 2,000 outputs × 70 quotes — is recorded the same way (2,000 adjoint
   lanes, 8.8 s, 4.4 ms per lane, identical lanes sharing one solve) and, as the AD-mode comparison, in forward mode:
   the whole problem instantiated once on `Dual<70>` (quote k the direction k; `solver::implicit_dual` per block with
   the earlier curves' tangents flowing through the block's parameters; the book on Dual) in 1.4 s — 6.2× cheaper for
   this shape on d448afd70180 (release, load 5–9 of 16: informational). The two agree to 5.8e-15 of the row scale over
   140,000 entries, and the adjoint agrees with Richardson bump-and-recalibrate (10 / 5 bp) to 5.1e-8. Forward mode
   here is M2's `Dual` on the templated maths, not a tangent interpreter over the IR; choosing the mode per Jacobian
   block is M4's rule (`PROBLEM.md` §7), and the reverse ladder stays the engine-native path G6 gates.
8. **No FX in Stage A.** pv in the reporting currency is the trade's pv node for USD and pv × the blueprint's
   placeholder 1.0 for EUR, a recorded product that Stage B replaces with an FX spot input (stated).
9. **A scan branch is straight-line code.** Two compounded coupons over the same start whose ends differ by a business
   day (a seasoned lockout trade whose anniversary adjusts onto another's, common in a real book) share their steps up
   to the divergence, and the later coupon's remaining steps form a chain whose initial value is an interior row of the
   trunk's chain. The scan layout of D41 refused the whole class for it (255,705 rows banned and split into some 330
   level domains on the 2,000-trade book). `src/ir/signature.cpp` now bans only the offending chain's nodes and infers
   again: the branch's steps become ordinary elementwise rows reading the trunk's row, the trunk and every other chain
   stay a scan (6 chains of 18 rows on the base problem). The G3 fixtures are unchanged (one round, no retry).
Measured on the base problem (Apple clang 21, d448afd70180, release; `tests/stage_a/`): 2,000 trades (378 seasoned,
7,537 compounded, 1,397 averaged and 15,297 term coupons, 2.2 M projected observation days), 70 quotes, 148 tape
inputs, 8,191 outputs; record 12.3 s (dependency discovery 0.2, the four record-time solves 0.9, the book 3.2, the
passes 7.6), 27,459,283 raw nodes → 517,036; the IR 67 domains, 423,235 values, 5 scan domains of 1,107 chains over
255,959 rows (3 scan classes), two DF domains (177 knot-time rows `exp(mul(@0,$0))`, 16,917 interpolated rows
`exp(mul(neg(@0),$0))`) both read by the residuals and the book, no DF computed twice; round-trip identity after the
passes (and raw on a 120-trade book: 2.6 M nodes, 1,377 chains); the program's run at the quotes 19 ms per lane at
B = 1 and 17 ms per lane at B = 8 (four recalibrations per lane, chord policy), bitwise the double maths at each lane's
solved knots, the first eight scenario lanes bitwise their single runs.

## D45 — The M3 gate: the Stage A gate set, the differential ball's reference, the M4 performance baseline (2026-09-23)
Implements `PROBLEM.md` §6 for M3/G6 (`tests/stage_a/gate_*_test.cpp`, `bench/stage_a/stage_a_bench.cpp`,
`scripts/exec_coverage.py`, `bench/results/d448afd70180/m3.json` + `m3.md`). Five choices:
1. **Every §6 gate is an explicit boolean with its worst number** (a `[  GATE    ]` line per gate: name, ok, worst,
   the gate value, the sample). The G5 tests already hold round-trip identity, the sharing assertion, the O1
   diagnostics, the O2 E0 statement and the full 2,000 × 70 ladder against forward mode; G6 adds the gates §6 names
   that were not yet explicit: the differential ball over the quotes (E0), the adjoint of the pricing graph (O2,
   no IFT) against finite differences and forward mode over the knots, the IFT ladder against bump-and-recalibrate
   on sampled (trade, quote) pairs and the book, 32 sampled O4 lanes against independent single runs, optimality per
   curve at the record point / at the run / on every sampled lane, and the recovery of the generating curves from
   their par quotes. Sample sizes are stated in each test and seeded (Philox, seed 20260923): 64 ball draws, 24
   trades for the O2 adjoint, 50 trades for the bump gate each paired with a quote drawn among the entries its
   ladder row is significantly sensitive to (>= 1e-3 of the row's largest; a EUR trade against a USD quote is zero
   on both sides and would test nothing), 32 lanes.
2. **The differential ball's reference is the double maths at the knots a separate single-lane solve finds.** The
   state of the ball is the 70 quotes (ρ = 0.005, the WORKLOADS §M2 radius); the compiled side is the batched
   `ImplicitProgram` (every lane recalibrated); the reference, per draw, runs its own single-lane `ImplicitProgram`
   and evaluates `price_stage_a_at` (O2), `CurveSet::residuals` (the residual outputs), the solved knots (O1) and the
   solve's diagnostics on double at that solution. A pass therefore states both "the batched solve is bitwise the
   single solve at random quotes" and "the whole-program interpreter is bitwise the templated maths at the solved
   knots". The solves' linear algebra (Eigen) is not pinned to `-ffp-contract=off`, so the gate is stated for the
   reference preset (CLAUDE.md Build); it holds under release too on d448afd70180 and both are reported. The
   reference's own book on double costs more than the compiled lane, which is why the ball has 64 draws, not 256.
3. **The M4 baseline is the `stage_a_stage_a` run.** Per D34 a baseline entry is keyed by the run name
   `bench/run.sh` derives from the binary, so the measured file is `bench/results/<fp>/stage_a_stage_a.json` and
   `perf_gate.py --accept` seeds `baseline.json` under that name; `m3.json` is the M3 gate summary in the `m1.json`
   sense (gate booleans and worst numbers, the structure counts, the medians and the coverage), not a run file, and
   `m3.md` reads it. The bench gains `BM_Calibrate/policy` (one lane's four block solves, per_iteration and chord,
   driven through `ResidualProgram` / `BlockSolver` as `ImplicitProgram` drives them) and `BM_Evaluate/B/L` (the
   whole-program interpreter alone at the record point, B identical lanes at lane tile L), so "one calibration" and
   "O2 evaluation" are separate rows beside `BM_Run` (solves + run); O3 is stated per AD mode: `BM_Adjoint/B` is
   the IFT reverse ladder (one adjoint lane per output) and `BM_ForwardLadder` the `Dual<70>` pass.
4. **The lane tile gates row fusion.** `exec::Interpreter` applies the inliner and the exp tails only when row fusion
   pays (`row_fusion_pays`: L = 1 or L ≥ 16); the scenario grid runs at the default lane tile 8, where neither
   fires (no domain inlined; the interpolated zero rates materialised before the DF exp). The plan test writes the
   L = 1 and L = 32 plans beside the grid's, and `BM_Evaluate` measures (64, 8), (64, 32) and (64, 64): a planner
   fact M4's cost model must price (the M1 best points were L = 1 at B = 1 and L = 32 at B = 64), recorded here,
   not changed — the grid's defaults stay what G5 measured.
5. **The `-DEPYKOS_EXEC_PROFILE` table holds 4,096 slots** (it held 64 and the Stage A program has 67 domains); the
   output copy is the last slot and ids beyond the table fold into the one before it. `scripts/exec_coverage.py`
   joins a plan, the IR facts per domain (readers through gathers / segments, scan readers, output rows) and a
   profile into the coverage table, and states for each plain-tile domain which of the two fusion rules' conditions
   it fails.

## D46 — GCC's cross-TU FMA contraction reaches header-only Scalar templates, not just the reference TUs D25 names (2026-09-24)
Refines D25 (M4 CI fix, unrelated to the M4 packages otherwise). D25 pinned the reference TUs (fixture generators,
the hand-fused kernel, the interpreter kernels) to `-ffp-contract=off` in every preset because GCC's ISO-mode
default contracts multiply-add pairs across statements even in C++, and it named the E0 gates that broke before
that fix. Two more assertions broke the same way without being E0 gates at all, and without anyone noticing: CI on
`ubuntu-latest / release` and `ubuntu-latest / reference` has been red on every push to `integrate/m1-m5` since
5a5fea4 (the G4 implicit-node package) at `tests/solver/m1_implicit_vs_dual_test.cpp:130`
(`EXPECT_EQ(out[...], pv[...].v)`) and `tests/stage_a/definition_test.cpp:141` (`EXPECT_EQ(exact.quotes[k++], p)`).
Both compare a value produced inside an E0-pinned TU against a *second*, independent instantiation of the same
header-only `Scalar`-templated function, freshly compiled in the comparing (non-`_e0`) test TU: contraction is a
per-TU, context-sensitive compiler decision (D25's own finding), so GCC is free to round the second instantiation's
arithmetic differently from the first even though the source is identical, while Apple clang in this configuration
happens to contract (or not) the two instantiations alike. Two fixes, one of each kind D25 already allows:
1. **`m1_implicit_vs_dual_test.cpp` (tolerance, not pinning).** The failing check compares
   `ImplicitProgram::adjoint`'s forward replay (E0-pinned, `src/adjoint/adjoint_e0.cpp`, D31.3) against
   `price_book<Dual<12>>` instantiated fresh in this test TU — the same crossing the M2/Q1 differential tester
   treats as an E1 tolerance under the release preset and bitwise only under reference (D30), and this file's own
   D33 classification already puts it in the mutation harness's *tolerance* gate class (`*_vs_dual_test$`, not
   `_e0_test$`): the value-channel sanity check was simply never brought up to the standard the rest of the file,
   and D33, already hold it to. Pinning it properly would mean forcing the Newton solve (`solve_newton`,
   `implicit_dual`) under `-ffp-contract=off` for the whole file, which is exactly the "genuine reason not to" the
   task instructions ask for: it would fight D33's own design of this file as a tolerance gate. Fixed to
   `epykos::verify::within(a, d, Tolerance::e1_relative(1e-12), scale)` with D26's leg scale (`|fixed_i| + |float_i|`
   per swap, `Σ|pv_i|` for the book) — the bound the same file already uses two paragraphs below for the Jacobian.
2. **`definition_test.cpp` (pinning, not tolerance).** "Quotes equal the generating par quotes exactly at zero
   noise" is a real invariant, not an accident of rounding (`include/epykos/fixtures/stage_a.hpp`'s
   `quote_noise_bp` comment: "0 = the generating curves' par quotes exactly (the O1 recovery gate)"), so the fix
   keeps it bitwise. Added `fixtures::stage_a_generating_par_quotes` (declared in
   `include/epykos/fixtures/stage_a.hpp`, defined in the already-pinned `src/fixtures/stage_a_e0.cpp`), a thin
   wrapper around the identical `instrument::par_quotes(cs, df)` call `make_stage_a` uses internally to build
   `StageA::quotes`; the test now calls this pinned wrapper instead of `epykos::instrument::par_quotes` directly,
   so both sides of the comparison run the same compiled object code instead of two separate template
   instantiations.
Other files: grepped every non-`_e0_test.cpp` file under `tests/` for a bitwise `EXPECT_EQ` / `ASSERT_EQ` of two
independently-computed floating-point paths (~90 matches across 50+ files). The `*_vs_dual_test` / `*_adjoint_test`
files elsewhere already do this correctly — `tests/adjoint/m1_adjoint_vs_dual_test.cpp`,
`adjoint/scan_adjoint_vs_dual_test.cpp`, `instrument/sample_adjoint_vs_dual_test.cpp`,
`curve/composite_vs_dual_test.cpp` gate their value/Jacobian channels at D26's tolerance and `EXPECT_EQ` only
integer failure counters — so the bug was specific to the two files above, both from the newer G4/G5 packages that
had not yet been through a D25-style review. Every `bits(a[k]) == bits(b[k])` round-trip comparison
(`tests/ir/`, `tests/tape/`, `instrument/sample_roundtrip_test.cpp`, `ir/scan_roundtrip_test.cpp`) compares
non-template, once-compiled evaluators (`Replayer`, `ir::Evaluator`, the E0-pinned interpreter kernels) whose
object code is fixed per preset regardless of which TU calls them, so none of them carry this risk, which is
presumably why the P7-fix GCC audit that produced D25 did not flag them. `tests/tape/record_test.cpp` and
`tests/scalar/dual_test.cpp` compare single, isolated operations (no adjacent multiply + add for contraction to
act on) against a literal computed the same way in the same statement: safe by construction. One finding is
accepted risk, not changed here: `tests/curve/composite_test.cpp`'s `SingleRegionEqualsTheCurveTemplateOnDouble`
and `tests/curve/schemes_test.cpp`'s `Linear` vs. `curve::linear::zero_rate` check bitwise-compare a
`curve::Curve<Scheme, Variable>` / `Scheme` instantiation against an independent equivalent, both freshly
instantiated in that (non-`_e0`) test TU — the same shape of risk as the two bugs fixed above. D38.6 already names
`curve_record_e0_test` as the real pinned bitwise gate for the template-vs-`Composite` equivalence; both checks
passed on GCC 13 throughout M3 and passed again in this fix's own full GCC 13 run (below), so they are left for a
follow-up (pin, or fold into `curve_record_e0_test`) rather than changed under this fix's scope.
Verification (fingerprints d448afd70180-equivalent hosts; no performance claim, tests only): GCC 13.5.0 in Docker
(`gcc:13`, reproducing `ubuntu-latest`), release and reference presets, 80/80 `ctest` both; Apple clang 21 on the
M1-M5 development machine, release and reference presets, 80/80 both (a first attempt using a non-standard build
directory produced one unrelated failure, `scripts_perf_gate_test` — `run.sh` cannot infer a preset name from a
path outside `build/<preset>/` — which disappeared building into the standard `build/release` / `build/reference`
directories D13 names); `scripts/mutation_test.sh` on GCC 13 in Docker, all registered mutants caught against the
40-test gate set (D33's regex), matching `RESUME.md`'s existing M3 mutation count. No engine, maths or kernel code
changed; no gate weakened; no new dependency; no `-ffast-math`.

## D47 — M4/R0: the Rule interface (match/propose over Program + PlanAnnotations), Program's plan
annotations excluded from its identity, the M1 planner re-expressed as rules (2026-09-24)
Implements `PROBLEM.md` §7 / `RESUME.md` §3's R0 package. A `rewrite::Rule` (`include/epykos/rewrite/rule.hpp`) is
a name, an exactness class (D8), `match(program, plan) -> vector<MatchSite>` and
`propose(program, plan, site) -> Proposal`, where `Proposal` holds EITHER a rewritten `ir::Program` (a structural
rewrite: R1-R7) OR a `PlanAnnotations` delta to be merged, never mutating its inputs. This one interface serves
both a GREEDY pass (`rewrite::apply_greedy` / `run_pipeline`, `include/epykos/rewrite/greedy.hpp`: fold the
Proposal onto a live program/plan and move on — "discard the original, keep the rewrite") and an e-graph driver
(M4/EG, unbuilt: keep the original and the Proposal as siblings of one e-class — "produce the alternative without
discarding the original", this package's own brief); `rewrite/rule.hpp` documents the contract for both, EG
implements neither itself (D12: no external e-graph library, EG writes its own on top of this interface).

`ir::Program` gains a `plan` field (`ir::PlanAnnotations`, `include/epykos/ir/annotate.hpp`): per-domain
materialisation choice (materialise / fuse-into-reduction / inline-into-consumer, with the fused domain's kept
rows or the inlined domain's consumer), fused-pair / chain-tail step groupings, emitted-output flags, and a
per-Jacobian-block AD-mode slot (forward / reverse / closed-form-affine; unpopulated and unread by anything in
this package — M4/EG's cost-model rule is the first consumer). `PlanAnnotations::operator==` is unconditionally
`true` and neither `ir::serialize` nor `ir::deserialize` touch `plan`: it is EXCLUDED from a Program's identity
by design, for three reasons recorded in `annotate.hpp`'s own header comment — round-trip identity (D10) is a
property of the recording, not of a plan for one execution of it; a materialisation choice is Options-dependent
(`row_fusion_pays(lane_tile)`, unmoved from before this package); and `exec::Interpreter` / `adjoint::Adjoint`
take `const ir::Program&` without owning it, so two Interpreters over the SAME Program at different lane tiles
(the M1 bench's B=1 / B=64 points) must never see each other's decisions. `Rule::match` / `propose` therefore
take the running `PlanAnnotations` as an explicit parameter rather than reading `program.plan`, so a multi-rule
pipeline never has to copy a Stage-A-sized (517,036-node) Program between rules purely to let each one see the
last one's annotations.

The M1 planner's five decisions (`Interpreter::Impl::decide_fusion`, `decide_inline`, and the shape tests inside
`build_pair_step` / `add_tail_step`, all `src/exec/interpreter.cpp` before this commit) are now pure functions in
`include/epykos/rewrite/planner.hpp` / `src/rewrite/planner.cpp` — `is_whole_segment`, `row_fusion_pays`,
`reduction_fusion_plan`, `emit_outputs_plan`, `inline_producers_plan`, `match_pair`, `match_tail`,
`fused_pairs_plan`, `chain_tails_plan` — wrapped one-to-one as `rewrite::planner::{ReductionFusionRule,
FusedPairsRule, ChainTailsRule, InlineProducersRule, EmitOutputsRule}` (`planner_rules.hpp`/`.cpp`), named
`planner.reduction_fusion` / `planner.fused_pairs` / `planner.chain_tails` / `planner.inline_producers` /
`planner.emit_outputs`, run in that fixed dependency order by `DefaultPlanner` (a rule's Options flag off ->
an `OffRule` in its slot, an identity no-op, reproducing `if (opt.fuse_reductions) ...`'s old skip exactly).
`exec::Interpreter`'s constructor now calls `build_plan()` once (uses `program.plan` verbatim if the caller set
it, otherwise `rewrite::planner::default_plan(program, options-derived)` into its OWN copy, never written back to
`*p`) and `decide_fusion` / `decide_inline` / the per-step loop of `build_group` become thin readers of that
plan; `build_pair_step` / `add_tail_step` (renamed `apply_tail`) keep only the kernel-selection half (resolving
`ir::Slot`s to `exec::detail::Operand`s and looking up `KernelTable` entries), calling `match_pair` / `match_tail`
for the shape a confirmed annotation names, so there is exactly one implementation of every shape test shared by
the rule objects and the interpreter — no way for them to disagree. `adjoint::Adjoint` is unchanged: it has no
materialisation decision to extract (D31 — every row value is already kept, none of the interpreter's fusions
apply in reverse), so nothing in R0 touches it beyond the shared `PlanAnnotations` schema its Jacobian-block slot
lives in for a later package.

`ReductionFusionParams::emit_min_bytes` exists on the FUSION rule, not only the emit rule, because
`decide_fusion`'s own keep-row computation was never independent of the emit byte-size gate: an output row that
cannot be emitted (its domain's region under `emit_min_bytes` at the interpreter's own lane tile) must be kept
materialised, or a fused-but-unemitted row is written nowhere — a real, deliberately-introduced-then-caught
regression during this package's own development (`tests/exec/interp_e0_test.cpp`'s
`HandBuiltProgramSumFallbackConstAndAffine` and `MiniBookWithSelectMatchesReplay` both failed with output stuck
at bit pattern 0 the first time the two rules were split without threading `lane_tile` through both); fixed by
giving `reduction_fusion_plan` the same `lane_tile` / `emit_min_bytes` inputs `emit_outputs_plan` already had.
Left as a coupling, not two independent thresholds, because that is what the M1 planner actually computed and
`PROBLEM.md` §7 requires the default plan to be bit-identical to it.

Also shipped: a per-rule verifier (`include/epykos/rewrite/verifier.hpp`/`.cpp`) — `compare_programs` /
`verify_annotations` build two `exec::Interpreter`s and two `adjoint::Adjoint`s (before/after) and reuse
`verify::differential` (the M2 harness) for the forward comparison plus a seeded-`out_bar` sweep over a sample of
outputs for the reverse one; `verify_rule` runs a `Rule`'s own `match`/`propose` and reports the first failing
site. `rewrite::stub_rule::IdentityStubRule<Tag>` backs eight always-empty-`match` placeholders, one file each
(`r1_fold_uniform_columns.hpp` .. `r7_block_linmap.hpp`, R4 split into R4a/R4b per `DESIGN.md` §6's own table),
for R-a/R-b/R-c to fill in independently.

Verification (fingerprint d448afd70180): `ctest --preset release` and `--preset reference` both green (see
`RESUME.md` §5's R0 landing entry for the exact count, unchanged from M3's 80 plus this package's own new
`rewrite_*` executables); `scripts/mutation_test.sh` green, 20/20 mutants caught against the same 40-gate set —
this package registers no new mutant of its own (its rules are verified by `rewrite::verify_rule` against the
M1 book and by the existing M1/M3 gates re-running bit-identically; a mutant of the planner rules is left to
whichever M4 package next changes their behaviour on purpose, since a mutant of an identity refactor has nothing
to catch it that the M1 gates do not already catch by construction). No SwapEngine file opened.

## D48 — The cost model: an annotation independent of the interpreter, per-tile cache tiering, fitted by relative-error least squares (2026-09-24)
Implements `PROBLEM.md` §7 for M4/CM (`include/epykos/optimise/cost.hpp`, `src/optimise/cost.cpp`, `tools/costmodel/`,
`tests/optimise/cost_test.cpp`). Five choices:
1. **The annotated Program is CM's own type, not `exec::Interpreter`'s internal plan.** M4/R0 (turning the
   interpreter's hard-coded fusion decisions into a Rule framework) and CM run in parallel (`RESUME.md` §3:
   "Order: {R0, CM} -> ..."), so CM cannot depend on R0's deliverable, and EG's later equality-saturation search
   will need to cost *candidate* programs that never become a real `Interpreter` at all. `optimise::Treatment`
   (Materialized / FusedIntoReduction / Inlined) and `optimise::Plan` are therefore a small IR-structural
   annotation CM defines and fills itself (`infer_plan`), reproducing `exec/interpreter.cpp`'s documented rules
   (`row_fusion_pays`, `inline_max_refs_per_row`) as pure predicates over `ir::Program` (HARD RULE 9: keyed on
   structure, no magic numbers for one fixture). `optimise::analyze` generalises the per-domain reader/output-row
   facts `tests/stage_a/gate_lanes_test.cpp` computed ad hoc (for `scripts/exec_coverage.py`) into engine code, so
   the cost model and that test's own coverage report can share one source of them.
2. **A whole-domain Sum/Affine producer can still be inlined.** `infer_plan`'s first cut disqualified any domain
   whose own group is a reduction shape from ever being folded into a consumer, on the assumption that "reduction"
   and "elementwise producer" were mutually exclusive categories. Calibration caught this immediately: the Stage A
   tape's interpolated-DF domain (16,917 rows, `affine(#0;%0)`, gathered 1:1 by exactly one consumer) measured
   0.2 us and was priced at 761 us — three orders of magnitude high, because it was never a *materialised* domain
   at all in the real interpreter. `exec/interpreter.cpp`'s `decide_inline` allows a whole-segment producer to
   inline when every row has the same member count (an interpolation vectorises like the bucketed pass either
   way); `infer_plan` now checks that (`has_uniform_row_length`) instead of excluding every reduction-shaped
   domain, with a regression test built by hand (`Cost.UniformAffineProducerCanBeInlined`, not the Stage A tape:
   sub-second, no fixture dependency).
3. **The cache tier is decided by one tile's worth, not by a domain's whole footprint.** DESIGN.md §4's own
   description of the tiled interpreter's scratch ("a contiguous vector of tile·L doubles ... L1-sized at the
   default tile for small L") makes tile rows x lane width the right working-set size for the L1/L2/L3/DRAM
   classification, not a domain's total rows x lanes: a domain can be five orders of magnitude larger than L1 and
   still stream through it a tile at a time. `byte_cost`'s two arguments (bytes moved, for the quantity; tier
   bytes, for the rate) keep this distinction explicit.
4. **Fitting is relative-error least squares, ridge-regularised toward `CostCoefficients::defaults()`.** The
   package's own gate is a MEAN RELATIVE error, and the two calibration workloads' measured domain times span five
   decades (tens of ns to milliseconds): plain least squares on absolute nanoseconds is dominated entirely by
   Stage A's largest domains and reproduces M1-book-sized ones only by accident — an early fit this way put a
   16-domain M1 program's dominant `Mul` domains three orders of magnitude high for exactly this reason. Every row
   is divided by its own measured time before the solve (`minimise sum((pred-measured)/measured)^2`, i.e. every
   row's target becomes 1), and every column is scaled to [-1, 1] before a small L2 penalty pulls the solution
   toward the documented defaults rather than toward zero — still one ordinary least squares call
   (`Eigen::MatrixXd::bdcSvd().solve()`) on an augmented system, D12's Eigen dependency, nothing new. Feature
   extraction never re-derives `cost.cpp`'s formula: `tools/costmodel/fit_main.cpp` calls
   `optimise::estimate_program` once per unknown coefficient with a one-hot coefficient vector and reads the
   result off as that coefficient's linear contribution to every domain (the model is exactly linear in every
   coefficient, so this is exact, not an approximation), which makes a fit-vs-engine drift structurally
   impossible.
5. **The M1 book and the Stage A tape are calibrated together, on a grid covering a tile sweep AND a batch/lane
   sweep for BOTH workloads** (`tools/costmodel/calibrate.py`'s `GRID`; 15 (case, B, tile, lane_tile) points, 492
   measured (case, config, domain) rows, 41 of 187 coefficients with data support — the rest stay at
   `CostCoefficients::defaults()`). Measured on this machine (`d448afd70180`; `bench/results/d448afd70180/
   cost_model.json`, `cost_model_validation.md`): **mean absolute relative error over every measured domain
   84.4%** (target < 25%, PROBLEM.md §7's own gate — NOT MET); restricted to domains that are at least 1% of their
   own config's measured time — the ones an extraction decision actually turns on — **69.3%** (also not met).
   Reported honestly as short of the target (CLAUDE.md: "report honestly ... never softened"), not silently
   narrowed to a friendlier subset — both numbers, the per-case breakdowns and the worst domain are in the
   committed report. Named causes, in the report and in `cost.hpp`'s own header comment: libm `std::exp` vs
   `exp_poly` share one `Exp` coefficient; cache effects across domains live *at once*, not per domain; fused
   pairs and chain tails are priced as two ops and two dispatches, so a rewrite that changes step fusion is
   over-costed, never under; a fused-into-reduction domain's `kept_rows` and an inlined domain's exact re-fetch
   count are approximated from IR structure, not a live plan; and a domain measuring a few tens of nanoseconds is
   measuring `ProfileScope`'s own two `std::chrono::steady_clock::now()` calls (`src/exec/interpreter.cpp`) as
   much as its own work — no coefficient can fit away instrumentation noise, and Stage A alone has hundreds of
   domains under half a microsecond. Widening the grid from 11 to 15 points (adding a Stage A tile sweep and a
   B=8 point for both workloads) moved the mean from 84.4%/69.3% to a statistically indistinguishable place
   (78.2%/65.8% on the narrower 11-point grid was the best seen across a lambda sweep on that grid; more points
   did not resolve it), which is itself evidence the shortfall is instrumentation noise and cross-workload
   coefficient sharing, not an under-sized calibration grid alone. Left for a follow-up: fit the two workloads'
   dispatch/byte coefficients separately (share only the per-op rates, which is where the actual arithmetic is
   workload-independent), and/or drop sub-microsecond domains from the fit target entirely rather than only from
   a secondary reported metric.
6. **Landed in parallel with D47's `ir::PlanAnnotations`, not yet reconciled with it.** M4/R0 (this same integration)
   extracted the interpreter's actual fusion / inlining decision (`rewrite::planner::default_plan`) as data
   attached to the Program itself; CM's `optimise::Plan` / `infer_plan` was written and calibrated against without
   it, per this package's own brief (point 1 above: R0 and CM run in parallel, `RESUME.md` §3, neither depends on
   the other's deliverable, so CM could not have depended on a type that did not exist yet when CM branched).
   `rewrite::planner::default_plan` is now the AUTHORITATIVE decision (it IS the interpreter's logic, not a
   reproduction of it), so a follow-up should retire `infer_plan`'s own approximation of the fusion / inlining
   rules in favour of reading `ir::PlanAnnotations` when one is attached to the Program being costed, keeping
   `infer_plan`'s structural inference only as EG's fallback for a candidate program that has no attached plan yet
   (an unmaterialised e-class member, DESIGN.md's own case for why CM defined its own annotation type at all,
   point 1 above). This would likely narrow some of D48 point 5's shortfall: `infer_plan`'s approximation of
   `decide_inline` (found and fixed once already, point 2 above) is one plausible source of it, on top of the
   named instrumentation-noise and coefficient-sharing causes measurement can already see directly.

## D49 — M4/EG-core: a two-tier e-graph over whole (Program, PlanAnnotations) alternatives, deferred
hash-consing with rebuild, extraction by CM cost with an exactness floor (2026-09-24)
Implements `PROBLEM.md` §7 / `RESUME.md` §3's EG package, the "core" slice: the e-graph and extraction only
(`include/epykos/optimise/egraph.hpp`, `extract.hpp`, `plan_bridge.hpp`, `src/optimise/*.cpp`) — the AD-mode-per-
Jacobian-block rule and the cross-stage sharing rules `RESUME.md`'s EG row also names are a later package's scope,
not touched here. Five choices:
1. **Two tiers, matching the two `rewrite::Proposal` payloads, not one flat term e-graph.** `rewrite::Rule`
   (D47) proposes either a whole rewritten `ir::Program` (a structural rewrite: R1-R7) or an `ir::PlanAnnotations`
   delta to merge (a planner decision: R0's five rules). Real equality saturation closes a term e-graph over
   sub-expressions; the landed `Rule` contract does not decompose a Program into sub-terms it rewrites in place —
   `propose` always returns either a WHOLE new Program or a WHOLE plan delta. EG's e-graph is therefore two tiers
   of whole-value alternatives: a PROGRAM tier (e-classes of `ir::Program`, hash-consed via `ir::serialize` — an
   exact identity key, not a hash liable to collide, per that function's own round-trip contract) and, one PER
   PROGRAM NODE, a PLAN tier (e-classes of `ir::PlanAnnotations`, hash-consed by this package's OWN structural
   equality/hash, `EGraph::plan_content_equal` / `plan_content_hash` — never `PlanAnnotations::operator==`, which
   D47 made unconditionally `true` by design, annotate.hpp point 1). With R1-R7 still `IdentityStubRule<Tag>`
   (`match` always empty), the program tier holds exactly one class today; the machinery is real and covered by a
   synthetic structural rule (`tests/optimise/egraph_test.cpp`), not yet exercised by a real one.
2. **Deferred hash-consing, rebuilt once per round, with a persistent-table pre-check so an always-matching rule
   does not grow the graph forever.** Every node existing at the START of a round is matched against every rule
   and every site; every resulting Proposal is folded (`merge_annotations`, for a plan delta) and queued, never
   replacing the node it came from ("produce the alternative without discarding the original", `rewrite/rule.hpp`
   D47's own words). A round ends with `rebuild()`, which hash-conses the WHOLE queue at once and unions any two
   pending nodes whose canonical content collides — the standard deferred-canonicalisation pattern (egg, Willsey
   et al. 2021; D12: no external e-graph library). A naive version of this (check the persistent tables only,
   never within a round) would miss two DIFFERENT derivations converging to the SAME content in ONE round (e.g.
   rule A onto rule B's result and rule B onto rule A's result, when A and B touch disjoint domains) — exercised
   directly by `EGraph.CongruenceAfterRebuildUnifiesPathIndependentDuplicates`. The opposite naive version (queue
   unconditionally, check only at rebuild, every round) would re-queue R0's five rules' own proposals forever,
   since every one of them is a whole-program, unconditional `match()` (`planner_rules.hpp`'s own documented
   shape) — `saturate()` therefore checks `plan_known` / `program_known` against the PERSISTENT (prior-round)
   tables BEFORE queueing, so a round that discovers nothing new queues nothing and `saturate()` reaches a real
   fixpoint rather than running to `max_iterations` every time by construction.
3. **A plan tier is keyed by the program NODE that created it, not by that node's program-tier CLASS — a stated,
   flagged gap, not a silent one.** Two structurally-different-looking program nodes the program tier later
   discovers are identical still keep two separate plan tiers rather than sharing one search space. This is free
   today (R1-R7 never match, so no program node is ever discovered congruent to another after the fact) and is
   named in `egraph.hpp`'s own header comment as the first thing whichever package lands R1-R7's first real
   structural rewrite should close.
4. **`plan_bridge.hpp` is the D48.6 reconciliation, exactly as that entry's point 6 asked for.** `optimise::Plan`
   (CM's own annotation) and `ir::PlanAnnotations` (R0's) are different types by design (D48 point 1: CM and R0
   ran in parallel, neither depending on the other). `plan_from_annotations` translates a plan-tier e-node's REAL
   `ir::PlanAnnotations` into an `optimise::Plan` 1:1 (`Materialize`/`FuseIntoReduction`/`InlineIntoConsumer` to
   the matching `Treatment`, `keep_rows.size()` to `kept_rows`, `optimise::DomainFacts::segment_readers` supplying
   the consumer ids `ir::DomainPlan` itself does not store), falling back to `infer_plan`'s structural guess only
   for the empty root plan node (`ir::PlanAnnotations{}`, "nothing decided yet") — CM's own stated fallback case
   for "a candidate program that has no attached plan yet", D48.6's own words. `tests/optimise/plan_bridge_test.cpp`
   proves the two paths actually differ (an explicit all-`Materialize` annotation is NOT the same candidate as the
   empty one, even though every entry is the same default value, because `infer_plan`'s smarter guess only ever
   runs on the empty case) and that they use the SAME cost model to reach a real, measured cost gap.
5. **Extraction is a bottom-up scan over representatives, not a search, and rejects by ACCUMULATED exactness.**
   With no external ILP/SAT solver (D12) and a saturation-bounded, already-fully-enumerated candidate set, argmin
   over (program-class representative x plan-class representative) pairs is exact. Every node's `Exactness` is
   the max of every rule in its own derivation history (an E1 rule anywhere in the chain taints the whole node);
   `extract()` skips a candidate whose combined exactness exceeds the caller's request before costing it, so E0
   extraction can never select a cheaper E1 rewrite — proven with a deliberately-wrong-for-its-own-fixture but
   cheap synthetic E1 rule (`Extract.E0ExtractionNeverSelectsAnE1RewriteEvenACheaperOne`: an `InlineIntoConsumer`
   annotation on a domain read only through a SEGMENT, never a GATHER, which `cost.hpp`'s own `folded_cost_ns`
   prices at zero for exactly that reason — a concrete stand-in for "a rewrite the cost model likes for the wrong
   reason", not a real rewrite). Ties (and re-running extraction on the same graph) are decided by ascending
   (program id, plan id), so two runs over equal inputs are bit-identical (`Extract.
   ExtractionIsDeterministicAcrossRepeatedCallsAndIndependentGraphs`).

**First experiment (PROBLEM.md §7's own ask; reported here, NOT a gate — no `bench/targets.json` entry, no
`--accept`):** `bench/optimise/egraph_m1_bench.cpp` builds two `exec::Interpreter`s over the M1 book, both with the
interpreter's own Options-driven internal plan derivation bypassed by an explicit `program.plan` (there is no
other "hard-coded" planner left to switch off since D47 — R1-R7 are still stubs) — `rewrite::planner::default_plan`
(M1's own fixed-order five-rule greedy pipeline, one specific point) against EG's extraction, saturating ONLY
those same five rules instantiated at four `lane_tile` choices at once (1, 8, 16, 32 — directly answering
`RESUME.md` §5's own M3-result note, "row-fusion's lane-tile gate ... is a cost-model decision waiting to be made,
not yet a rule") and extracted at E0 with the fingerprint's OWN fitted `cost_model.json` (D48) when present.
Measured on `d448afd70180`: the extracted plan passes `rewrite::verify_annotations` (bit-identical E0) at every
lane_tile tried and is never worse than `default_plan`'s OWN estimated cost (extraction's global minimum trivially
includes `default_plan`'s exact point in its search space) — but on REAL measured wall-clock time it does not
consistently beat the greedy default: faster at (B=1, lane_tile=1) and roughly tied at (B=64, lane_tile=8), slower
everywhere else, informational numbers only (D9: never gated, this machine only). **Read honestly, not softened**:
this is consistent with, and does not contradict, D48 point 5's own reported cost-model accuracy — 84.4% mean
relative error overall, 69.3% restricted to domains that are a material share of their config's time, "NOT MET"
against the package's own <25% target — an extraction search is only as good as the cost it argmins over, and the
model it is handed here has not been shown accurate enough yet to reliably beat a plan a person already hand-
verified against the M1 kill test. The framework itself (saturate -> extract -> verify) is doing exactly its job:
finding every reachable combination, costing each one consistently, and never selecting anything that fails E0 —
closing the gap is D48's own follow-up (a live `ir::PlanAnnotations`-aware `infer_plan`, point 4 above; the two
workloads' separated coefficients; sub-microsecond domains dropped from the fit target), not a defect this package
introduces or can fix by searching harder over the same numbers.

Verification (fingerprint d448afd70180): `ctest --preset release` and `--preset reference` both 89/89 (85 + this
package's own `optimise_egraph_test`, `optimise_extract_test`, `optimise_plan_bridge_test`,
`optimise_egraph_m1_extract_test`); `scripts/mutation_test.sh` 20/20 mutants caught against the unchanged gate set
— this package registers no new mutant of its own, the same precedent D48 set (point 5, CLAUDE.md's E0/E1
exactness classes are for rewrites that change what a tape computes; extraction only ever selects among candidates
R0's own rules already produced and R0's own verifier already covers, per rule.hpp's exactness contract, so a
mutant of the SEARCH has nothing new to catch that a mutant of a real rule does not already exercise). No
SwapEngine file opened.

## D50 — M4/R-c: R6 as a lane-tile-independent, stricter subset of planner.inline_producers; R7 as a real domain split, not an annotation (2026-09-24)
Implements `PROBLEM.md` §7 / `RESUME.md` §3's R-c package: R6 (`include/epykos/rewrite/r6_materialise_boundaries.hpp`/
`.cpp`) and R7 (`r7_block_linmap.hpp`/`.cpp`), both E0, both filling in the `IdentityStubRule` D47 left for this
package. Four choices, one scoped-out finding:
1. **R6 does not, and structurally cannot, override `planner.reduction_fusion` / `planner.inline_producers`'s own
   decision for a domain those rules already touched.** `rewrite::merge_annotations` (D47) copies `src.domain[d]`
   onto the running plan only when it is not the default-constructed `DomainPlan{}` — and `Materialise::Materialize`
   IS that default, so a rule proposing "definitely materialise this domain" is indistinguishable, at merge time,
   from "this rule has nothing to say about this domain." R6 therefore never tries to force Materialize (the
   annotation vocabulary has no way to say it that survives a merge); instead its own OWN candidates are exactly
   `planner.inline_producers`'s InlineIntoConsumer shape, decided by the SAME structural facts
   (`planner::analyze_inline`, reused directly rather than re-derived) but WITHOUT `row_fusion_pays(lane_tile)`'s
   gate and with a strict (default 1.0, not 1.25) per-row reference cap — the "would always be safe to inline, no
   lane_tile to weigh it against" subset PROBLEM.md §7 asks a catalogue-facing rule for. Two extra exclusions
   `planner.inline_producers` does not need (it only ever runs from an `exec::Interpreter`/`adjoint::Adjoint`
   context that already has other guards): a domain whose OWN group is a whole-domain Sum/Affine reduction never
   inlines itself into a consumer (R0's rule allows this under extra conditions this package does not replicate:
   see the code comment for why), and an Input domain never does either (not "an intermediate": D8's "materialise
   only at domain boundaries" reading of `DESIGN.md` §6's own table). Both were found by this package's own
   differential gate failing on the M1 book and on a hand-built two-domain program during development, not by
   inspection — recorded so a future reader does not have to rediscover them by the same route.
2. **R7 is a genuine structural rewrite (`Proposal::program`), not an annotation**, because a linmap's block
   structure has no slot in `ir::PlanAnnotations` to live in (unlike R6's decision, which fits the SAME vocabulary
   R0 already shipped). Splitting domain `d` into `k` blocks preserves every value id exactly (DESIGN.md §6 "per-
   row order unchanged": block 0 keeps `d`'s own `value_base`; block `i` starts where block `i-1`'s rows end) so
   NO gather or segment anywhere else in the program needs remapping — only bookkeeping that names a domain by
   its position (`Group::domain`, `Column`/`Gather`/`Segment`/`Scan::domain`, `Domain::reads`) needs fixing up,
   done by a blanket positional reset for the first and a from-scratch recomputation (mirroring
   `src/ir/signature.cpp`'s own derivation) for the second, rather than tracking an id-remap by hand through the
   whole program — correct regardless of how many of the new blocks a downstream reader's gathers turn out to
   span, which a hand-rolled remap of a single `reads` entry would not be. The block boundary itself is column-
   span-only (HARD RULE 9): a run of rows is one block while each new row's referenced span stays inside the
   running block's `[lo, hi)`; a row whose entire span lies beyond it starts a new one. `apply_greedy` applies at
   most one structural site per call (D47 / `greedy.hpp`'s own doc), so splitting every block of one linmap
   domain is the caller's loop, not a single `propose()` call — exercised by
   `tests/rewrite/r7_block_linmap_e0_test.cpp`'s fixed-point test.
3. **Both rules are keyed on IR structure alone** (HARD RULE 9): R6's fan-out/refs-per-row threshold and R7's
   `min_block_rows` are rule parameters with library defaults, never a curve count or a trade count; neither file
   mentions Stage A or the M1 book outside a comment.
4. **Measured, not assumed** (a hard rule of this package's brief: "a rule that never fires is a finding to report,
   not to hide", generalised here to "an untested guess about where a rule fires is a finding to fix, not to
   leave in a comment"): R6 finds ZERO candidates on the M1 book (every intermediate is itself a reduction, a
   reduction's direct member, or read far more than once per row by its one consumer — DF's `forwards` domain,
   reused ~6.5x per row by the float coupons sharing one curve's forward grid) and 3 domains (16,574 rows) into 3
   distinct consumers on the full Stage A tape (2,000 trades). R7 fires on BOTH: the M1 book's one-curve linmap
   domain still splits in two (a single outlier row — D22's "t = 0" — plus the other 2,560 advancing rows), and
   the Stage A tape's one linmap domain (four curves sharing the single Input domain, `ir/program.hpp`) splits
   into 5 (measured on the full tape: rows `[7456, 7668, 48, 3, 1742]`). Both rules' verifier gates
   (`rewrite::verify_rule` against the real rule on both fixtures) and mutation gates
   (`r6.ignore_reduction_boundary`, `r6.ignore_fanout_boundary`, `r7.no_offset_rebase`,
   `r7.wrong_block_value_base`; `docs/WORKLOADS.md` §M2) pass; `r6.ignore_fanout_boundary` needed a hand-built
   near-miss program (the same convention as the M2 adjoint mutants' near-miss shapes) because neither real
   fixture happens to contain a single-use-per-row producer read by more than one distinct consumer — every
   producer read by several consumers in both is ALSO read more than once per row by at least one of them, so it
   already fails a different check first.
5. **Scoped out, deliberately** (a finding, not an oversight — see `r7_block_linmap.hpp`'s own header): DESIGN.md
   §6's R7 entry also names exposing a linmap's Jacobian as a closed form (`AdMode::ClosedFormAffine`,
   `ir::PlanAnnotations::JacobianBlockPlan`) for the calibration IFT to pick up. D47 is explicit that this slot
   has NO CONSUMER YET (nothing in `exec::`, `adjoint::` or `solver::` reads `jacobian.mode`), so a rule
   populating it cannot be checked by CLAUDE.md's own gates (a differential/round-trip/adjoint comparison is
   blind to an annotation nothing consumes) and a mutant of that logic could not be caught by them either —
   exactly what "every rewrite ships with its differential test AND its mutation test" exists to keep out of the
   tree. `is_linmap_domain` (every R7 block is one) is the fact left for whichever package adds a consumer of
   `jacobian.mode` to gate it with — D49's own EG-core landed in parallel with this package and does not touch it
   either (its own point 1: the AD-mode-per-Jacobian-block rule is explicitly a later package's scope).

Verification (fingerprint d448afd70180, rebased onto D49/EG-core): `ctest --preset release` and `--preset reference`
both green, 91/91 (D49's 89 plus this package's two new `rewrite_r6_materialise_boundaries_e0_test` /
`rewrite_r7_block_linmap_e0_test` executables; the one `rewrite_rule_framework_test` this package edited to stop
asserting R6/R7 are still stubs is not a new executable); `scripts/mutation_test.sh` green, 24/24 mutants caught
against a 43-gate set (20 unchanged from before this package plus the four named in point 4; EG-core registers no
mutant of its own, D49's own point 5). No SwapEngine file opened.
