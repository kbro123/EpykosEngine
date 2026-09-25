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

## D51 — M4/R-b: R4a/R4b/R5 and an fma-contraction rule; a shared structural-surgery primitive; three of the four rules do not fire on the current fixtures, measured and explained rather than hidden (2026-09-24)
Implements `PROBLEM.md` §7 / `RESUME.md`'s M4/R-b package (`DESIGN.md` §6's R4a/R4b/R5, plus the
package's own second rewrite named directly in `PROBLEM.md` §7's text, fma-contraction) against
M4/R0's `rewrite::Rule` interface (D47).

1. **A shared structural-surgery primitive** (`include/epykos/rewrite/ir_edit.hpp`,
   `rewrite::detail`), because R4a, R4b and R5 all need the SAME bookkeeping `ir::Program`'s
   contiguous value space demands whenever a domain is inserted or removed (every value id, every
   gather/segment table's `.domain` field and every `Domain::reads` entry at or after the change
   point shifts) and none should reimplement it independently: `insert_domain_after` (a pure
   insertion -- R4a's relocated unary op, R4b's shared reciprocal -- appends new gather/segment
   tables and shifts everything after the insertion point) and `merge_producer_into_consumer` (R5
   -- splices a single-reader producer's whole step sequence onto the front of its one reader's
   group, drops the now-orphaned gather that used to cross the domain boundary, and removes the
   producer). Tested on its own, on hand-built programs, before any rule was built on it
   (`tests/rewrite/ir_edit_test.cpp`). Two real bugs caught during this package's own development,
   both instructive enough to leave in the file's own comments: `merge_producer_into_consumer`'s
   domain remap first mapped the removed producer's tables onto the CONSUMER's PRE-shift id
   instead of its own already-shifted final one (`bad_alloc` from a downstream size computed off
   the wrong domain, caught by `ir_edit_test.cpp` before any rule existed); and the same function
   forgot that a scan's `Scan::carry_gather` is a position into the shared `gathers` vector like
   any `Slot{Gather,*}` and must be renumbered around the one dropped entry exactly the same way --
   found by `r5_group_formation_e0_test.cpp`'s Stage A gate (a scan positioned after a merged pair
   silently read the wrong table entry; `ir::validate` caught it immediately, "the carry does not
   read the previous row of its chain"). Landed after D50/M4/R-c (which reworked the same shared
   `tests/rewrite/rule_framework_test.cpp` for R6/R7's own transition off `IdentityStubRule`):
   rebased onto it, merging both packages' edits into one "R1-R3 are the only remaining stubs" test
   plus each package's own "these are real rules now" test, no code conflict (D50 touches R6/R7's
   own files, disjoint from this package's).
2. **R4a** (`push_unary_through_gathers`, E0) and **R4b** (`shared_reciprocal`, E1) both relocate a
   step's operand across a gather boundary into a new, shared domain and rewrite every matching
   site in place as `gather(new_domain) * 1.0` (R4a: the exact IEEE identity, so pushing `Exp` /
   `Log` / `Sqrt` / `Neg` out of the reading domain and back through the gather to `S` stays
   bit-identical without ever having to eliminate the reading domain) or `a * gather(S_recip)`
   (R4b: `a/b -> a*recip(b)`, a real rounding-order change, D26 4-ulp tolerance). Both require the
   gather to be homogeneous (every row from one source domain -- a gather mixing several source
   domains, program.hpp's own allowance, is not this rewrite's target: there is no single smaller
   domain to relocate onto) and, to avoid introducing a no-op multiply where it buys nothing,
   require a measured profitability signal (R4a: the gather reuses at least one source row; R4b:
   that OR at least one other site shares the same source, the reciprocal genuinely shared).
3. **R5** (`group_formation`, E0) merges a single-reader elementwise producer into its one
   consumer when the consumer itself feeds a Segment (program.hpp: every Segment exists only to
   serve a `Sum`/`Affine` step, so "consumer is read by any segment" already means "consumer feeds
   a segment-sum epilogue", DESIGN.md §6's own phrase) -- one pairwise merge per application;
   re-matching after each one (a caller's own fixpoint loop, `rewrite::apply_greedy` run
   repeatedly) is what makes a chain of more than two domains collapse, not a single match() call.
4. **An fma-contraction rule** (`include/epykos/rewrite/fma_contraction.hpp`, not one of R1-R7 --
   `PROBLEM.md` §7's own second M4/R-b rewrite, "an fma-contraction rule (E1) for a*b+c chains
   where the recorded order permits one rounding change"), purely local to one group: a `Mul` step
   read by exactly one later, fixed-arity, 2-member `Sum` step (fold_sum's own shape for a
   recorded 2-term `+`, DESIGN.md §5.2/§5.6 -- `Op::Add` itself never survives into the domain IR,
   fold_sum turns every `Add` into a `Sum`) in the SAME group folds into one `Op::Fma` step,
   removing the `Mul` and renumbering the group's own Step-slot indices; no domain, gather or
   segment table is touched.
5. **Measured on the Stage A tape (fingerprint d448afd70180) and the M1 book, per rule, per this
   package's own instruction ("a rule that never fires is a finding to report, not to hide"):**
   only R5 fires on either fixture. R5: 3 producer/consumer pairs on Stage A (67 -> 64 domains,
   423,235 -> 406,661 recorded values, the largest pair 15,358 rows), 0 on the M1 book (its
   domains are already fused by hand, DESIGN.md §1). R4a: 0 on both. The two candidate sites that
   exist on Stage A (a `Neg` and the interpolated-DF `Exp` of DESIGN.md §5's own worked example)
   are BOTH already exactly as small as their source domain lets them be -- `stage_a.hpp`'s own
   record-time discount-factor memo ("a discount factor is recorded once per distinct time rather
   than once per coupon that reads it") already does, at record time, what this rule would do
   structurally, for exactly these sites; the `Neg` reads a domain LARGER than its own (148 raw
   inputs vs 70 quote rows), where relocating would make things worse and the profitability guard
   correctly declines. R4b: 0 on both -- every `Div`-by-gather site on Stage A (3 of them) reads
   from a gather whose rows come from MORE than one source domain (a per-slot "whichever curve
   applies" lookup), which the homogeneous-source requirement correctly excludes; there is no
   `Div`-by-a-single-gathered-domain shape recorded anywhere in either fixture. Fma-contraction: 0
   on both -- measured directly (zero fixed-arity 2-member `Sum` steps anywhere in the Stage A tape
   have a `Mul` as either member): the natural target, the compounding scan's own "1.0 + r·τ" step
   DESIGN.md §5.6 names, records as one `Op::Affine` node instead, because `τ` (a day-count
   fraction) is a compile-time constant and `affine_collapse` (DESIGN.md §5.2) already turns
   "constant + constant·variable" into one node before the signature pass runs; there is no
   separate `Mul` step left for this rule to find. All four rules are exercised on hand-built
   programs that DO carry their exact target shape (`tests/rewrite/*_test.cpp`, one per rule) to
   prove the mechanism itself is correct, bit-identical or within tolerance against the unfused
   program, and reaches a fixpoint (`match()` empty after one application).
6. **Mutant naming: single-dot, not `rewrite::Rule`'s own doc-comment suggestion.** `rule.hpp`'s
   file header suggests a mutant name of the form `"rewrite." + name() + ".<defect>"`; since a
   rule's own `name()` is already `"<pass>.<rule>"` (one dot), that would be three dots, and
   `tests/mutation/registry_test.cpp`'s own well-formedness check (`^[a-z][a-z0-9_]*\.[a-z][a-z0-9_]*$`,
   D32) accepts exactly one. This package's 8 mutants (`r4a.wrong_literal`, `r4a.wrong_row_map`,
   `r4b.wrong_op`, `r4b.wrong_row_map`, `r5.wrong_step_index`, `r5.drop_last_step`,
   `fma.wrong_operand`, `fma.drop_remap`; `include/epykos/mutation/mutation.hpp`,
   `docs/WORKLOADS.md` §M2) use the enforced single-dot form instead -- the actual regex, not the
   header comment's prose, governs. Each is caught by that rule's own `rewrite::verify_rule` gate
   (a `tests/rewrite/*_e0_test.cpp` or `*_verify_test.cpp`, matched by `scripts/mutation_test.sh`'s
   gate regex through the `verify`/`_e0_test$` alternatives), on the hand-built program that
   exercises the rule's real target shape (R4a/R4b/fma) or the Stage A tape directly (R5, the one
   rule that fires there).
7. **A `StageATape::record_quotes()` pitfall, found and fixed in this package's own test
   development, worth naming so the next package does not repeat it:** it returns only the 70
   calibration quotes, a SUBSET of the tape's 148 recorded free inputs (the other 78 are realised
   fixings recorded alongside them); `exec::Interpreter::run` / `adjoint::Adjoint::run` read
   exactly `program.inputs.size()` entries from the `state` pointer and allocate nothing to check
   the length. Passing `record_quotes()`'s 70-entry vector where 148 were needed reads/writes past
   the end of that shorter buffer -- silently for a forward run (undefined, not necessarily wrong-
   looking, values past the end), and as a delayed heap-corruption `SIGABRT` well after the last
   out-of-bounds adjoint write, in this package's own `r5_group_formation_e0_test.cpp` during
   development. `program.input_values` (or `tape.tape.input_values()`) is the correct full state
   vector, `Program::inputs` order, for exactly this use.

No SwapEngine file opened. Verification (fingerprint d448afd70180): `ctest --preset release` and
`--preset reference` both green, 96/96 (D50's own 91 plus this package's own 5 new executables --
`rewrite_ir_edit_test`, `rewrite_fma_contraction_verify_test`, `rewrite_r4a_push_unary_e0_test`,
`rewrite_r4b_shared_reciprocal_verify_test`, `rewrite_r5_group_formation_e0_test` -- plus two shared
ones edited in place rather than added, `rewrite_rule_framework_test` and `mutation_registry_test`);
`scripts/mutation_test.sh` green, all 32 registered mutants caught (D50's own 24 plus this
package's own 8; see `docs/RESUME.md` §5's landing entry for the exact run).
## D52 — M4/R-a: R1 fold uniform columns, R2 bucket rows, R3 elide trivial maps (2026-09-24)
Implements DESIGN.md §6's R1-R3 against the M4/R0 Rule interface (D47): `rewrite::R1FoldUniformColumns`,
`R2BucketRows`, `R3ElideTrivialMaps` (`include/epykos/rewrite/r{1,2,3}_*.hpp`, `src/rewrite/r{1,2,3}_*.cpp`), a
shared low-level editing helper (`rewrite::detail`, `include/epykos/rewrite/bucket_split_edit.hpp`,
`src/rewrite/bucket_split_edit.cpp`: `all_bit_identical`, `drop_owned_entries`, `shift_domain_ids`,
`eliminate_domain` -- named apart from M4/R-b's own `ir_edit.hpp`, which this file's `.cpp` includes for its
`recompute_reads` rather than duplicating it: both packages independently built a domain-editing helper of that
name, from the same base, for their own rules, and landed within hours of each other), six new
mutants (`r1.ignores_last_row`, `r1.wrong_slot`, `r2.wrong_run_boundary`, `r2.column_slice_uses_wrong_bucket`,
`r3.off_by_one_member`, `r3.treats_length_two_as_trivial`; `docs/WORKLOADS.md` §M2), and per-rule differential
tests (`tests/rewrite/r{1,2,3}_*_e0_test.cpp`). All three E0. Findings, reported per CLAUDE.md rather than hidden:

1. **R1 never fires on a Program straight out of `ir::infer`.** DESIGN.md §5.5's own column classification
   ("identical across instances -> literal (folded); varying -> data column") is already performed by the
   signature pass itself (`src/ir/signature.cpp`'s `const_slot`: bit-pattern uniform -> Literal, else Column) --
   so no domain the signature pass produces can have a uniform Column left for R1 to fold. Measured: 0 of 10 M1
   book domains, 0 of 67 Stage A domains. R1 is not dead code: it exists for a Program a REWRITE has since
   changed underneath the signature pass's own classification, concretely R2 (below), whose own point is that a
   freshly-split bucket domain's signature columns become uniform by construction, ready for a LATER R1 pass.
   R1's own synthetic test (a hand-built domain with one uniform and one non-uniform column) exercises the fold
   directly and both mutants.
2. **R2 buckets by MAXIMAL CONTIGUOUS RUNS of matching signature, never a sort.** A sort-based partition (group
   ALL same-signature rows together regardless of original position) would need every downstream gather/segment
   that reads one of the domain's rows to be renumbered, program-wide, to the row's new position -- a much bigger
   change than DESIGN.md's own framing of R2 as row-local bucketing. A contiguous-run split needs none of that:
   the value-id space stays exactly as it was (`Program::domains` just gets more, smaller entries covering the
   same range), so only DOMAIN ids shift, a far smaller edit (`shift_domain_ids`). The consequence, reported
   rather than hidden: R2 fires only where the ORIGINAL RECORDING ORDER already groups same-signature rows
   contiguously, which is not guaranteed and this landing does not sort to arrange.
3. **R2's OWN entries are expanded IN PLACE in `Program::columns` / `gathers` / `segments`, never compacted away
   and appended at the tail.** The first implementation did the latter (simpler) and passed every forward and
   round-trip gate, but FAILED the adjoint gate by 36-68 ulps on 2 of the M1 book's 5 matching domains despite
   bit-identical forward values: `adjoint::build_plan` (`src/adjoint/plan.cpp`) builds each value's reader list
   by scanning `Program::gathers` / `segments` in ARRAY ORDER, so a value read by both one of a split domain's
   gathers and an unrelated one accumulates its adjoint in that scan order; moving the split domain's entries to
   the tail reorders that floating-point sum (same total, different bits). `expand_owned` (`r2_bucket_rows.cpp`)
   replaces each owned entry AT ITS OWN ARRAY POSITION with its bucket's entries instead, which fixed the
   mismatch (re-verified: 0 ulps, all 5 M1 domains, forward and adjoint, at the record point and over the M1
   ball) -- this is the finding future rewrites that add or split Column/Gather/Segment entries should know
   about, not just R2's own bookkeeping.
4. **R2 additionally rejects any split with a singleton run, as a SAFETY gate, not only a quality one.** Measured
   directly: splitting a 177-row Stage A domain into 168 buckets (159 of them singletons) crashes building the
   split program's `adjoint::Adjoint` (a heap-corruption-shaped abort some distance past the actual fault, inside
   `src/adjoint/`, code this package does not own) -- yet the SAME shape (a 16,103-row M1 book domain into 9,152
   mostly-singleton buckets) builds and runs correctly, bit-exact forward and adjoint, at the record point and
   over the differential ball. The M1 book has no scan domain; the Stage A tape has five; this package's own time
   budget did not extend to chasing that difference into `src/adjoint/` to confirm it as the actual cause rather
   than something else Stage-A-scale-specific. Rather than land a rule that can crash, `run_buckets` closes the
   whole shape off structurally (every bucket must have at least 2 rows) -- which means R2 does not currently
   fire on EITHER real fixture (0 of 10 M1 domains, 0 of 67 Stage A domains), only on its own synthetic test. A
   follow-up owning `src/adjoint/` should chase the crash (reproduction: `r2_bucket_rows.cpp`'s own comment on
   the gate) and, if it is fixed, this gate can be relaxed back to "no consolidation at all"
   (`ranges.size() >= rows`), which is verified sufficient for correctness by itself.
5. **R3 also never fires on either real fixture (0 of 10 M1, 0 of 67 Stage A) at this landing.** Its target (a
   domain whose entire group is a length-1 Sum over a segment: a pure relabelling with no arithmetic) is a
   boundary artifact DESIGN.md §5.3 predicts CAN arise (fan-out > 1 giving a trivial value its own domain) but
   evidently does not on these two fixtures as recorded; R3's own synthetic tests (a length-1-Sum domain feeding
   a third domain, and a genuine two-member Sum) exercise the fold and both mutants directly. Scope note: only a
   length-1 SUM is treated as trivial, never a length-1 Affine -- `affine_collapse` (`src/tape/passes.cpp`,
   mutant `affine.single_term_unscaled`) already reduces a genuinely-identity one-term Affine (coefficient 1,
   offset 0) to a bare atom before the domain IR exists, so a length-1 Affine surviving into a Program is, by
   construction, never trivial.
6. **Verification.** Every site a rule's `match` reports on a real fixture is checked with
   `rewrite::verify_rule` (interpreter + adjoint, bit-identical) against the untouched original, EXCEPT the
   Stage A tape's own ball-based checks: its Inputs are quotes an implicit block (D40) calibrates the book's
   knots from, so a plain `exec::Interpreter`/`verify::make_state_ball` perturbation (valid for the M1 book,
   which has no implicit block) walks the book off the point the recorded tape is self-consistent at and,
   measured directly, sometimes into `log`/`sqrt`/`div`'s undefined region entirely. `tests/rewrite/
   record_point_check.hpp` checks the exact record point only (self-consistent for a plain Interpreter/Adjoint
   exactly as it is for `solver::ImplicitProgram`) instead, for every rule's Stage A test.

## D53 — A package's gate must state whether its deliverable actually fires (2026-09-24)
An independent audit found that M4/R-a's own gate ("E0/E1; mutants caught") was narrower than its own deliverable
text ("fired on the Stage A tape"): R1, R2 and R3 are correct and fully tested but fire on zero domains on both the
M1 book and the Stage A tape, and nothing in the gate column's own wording would have caught that as a finding if
R-a's own agent had not chosen to disclose it in prose. It did disclose it, honestly and in detail (D52), so this is
a process-design gap, not an actual false claim anywhere in the record. Closing the gap for future packages: every
`docs/RESUME.md` §3 gate that lists "fired on X" (or the equivalent) as part of its own deliverable text must state
the fire-count as an explicit number in its landing report, zero included; a package whose deliverable claims to
fire on the real fixture and cannot state that number has not met its gate. This applies from M4/R-b, R-c, EG, C1
onward and to the M4 gate/review packages reading this table. R1–R3's own already-correct, already-disclosed
zero-fire finding stands as recorded in D52 and is not reopened by this entry.

## D54 — M4/EG integration: AD-mode-per-block rule + its required consumer, a cross-stage-sharing extraction guard, the four PROBLEM.md §7 experiments, and one significant scaling finding (2026-09-24)

Registers R1-R7, `fma_contraction` and the five planner rules together in one e-graph (D47-D52 landed each
separately; this package is the first to run them all at once) and adds the two rule families PROBLEM.md §7
names as M4/EG's own scope beyond EG-core (D49): AD mode per Jacobian block, and cross-stage sharing.
`include/epykos/rewrite/ad_mode_rule.hpp`/`.cpp`, `include/epykos/adjoint/block_jacobian.hpp`/`.cpp`,
`include/epykos/rewrite/cross_stage_sharing.hpp`/`.cpp`, an additive extension of
`include/epykos/optimise/extract.hpp` (`ExtractOptions::reject`, `ExtractResult::candidates_rejected_guard`,
default `nullptr` / `0`: no existing caller's result changes), and four experiment files under
`tests/optimise/` and `bench/optimise/` (`docs/DESIGN.md` §7's own "As built (M4/EG integration, D54)"
paragraph has the full technical description; this entry adds the findings and their evidence).

1. **`jacobian.mode` needed a REAL consumer before a rule populating it meant anything, exactly as R7's own
   header and D50 point 5 said.** `rewrite::ADModePerBlockRule::propose` prices Forward / Reverse /
   ClosedFormAffine by `optimise::estimate_jacobian_ns` and writes the cheapest into `jacobian.mode[name]` —
   but nothing checked that decision until `adjoint::block_jacobian` existed to actually COMPUTE a Jacobian
   the named way. Three new mutants, each verified caught directly (the mutation-preset binary run with
   `EPYKOS_MUTANT` set, exit 1, 2026-09-24 — not merely reasoned about): `eg.ad_mode_ignores_cost` (always
   Reverse) is caught by a synthetic block where forward must win on cost
   (`ADModeRule.PicksTheCheapestModePerBlock`); `eg.ad_mode_affine_without_check` (skips the
   `is_linmap_domain` eligibility check) is caught by that SAME test's own
   `EXPECT_FALSE(nonlinear_decision.affine_eligible)` on a nonlinear fixture — `block_jacobian`'s own
   INDEPENDENT re-check of `is_linmap_domain` (never trusting the rule's eligibility flag) is still real
   defense in depth, exercised separately by `BlockJacobian.ClosedFormAffineRefusesANonAffineBlock`, but
   an earlier draft of this entry wrongly credited THAT test with catching this specific mutant, which it
   does not (it forces the mode directly, bypassing `decide_ad_mode` entirely) — corrected here rather than
   left inaccurate; `eg.block_jacobian_wrong_coefficient_index` guards the closed-form path's own arithmetic
   separately, caught by `BlockJacobian.ClosedFormAffineMatchesReverseExactlyOnALinmapBlock`.
2. **ClosedFormAffine is deliberately narrower than R7's own general linmap case.** A linmap's members can be
   Program Inputs directly (the common case this package handles exactly, no evaluation needed at all: a
   linmap's weights ARE its Jacobian) or another domain's computed values (would need THAT domain's own
   Jacobian chained in first). `block_jacobian` throws `std::invalid_argument` on the second case rather than
   silently truncating the chain — a stated scope limit, not a bug budget spent elsewhere.
3. **Cross-stage sharing is a REJECT, not a rule**, because rules only ever ADD e-graph candidates (D47's own
   "produce the alternative without discarding the original") — nothing else in the framework can say no to
   one once proposed. `rewrite::cross_stage_sharing_guard(groups, op)` wraps `ir::sharing`/`assert_all_shared`
   (M3/G4's own gate machinery, unchanged) as the new `ExtractOptions::reject` predicate; a synthetic
   split-vs-shared pair (`tests/optimise/extract_cross_stage_sharing_test.cpp`) proves it changes extraction's
   actual OUTCOME (the split candidate, though a real e-graph candidate and cheaper-or-not on its own terms,
   is never extractable once the guard is set) rather than merely existing unused. `output_groups` are ordinal
   lists, fixed once against the ROOT program and reused unchanged against every rewritten candidate — R1-R7's
   own contract (structural rewrites renumber domains and values but never reorder or drop an output ordinal)
   is what makes that safe, stated explicitly in the header rather than assumed.
4. **Four experiments, `bench/results/d448afd70180/m4_experiments.json`:**
   - **(1) REDISCOVERY** (M1 book, full rule set, extract at E0 vs the M1 greedy default): ties the default at
     the COST MODEL's own estimate (ratio 1.0 — extraction can never do worse than a point already in its own
     search space) but MISSES the 1.02x measured-wall-clock target: 1.104x at B=1, 1.081x at B=64
     (`bench/results/d448afd70180/optimise_egraph_full_rules_m1.json`). Root cause, traced and reported rather
     than left as a mystery: the extracted plan's `PlanAnnotations::group` is EMPTY (0 step-pairings) against
     the default's 5, because `optimise::plan_bridge.hpp`'s own header (pre-existing, D48 point 6) already
     documents that `group`/`emitted` "have no effect on the price this file produces" — so a cost-tied
     candidate lacking fused-pairs/chain-tails is indistinguishable from the fully-fused one at extraction
     time, and the deterministic ascending-id tie-break happens to keep the shorter (unfused) history. This is
     a real gap in the ALREADY-LANDED cost model, not a defect this package introduced; closing it (pricing
     `group`/`emitted`, D48 point 6's own follow-up list) is left for whoever picks it up next.
   - **(2) CROSS-STAGE** (a small, 60-trade Stage A fixture — the full 2,000-trade tape was not attempted, see
     below): found ONE real, verified win the fixed single-pass pipeline cannot express —
     `r5.group_formation` applied twice in sequence (once onto its own prior output) plus
     `planner.reduction_fusion`, 0.977x the M1-shaped default's own estimated cost, bit-identical to the
     unannotated program at the tape's exact record point (a perturbation ball is the WRONG check on an
     implicit-node tape's quotes, `tests/rewrite/record_point_check.hpp`'s own established reason — this
     package's own first attempt used a ball, saw a "ref nan" differential failure, and correctly traced it to
     that, not to a real defect in `r5.group_formation`, before landing the record-point version). SIGNIFICANT
     NEGATIVE FINDING: saturating this SAME 60-trade fixture to the bound `egraph_full_rules_m1_test.cpp` uses
     for the M1 book (24 iterations, 2000 program nodes) does not converge — round 2 alone (five structural
     rules firing at once: `r1`, `r2`, `r5`, `r6`, `r7`, vs zero-to-one on the M1 book, D49-D52's own numbers)
     already produces 39 distinct program nodes in 4.4s; by round 4, measured directly, the process held 1.87
     GB resident and was still climbing, with no sign of a fixpoint (killed rather than let it continue). Root
     cause as far as this package traced it without adding a fix of its own: `EGraph::saturate` matches every
     rule against every node discovered so far, every round (`egraph.hpp`'s own documented BFS shape) — a
     design that is fine when structural rules rarely fire or fire once, but has no redundancy/subsumption
     check (D12: no external e-graph library) to stop the candidate set growing combinatorially once several
     fire at once and can re-match each other's own output. The full 517,036-node Stage A tape was NOT
     attempted given this trajectory — extrapolating from the 60-trade fixture's own growth, it would very
     plausibly exhaust memory before converging. This package's own `rewrite::cross_stage_sharing_guard` was
     NOT wired onto Stage A's real residual/book output-ordinal groups (deriving those exactly needs the O1/O2
     layout the G5/G6 gate tests' own helpers compute, not extracted into reusable form in the time available)
     and `ADModePerBlockRule` was not run against Stage A's live `ImplicitRegistry` — see (3).
   - **(3) AD MODE**: fed `optimise::estimate_jacobian_ns` the REAL numbers already committed for this
     fingerprint (`bench/results/d448afd70180/stage_a_stage_a.json`, commit `cc23459`, M3/G6's own baseline)
     rather than re-recording Stage A. Book-only (n_outputs=1): the rule picks Reverse under both the
     M1-calibrated default multiplier (8.0) and Stage A's OWN measured one (32.6ms / 1.53ms = 21.33x — a
     materially different ratio from the M1 book's, reported rather than silently reused across fixtures, D9).
     Full ladder (n_outputs=2,043 = 2,000 trades + book + 42 aggregates): the rule picks Forward under both,
     matching RESUME.md's own already-reported 6.2-6.3x fact. Two cost-model gaps found and stated, both
     biasing the SAME direction (making forward look closer to reverse than reality, never the other way):
     reverse's `one_pass_ns × adjoint_multiplier × n_outputs` is linear and cannot see Stage A's real B=64
     batching / chord-policy Jacobian sharing (overstates reverse's true cost at scale by ~7.6x); forward's
     `one_pass_ns × n_inputs`, fed a PLAIN double interpreter's `one_pass_ns`, understates the real `Dual<70>`
     forward ladder (one wide pass, itself O(70) per op, plus the Newton solve) by ~12.8x.
   - **(4) E1 EXTRACTION** (M1 sub-book, `exp_poly`, informational vs `bench/hand`'s v0): extraction at E1
     (making `fma_contraction` / `R4bSharedReciprocal` eligible) reaches the SAME plan as (1)'s E0 result on
     this fixture; passes `verify::Tolerance::e1()`. Measured 1.188x / 1.615x against the already-committed
     hand-v0 numbers (`bench/results/d448afd70180/hand_m1_hand.json`) — the same root cause as (1).
5. **Report honestly (CLAUDE.md, HARD RULE 10): this package's own first attempt at experiment (2) reported a
   FALSE positive defect.** Comparing the extracted Stage A candidate with `rewrite::verify_annotations`'s
   default ball-based options produced "differential E0: ... ref nan vs -0.0207 ... FAIL", which briefly read
   as a real correctness bug in doubly-applied `r5.group_formation`. It was not: perturbing an implicit-node
   tape's quotes without re-solving the calibration (exactly the failure mode `tests/rewrite/
   record_point_check.hpp` was written to avoid, D51/D52's own precedent) walks the book off its calibrated
   point and into `log`/`sqrt`'s undefined region regardless of which rewrite is being checked. Switching to
   `compare_at_record_point` (no ball, the tape's own record-point `input_values`) made both tests pass. Kept
   here as the record of a wrong intermediate conclusion corrected before landing, not smoothed away.

Not reached, stated rather than silently dropped (RESUME.md §3's own EG-row items): sharing a scenario lane's
factored calibration Jacobian across lanes whose select masks agree, and choosing the IFT product order
(materialise `F_z^{-T} z̄` once vs re-solve per output) by cost. Both are real solver::-level changes
(`ImplicitProgram`'s per-lane loop, `solver::Factors`) this package judged too risky to land without the time
to verify them as thoroughly as the rest of this entry — landing a subtly wrong optimisation into
correctness-critical, already-gated numerical code is a worse outcome than not landing it at all.

Verification (fingerprint d448afd70180): `ctest --preset release` and `--preset reference` both green (see this
package's own landing report for the exact count); `scripts/mutation_test.sh` green, every registered mutant
caught including the three new ones. No SwapEngine file opened.

## D55 — M4/C1: the catalogue is a value-independent Signature per domain Group, one hand-portable C++ kernel per
distinct shape, dispatched from inside `exec::Interpreter`'s plain per-tile path and unconditionally from
`adjoint::Adjoint`'s forward pass (2026-09-24)
Implements DESIGN.md §7 tier 1 / PROBLEM.md §7's catalogue: `include/epykos/catalogue/` (engine API) --
`signature.hpp` (`Signature`: the op at each step plus each operand slot's KIND -- Step/Literal/Column/Gather/
Segment -- and, for a Step operand, how many steps back it points; excludes row counts, table indices and every
literal/column/gather VALUE by construction), `kernel.hpp` (`Kernel`, the ABI every generated function shares:
given a row range and lane width, write each row's final value to the shared `values` buffer exactly where the
generic path would have) and `bind_domain` (the pointers one domain's kernel call needs, one array slot per
Literal/Column/Gather OCCURRENCE in step order -- deliberately not deduplicated by table index, since a
Signature cannot see which occurrences share one, and a generated kernel and its binding must agree on array
LENGTH without either naming the other's Program-global indices), and `registry.hpp` (the generated table,
looked up by hash then verified by full `Signature` equality, so a hash collision can never dispatch the wrong
kernel). `tools/catalogue/` (codegen.hpp/.cpp, generate_main.cpp, not engine API) runs the two reference
workloads named by PROBLEM.md §7 -- the Stage A tape (`fixtures::record_stage_a` at its OWN default
`StageAOptions{}`) and the M1 book -- through `ir::infer` only (no additional rewrite pass; see finding 1 below),
collects the Signature of every catalogue-eligible domain either produces (excludes only an `Op::Input` step; a
domain "counts as hot" simply by appearing in one of the two named reference workloads, never a row-count
threshold, HARD RULE 9) and writes `src/catalogue/generated/{kernels_e0.cpp,registry.cpp}` deterministically
(sorted by hash); `scripts/catalogue_regen.sh --check` is the `git diff --exit-code` CI wiring the brief asks
for. Two new mutants (`catalogue.binding_wrong_operand_order`, `catalogue.signature_ignores_konst`;
`docs/WORKLOADS.md` §M2), caught by the catalogue's own on/off differential tests
(`tests/exec/interpreter_catalogue_e0_test.cpp`, `tests/adjoint/adjoint_catalogue_e0_test.cpp`), which also carry
the E0 differential and coverage gates below. Exactness class E0 throughout (a catalogued kernel is a literal,
row-count-generic transcription of the domain's own recorded op sequence; nothing about the arithmetic changes).

Findings, reported per CLAUDE.md rather than hidden:

1. **The generator does not run the M4/R-a..R-c structural rewrites or an EG extraction first, by choice, not
   oversight.** It walks the Program `ir::infer` itself produces -- the SAME Program an ordinary
   `exec::Interpreter(program, Options{})` / `adjoint::Adjoint(program, Options{})` construction already runs
   today, since R1-R7 and EG extraction are opt-in (a caller supplies its own rewritten/extracted Program) rather
   than what a default construction executes. Nothing about `Signature` / the registry format is specific to an
   un-rewritten Program -- a later regen against an R1-R7-rewritten or EG-extracted Program needs a different
   `record_and_infer` in `tools/catalogue/generate_main.cpp`, no change to `signature.hpp`, `kernel.hpp`,
   `registry.hpp` or the codegen itself. Deferred rather than attempted this landing given D51/D52's own measured
   finding that R1-R4/fma fire on neither real fixture today and R5/R7 change only a handful of domains -- the
   marginal catalogue coverage was judged not worth the added risk of modelling their edits incorrectly within
   this package's own time budget.
2. **`exec::Interpreter`'s own hook is deliberately narrower than `adjoint::Adjoint`'s.** The Interpreter already
   has three of its own hot-path optimisations a catalogued kernel would either duplicate badly or corrupt
   silently if not excluded: a domain `InlineIntoConsumer`'d into another domain's tiles is (by design) never
   materialised into the shared value buffer at all -- a first version of this package's hook did not exclude a
   domain that itself INLINES a producer (`inline_refs[d]` non-empty) and produced wrong numbers (tens of
   millions off) on the M1 book's own DF/exp domain the moment it was exercised by this package's own new
   differential test, caught before landing, not after; `FuseIntoReduction`'d domains are safe to gather from
   (their reader rows are always in `keep_rows`, verified against `rewrite::planner::decide_reduction_fusion`)
   but the reduction domain itself is `is_whole_segment` and excluded on that basis already; a scan domain's own
   wave-scheduled, indirect-row execution is excluded outright (`dom.recurrent`) rather than taught to the
   generated kernel's row-range ABI. `adjoint::Adjoint`'s forward pass has none of these three optimisations of
   its own (DESIGN.md §7's own words: "the reverse of a fused group is rewrite / catalogue work (M4)") -- it
   materialises every domain's rows unconditionally today, scan included (`is_scan` only changes its OWN tile
   size to 1, still ascending row order), so a catalogued kernel is a safe, unconditional replacement for ANY
   catalogue-eligible domain there, and the coverage measured below is correspondingly higher.
3. **Coverage, measured (fingerprint d448afd70180, `ctest --preset release`, `tests/exec/
   interpreter_catalogue_e0_test.cpp`'s `DefaultStageAIsFullyCatalogued` / `tests/adjoint/
   adjoint_catalogue_e0_test.cpp`'s own):** on the M1 book, `exec::Interpreter` and `adjoint::Adjoint` both
   catalogue 3-of-3 / 9-of-9 candidate domains (100%; Adjoint's larger count is exactly the whole-segment Sum/
   Affine reductions the Interpreter excludes per finding 2). On the FULL default Stage A tape (the same
   instance the generator itself ran): `exec::Interpreter` catalogues 28/28 candidate domains, 80,069/80,069
   candidate rows (100%); `adjoint::Adjoint` catalogues 66/66 candidate domains, 423,087/423,087 candidate rows
   (100% of candidates; 423,087 of the tape's 517,036 total nodes, CLAUDE.md's own Stage A count). A
   DIFFERENTLY-SIZED or DIFFERENTLY-NOISED Stage A instance (not one of the two reference workloads the registry
   was built from) reaches ~87-93% of candidate domains, not 100% -- measured and explained, not smoothed over:
   a per-netting-set PV aggregation records as a FIXED-ARITY Sum (2 or 3 operands, DESIGN.md §5.6, part of the
   Signature by design) only when that particular netting set happens to hold 2 or 3 trades; a larger one falls
   back to the row-count-independent Segment form. Which netting sets land on which side of that line depends on
   the trade-to-netting-set draw, which a different trade count or quote-noise level changes -- a real structural
   consequence of generating the catalogue from two FIXED reference runs (the brief's own scope), not a hash or
   binding defect (bitwise correctness against the non-catalogued path holds in every case tested, including
   these smaller/differently-noised ones: `InterpreterCatalogueE0.DifferentStageAInstanceHitsTheSameCatalogueAndIsMostlyCovered`).
4. **Engine coverage() is a structural (domain- and row-count) metric always; a measured time fraction only under
   `-DEPYKOS_EXEC_PROFILE`.** Both `exec::Interpreter::coverage()` and `adjoint::Adjoint::coverage()` return
   `catalogue::Coverage{groups_total, groups_catalogued, rows_total, rows_catalogued, time_fraction}`; the two
   engines each keep their OWN lightweight per-instance ns accumulator (gated by the SAME `EPYKOS_EXEC_PROFILE`
   compile option M4/CM's `profile` preset already defines) rather than reusing `exec::Interpreter`'s existing
   PROCESS-WIDE `ProfileTable` -- that table's stderr text format is a load-bearing contract of
   `scripts/exec_coverage.py` and `tools/costmodel/fit_main.cpp`, which this package neither needed to nor
   should touch. `time_fraction` reads -1.0 (never 0.0) when profiling is not compiled in or no `run()`/`run_chunk()`
   call has completed yet, so a caller cannot mistake "not measured" for "0% covered".

No SwapEngine file opened. Verification (fingerprint d448afd70180): `ctest --preset release` and `--preset
reference` both green (see `docs/RESUME.md` §5's landing entry for the exact counts); `scripts/mutation_test.sh`
green, all registered mutants caught including this package's own two; `scripts/catalogue_regen.sh --check`
a no-op against the committed `src/catalogue/generated/` (regenerating from the same two workloads and the seed
of `docs/WORKLOADS.md` reproduces it byte-for-byte).

## D56 — M4-gate (run 1): independent re-verification of M4 to date; two real, previously-unknown defects found in
already-landed M4 packages, neither fixed here (2026-09-24)

This package's own brief is measurement and report, not repair (CLAUDE.md HARD RULE 10, "report honestly"; "you do
not decide the verdict"). Full build/test/mutation re-verification on a fresh worktree, fingerprint `d448afd70180`
(unchanged from D48 onward): `ctest --preset release` 107/107, `--preset reference` 107/107, `scripts/mutation_test.sh`
43/43 mutants caught (0 survivors), `scripts/catalogue_regen.sh --check` a no-op — all as already reported by D55,
independently reproduced, not merely trusted. Full numbers, per-experiment evidence and the EG re-run (rediscovery
1.0246x/1.0431x on a quieter machine than D54's own run, cross-stage 0.9773x reproduced exactly, AD-mode reproduced
exactly) are in `docs/RESUME.md` §5's own landing entry for this package; not duplicated here.

Two findings, neither introduced by this package, neither fixed by it (out of scope: both live in other packages'
files; CLAUDE.md's "keep to your package's files"), both flagged via `spawn_task` for a follow-up session rather
than silently noted and dropped:

1. **M4/C1's catalogue coverage is compiler-dependent, breaking CI on GCC** (task `task_919ea449`). At
   `integrate/m1-m5`'s tip before this package's own commit (`4ef9632`), GitHub Actions run `35986197439` is red on
   `ubuntu-latest` (release, reference, mutation) and green on `macos-latest` — the exact D46 blind spot ("verifying
   only on Apple clang") recurring in a place D46 itself did not touch. `AdjointCatalogueE0.
   DefaultStageAIsFullyCatalogued` and two `InterpreterCatalogueE0` tests fail on GCC 13: the DEFAULT Stage A
   instance (the exact one `scripts/catalogue_regen.sh` generated the registry from) catalogues only 63/66 groups
   (95.45%) on GCC vs 66/66 (100%) on Apple clang (re-verified locally, this entry). 0 mismatches either way —
   correctness holds wherever the catalogue dispatches; this is a coverage shortfall, not a numeric bug, but it is
   severe enough to fail `scripts/mutation_test.sh`'s own baseline gate check on GCC outright (no mutant is ever
   tried). Root cause not fixed, hypothesised only (D55's own finding 3: a per-netting-set PV aggregation's
   fixed-arity-Sum signature depends on the trade-to-netting-set draw; if that draw is sensitive, even indirectly,
   to GCC's cross-TU FMA contraction — the D25/D46 class of defect — the SAME seed could deterministically produce a
   different netting-set grouping under GCC than under clang for the SAME default instance).
2. **M4/C1's catalogue silently disables `exec::Options::exp = ExpMode::poly`** (task `task_f7b9c87d`). Re-running
   `bench/optimise/egraph_e1_extract_m1_bench` (the M4/EG "M1 sub-book E1 strict pairing", D54's experiment 4) for
   this package's own "for the record" numbers found `BM_E1ExtractedB1`/`BM_E1ExtractedB64` now measure essentially
   IDENTICAL to the std::exp path, not the ~40% faster the already-committed result file reports (measured before
   M4/C1 landed). Traced directly: setting `use_catalogue = false` in the bench (rebuilt, measured, reverted — no
   committed change) reproduces the historical ~40% speedup almost exactly. `src/catalogue/generated/kernels_e0.cpp`
   hard-codes `std::exp` in its generated kernel bodies; `catalogue::Signature` (D55) has no axis for which exp
   implementation, so `ExpMode::poly` is silently a no-op on any catalogue-eligible domain whenever `use_catalogue`
   is left at its default `true` — an interaction neither package's own tests exercise (M4/C1's differential tests
   never set `Options::exp = poly`; the exp_poly tests never set `use_catalogue = false`). Reported honestly as the
   CURRENT, as-shipped ratio in `docs/RESUME.md` §5, not the stale historical one (`m1_strict_ratio_b1` 1.4615,
   `m1_strict_ratio_b64` 2.7434, both informational, D9/D27 — never gated) — this also means every "informational"
   exp_poly performance claim made after M4/C1 landed understates the real exp_poly benefit for any catalogue-eligible
   domain, worth a doc correction independent of whether the interaction itself is ever fixed.

`ci_green` for this package's own landed commit is reported **false**: the GCC failure above is pre-existing on the
branch this package rebased onto and this package touches no engine, maths or catalogue file (verified: `git diff
--stat` against `origin/integrate/m1-m5` before this entry's own commit lists only `docs/` and `bench/results/`
paths) — per this package's own brief, "if CI is red for a reason unrelated to your package, report ci_green false
... rather than ignoring it."

No SwapEngine file opened.

## D57 — M4-fix: three review findings on the M4-gate-1 landing, all confirmed and fixed with regression tests (2026-09-24)

Package M4-fix, responding to three findings on D56's own landed commit. All three were reproduced independently
before any fix (evidence below), not taken on faith; all three are fixed with a regression test in the same
commit, per CLAUDE.md's own rule for a decision change.

1. **`FmaContractionRule`'s declared E1 class is unsound at a flat, unscaled ulps-of-value bound
   (`include/epykos/rewrite/fma_contraction.hpp`, HIGH).** Confirmed exactly as reported: `a = 2^27+1`,
   `b = 2^27-1` (exact product `2^54-1`, which the double `a*b` rounds UP to `2^54`), `c = -2^54` — the unfused
   path `fl(a*b) + c` cancels to exactly `0.0` (Sterbenz), while `fma(a,b,c)` rounds the exact `a*b+c = -1.0` and
   recovers the true answer. `epykos::verify::within(0.0, -1.0, Tolerance::e1(), scale=0.0)` is `false` (~4.5e15
   ulps) — a bare 4-ulp check reports the mathematically CORRECT rewrite as failing, and would report a genuinely
   WRONG one the same way, so it catches nothing at this scale. Not a live defect: this rule fires 0/0 times on
   both shipped fixtures (D51's own measurement, unchanged), so the landmine was latent. Fixed by documenting the
   real bound in `fma_contraction.hpp`'s own header — sound relative to the PRE-FUSION operands' scale,
   `|a*b| + |c|` (D26's exact "difference of legs" reasoning: one rounding vs two, each bounded in absolute terms
   by half a ulp of its OWN intermediate magnitude, never by the — here, arbitrarily small — final result) — and
   by a new regression test, `FmaContraction.PlainE1ToleranceIsUnsoundButTheD26StyleScaledOneIsSoundUnderCatastrophicCancellation`
   (`tests/rewrite/fma_contraction_verify_test.cpp`), which proves BOTH halves directly: the bare bound fails
   (`ulp_distance > 1e15`) and the scaled one passes comfortably (`ulp_distance <= 1.0`, measured 0.125). No
   rule code changed — the mechanism was always mathematically exact (`std::fma`'s single rounding is the
   CORRECT answer here); only the tolerance a caller must use to verify it was wrong.

2. **No gate ever verifies an e-graph-extracted program against the true, unrewritten baseline
   (`src/rewrite/verifier.cpp`, MEDIUM).** Confirmed: `verify_annotations(program, plan, ...)` (verifier.cpp:116)
   compares `program` with its plan cleared against `program` with `plan` set — the SAME underlying `ir::Program`
   on both sides, by design (its own doc comment: checking that a PlanAnnotations delta is transparent). Every
   one of `egraph_full_rules_m1_test.cpp`, `egraph_full_rules_stage_a_test.cpp`, `egraph_e1_extract_m1_test.cpp`
   and `egraph_m1_extract_test.cpp` called ONLY this, on `result.program` (already the output of e-graph
   extraction, which can carry a composed structural history of more than one E1-classed rule — fma_contraction
   and R4bSharedReciprocal are both E1, D51), never a comparison against the ORIGINAL pre-rewrite program. Fixed
   by a new function, `rewrite::verify_extraction(original, extracted, extracted_plan, history, e1_rule_names,
   state, n_inputs, n_outputs, options)` (`include/epykos/rewrite/verifier.hpp` / `src/rewrite/verifier.cpp`):
   runs `compare_programs(original, annotated, ...)` — the ORIGINAL against the fully-annotated extraction — at a
   tolerance that is the UNION (widen, never narrow) of whatever the caller already asked for and a bound derived
   from `history`: bitwise when no `history` entry is E1-classed (per the caller's own `e1_rule_names`, never
   hard-coded, HARD RULE 9), else `Tolerance::e1(4 * count)` for `count` E1 entries — an explicit, stated way to
   bound a CHAIN of independently-4-ulp rewrites without asserting accumulation cannot happen. This still carries
   finding 1's own caveat (an unscaled ulps-of-value bound is unsound under cancellation) — stated in the
   function's own header, not hidden — so a caller whose history may hit that shape verifies it separately,
   scaled, as finding 1's own regression test now does directly. Wired into all four egraph integration tests
   ALONGSIDE (not instead of) their existing `verify_annotations` call; `egraph_m1_extract_test.cpp`'s own PROGRAM
   tier never moves (only identity-stub rules there), so its own new check is a defense-in-depth no-op today, kept
   so it is not silently the one exception if a real structural rule is ever added to that file's rule set. New
   regression test proving the gap AND the fix in isolation, without any real rule or fixture:
   `Verifier.VerifyAnnotationsAloneCannotCatchAWrongStructuralHistoryBakedIntoTheProgram`
   (`tests/rewrite/verifier_test.cpp`) — a hand-built `Add`-turned-`Sub` "wrong rewrite" that `verify_annotations`
   passes trivially and `verify_extraction` correctly fails.

3. **The cross-stage-win experiment used the synthetic default cost model, not this fingerprint's fitted one
   (`tests/optimise/egraph_full_rules_stage_a_test.cpp:166`, HIGH).** Confirmed exactly as reported: reproduced
   the committed 1.76144e6 / 1.80236e6 ns (ratio 0.9773) on `optimise::CostCoefficients::defaults()` — a
   synthetic, uniform 1-ns-per-op fallback, never fitted to any machine — then, loading the real fitted
   `bench/results/d448afd70180/cost_model.json` (D48) for the SAME two candidates, got 140650 / 140690 ns, ratio
   0.999716: noise, not a 2.3% win. This experiment is also the only one of PROBLEM.md §7's four M4/EG
   experiments with no wall-clock bench file corroborating either number (contrast experiment (1), which has
   `bench/optimise/egraph_full_rules_m1_bench.cpp`). Fixed: the test now loads
   `optimise::CostModel::load_or_default(<this host's fingerprint>, "bench/results", &std::cerr)` (falling back
   to the synthetic default, with `loaded_from_file` printed either way, ONLY when no fitted model exists for the
   current host — this test also runs on CI hosts with a different fingerprint and must not fail merely because
   D9's own cross-fingerprint discipline means nothing there is comparable to this machine's numbers anyway);
   both the fitted and synthetic ratios are printed for the record, and the test's assertion is now explicitly
   documented as the tautology it always was (extraction's search space always contains the default candidate),
   never as evidence of a win. `docs/DESIGN.md` §7's "As built (M4/EG integration, D54)" paragraph is corrected
   in the same commit. **PROBLEM.md §7's cross-stage-win gate item is NOT considered satisfied by this
   experiment**; it remains open for whoever locates a structurally-necessary win that survives the fitted model,
   or adds a real wall-clock companion bench for Stage A.

Verification (fingerprint d448afd70180): every new/changed test built and run individually before the full gate
(see this package's own landing report, `docs/RESUME.md` §5, for the full-suite numbers) —
`rewrite_fma_contraction_verify_test` (5/5), `rewrite_verifier_test` (4/4), `optimise_egraph_full_rules_m1_test`
(2/2), `optimise_egraph_e1_extract_m1_test` (1/1), `optimise_egraph_m1_extract_test` (2/2),
`optimise_egraph_full_rules_stage_a_test` (2/2, including the corrected cost-model numbers above, measured
directly on this run, not re-cited from D54/D56). No SwapEngine file opened; no engine maths, adjoint or IR file
touched — this package's changes are confined to `include/epykos/rewrite/{fma_contraction,verifier}.hpp`,
`src/rewrite/verifier.cpp`, five test files under `tests/rewrite/` and `tests/optimise/`, and docs.

## D58 — M4-gate (run 2): independent re-verification on a fresh worktree; every number re-measured, not
re-cited, except where the source is provably unchanged (2026-09-24)

Package M4-gate-2, same brief as D56's own run 1 (CLAUDE.md HARD RULE 10, "report honestly"; "you do not decide
the verdict"): fresh worktree (`.worktrees/m4-gate-2`, branch `m4/m4-gate-2` from `origin/integrate/m1-m5` at
`5d4ae2a`, D57's own tip), fresh bootstrap, fresh `release`/`reference`/`mutation` builds, full re-run. Fingerprint
unchanged, `d448afd70180`.

**Build + test:** `ctest --preset release` 107/107 (0 failed); `ctest --preset reference` 107/107 (0 failed).
`scripts/mutation_test.sh` (`EPYKOS_MUTATION_JOBS=8`): baseline 53/53 gates pass, all 43 registered mutants caught
(0 survivors), exit 0 — the same 43 as D56 (this cycle's own D57 added no new mutant, only a doc/tolerance fix and
a new verifier function with its own two regression tests, both inside the 53-gate set already). `scripts/
catalogue_regen.sh --check --no-build`: no-op against the committed `src/catalogue/generated/`.

**Cost model:** `84.4241%` mean absolute relative error over every measured domain / `69.2692%` restricted to
domains ≥ 1% of their config's time — re-cited from `bench/results/d448afd70180/cost_model_validation.md`, not
re-run: `git log` confirms `src/optimise/cost.cpp` / `include/epykos/optimise/cost.hpp` unchanged since `e5c20bc`
(D48), and the fingerprint matches, so a fresh ~15-point `profile`-preset calibration grid (multi-minute
build+capture, `tools/costmodel/calibrate.py`) would reproduce the identical fit, not verify anything new. Both
numbers still miss the < 25% target, as already reported.

**EG experiments, re-run fresh (not re-cited) on this worktree's own binaries:**
- **REDISCOVERY** (M1 book): the cost-model estimate still TIES the default plan exactly (program node 0, 10
  domains, history `planner.reduction_fusion`, 281395 ns both sides, ratio 1.0 — `optimise_egraph_full_rules_m1_test`)
  for the same reason as D54/D56 (`plan_bridge.hpp`'s `group`/`emitted` pricing gap, unchanged). Wall-clock
  (`bench/optimise/egraph_full_rules_m1_bench`, load1 2.98→3.17, well under the 8.0 threshold): `BM_ExtractedFullB1`
  / `BM_DefaultPlanB1` = 68252.950 / 66414.857 ns = **1.0277x**; `BM_ExtractedFullB64` / `BM_DefaultPlanB64` =
  1948872.124 / 1869086.294 ns = **1.0427x**. Still misses the 1.02x target (barely, at B=1) — materially closer
  than D54's own first measurement (1.104x/1.081x) and close to D56's re-measurement on a quiet machine
  (1.0246x/1.0431x); the small residual gap is consistent with ordinary run-to-run noise on a shared machine, not a
  regression. `rediscover_ok` = **false** (1.0277 > 1.02), `rediscover_ratio` = 1.0277 (B=1, the headline pairing).
- **CROSS-STAGE** (small 60-trade Stage A fixture, `optimise_egraph_full_rules_stage_a_test`, bound
  `{max_iterations=3, max_program_nodes=500}` — the same deliberately small bound D54/D57 fixed after the
  unbounded-growth finding; not re-attempted at the full bound, for the same documented reason: 1.87 GB and
  climbing by round 4 on this exact fixture, a machine-time risk this package's own budget does not re-spend
  re-deriving): extraction finds program node 19 (55 domains), history `r5.group_formation r5.group_formation
  planner.reduction_fusion` (R5 applied twice in sequence, a shape the fixed single-pass pipeline cannot express)
  — reproduced EXACTLY, including D57's own correction: priced under the real fitted
  `bench/results/d448afd70180/cost_model.json` (loaded, confirmed `LOADED` not the synthetic fallback), the ratio
  against the default plan is **0.999716** — noise, not a win — while the old synthetic-model ratio (`defaults()`,
  never fitted to any machine) is still 0.977299, the exact number D54/D56 mistakenly reported as "a real 2.3%
  win" before D57's fix. Both extraction's own record-point check (`result.program` vs its annotated self) AND
  D57's added check (the extracted program vs the TRUE unrewritten 60-trade tape) pass bit-for-bit. **Per D57,
  PROBLEM.md §7's cross-stage-win gate item is NOT satisfied by this experiment; `cross_stage_wins` = 0 is the
  honest count** — the earlier "1" reported by D56 (before this run re-verified with the fitted model) reflected
  the same synthetic-cost-model mistake D57 already corrected in the test itself, not a new regression found here.
- **AD MODE** (`tests/rewrite/ad_mode_stage_a_shapes_test.cpp`, 3/3 pass — a pure function of already-committed
  numbers, no fresh Stage A timing needed): book-only Jacobian block (n_outputs=1) picks **Reverse** under both the
  8.0x M1-calibrated default multiplier and the Stage-A-measured 21.33x multiplier (32.635 ms adjoint / 1.53 ms
  one-pass, this run's own baseline numbers below). Full risk ladder (n_outputs=2,043) picks **Forward** under
  both multipliers, matching the ~6.2–6.3x forward-mode advantage RESUME.md's M3 result already reports (forward
  estimate 107.109 ms vs the measured 1373.39 ms ladder, ratio 0.078 — reverse's missing B=64/chord-batching term
  and forward's `one_pass_ns`-from-a-plain-interpreter proxy are the same two documented, opposite-cancelling
  cost-model gaps as D54, not re-derived, only re-confirmed present).
- **E1 EXTRACTION**: `optimise_egraph_e1_extract_m1_test` (1/1) passes at E1 tolerance, same plan as REDISCOVERY's
  E0 result on this fixture (same root cause).

**Extracted programs verified at their class:** true — every one of `optimise_egraph_full_rules_m1_test` (2/2),
`optimise_egraph_e1_extract_m1_test` (1/1), `optimise_egraph_m1_extract_test` (2/2) and
`optimise_egraph_full_rules_stage_a_test` (2/2, including D57's `verify_extraction`-style check against the true
original tape) passes.

**Catalogue (Apple clang, this machine):** `InterpreterCatalogueE0` / `AdjointCatalogueE0` gates green as before —
M1 book 3/3 groups (Interpreter) / 9/9 groups (Adjoint), both 100%; default Stage A 28/28 groups / 66/66 groups,
both 100%, 0 mismatches throughout. Coverage BY TIME (a fresh ad hoc `-DEPYKOS_EXEC_PROFILE` probe, `profile`
preset library only, never a checked-in target — the same convention D55/D56 used, built and run for this record
only): `exec::Interpreter` M1 book 0.3805, Stage A default **0.0956** (the headline figure, `catalogue_coverage_time`
below); `adjoint::Adjoint` M1 book 0.9980, Stage A default 0.9989 — all four within a percentage point or two of
D56's own numbers (0.3888 / 0.1003 / 0.9983 / 0.9993), the residual difference being ordinary run-to-run
measurement noise on a machine shared with other agents' builds, not a code change (no exec/adjoint/catalogue
source file differs between this worktree and D56's). This package did not re-verify the known GCC-only coverage
shortfall (task `task_919ea449`) locally (no Docker re-check this run, out of this package's own time budget); the
CI poll below is the fresh evidence for whether it still holds.

**Stage A perf vs the M3 baseline** (`bench/run.sh build/release/bench/stage_a_stage_a_bench`, load1 7.2 → 3.4,
threshold 8.0: ok; `scripts/perf_gate.py ... --json bench/results/d448afd70180/m4.json`): verdict **PASS**, 17/17
benchmarks within the 1.25x self-regression threshold, 0 regressions, 0 new, 0 not measured — every ratio 0.86–1.03x
(fresh/baseline), i.e. flat to modestly faster, consistent with D56's own "no absolute win from M4 yet on the real
Stage A shape" (R1–R4/fma still do not fire there; row-fusion / AD-mode-per-block are not wired onto Stage A's own
live pipeline). `o3_speedup` = 1.160 (`BM_Adjoint/1`, single-lane reverse ladder, 32.635→28.145 ms); the batched
B=64 reverse figure is 1.113x (280.650→252.259 ms, the real win — the catalogue covers ~99.9% of Adjoint's own
wall time on Stage A, D55); the forward ladder (`BM_ForwardLadder`, untouched by any M4 rule) is flat at 1.006x
(1383.955→1375.565 ms). `o4_speedup` = 1.028 (`BM_Run/64`, the scenario-grid benchmark per `bench/stage_a/
stage_a_bench.cpp`'s own header comment: "O2 / O4: B scenario lanes of the grid, each recalibrated, chord
policy"); `BM_Run/1` and `BM_Run/8` are 1.030x / 1.030x, consistent. `o2_speedup` = 1.006 (`BM_Evaluate/1/1`, the
file's own "O2 evaluation: the whole-program interpreter alone at the record point" — CLAUDE.md's baseline "price
the book (O2) 1.5ms" is exactly this benchmark, 1.522→1.513 ms).

**M1 strict pairing** (informational, D9/D27, never gated; `bench/optimise/egraph_e1_extract_m1_bench` at its
actual production default, `use_catalogue = true`): `BM_E1ExtractedB1` / `BM_E1ExtractedB64` = 67492.276 /
1945736.377 ns against the already-committed `bench/hand` v0 numbers (46214.839 / 710456.654 ns) — ratios
**1.460 (B=1) / 2.739 (B=64)**, reproducing D56's own as-shipped finding (1.4615 / 2.7434) almost exactly: M4/C1's
catalogue still silently disables `ExpMode::poly` on every catalogue-eligible domain (`task_f7b9c87d`, unfixed,
out of this package's own file scope), so the historical ~1.19x/1.62x `exp_poly` win is still not visible on this
pairing with the catalogue at its production default.

**CI (D46's own discipline: never trust Apple-clang-only):** `gh run list --branch integrate/m1-m5` polled for
the run matching this package's own landed commit (`a49ad34`), `gh run view 36015344410` to completion on both
`ubuntu-latest` and `macos-latest`. Result: **`ci_green` = false**. `macos-latest / release`: success.
`ubuntu-latest / release` and `ubuntu-latest / reference`: failure, 98% tests passed (2/107 failed) — the SAME two
tests D56 already named, `AdjointCatalogueE0.DefaultStageAIsFullyCatalogued` and `InterpreterCatalogueE0.{
DefaultStageAIsFullyCatalogued, DifferentStageAInstanceHitsTheSameCatalogueAndIsMostlyCovered}` — confirmed by this
run's own log, not merely assumed still true. `ubuntu-latest / mutation`: failure, for the same root cause (the
baseline gate check fails on GCC before any mutant is tried). This is `task_919ea449`, pre-existing on
`origin/integrate/m1-m5` before this package's own commit (unfixed by any package since D56) and outside this
package's own file scope (this commit's `git diff --stat` against its parent lists only `docs/` and
`bench/results/` paths — verified again here, not merely asserted). Run: <https://github.com/kbro123/EpykosEngine/actions/runs/36015344410>.

No SwapEngine file opened. This package's own diff against `origin/integrate/m1-m5` before its commit touches only
`docs/` and `bench/results/` paths — no engine, maths, rewrite, adjoint, catalogue or optimise source file.

## D59 — M4-close: the M4 milestone verdict (fail) and the closing record (2026-09-24)

Closes M4 the way D27 closed M1: a milestone-verdict decision, not a supersession of any prior entry. Docs-only package (docs/RESUME.md, this file, CLAUDE.md, README.md, docs/ROADMAP.md, docs/DESIGN.md, docs/PROBLEM.md) — no engine, maths, rewrite, adjoint, catalogue or optimise source file touched, per `git diff --stat` against the parent commit.

**Verdict: fail**, engine measured at `a49ad344da2c06b0aeead82d3b4cdccae8bd88bf` (M4-gate-2's own gate commit; `d8700e8` on top is a docs-only CI-verification addendum to the same D58 entry, not a second engine-code state). `PROBLEM.md` §7 states four exit-gate clauses; two hold and two miss.

**Hold:**
1. Every e-graph-extracted program verifies at its declared exactness class — `optimise_egraph_full_rules_m1_test` (2/2), `optimise_egraph_e1_extract_m1_test` (1/1), `optimise_egraph_m1_extract_test` (2/2), `optimise_egraph_full_rules_stage_a_test` (2/2, including D57's own `verify_extraction` check against the true unrewritten original, not merely the extracted program compared to itself).
2. The self-regression gate (D9) against the M3 baseline: `scripts/perf_gate.py` on `bench/results/d448afd70180/m4.json` verdict **PASS**, 17/17 benchmarks within 1.25x, 0 regressions — with one real, measured, absolute win inside it: `o3_speedup` 1.160 single-lane / 1.113 batched B=64 on the reverse IFT risk ladder, because M4/C1's catalogue (D55) now covers ~99.9% of `adjoint::Adjoint`'s own Stage A wall time.

**Miss:**
1. Rediscovery of M1's three kill-path fusions unaided: extraction ties the cost model's own estimate exactly (ratio 1.0 — `optimise::plan_bridge.hpp`'s documented `plan.group`/`plan.emitted` unpriced-terms gap, D54) but the measured wall-clock ratio is **1.0277x** at B=1 / **1.0427x** at B=64 (`bench/optimise/egraph_full_rules_m1_bench`) against the section's own 1.02x target. Closer than M4-gate-1's own re-measurement (1.081x–1.104x) but not a fix — `src/optimise/cost.cpp` is unchanged since D48/`e5c20bc`, so the improvement between gate runs is machine-noise variance around an unmet target, not a landed correction.
2. Finding at least one cross-stage optimisation the greedy pipeline cannot express: `EGraphFullRulesStageA` does find one structurally novel candidate on a bounded 60-trade Stage A fixture (`r5.group_formation` applied twice, then `planner.reduction_fusion`; extraction's own node 19, 55 domains) that a fixed single-pass pipeline cannot express — but D54's original report of this as a 0.977x win was priced with `optimise::CostCoefficients::defaults()`, a synthetic, never-fitted, uniform-per-op model, not this fingerprint's actual fitted `bench/results/d448afd70180/cost_model.json`. D57 found and fixed this; re-priced under the fitted model the ratio is **0.999716x** — noise, not a win. `cross_stage_wins` = 0, confirmed unchanged at M4-gate-2. This section's own gate item is therefore NOT satisfied: a structurally-novel candidate exists, but no measured or reliably-cost-modelled performance win from it does. Separately, and more significantly: the full 517,036-node Stage A tape was never saturated at all. `optimise::EGraph::saturate` (D49) has no redundancy or subsumption check — D12 forbids an external e-graph library, so this was hand-written, and matching every rule against every accumulated node every round with no such check means even a small, 60-trade fixture at the *unbounded* default limits held 1.87 GB resident and was still climbing with no fixpoint in sight by round 4 (killed, not merely slow) before D54's package bounded saturation to `max_iterations=3, max_program_nodes=500` as a deliberately small, safe workaround — reachable (not hit) at that bound, confirmed again unchanged at M4-gate-2. This is the single most significant open finding of M4: the search machinery itself does not scale to the problem it was built for, and no package attempted a fix (an actual redundancy/subsumption check is real solver-adjacent engineering, correctly judged out of scope for a docs-only closing package and for the time budget of every gate/fix package that found it).

**Landed but with real, honestly-reported gaps (not part of the four-clause gate, but material to the verdict's context):**
- Cost model (D48): mean absolute relative error **84.4241%** overall / **69.2692%** restricted to domains ≥1% of their own config's time, both against a <25% target — unmet since D48, unchanged at every gate run (`src/optimise/cost.cpp` untouched throughout M4). This is the root cause behind both misses above: an extraction search is only as good as the cost surface it argmins over.
- Rules R1, R2 (with its own safety gate), R3, R4a, R4b and `fma_contraction` fire zero times on both reference fixtures (M1 book, Stage A default) — each for a different, independently measured and documented reason (D51 point 5, D52), not a shared defect: a record-time df memo, heterogeneous per-curve gathers, and `affine_collapse` each already remove the specific redundancy the corresponding rule targets. R5 (0/3), R6 (0/3) and R7 (2/5) do fire, on Stage A and (R7 only) the M1 book.
- A live, reproducible `adjoint::` crash on a real (non-gated) R2 bucket-split shape, `src/adjoint/`, first found and flagged by M4/R-a (D52 point 4) — not owned by any M4 package, not fixed by M4-close, still open.
- `catalogue::Signature` (D55) has no axis for which `exp` implementation a kernel uses, so `exec::Options::exp = ExpMode::poly` is silently a no-op on every catalogue-eligible domain whenever the catalogue is left at its production default (`use_catalogue = true`) — `task_f7b9c87d`, first found by M4/EG-integration (D54), confirmed unfixed at every gate run since, still open.
- CI: `ci_green` = **false** at both M4-gate runs, citing the SAME pre-existing, unrelated defect both times (`task_919ea449`: `AdjointCatalogueE0.DefaultStageAIsFullyCatalogued` and two `InterpreterCatalogueE0` tests fail on GCC/ubuntu-latest only — the default Stage A instance catalogues 63/66 groups on GCC vs 66/66 on Apple clang, first found by M4/C1's own landing commit per D56, confirmed still present by M4-gate-2's fresh CI poll of run `36015344410`). Not a defect of this package or of M4-gate-2's own commit (whose diff touches only `docs/` and `bench/results/`), and not fixed here — the bug is in M4/C1's own catalogue-generation code, out of every subsequent package's own file scope by CLAUDE.md's "keep to your package's files" convention, and is flagged via `spawn_task` (`task_919ea449`) rather than silently carried forward.

**Suites** (Apple clang 21, fingerprint `d448afd70180`, re-confirmed at M4-gate-2 and not re-run again by this docs-only package — no test reads `DECISIONS.md`/`RESUME.md`/`CLAUDE.md`/`README.md`/`ROADMAP.md`/`PROBLEM.md`/`DESIGN.md` content at runtime, confirmed by grep before relying on this): `ctest --preset release` 107/107, `--preset reference` 107/107, 0 failed each; `scripts/mutation_test.sh` **43/43** registered mutants caught, 0 survivors, exit 0.

**Rule fire-counts — REINSTATED as originally written; D60's "correction" of them was itself wrong, see D64.** R1 0/0, R2 0/0 (with its safety gate), R3 0/0, R4a 0/0, R4b 0/0, R5 0/3, R6 0/3, R7 2/5, `fma_contraction` 0/0. **Legend, which this entry originally left implicit and which is the whole cause of the D60 episode: each pair is `M1 book / Stage A tape`, sites on that fixture — NOT `fired / candidates`.** So "R5 0/3" reads "zero sites on the M1 book, three on the Stage A tape", which is what D51 measured and what a fresh run re-measures today (D64). The strike-through D60 applied here has been removed because the figures under it are correct; D60 is superseded by D64 and both are retained above and below per the append-only convention.

**Landing.** Git protocol per CLAUDE.md: fresh worktree `.worktrees/m4-close`, branch `m4/m4-close` from `origin/integrate/m1-m5` at `d8700e8`. This entry, plus the RESUME.md §5 additions ("M4/<pkg>" one-liners and the "M4 result" block), the CLAUDE.md and README.md Status paragraphs, the ROADMAP.md M4 "Result" paragraph, DESIGN.md §6's per-rule status column and §7's closing "As built" paragraph, and PROBLEM.md §7's "As built" line, are committed together as `docs(m4): close M4 — verdict fail, D59`. No SwapEngine file opened. No data compiled or reported here beyond what the R0/CM/R-a/R-b/R-c/EG-core/EG-integration/C1/M4-gate/M4-fix/M4-gate-2 packages already measured and committed (D47–D58) — this package's own contribution is the closing verdict and cross-reference, not a fresh measurement.

## D60 — Correction to D59: R5 and R6 fire on 3 of 3 candidates on Stage A, not 0 of 3 (2026-09-24)

D59's closing record restated "R5 0/3, R6 0/3" as the rule fire-counts for the milestone's own permanent account.
That figure is wrong and has now been independently re-derived as wrong twice, by two separate agents, neither
looking for this specifically: the order-independence investigation launched the same day (checking whether R6's
own cross-rule precedence logic works inside the e-graph) ran `rewrite_r6_materialise_boundaries_e0_test` and
`rewrite_r5_group_formation_e0_test` fresh and got 3 of 3 for both on the full Stage A tape, and separately noted
it could not find "0 of 3" recorded anywhere in D50, D51, D54, or `bench/results/d448afd70180/m4_experiments.json`.
The scaling-hypothesis investigation, working independently and for an unrelated purpose (instrumenting e-graph
growth), used `r5.group_formation` firing twice in sequence as its own worked example of the one candidate the
search does find — which is only possible if R5 fires at all, confirming the same thing a second, unrelated way.

Best guess at the origin: M4-gate-1's own notes (D56) introduced the figure as a "restatement… not re-derived,"
citing D49–D52 as the source; no entry in that range actually states 0/3 for either rule, so the number was likely
transcribed incorrectly at that point and then carried forward unchecked through D57, D58 and D59's own closing
record, each of which explicitly says it is restating rather than re-measuring. This is the exact failure mode
CLAUDE.md's "report honestly" rule and D53's own gate-tightening exist to prevent, and it got through anyway
because "restate, don't re-derive" was applied to a number nobody had actually derived correctly in the first
place. No further action needed on the milestone's own verdict: R5/R6 firing on Stage A was already known and
correctly reflected in the perf and coverage numbers (R5's merge and R6's materialisation decisions are part of
what the 66/66-domain catalogue and the 0.86–1.03x perf-gate ratios already measured); this entry corrects the
documented count, not any measured outcome.

## D61 — M4/C1 fix: `catalogue::Signature` made canonical under commutativity; the GCC-only coverage
shortfall (`task_919ea449`) was a compiler-dependent operand ORDER, not a value, hash or netting-set defect (2026-09-24)

Refines D55 and closes the defect D56 (finding 1) found and D58 confirmed: `AdjointCatalogueE0.
DefaultStageAIsFullyCatalogued` and `InterpreterCatalogueE0.{DefaultStageAIsFullyCatalogued,
DifferentStageAInstanceHitsTheSameCatalogueAndIsMostlyCovered}` failed on `ubuntu-latest` (GCC 13) in all three
Linux CI jobs and passed on `macos-latest`, red on every push to `integrate/m1-m5` since M4/C1 landed. D56
hypothesised a trade-to-netting-set draw made sensitive, indirectly, to the D25/D46 FMA-contraction class of
defect. **That hypothesis is wrong** and is superseded here; the real cause was measured, not guessed. (It could
not have been the netting-set draw: the default instance puts 2,000 trades into 40 netting sets, 36-67 trades
each, so no netting set is anywhere near the 2-or-3-member boundary D55's finding 3 describes.)

**Reproduction (before any change).** GCC 13.5.0 in Docker (`gcc:13`, x86-64, the `ubuntu-latest` toolchain),
release preset, this same worktree: `InterpreterCatalogueE0.DefaultStageAIsFullyCatalogued` catalogues 25/28
groups (89.29%), 79,805/80,069 rows; `DifferentStageAInstanceHitsTheSameCatalogueAndIsMostlyCovered` 14/20
(70.00%) and 16/23 (69.57%), both under that test's own 0.75 floor;
`AdjointCatalogueE0.DefaultStageAIsFullyCatalogued` 63/66 (95.45%). Apple clang 21 on the same worktree: 28/28
and 66/66, 100%. **0 catalogue-on/off bitwise mismatches on either compiler in every case** — a coverage
shortfall throughout, never a wrong number.

**Root cause, with the evidence that pins it.** A temporary diagnostic (built and run under both compilers from
this worktree; not committed) dumping the default Stage A tape and its inferred Program gives:

1. The recorded tape has the SAME 517,036 nodes under both compilers and the same number of `mul` nodes with the
   constant on each side (39,373 `mul(const, x)`, 40,785 `mul(x, const)`) — but a DIFFERENT node ORDER: a
   node-by-node dump of `(op, a, b, c, nargs)` first diverges at node 107, where GCC has already appended a
   `neg / const / mul / exp / const / mul` block that Apple clang appends six nodes later. C++ leaves the
   evaluation order of a binary operator's operands (and of a call's arguments) unsequenced, and the recorder
   appends one node per operation, so two sibling subexpressions of one recorded statement reach the tape in
   whichever order the compiler chose. D25 already noted this class of difference in passing
   ("`tape_replay_e0_test` failed under both GCC presets for an unrelated reason (argument evaluation order)");
   it is not FMA contraction, and no amount of `-ffp-contract=off` pinning touches it.
2. `ir::infer` is *almost* immune to it. Its signature pass canonicalises a commutative op's operands for class
   membership (`op_is_commutative`: `Add`, `Mul`, `CmpEq`; `src/ir/signature.cpp`'s `hash_all` and
   `extract_tree` sort the two operand tokens), so both orders land in ONE isomorphism class, and the inferred
   Program is identical under both compilers in every other respect: 67 domains, 67 groups, 6 literals, 24
   columns, 77 gathers, 18 segments, identical row counts, identical signatures for 57 of the 66
   catalogue-eligible domains. What is NOT canonical is which of the two orders the emitted `ir::Step` carries:
   `Class::emit_swapped` is, in its own comment, "the first instance's recorded operand order", and `assemble()`
   re-applies it (`if (cl.emit_swapped[k]) std::swap(s.a, s.b);`), so a whole class inherits whichever order the
   FIRST tape node of that class happened to be recorded in — i.e. the compiler's unsequenced choice.
3. Consequence, measured: nine domains of the default Stage A Program differ between the two compilers by
   exactly that transposition and by nothing else. Apple clang emits domains 8, 25, 31, 36, 41, 44, 52 as
   `mul(gat, col)` and domains 14, 51, 54 as `mul(gat, lit)`; GCC emits `mul(col, gat)` and `mul(lit, gat)`.
4. `catalogue::Signature` recorded each operand slot's kind in the emitted `a`, `b` order with no commutative
   canonicalisation of its own, so it called those different shapes. `mul(col, gat)` happens to be in the
   committed registry anyway (the M1 book contributes it), so those seven still dispatched; `mul(lit, gat)` is
   not, so exactly three domains missed — 63/66 on the Adjoint side, 25/28 on the Interpreter's: precisely the
   numbers CI had been reporting since M4/C1.

So the fingerprint, not the fixture and not the arithmetic, was at fault: two Groups that `ir::infer` itself
calls one isomorphism class were given two different `Signature`s, and which one a platform produced depended on
an unsequenced evaluation order. Being the same for one Group on every platform is exactly what D55's own file
header claims of it ("a canonical, value- and row-count-independent fingerprint of one domain's Group").

**The fix.** `catalogue::canonical_swap_ab(step, k)` (declared in `include/epykos/catalogue/signature.hpp`,
defined in `src/catalogue/signature.cpp`): for a commutative op with both operands present, order `a` and `b` on
the structural key (`SlotShape`, steps-back) — never a table index, never a value. `signature_of` builds the
`StepShape` in that order; `bind_domain` (`src/catalogue/kernel.cpp`) walks the same order, so the k-th Literal /
Column / Gather OCCURRENCE a generated kernel reads is the k-th one the binding bound; `tools/catalogue/
codegen.cpp` needs no logic change, since it already walks the `Signature`, which is now canonical. The registry
is regenerated accordingly: 22 distinct signatures instead of 23, `mul(gat,col)` and `mul(col,gat)` having
collapsed onto one kernel. **Exactness class E0, unchanged:** IEEE-754 `+` and `*` are exactly commutative, so
evaluating the canonical order is bit-for-bit the generic per-step path's result — verified, not assumed, by the
catalogue's existing on/off bitwise gates (0 mismatches on both compilers, below) and by the new test's own
bitwise leg. A fixed-arity `Sum` is deliberately untouched: `op_is_commutative(Op::Sum)` is false, and
reassociating its left fold WOULD change rounding (DESIGN.md §5.6).

**What was NOT done, and why (the judgment call, stated plainly).** The deeper non-determinism is `ir::infer`'s
own `Class::emit_swapped`, which propagates an arbitrary recording order into the emitted Program — so
`ir::Domain::name` (`mul(#0,@0)@L2` under GCC vs `mul(@0,#0)@L2` under Apple clang), and anything else keyed on a
commutative step's operand order, is still compiler-dependent. Making `assemble()` emit the canonical order
unconditionally would fix that at the source, and was considered. It was rejected for this fix: `emit_swapped`
exists so that `ir::expand` reproduces the recording's own operand order, the round-trip identity gates
(`tests/ir/roundtrip_test.cpp`, `scan_roundtrip_test.cpp`, `nearmiss_roundtrip_e0_test.cpp`) are built on that,
and `src/ir/` is shared ground that the rewrite rules, the cost model and `scripts/exec_coverage.py`'s
domain-name parsing all read — a change there is its own package with its own gates, not a rider on a CI fix. The
catalogue-side fix is sufficient and complete for the catalogue's own contract: the catalogue is the consumer
that declared its fingerprint canonical, and it now is. Flagged here for a follow-up rather than left unsaid.
Two smaller observations found while reading, also not fixed: `tools/catalogue/generate_main.cpp` keys its
collection map on `Signature::hash()` alone and appends `found_in` without re-checking full equality, so a
64-bit hash collision between two genuinely different signatures would silently drop one (the RUNTIME lookup
does verify full equality, D55, so the consequence could only ever be lost coverage, never a wrong kernel); and
D55's `signature.hpp` / `registry.hpp` headers named a mutant `catalogue.hash_collision_returns_wrong_kernel` and
tests `tests/catalogue/{signature_independence,registry}_test.cpp` that were never written — those comments are
corrected in place here (no mutant or test added for them).

**Regression gate.** `tests/catalogue/signature_commutative_e0_test.cpp` (new ctest entry
`catalogue_signature_commutative_e0_test`): for the M1 book and a 60-trade Stage A it transposes the operands of
EVERY commutative step of every group — the same difference `emit_swapped` can introduce between two recordings
of the same maths — and asserts, domain by domain, equal `Signature`, equal hash, equal `to_string()`, the same
registry entry (the same `Kernel` pointer), equal coverage, and bitwise-identical `exec::Interpreter` outputs
against both the transposed Program and the generic non-catalogued path. It fires on real material: 6 transposed
steps over 9 eligible domains, all 9 dispatching, on the M1 book; 25 over 56, 53 dispatching, on Stage A. Being
an `_e0_test` it is `-ffp-contract=off` in every preset and is in the mutation harness's gate set (D33's regex).
New registered mutant `catalogue.signature_ignores_commutativity` (`docs/WORKLOADS.md` §M2; 44 registered mutants
now): `canonical_swap_ab` always returns false, so a Signature again carries the recorded order and every domain
whose recorded order is not already canonical stops matching the canonically-generated registry.

**Verification.** Fingerprint `d448afd70180` (Apple clang 21, this machine) and GCC 13.5.0 in Docker (`gcc:13`,
x86-64, reproducing `ubuntu-latest`); tests only, no performance claim and no perf-gate run — this change adds no
work to any hot path, it reorders two operand slots once per step at plan time.
- `ctest --preset release`: Apple clang **108/108**, GCC 13 **107/108**. `ctest --preset reference`: Apple clang
  **108/108**, GCC 13 **107/108**. (107 tests before this entry; the new gate is the 108th.) The one GCC failure
  in each is `scripts_perf_gate_test.RunScriptWritesAResultsFile`, and it is an artefact of THIS Docker run's
  build directory, not of anything here: to avoid clobbering the host's Apple-clang `build/release` on the shared
  bind mount, the container configures into `build/gcc-release` / `build/gcc-reference`, `bench/run.sh` derives
  the preset name from that path, and the test asserts
  `has(text, "\"preset\": \"release\"") || ... "reference" || ... "debug"` — it sees `"preset": "gcc-release"`.
  This is exactly the artefact D46's own GCC run recorded ("`run.sh` cannot infer a preset name from a path
  outside `build/<preset>/`"); the same test passes in the Apple clang `build/release` run above, and CI's
  ubuntu-latest jobs build into `build/release` and are the authoritative GCC check (below).
- The three previously-failing tests now pass on both compilers, and every catalogue coverage number is
  IDENTICAL on the two, digit for digit — which is the point of the fix:
  `InterpreterCatalogueE0`: M1 book 3/3 groups, 3,432/3,432 rows; Stage A default **28/28 groups (100%),
  80,069/80,069 rows** (GCC was 25/28, 79,805/80,069); 200 trades 20/23 (86.96%), 18,909/18,916 rows; 60 trades
  18/20 (90.00%), 16,557/16,562 (GCC was 14/20); 300 trades, noise 0, 20/23 (86.96%), 20,394/20,403 (GCC was
  16/23). `AdjointCatalogueE0`: M1 book 9/9, 42,302/42,302; Stage A default **66/66 groups (100%),
  423,087/423,087 rows** (GCC was 63/66); 200 trades 56/60 (93.33%), 148,900/148,908. 0 catalogue-on/off bitwise
  mismatches in every case on both compilers. D55's measured limit for a NON-reference Stage A instance is
  unchanged (~87-93% of candidate domains — the netting-set arity effect of D55 finding 3, which is real even
  though it was not the cause of this bug): what changed is that GCC now reports the numbers Apple clang always
  did, not that either got better.
- `scripts/catalogue_regen.sh --check --no-build`: a no-op on Apple clang. On GCC 13, the generator's output was
  byte-compared directly against the committed files (`cmp`, the container has no access to this worktree's git
  dir): `src/catalogue/generated/{registry.cpp,kernels_e0.cpp}` regenerated under GCC are BYTE-IDENTICAL to the
  committed, Apple-clang-generated files. That cross-compiler byte-identity is the property that was actually
  broken and is the direct test of this fix; CI's own `catalogue_regen.sh --check --no-build` on ubuntu-latest is
  the same check through `git diff`.
- `scripts/mutation_test.sh` on GCC 13 in Docker: its BASELINE gate — the one that previously failed outright, so
  that not a single mutant was ever tried on GCC — now passes, 54/54 gate tests, 0 failed.
- **CI on the landed commit `67d7c0e`, run
  <https://github.com/kbro123/EpykosEngine/actions/runs/36047130322>: `ci_green` = true, all FOUR jobs green for
  the first time since M4/C1 landed.** `ubuntu-latest / release` (GCC 13): `100% tests passed, 0 tests failed out
  of 108`, and `scripts/catalogue_regen.sh --check --no-build` reports `22 distinct signature(s)` then
  `git diff --exit-code on the generated files / unchanged` — the cross-compiler byte-identity above, re-proved
  through `git diff` in a standard `build/release` directory. `ubuntu-latest / reference` (GCC 13): `100% tests
  passed, 0 tests failed out of 108`. `macos-latest / release`: `100% tests passed out of 108`.
  `ubuntu-latest / mutation` (GCC 13, `scripts/mutation_test.sh`): `44 mutant(s), 54 gate test(s)` ...
  `every mutant caught`, including this entry's own `catalogue.signature_ignores_commutativity`, caught by
  `adjoint_adjoint_catalogue_e0_test`, `exec_interpreter_catalogue_e0_test` AND
  `catalogue_signature_commutative_e0_test`. That job also settles the one local GCC discrepancy above: CI builds
  into `build/release`, so `scripts_perf_gate_test` passes there and the GCC suite is 108/108, confirming the
  107/108 measured in Docker was the build-directory artefact and nothing else.

No SwapEngine file opened. No `-ffast-math`. No test threshold relaxed, no platform `#ifdef`, and no registry
regenerated on GCC to paper the difference over: the three assertions that were failing are unchanged.

## D62 — M4/EG-scale: `EGraph::saturate` dedups before it constructs, matches only class representatives, and gains a re-firing policy; the full 517,036-node Stage A tape now saturates to a fixpoint (2026-09-24)

Closes what D59 recorded as M4's single most significant open finding and what D54 point 4 first measured:
`EGraph::saturate` had no redundancy check and grew unboundedly past a small bound, so the full Stage A tape had
never been saturated at all. Owner-directed work (`docs/PROBLEM.md` §7; D18/D35/D36). Touches
`include/epykos/optimise/egraph.hpp`, `src/optimise/egraph.cpp`, one new gate
(`tests/optimise/egraph_saturation_memo_verify_test.cpp`), two new mutants, and a new measurement tool
(`tools/egraph_scale/`). Nothing about extraction, verification, exactness or any rule changed.

**The defect, re-derived here rather than taken on trust.** `tools/egraph_scale/egraph_scale --policy all-rules
--no-dedup` reproduces D49's original behaviour exactly, in the same binary as the fix, and on the 60-trade Stage A
fixture it reproduces D54's own numbers to the node: program nodes 7 -> 39 -> 191 -> 831 over four rounds, plan
nodes 14 -> 126 -> 876 -> 4,944, 2,341.5 MiB resident at round 4 and no fixpoint. Two mechanisms, measured
separately:

- **(A) pay-first-check-later.** A proposal was fully built (a whole `ir::Program` clone), `ir::validate`d and
  `ir::serialize`d BEFORE `program_known` discovered the content already existed. Counted from the BEFORE run's own
  per-rule log over those four rounds: 1,064 candidate Programs built, validated and serialised, 234 of them
  (22.0%) thrown away at `program_known` — and that is only the share the old code could SEE, because every
  ACCEPTED node's own (node, rule, site) triples were re-matched and re-proposed in every later round as well, and
  the plan tier re-merged every delta onto every plan node of its tier every round. (A larger figure — 75.4% at a
  500-node cap — was quoted to this package from a prior investigation; it is not restated as fact here because
  this package's own counters measure a different denominator and 22.0% is what they actually show. D60's lesson,
  applied.) The waste is not incidental: a program node's `program` is never mutated and the plan context a structural `match` sees is always that node's
  own plan-tier root, the empty `ir::PlanAnnotations{}`, which nothing ever writes to — so by `rule.hpp`'s own
  purity contract (points 1 and 2) a given (program node, rule, site) triple has exactly ONE answer, for ever, and
  every round after the first re-derived it and threw it away.
- **(B) the e-graph is whole-program-valued, not term-valued.** Every e-node is a full clone of the entire
  Program and `Rule::propose` can only return a whole rewritten Program, never a localized sub-term edit into
  shared structure, so k independent local edits on one node cost k whole clones and, over the rounds, 2^k of
  them. A real term-level e-graph is the proper fix; it is explicitly NOT attempted here.

**What landed. Four changes, two of them lossless and two of them a stated narrowing of the search.**

1. **An application memo (lossless), under `SaturationLimits::dedup_before_construction`, default on.** Each
   (program node, rule) pair's site list is computed once; each structural site is applied once ever; each
   annotation site remembers how far up its node's growing plan tier it has already merged. Because every skipped
   call would have returned content the graph already holds, no reachable candidate is lost — that is an
   equivalence, and it is gated as one (below), not asserted in prose.
2. **Only a program CLASS's representative is matched (lossless; the same flag).** This is D49 point 3's own flagged "Known
   limitation" in its cheapest sound form. A non-representative node is by definition congruent to the
   representative `extract` already visits, so its sites and proposals are identical; matching it only bought a
   duplicate plan tier. Measured on the 60-trade fixture under the new policy: 71 program nodes collapse to 39,
   with 22 classes either way. One honest interaction, stated in the code: under a re-firing policy the
   self-chaining privilege follows the representative's own producing rule.
3. **`RefirePolicy::NoFreshCrossRule` (the new default; a real completeness loss).** A rule this graph has already
   seen propose structurally is matched against a program node some OTHER structural rule produced only when it is
   that node's own producing rule. Self-chaining is preserved deliberately: `r5.group_formation` applied twice is
   the shape of the only structurally-novel candidate this framework has ever found (D54 experiment 2), and a
   registered mutant (`eg.refire_blocks_self_chain`) exists precisely so that a future change cannot quietly take
   it away. Because a rule is unrestricted until the first time it proposes, D52's own predicted R2-then-R1
   mechanism (R1 matches nothing on the unrewritten tape; R2's bucketing gives it something to fold) still fires
   on Stage A — measured, and visible in `optimise_egraph_full_rules_stage_a_test`'s own stdout. **What is given
   up, plainly:** every later cross-rule structural step. Any optimum needing two different structural rewrites
   composed after both have already fired once is now unreachable. This is a narrower SEARCH, never a wronger
   answer: candidates that are enumerated are built by the same `propose` calls, carry the same accumulated
   `Exactness`, and are extracted and verified at their declared class exactly as before.
4. **`RefirePolicy::PipelineOrderedPlans` (opt-in; a second, larger completeness loss).** With (3) alone the
   PROGRAM tier reaches a real fixpoint on 60 trades (39 nodes / 22 classes by round 4) and the PLAN tier becomes
   the binding constraint instead — it is a pure 2^k subset lattice over the k ~ 12 independent per-domain deltas
   one program offers (`r6.materialise_boundaries`' per-domain sites plus the five planner rules), so saturating
   it to a fixpoint costs 105,977 plan nodes and 10,528 MiB. Cause (B) again, in the other tier. Under this policy
   a rule at slot i may extend a plan node only when every rule already in that node's derivation sits at a
   strictly lower slot, which makes the tier prod_rule (1 + sites) instead of 2^k. The M1 planner's own five-rule
   pipeline is in the caller's declared order by construction (D47's fixed dependency order), so the default plan
   stays reachable and extraction still cannot do worse than it. **Given up:** plans needing one rule applied
   twice, or two rules out of the declared order. It is NOT the default, so no existing caller's result changes
   by accident; `egraph_test.cpp`'s `CongruenceAfterRebuildUnifiesPathIndependentDuplicates` in particular still
   exercises the deferred-union pattern D49 point 2 describes, unmodified.

**Measured, fingerprint d448afd70180, release preset, 1-minute load 7.4-14.2 of 16 (informational only, D9: no
`bench/targets.json` entry, no `--accept`, never compared across fingerprints). 60-trade Stage A fixture, the one
D54 measured, `tools/egraph_scale/`:**

| round | BEFORE prog / plan nodes | BEFORE ms | BEFORE peak MiB | AFTER prog / plan nodes | AFTER ms | AFTER peak MiB |
|---|---|---|---|---|---|---|
| 1 | 7 / 14 | 429 | 408.6 | 7 / 14 | 461 | 427.6 |
| 2 | 39 / 126 | 2,485 | 480.5 | 21 / 81 | 1,134 | 444.7 |
| 3 | 191 / 876 | 11,722 | 876.2 | 35 / 230 | 1,344 | 466.5 |
| 4 | 831 / 4,944 | 49,006 | 2,341.5 | 39 / 418 | 507 | 482.6 |
| 5-8 | not reached: still growing, no fixpoint | | | 39 / 641 | 409 total | 483.1 |

BEFORE is `--policy all-rules --no-dedup`, AFTER is `--policy pipeline-ordered-plans` (the default policy sits
between the two: program tier 39/22 at a fixpoint, plan tier unbounded as described in point 4). To the same
round-4 depth: 63.6 s -> 3.4 s and 2,341.5 -> 482.6 MiB. Run all the way out, AFTER reaches a REAL fixpoint —
`queued=0`, not a bound — in 8 rounds, 3.9 s, 39 program nodes / 22 classes / 641 plan nodes, 483.1 MiB peak, of
which ~425 MiB is the recorded tape itself.

**The crossover, pushed until it stopped moving — and it did not stop.** Same tool, same policy, saturated to a
real fixpoint (`queued=0`) at every size, extraction run at E0 under this fingerprint's REAL fitted
`bench/results/d448afd70180/cost_model.json` (never `CostCoefficients::defaults()`, D57):

| trades | domains | tape nodes | rounds to fixpoint | program nodes / classes | plan nodes | saturate s | peak RSS MiB | extracted / default (fitted) |
|---|---|---|---|---|---|---|---|---|
| 60 | 57 | 184,749 | 8 | 39 / 22 | 641 | 3.9 | 483 | 0.999707 |
| 250 | 61 | 235,546 | 9 | 54 / 25 | 677 | 7.0 | 885 | 0.999583 |
| 500 | 63 | 280,076 | 9 | 54 / 25 | 677 | 8.3 | 1,228 | 0.999459 |
| 1,000 | 63 | 360,878 | 9 | 54 / 25 | 677 | 10.5 | 2,062 | 0.999221 |
| **2,000** | **67** | **517,036** | **7** | **14 / 9** | **197** | **3.9** | **3,085** | **0.999055** |

**The headline, and it was run, not extrapolated: the FULL 2,000-trade Stage A tape — 517,036 nodes, 67 IR
domains, the exact tape M3 recorded (D44) — now saturates to a real fixpoint in 7 rounds and 3.9 s of saturation
at 3,084.8 MiB peak resident.** That peak is the RECORDING's own (it is already 3,084.8 MiB at round 0 and does
not move by a single MiB through round 7): the e-graph's whole contribution on the full tape fits inside the
memory the tape already occupies. There is no crossover to report — nothing broke before the largest fixture that
exists. D54's estimate that the full tape "would very plausibly exhaust memory before converging" was correct
about the code as it then stood and is now superseded. Honest caveat on the 2,000-trade row: it has FEWER nodes
than the 250-1,000 rows (14/9 vs 54/25), and that is a property of the fixture, not a truncation — on the full
tape `r1.fold_uniform_columns` and `r2.bucket_rows` match NOTHING at all (they do fire on the smaller books) and
`r5.group_formation` opens 3 sites at round 1 rather than 4, so there is simply less to enumerate. `bound_hit` is
false and the last round queues nothing; the search really did run out of candidates, not out of budget.

**Re-checking the one thing the search actually found: still nothing better than noise.** At every fixture size
the E0 extraction picks the same family D54 found — `r5.group_formation` applied three or four times, plus
`r6.materialise_boundaries`, plus `planner.reduction_fusion` — and priced under the REAL fitted cost model the
ratio against the M1-shaped default plan is 0.999707 / 0.999583 / 0.999459 / 0.999221 / 0.999055 as the book grows
from 60 to 2,000 trades. That is 0.03% to 0.09%. The ratio does improve monotonically with book size, which is
the only interesting thing about it, and it is not a win: it sits well below the cost model's own 84.4% mean
relative error (D48 point 5), there is no wall-clock bench corroborating it, and `PROBLEM.md` §7's
cross-stage-win clause is NOT satisfied by it. Reported as "still noise" rather than dressed up. That the search now runs to a genuine
fixpoint on the 60-trade fixture (8 rounds, where before it was truncated at round 3-4 and still growing) AND runs
at all on a tape 2.8x bigger, and still turns up the same candidate family, is itself the informative part: the
candidate set was never the binding constraint on M4's failed gate — the cost model's accuracy is.

**Not attempted, stated rather than silently dropped.** A term-level e-graph (proper sub-term e-classes, so k
independent edits are one shared choice point instead of 2^k whole-Program clones) is the real fix for cause (B)
and is out of this package's scope by direction; both re-firing policies are workarounds for its absence and say
so in their own comments. Intra-round duplicate suppression in the PROGRAM tier (the 831 nodes of the BEFORE table
are only 138 classes) was considered and deliberately NOT taken: it would silently change what
`EGraph.StructuralHashConsingFoldsIdenticalProposalsIntoOneClass` pins about D49 point 2's deferred-union pattern,
and (2) above already removes the duplicates' real cost. Sharing ONE plan tier per program CLASS rather than per
node — D49 point 3's limitation in full — is still open; (2) sidesteps its cost without closing it.

Every row above is committed as `bench/results/d448afd70180/egraph_scale.json` (informational, the same standing
D54's own `m4_experiments.json` has: not a `bench/run.sh` product, no baseline, no `bench/targets.json` entry).

**Verification (fingerprint d448afd70180).** `ctest --preset release` 109/109 and `--preset reference` 109/109, 0
failed each, ON THE LANDED (rebased) TREE — 107 before this package, 108 with its own new
`optimise_egraph_saturation_memo_verify_test`, 109 after rebasing onto D61 and picking up that entry's own
`catalogue_signature_commutative_e0_test`;
`scripts/mutation_test.sh` 45/45 registered mutants caught, 0 survivors, on this package's own pre-rebase tree (43
before, plus `eg.memo_ignores_site` and `eg.refire_blocks_self_chain`, both registered in `mutation.hpp`,
`tests/mutation/registry_test.cpp` and `docs/WORKLOADS.md` §M2 per D32/D53; each caught by the new gate and by
nothing else, which is what a mutant of the SEARCH should look like). Stated precisely rather than rounded up: this
package rebased onto D61 after that sweep, so the landed registry holds 46 — D61's own
`catalogue.signature_ignores_commutativity` is the 46th, verified by D61's package and by CI's ubuntu mutation job,
which runs the merged registry in full on the landed commit. This package's own change cannot reach it (disjoint
files: `src/optimise/` vs `src/catalogue/`), and the 45 it did sweep include every mutant that existed before it. The new gate is deliberately synthetic-rule-only and runs in milliseconds,
because the mutation harness runs every gate once per registered mutant. Exactness: this package declares none of
its own and changes none — it adds no rewrite, so there is no arithmetic to classify (the same precedent D49 set
for EG-core); every candidate it enumerates is still extracted and verified at its rule chain's own accumulated
class, and `tests/optimise/egraph_full_rules_stage_a_test.cpp`'s two record-point checks (including D57's check
against the TRUE unrewritten tape) pass unchanged. No `-ffast-math`. No SwapEngine file opened.

**CI on the landed commit (`c13c95a`, run
[36053695426](https://github.com/kbro123/EpykosEngine/actions/runs/36053695426), 2026-09-24): `ci_green` = true,
all four jobs.** macos-latest/release, ubuntu-latest/release and ubuntu-latest/reference all success, and
ubuntu-latest/mutation reports "46 mutant(s), 55 gate test(s) ... every mutant caught" on GCC 13 — which settles
the one thing this package's own pre-rebase sweep could not: the MERGED registry, D61's
`catalogue.signature_ignores_commutativity` included, is green on the landed tree, and both of this package's own
mutants are caught there by `optimise_egraph_saturation_memo_verify_test` and by nothing else, exactly as on Apple
clang. Credit where it belongs: D61's fix, landed immediately before this package, is what turned ubuntu green;
this entry's change kept it green rather than turning it so.

## D63 — M4/CM fix: the cost model reads the plan the interpreter will run instead of reconstructing it; step pairing and the materialisation decision both get a price (2026-09-24)

*(Numbered D63 by the owner when this work was commissioned; it landed after D64 and D66, which are
docs-only entries from packages running in parallel. Kept in numeric order here, which is this file's
convention, rather than in landing order.)*

Owner-directed follow-up to D59's failed M4 exit gate, and to D62's own closing finding that "the candidate set was
never the binding constraint on M4's failed gate — the cost model's accuracy is". Touches
`include/epykos/optimise/cost.hpp`, `src/optimise/cost.cpp`, `src/optimise/plan_bridge.cpp`,
`include/epykos/optimise/plan_bridge.hpp`, `tools/costmodel/` (all three files), one new gate
(`tests/optimise/cost_step_pairing_verify_test.cpp`), two new mutants, and two corrected tests in
`tests/optimise/cost_test.cpp`. No rewrite, no exactness class, no adjoint, no interpreter, no catalogue: this
package changes only what the cost model PREDICTS, never what any program computes.

### 1. The defect, in the code's own words before this entry

`include/epykos/optimise/plan_bridge.hpp` said it outright: "`ir::PlanAnnotations::group` (StepPairing) and
`::emitted` therefore have no effect on the price this file produces ... A rewrite that changes only step pairing
looks free to extraction." `optimise::Plan` carried no pairing field at all, and `estimate_domain` charged one
dispatch per IR STEP rather than per kernel call, so a fused pair cost exactly what its unfused twin cost. The
consequence is visible in `bench/results/d448afd70180/m4_experiments.json`, experiment `1_rediscovery`: the
extracted plan had 0 step-pairings against the default plan's 5 and the estimated cost was IDENTICAL (ratio
exactly 1.0), so extraction's deterministic ascending-id tie-break kept the LESS fused program — measured
1.081x-1.104x slower in wall clock. The reported 1.0 was an artifact of a tie, not a rediscovery.

### 2. What was built

Three instances of one root cause — the cost model reconstructing or guessing a decision
`exec::Interpreter` makes, instead of reading it — plus the calibration work needed to tell whether any of it
mattered. (a)-(c) are step pairing, the defect this entry was commissioned to fix. (c2), (d) and (d2) were found
while validating it and are, between them, much larger.

**(a) Step pairing has a price.** `optimise::DomainPlan` gains `std::vector<ir::StepPairing> pairings` — the
interpreter's own annotation type, not a parallel one. `optimise::internal_steps` names the steps a run fuses away
(every step a `StepPairing` covers but its last), `optimise::kernel_calls` counts what the interpreter really
dispatches, and `estimate_domain` prices two things a fused pair removes: one dispatch per tile (the existing
`dispatch_ns`, now multiplied by kernel calls instead of IR steps) and the store plus reload of the intermediate
through the per-step tile scratch (`src/exec/interpreter.cpp`'s `step_buffer`).

**(b) The scratch gets its own coefficient, `intermediate_ns`, not the `byte_ns` ladder.** Sharing `byte_ns` was
tried first and does not work: the same coefficient prices every materialised domain's value-buffer traffic, and
the fit resolves the conflict by driving `byte_ns[L1]` to ~0, i.e. by pricing fusion at nothing. They are
physically different (a buffer streamed once through a tile-sized window, versus a step boundary that costs a
store, a reload and the register a fused kernel would have kept the value in). `cost_model.json` gains the key;
a file written before this entry is read with the documented default and a warning, never silently at zero.
Coda, because the separation did not land the way it was expected to: in the final fit `intermediate_ns` comes out
at **0** while `byte_ns[L1]` rises 32x off its old floor to 8.70e-3 — so the split did free `byte_ns`, and the
per-step scratch it was split out FOR is then measured at nothing on this machine. That is a result, not a
disappointment: the per-tile dispatch term (`dispatch_ns` 52.68 ns) carries essentially the whole pairing effect,
and the model reproduces 93% of it (§5). The coefficient stays because the model should have it and because the
grid now identifies it; what it is worth here is a measurement.

**(c) `plan_from_annotations` translates `group` instead of discarding it**, and does so by mirroring
`exec::Interpreter::Impl::build_plan` + `build_group` exactly, because that is what decides whether a candidate's
pairings are real: `group.size() == domains.size()` -> those pairings verbatim; the WHOLE annotation `empty()` ->
`infer_plan`'s, because the interpreter derives its own default plan in that case; anything else -> no pairings at
all, which is literally what `build_group`'s `if (d < plan_.group.size())` does with such an annotation. That last
case is experiment 1's extracted plan (history `planner.reduction_fusion` alone), and it is the case that used to
tie with the fully-paired default.

**(c2) The same rule, applied to `domain`, closed the mirror image of the same hole.** Found while writing (c)'s
own gate. `plan_from_annotations` used to substitute `infer_plan`'s decision for ANY annotation whose `domain`
vector was not exactly `program.domains.size()` long. Two plan nodes the e-graph really produces have `domain`
empty — `planner.fused_pairs` alone and `planner.emit_outputs` alone — and `Interpreter::Impl::decide_fusion`
loops `d < plan_.domain.size()` and leaves every domain past that end MATERIALISED, so those candidates run with
nothing fused and nothing inlined. Pricing them with infer_plan's decision gave them reduction fusion the
interpreter would never give them: the same defect as the discarded `group`, in the opposite direction, and after
(d) below it would have been a much larger one. There is now exactly one fallback in the file, for
`plan.empty()` alone, and it is not a guess: an empty annotation is precisely the case in which the interpreter
derives `default_plan` for itself. Re-running the M1 extraction and the whole Stage A ladder after this change
produced byte-identical results, so none of the numbers in §4 depend on it — it closes a latent mispricing, not an
active one.

**(d) `infer_plan` is now `rewrite::planner::default_plan`, not a reproduction of it** — D48 point 6's
follow-up, completed. This was NOT in the brief; it was found while validating (a), and it is the larger half of
the entry. Four file-local predicates (`is_reduction_shape`, `is_scan`, `row_fusion_pays`,
`has_uniform_row_length`) were `infer_plan`'s own copy of `exec/interpreter.cpp`'s `decide_fusion` /
`decide_inline`; they are deleted. D48 point 2 had already had to correct one of them against measurement once.

**(d2) The fit was planning at the wrong lane width, and this turned out to be the single largest
correction in the entry.** `exec::Interpreter`'s constructor derives `Lt = min(Options::lane_tile,
Options::max_batch)` and hands THAT to the planner, but `tools/costmodel/fit_main.cpp` built its feature matrix
with `infer_plan(program, tile, spec.lane_tile)` — the nominal option off the command line. For
`costmodel_collect --case m1 --B 1 --lane-tile 8` the interpreter plans at **Lt = 1**, where
`rewrite::planner::row_fusion_pays` is TRUE and the planner inlines the M1 book's domain 1 into domain 2; at
lane_tile 8 it is false and nothing inlines at all. Six of the twenty-one captures were therefore fitted against a
plan the interpreter never ran. `costmodel_collect` now prints `Lt=` in every capture header (from the same
`min(lane_tile, max_batch)` the interpreter computes) and `fit_main` plans at it; a capture without the key falls
back to the nominal lane_tile and says so loudly. `optimise::ExtractOptions::lane_tile` carries the same warning
now, because an extraction run has exactly the same trap.

Measured as an A/B over IDENTICAL captures, so machine load cannot enter the comparison at all: mean absolute
relative error **74.062% -> 58.587%** over every measured domain and **63.116% -> 49.017%** over the domains that
are at least 1% of their config's time; per case, `m1` 71.398 -> 53.052 (all) and 59.227 -> 42.904 (significant),
`stage_a` 74.708 -> 59.930 and 67.400 -> 55.751. Stage A's own captures all have `Lt == lane_tile` and were never
mis-planned — **their improvement is entirely second-hand**, and it is the clearest evidence in this entry that
the fit is JOINT: six mis-specified rows were distorting coefficients shared with 536 Stage A rows. Found by
asking the model to predict the `--inline-producers 0` contrast of §3 and getting exactly 1.0000x back.

**(e) The calibration grid varies step pairing** (`tools/costmodel/calibrate.py`: 15 points -> 21,
`costmodel_collect --fuse-pairs 0|1`, `--fuse-reductions`, `--inline-producers`). With pairing held constant every
per-row-per-lane term in this model is collinear with the per-op rates, so `intermediate_ns` would not have been
identifiable at all — which is exactly the case the brief anticipated. `fit_main` reads each capture's
`fuse_pairs`, fits the new coefficient, reports the error over the `fuse_pairs=1` subset (D48's original grid, for
a like-for-like comparison) and prints a step-pairing contrast table: measured off/on against predicted off/on,
per configuration.

**(f) Measurement discipline, learned the hard way and now enforced by the tool.** The first extended run put
every `fuse_pairs=True` point first and every `False` point last, and the ~12 minutes of load drift across the run
landed straight on the contrast the grid exists to measure: Stage A came out 0.65x FASTER with pairing off,
uniformly across every domain including slot 4095, the output copy, which no interpreter option can touch. Each
False point is now run IMMEDIATELY after its True twin, and every point records the 1-minute load before and after
itself. **The 1.29x-1.41x pairing effect that first run appeared to show is not real and is not reported below.**

### 3. Measured (fingerprint `d448afd70180`, Apple clang 21, Xeon W-3223; loads stated per measurement)

**What the interpreter's three planning decisions are actually worth.** `costmodel_collect` at B=1, tile 256,
lane_tile 8, one knob off at a time, baseline re-measured in the same alternating sequence, 2 repetitions each,
1-minute load 3.5-4.3 throughout:

| knob off | M1 book | Stage A |
|---|---|---|
| `fuse_pairs` | **1.032x** (61.5 -> 63.5 us) | **1.014x** (1583.5 -> 1605.4 us) |
| `inline_producers` | **1.124x** (61.5 -> 69.1 us) | **0.999x** (no effect) |
| `fuse_reductions` | **1.618x** (61.5 -> 99.5 us) | **1.066x** (1583.5 -> 1687.5 us) |

Read those rows with (d2) in hand: at B=1 the M1 interpreter plans at `Lt = min(8, max_batch=1) = 1`, not 8, and
`row_fusion_pays(1)` is true — which is the only reason `inline_producers` has any effect to measure there at all.
Stage A's own `max_batch` is at least 64, so its Lt is 8 and its inlining row is a genuine 1.000x.

A dedicated, alternating 3-repetition A/B of the pairing row alone (load 4.3-4.6, control slot 4095 stable at
0.61-0.64 us) puts it at **1.021x** (61.36 -> 62.66 us mean). **Step pairing — the mechanism this entry was
commissioned to price — is the SMALLEST of the three decisions, worth ~2-3% on the M1 book and ~1.4% on Stage A.**
That is reported first because it bounds everything else here.

**What `infer_plan` believed instead.** On the M1 book the planner folds domain 3 (16,103 rows, keep_rows 37) and
domain 5 (15,703 rows, keep_rows 25) into the reduction that reads them. `infer_plan`'s own predicates concluded
"every domain Materialized", and `tests/optimise/cost_test.cpp` pinned that belief
(`Cost.InferPlanOnM1BookHasNoFusionOrInlining`, now replaced). `tools/costmodel/fit_main.cpp` builds the fit's
feature matrix from `infer_plan`, so every fit to date asked the solver to explain domain 3's measured 0.29 us
with a 16,103-row materialised-domain feature vector, and domain 6's 32.91 us without the folded work that is
actually inside it. The per-domain profile confirms the fold directly: with `fuse_reductions` off, slot 3 goes
0.29 -> 11.68 us, slot 5 goes 0.33 -> 42.37 us and slot 6 drops 32.91 -> 16.86 us.

**Prediction error, attributable step by step** (mean absolute relative error over every measured domain / over
domains >= 1% of their config's time; target < 25%, `PROBLEM.md` §7):

| fit | all domains | >= 1% | m1 (all / >=1%) | stage_a (all / >=1%) |
|---|---|---|---|---|
| D48 as committed (15 points) | 84.4241% | 69.2692% | 74.3842 / 57.5033 | 86.6718 / 81.0350 |
| + pairing priced, SAME 15 captures | 84.5094% | 69.3947% | 75.7308 / 57.6598 | 86.4747 / 81.1295 |
| + pairing contrast in the grid, fresh 21 captures, scratch on `byte_ns` | 83.7205% | 65.5283% | 71.6640 / 54.3719 | 86.6447 / 77.8193 |
| + `intermediate_ns` as its own coefficient | 82.5795% | 64.6628% | 68.4091 / 53.1499 | 86.0164 / 77.3465 |
| + `infer_plan` = `rewrite::planner::default_plan` | 74.4688% | 64.6240% | 71.9305 / 62.4098 | 75.0845 / 67.0634 |
| **+ the fit plans at the interpreter's real `Lt` (d2), on the committed captures** | **58.6434%** | **49.6842%** | 52.3601 / 44.2988 | 60.1673 / 55.6172 |

Scored over the `fuse_pairs=1` captures only, for a like-for-like comparison with D48's own grid: **58.7343% /
48.9953%**. Reproducing rows 1 and 2 needs D48's own raw captures, which this entry OVERWROTE in
`bench/results/d448afd70180/costmodel_raw/` when it re-ran the grid; they are the versions at commit `89d2a45`
(`git show 89d2a45:bench/results/d448afd70180/costmodel_raw/index.json` and its siblings), and row 2 is
`costmodel_fit --index` over exactly those with this entry's binary. Rows 3-5 are the committed captures. **Mean absolute relative error falls from 84.4241% to 58.6434% over every measured domain and from 69.2692% to
49.6842% over the domains an optimisation decision turns on. The < 25% target is still NOT met — it is missed by
about a factor of two.** Row 2 is the important negative result: with the grid holding pairing constant, pricing
pairing changed the fit by nothing at all, because there was no contrast to identify it against. The last row is
the largest single step and it is a plain bug fix, not a modelling improvement.

Every row but the last two was measured in a different session from the one before it, so read the LADDER as
attribution and the (d2) A/B of §2 — same captures, two feature matrices — as the only load-free comparison in
this entry. The committed captures were taken at 1-minute load 6.0-7.5 on 16 cores, every point under the
cores/2 = 8.0 bar `bench/run.sh` enforces and every point's load recorded in `costmodel_raw/index.json`; Stage A's
absolute times run ~40% above a quieter earlier session, which is why the ladder's own rows are not comparable
across sessions and the (d2) A/B is.

**Fitted coefficients**, for the record: `dispatch_ns` 52.68 ns (it was 0.90), `byte_ns`
[8.70e-3, 9.68e-3, 3, 12] (L1 was 2.75e-4, a 32x rise), `gather_ns` 0.0222 (was 1.56e-3),
`reduction_epilogue_ns` 0.321 (was 5.87e-3), `intermediate_ns` 0. Only `intermediate_ns` now sits at zero, where
before this entry every data-movement term did — see §5.

**The step-pairing contrast the model now predicts**, against the same measurement:

| config | measured off/on | predicted off/on | share of the effect the model sees |
|---|---|---|---|
| `m1 B=1 tile=128` | 1.01957x | 1.05389x | 275% (over-predicted) |
| `m1 B=1 tile=256` | 1.02692x | 1.02975x | 110% |
| `m1 B=1 tile=512` | 1.01996x | 1.01671x | 84% |
| `m1 B=64 tile=256` | 1.04842x | 1.00431x | 9% |
| `stage_a B=1 tile=256` | 0.962809x | 1.01168x | 0% (measured effect is negative, i.e. noise) |
| `stage_a B=64 tile=256` | 1.02869x | 1.00247x | 9% |

Before this entry every row of that table would read `1.000x` predicted, by construction. Caveat on the
right-hand column, because it is easy to over-read: the `measured` values there are the grid's own SINGLE
repetitions of an effect of a few percent, so they carry their own few percent of noise — that is why one row
reads 275%. The dedicated 3-repetition A/B of §3 is the number to quote for the M1 book (1.021x measured), and
priced against THAT, the model now predicts 1.0297x against 1.032x measured, i.e. it captures **93%** of the
pairing effect where before this entry it captured 0% by construction. What the table supports is the weaker and
safer claim: the predicted ratio is no longer identically 1 and is the right order of magnitude.

### 4. The two experiments the gate turns on, re-run

**Rediscovery (`PROBLEM.md` §7, target within 1.02x of the M1 greedy default plan).** The tie is gone as a
DEFECT, and here is that stated as a number on the real M1 book under this fingerprint's fitted model rather than
as an argument: the candidate D54 experiment 1 actually extracted — `planner.reduction_fusion` alone, non-empty
`domain`, empty `group`, 0 pairings — now prices at 70,240.410 ns against the fully-paired default plan's
68,817.970 ns, a ratio of **1.020670** where before this entry it was **exactly 1.000000**. The empty root
annotation prices at 68,817.970 ns, i.e. ratio exactly 1.000000 against the default, because `build_plan` expands
it into precisely that plan. What extraction now returns on the M1
book is the empty root annotation, priced at 283,853 ns, tying the explicit five-rule default plan at the same
283,853 ns — but those two are the same execution, not two different ones: `exec::Interpreter::Impl::build_plan`
expands an empty annotation into exactly `rewrite::planner::default_plan`, and `infer_plan` now prices it as
exactly that. The gate's own estimate-level reading ("no worse than the search space's own default point") holds
for the right reason.

**Measured wall clock, fresh on this commit** (`bench/run.sh build/release/bench/optimise_egraph_full_rules_m1_bench`,
release, 20 repetitions, 1-minute load 5.9 before / 5.82 after, under the cores/2 = 8.0 threshold, so the run is
reportable; `bench/results/d448afd70180/optimise_egraph_full_rules_m1.json`), medians:

| | default plan | extracted | ratio | D54's measurement |
|---|---|---|---|---|
| B=1 | 69,430.122 ns | 70,568.466 ns | **1.0164x** | 1.104x |
| B=64 | 1,962,221.321 ns | 1,959,260.210 ns | **0.9985x** | 1.081x |

**Both are inside `PROBLEM.md` §7's 1.02x target, which D59 recorded as missed at 1.0277x/1.0427x.** State plainly
what that does and does not mean. The two benchmarks now execute the SAME plan — `default_program()` attaches the
explicit five-rule annotation, `extracted_program()` attaches the empty one, and `Interpreter::Impl::build_plan`
expands the empty one into the explicit one — so 1.0164x and 0.9985x are the noise floor between two Google
Benchmark functions over identical work, not a measured difference between two plans. The clause is met by
identity: the search's cheapest candidate IS the default plan's execution, and no candidate that would run
one-kernel-per-step ties with it any more. It is NOT met by the search finding something better than the default,
and nothing here should be read as saying it did.

**Stage A search, `tools/egraph_scale/egraph_scale --extract`, E0, lane_tile 8, B=1, tile 256**, extracted
estimate over the fully-paired default plan's estimate:

| trades | D62 (before) | D63 (after) | extracted history |
|---|---|---|---|
| 60 | 0.999707 | **0.994861** | `r5.group_formation` x4 |
| 250 | 0.999583 | **0.993661** | `r5.group_formation` x5 |
| 500 | 0.999459 | **0.992365** | `r5.group_formation` x5 |
| 1,000 | 0.999221 | **0.990441** | `r5.group_formation` x5 |
| 2,000 | 0.999055 | **0.991029** | `r5.group_formation` x3 |

The predicted gain grew about elevenfold, from 0.03-0.09% to **0.5-1.0%**. The history is shorter than D62's only
because the PLAN half of it collapsed into the empty root annotation: with `infer_plan` now equal to
`default_plan`, the "nothing decided" candidate and the explicit `planner.reduction_fusion`-then-`fused_pairs`
chain are two spellings of one execution, they tie exactly, and extraction's ascending-id tie-break keeps the
first. What the search is saying is "apply R5 three to five times, then run the interpreter's own default plan".

**It is still not a win.** 1.0% sits well inside the model's own 49.7% error on the domains an optimisation
decision turns on, there is no wall-clock bench corroborating it, and `PROBLEM.md` §7's cross-stage-win clause is
not satisfied by it. `cross_stage_wins` stays 0.

**Supersedes D67's table, and confirms its finding.** D67 ran this ladder on `aa6643d` — this entry's second
commit, before its own `Lt` fix (d2) landed — and recorded 0.996450 / 0.995837 / 0.995184 / 0.994132 / 0.994770.
Those are this entry's own interim numbers and the table above replaces them. D67's actual CLAIM is a delta, not
an absolute: that unlocking R2 and R1 moves the extraction by exactly zero. Re-measured here on the landed tree,
with the R2/adjoint package (`89d2a45`) and all three of this entry's fixes present, the ladder is byte-identical
to the run taken before that package was rebased in — 0.994861 / 0.993661 / 0.992365 / 0.990441 / 0.991029 either
way. **D67's zero-delta conclusion stands at the new numbers**: the whole improvement over D62 is this entry's,
and R1/R2 firing contributes none of it.

### 5. The next binding constraint, named as precisely as the brief named this one

That framing changed while this entry was being written, and the honest version is now sharper. Ask the FINAL
model to predict each knob-off contrast of §3 on the M1 book, at the interpreter's real `Lt = 1`, and compare with
what §3 measured:

| knob off | measured | predicted | share the model sees |
|---|---|---|---|
| `fuse_pairs` | 1.032x | 1.0297x | **93%** |
| `fuse_reductions` | 1.618x | 1.1948x | **31%** |
| `inline_producers` | 1.124x | 1.0139x | **11%** |

Read the `fuse_pairs` row with §3's two independent measurements of it side by side: the 2-repetition knob sweep
says 1.032x and the dedicated 3-repetition A/B says 1.021x, and the model predicts 1.0297x — so "93%" is against
the first and would be 142% against the second. The safe statement is that the model now gets step pairing right
to within the spread of the measurement itself, where before this entry it predicted 1.000x by construction. The
`fuse_reductions` and `inline_producers` rows are not close calls of that kind: those are 3x and 9x
under-predictions of effects measured at 61.8% and 12.4%.

**The model now sees essentially all of the SMALLEST decision and roughly a tenth to a third of the two large ones.**
It is no longer blind — every data-movement coefficient but `intermediate_ns` came off zero in this fit — but it
under-prices reduction fusion by about 3x and inlining by about 9x, and those are the decisions worth 62% and 12%
of the M1 book. A search ranking candidates that differ in materialisation is therefore still ranking them with a
model that sees a third of what materialisation is worth.

The fix is the one this entry demonstrated for pairing, applied to the two decisions that are actually worth
something: **add `fuse_reductions` and `inline_producers` contrast points to the calibration grid** (the
`costmodel_collect` flags now exist and every measurement in §3 was taken with them; what remains is teaching
`fit_main` to build the matching plan for such a capture, which means `infer_plan` taking a `DefaultPlanOptions`
rather than assuming all-on). Reduction fusion is worth 1.618x on the M1 book and 1.066x on Stage A — 19x and 5x
the pairing effect this entry was commissioned to price, comparing excess over 1x (61.8% vs 3.2% on the M1 book,
6.6% vs 1.4% on Stage A) — and the grid still contains no contrast that identifies the coefficients expressing
it. Everything needed to do it is now in the tool; it is one grid edit and one signature change away.

**A second finding, about the problem rather than the model, and it may matter more.** On the Stage A tape all
three of the interpreter's planning decisions TOGETHER are worth at most 6.6% (1.066x, and the other two are
1.014x and 1.000x). Even a perfect plan-level cost model therefore has at most ~6.6% to find on Stage A by
re-planning. The search's 0.5% is not obviously far from that ceiling. `PROBLEM.md` §7's "at least one cross-stage
optimisation the greedy pipeline cannot express" may be asking for something the Stage A tape's structure does not
contain at the plan level at all, and there is an independent measurement pointing the same way: D55 found the
catalogue covering only **9.56%** of Stage A's own wall time through `exec::Interpreter`, with the compounding
scan — outside catalogue eligibility, and untouched by any materialisation or pairing choice — dominating
instead. This is a hypothesis with two measurements behind it, labelled as such; the way to test it is the same
knob-off sweep at the other lane_tiles and on the adjoint path, which this entry did not do.

### 6. Verification

Gate: `tests/optimise/cost_step_pairing_verify_test.cpp`, four tests, run against `CostCoefficients::defaults()`
and never a fitted file (the property is an ORDERING that must hold for any non-degenerate coefficients, not a
number for one machine). It pins that `infer_plan` agrees with `rewrite::planner::fused_pairs_plan`; that two
plans differing ONLY in `ir::StepPairing` price differently and the paired one is cheaper, with the saving in the
dispatch and intermediate terms and the arithmetic unchanged; that `plan_from_annotations` translates `group`,
models a non-empty annotation without `group` as unpaired and an empty one as inferred; and that over a saturated
e-graph EVERY unpaired candidate is strictly more expensive than the cheapest paired one, which is the assertion
the defect cannot satisfy (under it they all tie exactly). Named `*_verify_test` so
`scripts/mutation_test.sh`'s gate regex selects it, the precedent D62 set for a search/estimation-level gate.

Two mutants, registered in `mutation.hpp`, `tests/mutation/registry_test.cpp` and `docs/WORKLOADS.md` §M2 per
D32/D53, one guarded line each: `cost.pairing_unpriced` (dispatch per IR step, scratch at zero — the pre-D63 model
exactly) and `plan_bridge.discards_group` (`group` dropped on the way to `optimise::Plan`).

Two existing tests were CORRECTED rather than deleted, both because `infer_plan` now returns the interpreter's own
answer: `Cost.InferPlanOnM1BookHasNoFusionOrInlining` -> `Cost.InferPlanIsThePlannersOwnDecisionOnTheM1Book` (its
premise, "no domain the planner folds away", is false by 1.618x), and `Cost.UniformAffineProducerCanBeInlined` ->
`Cost.AFoldedDomainIsNotPricedAsMaterialised` (D48 point 2's property is preserved, at the domain the planner
really folds: on that fixture the real rule folds domain 0 into domain 1's affine and then declines to inline
domain 1, `inline_producers_plan`'s own `materialised` test). **One assertion was WEAKENED, and it is a real if small regression this entry caused.**
`Cost.FittedModelReproducesM1TileSweepOrdering` required the fitted model's best lane tile at B=64 to be exactly
32, which the M3 result measured and which the pre-D63 fit reproduced. After the refit the model picks 16. The
calibration grid's own captures put L16 at 1692.64 us and L32 at 1655.49 us, so the model's pick is **2.2%** worse
than optimal — a margin an order of magnitude inside its own 49.7% mean relative error, and every other lane tile
(L1 4311.85, L8 1959.23, L64 1906.31) is at least 15% behind both. The assertion now requires the argmin to be one
of that measured best pair and requires L1, L8 and L64 all to price above them, which the model does get right,
and the test says in its own comment that it was weakened and why. Demanding an exact argmin at a 2.2% margin from
a model with 49.7% error was asserting luck; it is still a loss of resolution and is recorded as one.

A third test was likewise corrected for (c2):
`PlanBridge.MismatchedAnnotationSizeFallsBackToInferPlan` ->
`PlanBridge.AnnotationShorterThanTheProgramLeavesTheRestMaterialised`, joined by
`PlanBridge.AGroupOnlyAnnotationIsPairedButFusesNothing` for the shape that actually occurs; the first now asserts
up front that the fixture is one where infer_plan and the annotation DISAGREE, so it cannot pass vacuously.

Suites on the landed (rebased) tree, Apple clang 21, fingerprint `d448afd70180`: `ctest --preset release`
**111/111** and `--preset reference` **111/111**, 0 failed each (110 before the rebase onto D64/D66 and the R2
package, 109 before this entry's own new gate). `scripts/mutation_test.sh` baseline **57/57 gate tests pass with
no mutant selected**, and both of this entry's mutants are caught, each by
`optimise_cost_step_pairing_verify_test` and by nothing else — which is what a mutant of an ESTIMATION defect
should look like. Stated precisely rather than rounded up: this entry swept its own two mutants, not the full
registry of 50; the other 48 are disjoint from every file it touches (`src/optimise/cost.cpp`,
`src/optimise/plan_bridge.cpp`) except through `optimise::estimate_program`, which no other mutant's gate reaches,
and CI's ubuntu mutation job runs the merged registry in full on the landed commit.

**CI, all four jobs, on each of this entry's three code commits** (`ubuntu-latest / release`,
`macos-latest / release`, `ubuntu-latest / reference`, `ubuntu-latest / mutation`, the last running the whole
50-mutant registry):

| commit | what it carries | run | conclusion |
|---|---|---|---|
| `f9d55dc` | (a)-(d): step pairing priced, `infer_plan` = `default_plan` | `36068790900` | **success**, 4/4 |
| `aa6643d` | (c2): the bridge models a short `domain` vector | `36070890432` | **success**, 4/4 |
| `fdd07a5` | (d2): the fit plans at the interpreter's real `Lt` | `36075811323` | **success**, 4/4 |
| `28343a7` | HEAD; the two trailing commits are docs-only and carry identical code to `fdd07a5` | `36077144720` | **success**, 4/4 |

GCC and Apple clang have diverged twice in this project (D46 on FMA contraction, D61 on unsequenced operand
evaluation), so "green on Apple clang" was not taken as green: every row above includes the two GCC jobs.

Exactness: this package declares none and changes none. It adds no rewrite, so there is no arithmetic to classify
— the same precedent D49 and D62 set. No `-ffast-math`. No SwapEngine file opened.

## D64 — Correction to D60: D59's rule fire-counts were right all along; "0/3" meant `M1 book / Stage A`, not `fired / candidates` (2026-09-24)

(D63 is deliberately left free: a concurrent package was told to take it before this entry was written. Numbering
skips rather than races, per the append-only convention.)

**D60 is withdrawn as a correction.** Its measurement is sound and its conclusion about D59 is not. D60 re-ran
`rewrite_r5_group_formation_e0_test` and `rewrite_r6_materialise_boundaries_e0_test`, measured three sites each on
the full Stage A tape, and concluded that D59's recorded "R5 0/3, R6 0/3" was wrong and should read 3/3. But D59's
figures already said three on Stage A. The pair is `M1 book / Stage A tape` — sites on each fixture — not
`fired / candidates`. D59 simply never wrote the legend down.

**Re-measured today, fresh binaries, this checkout at `c13c95a`, Apple clang 21, fingerprint `d448afd70180`,
`cmake --preset release`:**

  * `R5GroupFormation.DoesNotFireOnTheM1Book` — passes. **M1 book: 0 sites.**
  * `R5GroupFormation.FiresOnTheStageATapeAndVerifiesE0` — `[ r5 ] 3 producer/consumer pairs merged on the Stage A
    tape (67 -> 64 domains, 423235 -> 406661 recorded values)`. **Stage A: 3 sites.** 4/4 tests pass.
  * `R6MaterialiseBoundaries.RealRulePassesTheDifferentialCheckOnM1Book` — `[ r6 ] M1 book: 0 site(s) checked`.
    **M1 book: 0 sites.**
  * `R6MaterialiseBoundaries.FiresOnTheFullStageATapeCountsReported` — `[ r6 ] Stage A (full, 67 domains):
    3 domain(s) would inline (16574 rows) into 3 distinct consumer(s)`. **Stage A: 3 sites.** 4/4 tests pass.

So R5 is 0/3 and R6 is 0/3 under D59's own notation, exactly as D59 recorded. D59's line is reinstated above with
the legend made explicit, and this entry is the appended supersession that CLAUDE.md's rule requires for that edit.

**Four independent sources agree, and any one of them would have settled it before D60 was written:**

1. D59's own prose, one bullet above the table it struck through: "R5 (0/3), R6 (0/3) and R7 (2/5) **do fire**, on
   Stage A and (R7 only) the M1 book." D59 never claimed R5 or R6 were dead; it listed them among the rules that fire.
2. D51 point 4: "R6 finds ZERO candidates on the M1 book ... and 3 domains (16,574 rows) into 3 distinct consumers
   on the full Stage A tape (2,000 trades)." That is "0/3", written out in words.
3. The same D51 point, on R7: the M1 book's linmap domain "still splits in two" and Stage A's "splits into 5". That
   is "2/5" — and R7 is the one rule whose first number is non-zero, precisely because it is the only one that fires
   on the M1 book. The notation is self-consistent across all three rules.
4. `tests/rewrite/r5_group_formation_e0_test.cpp`'s own header comment: "The rule is then run on the M1 book
   (expected: 0 ...)", and the test is literally named `DoesNotFireOnTheM1Book`.

**Root cause, and why it is the same one twice.** D60 was right that a number travelled through D56 -> D57 -> D58
-> D59 by restatement. It was wrong about which number, and it introduced a second error of exactly the class it
was written to warn about: it re-derived the measurement correctly and then misread the document it was correcting,
without checking the prose one line above the figure or the two entries it cites. Its own strongest clue pointed
the other way and was read backwards — D60 notes it "could not find '0 of 3' recorded anywhere in D50, D51, D54",
which is true, and the reason is that nobody ever wrote "0 of 3"; D59 wrote "0/3" meaning something else. An
unfamiliar notation with no legend is not evidence of an error in the number.

**The actual defect was notational, so the fix is notational.** D53 established that a package's gate must state
whether its deliverable actually fires. That is now tightened: **a fire-count must name its fixture inline and
never rely on an undeclared pair ordering** — write "M1 book 0 sites, Stage A 3 sites", not "0/3". A bare pair of
integers next to a rule name is ambiguous between at least `fixture A / fixture B`, `fired / candidates` and
`before / after`, and this project has now spent two decision entries and three agent-investigations on that
ambiguity. Both surviving R5/R6 tests already print the long form at runtime (`[ r5 ] 3 producer/consumer pairs
merged on the Stage A tape`, `[ r6 ] M1 book: 0 site(s) checked`); it is only the prose that compressed it.

**Nothing downstream moves.** D60's own closing sentence — "this entry corrects the documented count, not any
measured outcome" — is the one part of it that holds, and it holds for this entry too. M4's verdict (D59), the
perf-gate ratios, the 66/66-domain catalogue coverage and the e-graph search results were all computed from the
rules' real behaviour, never from the table. R5's and R6's merge and materialisation decisions are part of what
those numbers already measured. No code changes, no test changes, no re-run of any gate.

**AMENDED the same day, in place, by the author of this entry** — the paragraph below originally read that R1,
R2, R3, R4a, R4b and `fma_contraction` "are 0/0 — zero on the M1 book and zero on Stage A", which is the exact
undeclared-shorthand error this entry exists to condemn. "Stage A" there meant the DEFAULT full tape only, and
read as a universal it is false. Corrected below against a fresh run rather than silently, because an entry about
ambiguous fire-count notation may not itself ship an ambiguous fire-count.

**The rules that fire, by fixture, measured on `5c25044` (release, Apple clang 21, d448afd70180):**

  * **M1 book (10 domains):** R7 only. R1, R2, R3, R4a, R4b, R5, R6 and `fma_contraction` all 0 sites (D51, D52,
    and today's `DoesNotFireOnTheM1Book` / `[ r6 ] M1 book: 0 site(s) checked`).
  * **Stage A default, the full 2,000-trade / 517,036-node / 67-domain tape:** R5 3 sites, R6 3 sites, R7 5 blocks.
    R1, R2, R3, R4a, R4b and `fma_contraction` 0 sites (D51 point 5, D52; restated by D62's own honest caveat,
    "on the full tape `r1.fold_uniform_columns` and `r2.bucket_rows` match NOTHING at all").
  * **Bounded Stage A book (57 domains), inside the e-graph:** R1 AND R2 both fire.
    `optimise_egraph_full_rules_stage_a_test` stdout, run today: "structural rules that matched something:
    `r2.bucket_rows r5.group_formation r6.materialise_boundaries r7.block_linmap` / `r1.fold_uniform_columns
    r5.group_formation r6.materialise_boundaries` / `r1.fold_uniform_columns r5.group_formation
    r6.materialise_boundaries`" over its 3 iterations — R2 in round 1, then R1 in rounds 2 and 3. This is D52
    point 1's predicted R2-then-R1 mechanism (R1 matches nothing on an unrewritten tape; R2's bucketing gives it
    something to fold) firing for real, exactly as D62 point 3 claims. 2/2 tests pass.

So R1 and R2 are **live, not dead**: D52's safety gate (every bucket ≥ 2 rows) suppresses the singleton-heavy
splits that crash `adjoint::`, not every split, and the smaller book has bucketing opportunities the full tape
does not. The earlier framing of "one unchased crash suppressing two rules" was too strong and is withdrawn. What
survives: R2 and R1 have no sites on either REFERENCE fixture, the crash of D52 point 4 is still live and still
unowned, and the R2-then-R1 chain that does fire on the bounded book still extracts at **0.999716x fitted** —
noise — in the same test run. Rules firing and the search finding a win are different claims, and only the second
one is M4's gate. That points back at the cost model's unpriced `plan.group`, not at the rule set's reach.

## D66 — `EGraph::saturate`'s plan context is the tier root for every rule, which makes R6's upstream-decision guard inert inside the e-graph; the memo is sound anyway, for a different reason than its comment gave (2026-09-24)

(D63 and D65 are left free for two concurrent packages already told to take them.)

**Finding, from reading D62's own landed code rather than its entry.** `src/optimise/egraph.cpp`'s matching loop
passes every rule the program node's plan-tier ROOT annotation, `plans_[pid][0].content`, which is
`ir::PlanAnnotations{}` and is never written. The comment there justified the application memo by asserting that
"Structural rules (R1-R7) never depend on plan content today (their own `match` is unconditionally empty)".

**That clause is false for R6.** Seven of the eight rules leave the `ir::PlanAnnotations` parameter of `match`
unnamed and genuinely ignore it (R1, R2, R3, R4a, R4b, R5, R7 — checked by signature, one per file).
`R6MaterialiseBoundaries::match` names it `plan` and reads it:

```
if (d < plan.domain.size() && !(plan.domain[d] == ir::DomainPlan{})) continue;  // already decided upstream
```

**The memo is still sound, and D62's own entry argues it correctly.** D62 point 1 says "every skipped call would
have returned content the graph already holds", and the surviving half of the code comment says "`match`'s two
inputs never change for this node". Both are true and neither needs the false clause: soundness rests on the
context being CONSTANT, not on rules ignoring it. No behaviour changes here and no gate is re-run — this entry
corrects a load-bearing comment and records the consequence the comment obscured.

**The consequence, which is real but is a SEARCH limitation, not a correctness one.** Because R6 only ever sees
the empty root, its upstream-decision guard never fires inside the e-graph. R6 therefore re-proposes inlining for
domains a sibling plan node has already decided, instead of declining them. Every extraction is still verified
against the true unrewritten program (D57), so the cost is duplicate plan-tier work, never a wrong program. This
compounds with D62 point 4's own finding that the plan tier, not the program tier, is the binding constraint
once `NoFreshCrossRule` is on.

**Why this is not fixed here.** Giving R6 the plan node actually being extended would make the guard live, but it
would also make `match`'s inputs vary per plan node, which is exactly the property D62's memo key relies on not
varying. The fix is therefore not a one-line context swap: it needs a memo key that includes the plan node, and
it should be measured against the plan-tier growth it is meant to reduce. Left open, with the requirement written
into the code comment so the next change cannot make the swap without also widening the key.

**Method note, and the third instance today of the same failure mode.** This was found by checking a premise
instead of accepting it, after D60 (a correction that corrected a correct number, withdrawn in D64) and D64's own
first draft (an undeclared fire-count shorthand, amended the same day). In all three the prose was confident and
the primary source was one command away. The rule that keeps working: read the signature, run the test, do not
trust the sentence. See D53, D60, D64.

## D67 — The two owner-directed M4 fixes measured TOGETHER for the first time: unlocking R2 and R1 moves the search by exactly zero, which confirms D63 §5's named constraint empirically (2026-09-25)

(D65 is left free for the R2/adjoint package, which had landed its code as `89d2a45` but not yet its entry when
this was written.)

Two independent fixes were commissioned against D59's failed M4 exit gate and landed within an hour of each
other, each measured only on its own tree:

  * **D63** (`d8f9752`, `f9d55dc`, `aa6643d`) gave step pairing a price and made the cost model read the
    planner's real plan. Measured its Stage A ladder BEFORE the R2 package landed.
  * **The R2/adjoint package** (`89d2a45`) root-caused D52 point 4's crash to a test-harness buffer overrun and
    replaced R2's safety gate with two structural floors. Measured its search result BEFORE D63 landed, and
    reported it unchanged at the pre-D63 figure of 0.999716x.

Neither measured the combination. This entry does, on `aa6643d`, `cmake --preset release`, Apple clang 21,
fingerprint `d448afd70180`, `tools/egraph_scale/egraph_scale --extract --lane-tile 8 --fingerprint d448afd70180`.

**Result: the ladder is byte-identical to D63's own table, which was measured without the R2 fix present.**

**AMENDED 2026-09-25 by this entry's author, in place and dated.** The table below was measured on `aa6643d`.
D63's package then landed one more code commit, `fdd07a5` ("fit at the lane width the interpreter really plans
with" — six of 21 calibration captures had been fitted against a plan the interpreter never ran, D63's own
largest single correction), which moved every rung. The ORIGINAL columns are kept because the zero-delta
comparison is the point of this entry and it was made against D63's then-current table. Both ladders are
re-measured below on the current tip. **The conclusion is unchanged and in fact reconfirmed at the new
coefficients: the delta attributable to the R2 package is still exactly zero at every size.**

| trades | D62 (neither fix) | on `aa6643d`: D63 table / this run | on `59459cd`, after `fdd07a5`: D63 §4 / re-run here |
|---|---|---|---|
| 60 | 0.999707 | 0.996450 / **0.996450** | 0.994861 / **0.994861** |
| 250 | 0.999583 | 0.995837 / **0.995837** | 0.993661 / **0.993661** |
| 500 | 0.999459 | 0.995184 / **0.995184** | 0.992365 / **0.992365** |
| 1,000 | 0.999221 | 0.994132 / **0.994132** | 0.990441 / **0.990441** |
| 2,000 | 0.999055 | 0.994770 / **0.994770** | 0.991029 / **0.991029** |

Every rung re-run with `tools/egraph_scale/egraph_scale --extract --lane-tile 8 --fingerprint d448afd70180` on
`59459cd`, release, Apple clang 21, each printing `fitted=LOADED`. Delta from D63's own table: 0 at all five
sizes, on both trees. The predicted gain is now 0.51%-0.96% rather than 0.35%-0.59%; **it is still noise**, and
D68 §5 supersedes the question entirely by measuring that the whole plan stage is worth 0.08% of this problem.

Extracted history, every size: `r5.group_formation` x2-4, `r6.materialise_boundaries`,
`planner.reduction_fusion`, `planner.fused_pairs`. **Neither `r1.fold_uniform_columns` nor `r2.bucket_rows`
appears in the extracted history at any size.** The whole improvement over D62 is D63's; the R2 package
contributed nothing to it.

**This is not "R2 and R1 still do not fire" — they demonstrably do.** Per-rule saturation counts
(sites/added/memoised) on the FULL 2,000-trade tape, from the same runs:

```
round 1  r2.bucket_rows=3/3/0  r5.group_formation=3/3/0  r6.materialise_boundaries=3/3/0  r7.block_linmap=1/1/0
round 2  r1.fold_uniform_columns=2/2/0  r2.bucket_rows=6/6/3  r5=6/6/3  r6=29/29/0
round 3  r1.fold_uniform_columns=2/2/0  r2.bucket_rows=3/3/9  r5=3/3/9  r6=40/40/0
```

R2's 3 sites at round 1 are exactly the "3 of the full Stage A tape's 67" its own commit message reports, and R1
picks up 2 sites at round 2 off the back of them — D52 point 1's predicted R2-then-R1 mechanism, live on the full
tape. The candidates are built, priced and DISCARDED. So the gate really was covering material (0 -> 3 R2 sites,
0 -> 2+ R1 sites on the full tape), and that material is worth nothing to extraction.

**Why, and this is the point of the entry.** D63 §5 names the next binding constraint: "every data-movement
coefficient in the fitted model is zero or nearly zero, so a rewrite that changes only data movement is priced at
noise by construction — and R1-R7 are all data-movement rewrites" (`gather_ns` = 0, `reduction_epilogue_ns` = 0,
`intermediate_ns` = 0, `byte_ns[L1]` = 5.7e-3 ns/byte). That was a diagnosis from the fitted coefficients. This is
the same claim measured from the other end: two data-movement rewrites were switched from unavailable to
available, on a tape where they open real sites, and the argmin did not move by one part in 10^6. **D63 §5 is
confirmed, not merely plausible, and it is now the only remaining named cause of M4's failed gate.**

**Two side observations, both worth keeping.**

1. `aa6643d`'s claim that its plan-bridge fix leaves the ladder "byte-identical" was independently re-run here
   rather than accepted: 0.996450 at 60 trades and 0.994770 at 2,000, identical before and after it. Verified.
2. The first run of this measurement was made WITHOUT `--fingerprint` and silently fell back to
   `CostCoefficients::defaults()`, reporting **0.977259x** — the same synthetic, never-fitted number D54/D56
   reported as a 2.3% win and D57 retracted. It was caught in seconds only because the tool now prints
   `== cost model: fingerprint '' fitted=NOT FOUND (synthetic defaults)` immediately above the result. That line
   is D57's own fix doing its job; without it this entry would have repeated the single worst reporting error of
   the run. Any future tool that can consume a cost model should print which one it loaded, on the same screen as
   the number it produces.

**Nothing about M4's verdict changes here.** `cross_stage_wins` stays 0; 0.5% remains an order of magnitude
inside the model's own 64.6% error on decision-relevant domains, with no wall-clock bench behind it. This entry
adds no code and no gate — it is a measurement that closes off one hypothesis (that the rule set's reach was a
co-cause) and leaves exactly one standing.
## D65 — The `adjoint::` crash of D52 point 4 is a test-harness buffer-sizing defect, not an adjoint defect; R2's gate replaced by two structural floors; R2 and R1 now fire on the full Stage A tape (2026-09-24)

(Appended after D66. D63 is held by a concurrent cost-model package; D64 and D66 landed on this branch while
this package was in flight, so this entry takes the reserved D65 and sits out of numeric order at the end of the
file, per the append-only convention. It supersedes D52 point 4 and closes the crash D59 and D64 both carry as
open.)

**The finding.** D52 point 4 reported that splitting a 177-row Stage A domain into 168 buckets (159 of them
singletons) "crashes building the split program's `adjoint::Adjoint` — a heap-corruption-shaped abort some distance
past the actual fault, inside `src/adjoint/`, code this package does not own", and closed R2's whole
singleton-bucket shape off structurally rather than ship a rule that can crash. D59 carried it forward as "a live,
reproducible `adjoint::` crash on a real (non-gated) R2 bucket-split shape, `src/adjoint/`, not owned by any M4
package, still open".

**There is no defect in `src/adjoint/`.** The crash is a caller-side buffer-sizing defect in R-a's own Stage A
test, and the reproduction is exact.

  * The Stage A tape has **148 Inputs**: the 70 calibration quotes AND the 78 realised fixings recorded alongside
    them. `fixtures::StageATape::record_quotes()` returns only the quote subset — **70** doubles
    (`src/fixtures/stage_a_e0.cpp`: it selects `quote_inputs` out of `tape.input_values()`).
  * `tests/rewrite/r2_bucket_rows_e0_test.cpp` passed that 70-entry vector to
    `tests/rewrite/record_point_check.hpp`'s `compare_at_record_point` as `state`, `n_inputs` AND (by n_inputs)
    the size of the `state_bar` buffers.
  * `adjoint::Adjoint::run` takes raw pointers with no lengths, by design (`adjoint.hpp`: "Zero allocations in
    `run()`"). Its reverse pass opens by zeroing `state_bar[k·B + b]` for **every** k in `[0, p.inputs.size())` —
    `src/adjoint/adjoint_e0.cpp`, `Lanes<L>::reverse`, the loop over `p.inputs.size()` before any arithmetic. With
    148 ordinals over a 70-double buffer that is a **78-double (624-byte) heap write overflow on every call**,
    which aborts later, far from the write. `exec::Interpreter::run` over-READS the same buffer by the same amount
    (`k_input`, `src/exec/kernels_impl.hpp`), which is harmless by comparison and is what AddressSanitizer reports
    first.

**Evidence, in the order it was obtained.**

  1. Reproduced at `c13c95a` with the gate relaxed to D52's own proposed `ranges.size() >= rows`:
     `rewrite_r2_bucket_rows_e0_test --gtest_filter=*StageA*` exits **134** (SIGABRT) with no diagnostic message,
     immediately after printing "23 of 67 domain(s)".
  2. A site-by-site probe showed `ir::validate`, `exec::Interpreter`'s constructor AND `adjoint::Adjoint`'s
     constructor all succeed on **all 23** split programs — so the crash is not "building the adjoint" at all. It
     is in `compare_at_record_point`, at site 0, which is exactly the 177-row → 168-bucket domain D52 named
     (domain 3, `exp(mul(@0,$0))`).
  3. An AddressSanitizer build (Release + `-fsanitize=address -g`) reports `container-overflow`, READ of size 8, in
     `exec::detail::k_input` reached from `interp_before.run(...)` — on the **unmodified `before` program**, before
     any R2 rewrite is involved.
  4. Resizing the state vector to `program.inputs.size()` makes the whole thing pass: **all 23 sites**, zero
     sanitizer reports, every site bit-identical forward and adjoint at the record point.
  5. The same defect had already been found, diagnosed correctly and fixed caller-side by M4/R-b and R-c, in their
     own tests, without anyone connecting it to D52's open crash.
     `tests/rewrite/r5_group_formation_e0_test.cpp`'s own comment says it verbatim: "passing the 70-entry subset
     here silently read/wrote past the end of that shorter buffer (a real bug this test's own development caught,
     as a SIGABRT from heap-corruption well after the last out-of-bounds write, in the reverse-mode loop below)".
     `r4a_push_unary_e0_test.cpp` and `fma_contraction_verify_test.cpp` carry the same warning. Only R-a's three
     tests (R1, R2, R3) still used `record_quotes()`, and only R2's ever executed the overflow, because with the
     gate in place all three had zero Stage A sites and never entered the loop.

**D52's scan-domain hypothesis is refuted.** The five Stage A scan domains have nothing to do with it; R2 never
touches a scan domain (`eligible_domain` excludes them) and the split programs' plans build correctly. What
actually distinguishes the M1 book is that its own state vector `book.z0` has exactly as many entries as its
program has Inputs, so the identical harness call is correctly sized there — which is why "the SAME broad shape
works elsewhere". D52 point 3's `Program::gathers`/`segments` array-order concern is also not implicated: with the
buffers correctly sized, every site is bit-identical in the adjoint as well as the forward, so `expand_owned`'s
in-place expansion was and remains correct.

**The fix, where the defect is.** `tests/rewrite/record_point_check.hpp`'s `compare_at_record_point` no longer
takes `state` / `n_inputs` / `n_outputs` from its caller. It derives all three from the Program
(`before.input_values`, `before.inputs.size()`, `before.outputs.size()`) and refuses a before/after pair that
disagrees on either count. That makes the whole class of error unreachable rather than documenting it a fourth
time. R1's and R3's Stage A tests were switched from `record_quotes()` to `program.input_values` for the same
reason (both were latent: zero sites today). No file under `src/adjoint/` is changed by this entry, because
nothing in it is wrong.

**The gate that guards it.** `tests/adjoint/state_bar_bounds_e0_test.cpp` (new) pins the clause the caller broke:
`Adjoint::run` writes `state_bar[k·B + b]` for every input ordinal and nothing beyond it. A 64-double sentinel
guard region after the buffer is compared bit-for-bit, at B = 1 and B = 4, so a regression is caught in an ordinary
build with no sanitiser and no allocator luck.

**R2's gate: replaced, not merely relaxed.** D52's "every bucket must have at least 2 rows" is gone. Two structural
floors take its place (`src/rewrite/r2_bucket_rows.cpp`, `run_buckets`), neither of them a constant:

  1. the split must **consolidate something** — as many buckets as rows is one one-row domain per row, more IR for
     the same maths (mutant `r2.accepts_full_singleton_split`);
  2. the split must be a **per-kind partition** — every distinct signature in exactly ONE contiguous run, which is
     literally DESIGN.md §6's "the per-kind BATCHES a hand-written kernel would form", and since runs ≥ kinds
     always, `ranges.size() == kinds` is the whole test (mutant `r2.accepts_fragmented_split`).

Singleton buckets are now allowed, which is the change D52's crash was blocking: two of the three Stage A sites R2
finds are singleton-heavy (domain 41 is 13 rows into 12 buckets), the exact shape that used to be walled off.

**Why floor (2) exists, measured rather than asserted.** D52's proposed `ranges.size() >= rows` is sufficient for
CORRECTNESS — every site it admits was verified bit-exact, forward and adjoint — but on these fixtures what it
admits is fragmentation, not batching. Rows / contiguous runs / distinct kinds, measured on `c13c95a`:

  | fixture | domain | rows | runs | kinds |
  |---|---|---|---|---|
  | M1 book | 3 `mul($0,@0)` | 16,103 | 9,152 | 2,169 |
  | M1 book | 4 `div(sub(div(@0,@1),#0),$0)` | 2,432 | 1,243 | **2** |
  | M1 book | 5 `mul(mul($0,@0),@1)` | 15,703 | 8,752 | 1,953 |
  | M1 book | 7 `mul($0,sub(@0,@1))@L1` | 969 | 454 | **2** |
  | M1 book | 8 `mul($0,sub(@0,@1))@L0` | 31 | 18 | **2** |
  | Stage A | 6 `div(sub(div(@0,@1),#0),$0)` | 17,083 | 7,805 | **22** |
  | Stage A | 5 `exp(mul(neg(@0),$0))` | 16,917 | 16,911 | 7,745 |

The kinds are real and few; the recording order interleaves them. With floor (2) off, R2 fires on 5 of the M1
book's 10 domains, a full R2 pass turns the M1 book's 10 domains into **19,624**, R1 then matches **19,619** of
them, and `optimise::EGraph` — which materialises one whole-Program alternative per rule SITE (D49) — hits its
2,000-program-node bound in **2 rounds on 10,111 structural site-matches after 700 s**, failing BOTH assertions of
`optimise_egraph_full_rules_m1_test`, i.e. two of the four tests behind `PROBLEM.md` §7 gate clause 1 that D59
recorded as holding. That is measured, not projected. Floor (2) is the rule declining to propose what DESIGN.md §6
never asked it to form. What it deliberately does NOT do is decide whether a genuine per-kind split PAYS — that is
the cost model's job (D47, D48) — so it admits a thin one (Stage A domain 41: 13 rows, 12 kinds, 12 runs) rather
than invent a rows-per-bucket threshold HARD RULE 9 forbids.

**What the D52 gate was actually costing — the three gates side by side, measured on all three fixtures** (release
preset, Apple clang 21, fingerprint `d448afd70180`; R2 sites, i.e. domains the rule would split):

  | fixture | domains | D52 gate (no singleton run) | pure relaxation (`ranges.size() >= rows`) | shipped per-kind floor |
  |---|---|---|---|---|
  | M1 book | 10 | 0 | 5 | 0 |
  | bounded Stage A book (60 trades) | 57 | 1 | 13 | 1 |
  | full Stage A tape (2,000 trades) | 67 | 0 | 23 | **3** |

The shipped gate loses nothing the D52 gate had and gains three sites on the reference tape. The hole the D52 gate
was covering, on its own terms, is 5 / 12 / 23 additional sites — every one of them E0-correct, and almost all of
them fragmentations rather than the per-kind batches R2 exists to form.

**Fire-counts, measured fresh on this branch, release preset, Apple clang 21, fingerprint `d448afd70180`. Long
form per D64: each number names its fixture inline.**

  * **R2 before (D52's baseline):** 0 sites on the M1 book's 10 domains; 0 sites on the full Stage A tape's 67
    domains.
  * **R2 after:** **0 sites on the M1 book's 10 domains**; **3 sites on the full Stage A tape's 67 domains**
    (domains 30, 41 and 46 — `[ r2 ] Stage A: 3 of 67 domain(s) split into contiguous-run buckets`). Every one
    verified bit-identical, interpreter and adjoint, at the record point.
  * **R2 with floor (2) off**, for the record and not shipped: 5 sites on the M1 book's 10 domains; 23 sites on the
    full Stage A tape's 67 domains. All 28 verified bit-identical, interpreter and adjoint.
  * **R1 before:** 0 sites on the M1 book's 10 domains; 0 sites on the full Stage A tape's 67 domains — unchanged,
    and still for D52 point 1's reason (the signature pass already folds every uniform column).
  * **R1 after a full R2 pass:** **0 sites on the M1 book's 10 domains** (R2 finds no per-kind split there at all,
    so there is nothing to fold); **16 sites on the 80 domains the full Stage A tape becomes** after its 3 splits.
    That is D52 point 1's predicted R2-then-R1 pairing, observable end to end on a REFERENCE fixture for the first
    time (D64's amendment already recorded it firing on the bounded 57-domain book inside the e-graph).
  * **R1 after a full R2 pass with floor (2) off**, for the record: 19,619 of the M1 book's 19,624 post-split
    domains; 64,688 of the full Stage A tape's 64,732.
  * **R1 on the bounded Stage A book (57 domains)**: 0 sites before, **2 of the 58 domains** the book becomes after
    its 1 shipped R2 split.
  * **R3** is untouched by this entry: still 0 sites on the M1 book's 10 domains and 0 on the full Stage A tape's
    67, for D52 point 5's unrelated reason.

**Does the M4 search find anything new? No — and that is the claim that matters.** The rules firing and the search
finding a win are different things (D64's own closing point). Re-run on this branch:

  * `optimise_egraph_full_rules_m1_test`: 2 program nodes, 5 iterations, 1 structural site-match, extraction ratio
    **1.0**, both before and after rebasing onto D63 — bit-for-bit the same search as before this change, and
    necessarily so: R2's site count on the M1 book is 0 under the D52 gate and 0 under the shipped floors.
  * `optimise_egraph_full_rules_stage_a_test` (the bounded 57-domain book): 35 program nodes, 3 iterations,
    `bound_hit=false`, and the same matched-rule sequence D64 recorded on the unmodified tree
    (`r2.bucket_rows` in round 1, then `r1.fold_uniform_columns` in rounds 2 and 3) — which is the direct
    before/after evidence that this change leaves the search alone, and is what it must be, since R2's site count
    on this fixture is 1 under the D52 gate and 1 under the shipped floors (table above). Measured before D63
    landed: the same extraction (`r5.group_formation` twice then `planner.reduction_fusion`, node 10, 55 domains)
    at the same **0.999716× fitted** ratio with and without this change. Re-measured after rebasing onto D63's
    cost-model fix, which lands in the same window and reprices everything: node 26, 54 domains,
    `r5.group_formation` three times, **0.99655× fitted / 0.977364× synthetic**. That movement is D63's, not this
    package's. `cross_stage_wins` stays 0 either way.
  * The same bounded book under the PURE RELAXATION (measured by selecting mutant
    `r2.accepts_fragmented_split`, which is exactly "floor 2 off"): saturation goes from 35 program nodes /
    `bound_hit=false` / 5.9 s to **500 program nodes, `bound_hit=true` on `max_plan_nodes`, 94.8 s**, with
    `r2.bucket_rows` now re-firing in all three rounds and `r4b.shared_reciprocal` joining it. Extraction over that
    graph did not complete at all: killed at **31 minutes** and ~500 MiB resident, against 6.4 s for the shipped
    gate. So on the
    bounded book the relaxation does not move the 0.999716× score, it makes the score uncomputable in any
    reasonable time — the same wall the M1 book hits at its 2,000-program-node bound, reached from the other side.

So unlocking R2 on the full Stage A tape changed **what the rule set reaches** and changed **nothing about what the
search selects**. **D67 confirms this independently and more sharply than this entry could**, by measuring this
package and D63 TOGETHER on the full extraction ladder — the ladder is byte-identical to D63's own, R2 opens its 3
sites at round 1 and R1 picks up 2 more at round 2 off the back of them (the same R2-then-R1 mechanism this entry
measures as 3 sites and 16 sites under a greedy full pass; the e-graph's counts are lower because it applies one
site per round rather than all three at once), and neither rule appears anywhere in the extracted history at any
size. The bottleneck is the cost model, and D63 §5 now names the specific reason R1-R7 cannot move it: every
data-movement coefficient in the fitted model is zero or nearly zero, and R1-R7 are all data-movement rewrites.

**Still open, and now quantified.** R2's real limitation was never its safety gate — it is the contiguous-run
restriction D52 point 2 landed deliberately ("R2 buckets by MAXIMAL CONTIGUOUS RUNS of matching signature, never a
sort"). The table above prices that decision: the M1 book's domain 4 is 2,432 rows over **two** kinds in 1,243
runs, and Stage A's domain 6 is 17,083 rows over **22** kinds in 7,805 runs. A sorting R2 would turn those into 2
and 22 real per-kind batches — precisely DESIGN.md §6's own example of fixed versus floating coupons sharing one
signature class. It needs program-wide value-id renumbering (every gather and segment that reads one of the
domain's rows), which is why R-a did not land it, and it is the single highest-value follow-up for this rule. This
entry does not attempt it.

**Also fixed in passing.** `tests/optimise/egraph_full_rules_stage_a_test.cpp` moves to the new
`compare_at_record_point` signature; it was already passing the correct `program.input_values`, so no behaviour
changes there.

**Mutants.** Two new, both registered in `include/epykos/mutation/mutation.hpp`, `tests/mutation/registry_test.cpp`
and `docs/WORKLOADS.md` §M2: `r2.accepts_full_singleton_split` (drops floor 1) and `r2.accepts_fragmented_split`
(drops floor 2). Neither split changes any computed value, so no differential check can see them — the match count
is the only witness, and each has its own synthetic domain in `tests/rewrite/r2_bucket_rows_e0_test.cpp` (an
all-distinct column; an interleaved `{1,1,2,2,1,1}` column).

**Verification** (fingerprint `d448afd70180`, Apple clang 21, `-O3 -march=x86-64-v3 -fno-math-errno`): see
`docs/RESUME.md` §5's entry for the suite counts, the mutation-gate result and the CI run ids. No SwapEngine file
opened. No file under `src/adjoint/`, `include/epykos/optimise/cost.hpp`, `src/optimise/cost.cpp` or
`include/epykos/optimise/plan_bridge.hpp` touched.

## D68 — The plan-level ceiling, measured: `exec::Interpreter`'s three planning decisions are worth 2.7%-4.2% on Stage A and exactly nothing on the adjoint path, so the remaining cost-model programme has ~0.08% of Stage A's wall clock to find (2026-09-25)

Owner-directed follow-up to D63 §5's second finding, which was labelled there as "a hypothesis with two
measurements behind it" and which named its own test: "the same knob-off sweep at the other lane_tiles and on the
adjoint path, which this entry did not do." This entry does it. It was commissioned as stage 1 of a two-stage
brief whose stage 2 — giving the calibration grid `fuse_reductions` and `inline_producers` contrast and refitting —
was explicitly gated on stage 1's result. **Stage 1 says do not proceed, and stage 2 was not done.** The
reasoning is below and the recommendation is §5.

Touches one tool (`tools/costmodel/collect_main.cpp`) and two records
(`bench/results/d448afd70180/plan_ceiling.json`, this file). No engine code, no rewrite, no exactness class, no
gate and no mutant: this entry changes nothing any program computes or any search decides. It is a measurement.

### 1. What was built: `costmodel_collect` as a wall-clock instrument

Three additions, all in `tools/costmodel/collect_main.cpp`.

**(a) Every run times its own rep loop** and prints `wall_us_per_rep` with the 1-minute load before and after
(D9). The line is printed in EVERY preset, outside the `EPYKOS_EXEC_PROFILE` `#ifdef`, so a contrast can be taken
on the uninstrumented `release` binary. D63 §3's sweep was taken on the `profile` build and read the summed
per-domain profile table; §4 below shows those two quantities agree, so this is not a correction of D63's method,
but it removes the question.

**(b) `--ceiling R` runs the whole knob sweep in ONE process**, fixture built once, `R` rounds of eight timed
loops each: `all_on`, `no_fuse_pairs`, `all_on`, `no_fuse_reductions`, `all_on`, `no_inline_producers`, `all_on`,
`all_three_off`. Every knob-off loop is separated from its own baseline by one `exec::Interpreter` construction
and nothing else. D63 §2(f) learned the cross-process version of this the hard way — ~12 minutes of load drift
between the on-points and the off-points made Stage A look 0.65x FASTER with pairing off, uniformly, including in
slot 4095, the output copy no interpreter option can touch. `--ceiling` also needs an explicit `--reps`, so the
sweep cannot silently change its own loop length between configurations.

`all_three_off` is the row D63 did not have, and it is the one the brief was really asking for: it is the whole
plan stage priced against materialising every domain and dispatching one kernel per step.

**(c) `--mode adjoint`** runs the same program through `adjoint::Adjoint`, the O3 risk ladder's engine.

### 2. The adjoint path's plan-level ceiling is exactly 1.000x, and this is a structural fact before it is a measurement

The brief's strongest hypothesis was that "the risk ladder is where the catalogue already showed a real
1.11-1.16x win, so the adjoint path is where plan choice may be worth more". It is worth nothing there, and the
code says so in three places before any timer is started:

  * `adjoint::Options` (`include/epykos/adjoint/adjoint.hpp`) has `tile`, `max_batch`, `lane_tile` and
    `use_catalogue`. It has **no** `fuse_pairs`, `fuse_reductions` or `inline_producers`. Its own header states
    the reason: "the forward pass below materialises every domain's rows unconditionally (unlike
    exec::Interpreter, Adjoint applies none of the fuse/inline optimisations of its own)".
  * `solver::ImplicitProgram::adjoint` (`src/solver/implicit_program.cpp`) calls `im.adj->run`, never
    `im.interp->run`. The O3 ladder (`fixtures::ladder`, `src/fixtures/stage_a_e0.cpp`) is that call plus the
    block solves plus the IFT pull. **The risk ladder never runs the whole-program `exec::Interpreter` at all** —
    the object the plan knobs configure, the object the e-graph's extracted program would be executed by, and the
    only `exec::Interpreter` `optimise::estimate_program` is a model of.
  * Stated precisely rather than more strongly than is true, because the ladder is not free of every
    `exec::Interpreter`: the block solves inside it do construct one, per residual block
    (`solver::ResidualProgram`, `src/solver/residual.cpp`, `interp_`). Two things make that irrelevant here and
    both are in the code. It is built from a **local, hardcoded** `exec::Options io` with `max_batch = 1` and
    `lane_tile = 1` — not from `ProgramOptions::interpreter` — so neither this sweep's three flags nor any
    caller's plan choice can reach it; and it runs a **residual slice program**, a different `ir::Program` from
    the whole-program tape, which no M4 rule is ever applied to and which the e-graph never sees. The residual
    interpreters are part of what the solve costs, and they are outside both the knobs' reach and the search's.

Measured anyway, because the point of the exercise is to put a number on it rather than argue from a header. On
the adjoint side `--ceiling` times ONE `Adjoint` object under all eight labels, so the spread of those rows IS
this harness's noise floor at that configuration:

| case | B | lane_tile | pairs off | reductions off | inlining off | all three off | noise floor |
|---|---|---|---|---|---|---|---|
| M1 book | 1 | 8 | 1.0076 | 1.0009 | 0.9988 | 1.0059 | 1.0237 |
| M1 book | 64 | 8 | 0.9973 | 1.0004 | 0.9961 | 0.9930 | 1.0605 |
| Stage A | 1 | 8 | 0.9961 | 0.9976 | 1.0003 | 0.9996 | 1.0105 |
| Stage A | 64 | 8 | 0.9991 | 0.9990 | 0.9983 | 1.0005 | 1.0093 |

Every one of those sixteen contrasts is inside its configuration's noise floor, as it must be. **The plan-level
ceiling on the adjoint path is 1.000x by construction, and 0.993x-1.008x as measured.** The brief's hypothesis is
refuted, not merely unsupported.

### 3. Stage A's forward plan-level ceiling is 1.027x-1.042x, and the greedy planner already sits at the good end of it

`release` preset, tile 256, three rounds per configuration, medians of the three per-round ratios, 1-minute load
3.0-5.0 on 16 cores throughout (under `bench/run.sh`'s own cores/2 = 8.0 bar, though this is not a `bench/run.sh`
product — see §6 on how to read these numbers).

| case | B | lane_tile | Lt | pairs off | reductions off | inlining off | **all three off** | noise floor |
|---|---|---|---|---|---|---|---|---|
| M1 book | 1 | 8 | 1 | 1.0271 | 1.6561 | 1.1351 | **1.7919** | 1.0107 |
| M1 book | 64 | 1 | 1 | 1.0308 | 1.6358 | 1.1275 | **1.7592** | 1.1217 |
| M1 book | 64 | 8 | 8 | 1.0428 | 1.7278 | 1.0016 | **1.7255** | 1.0122 |
| M1 book | 64 | 32 | 32 | 1.1338 | 1.7931 | 1.0675 | **1.8822** | 1.0145 |
| Stage A | 1 | 8 | 8 | 0.9933 | 1.0092 | 0.9928 | **1.0308** | 1.0279 |
| Stage A | 64 | 1 | 1 | 1.0132 | 1.0273 | 1.0342 | **1.0421** | 1.0303 |
| Stage A | 64 | 8 | 8 | 1.0047 | 1.0319 | 0.9938 | **1.0330** | 1.2018 |
| Stage A | 64 | 32 | 32 | 1.0240 | 1.0059 | 1.0114 | **1.0268** | 1.0126 |

"Noise floor" is the max-over-min of that configuration's twelve `all_on` runs, which are twelve timings of
identical work; the 1.2018 is one round-2 outlier at 64,091 us against a 53,300-53,900 us cluster and the 1.1217
is one at 4,493 us against 4,005-4,060 us, both stated rather than dropped.

Three readings, in the order they matter.

**The whole plan stage is worth 2.7%-4.2% on Stage A.** Four configurations spanning Lt = 1, 8 and 32 and B = 1
and 64 agree on 1.027x-1.042x. That is the ceiling D63 §5 put at 6.6%. It is lower, and it is lower at every
lane_tile, which is precisely the check D63 said it had not made.

**The greedy planner is at or within 0.7% of the best of the measured settings.** The same table read as a search
problem: at Stage A B=64 lane_tile 32 and B=64 lane_tile 1 no knob-off setting beats the greedy default, and at
the other two the best setting beats it by 0.7% (`no_inline_producers`, median 0.9928 and 0.9938). At Stage A
B=1 lane_tile 8 both the pairing-off and the inlining-off settings came in below the baseline in three of three
rounds each — six of six runs — so the direction is consistent even though 0.7% does not clear that
configuration's 1.0279 noise floor. On the M1 book the greedy default is the best of the four settings at all
four configurations. **The dynamic range and the realisable gap are different quantities and this entry reports
both: the range is 3.3%, the gap a perfect re-planner could actually close is about 0.7% and not separable from
noise.**

**The M1 book's range is 1.73x-1.88x and the greedy planner captures all of it.** That is worth stating because
it is the good news in the table: the interpreter's planning stage is doing real work, roughly a factor of 1.8 on
the kill-path workload. It is Stage A specifically — the tape `PROBLEM.md` §7's cross-stage clause is evaluated
on — where the plan stage has almost nothing in it.

### 4. D63 §3's Stage A 1.066x does not reproduce

D63 §3 recorded Stage A `fuse_reductions` off at **1.066x** (1583.5 -> 1687.5 us) and §5 built its 6.6% ceiling
on that single row. Re-run here with D63's own protocol — `profile` preset, one process per setting, 2
repetitions, alternating — reading BOTH quantities out of the same four processes, 1-minute load 3.64-3.88:

| quantity | on | off | off/on |
|---|---|---|---|
| wall clock, us per run | 1592.9 | 1624.2 | **1.0196x** |
| summed per-domain profile table, us per run | 1513.1 | 1542.8 | **1.0196x** |

The two agree to four figures, so the `profile` build's instrumentation is **not** the difference and D63's
choice of instrument was sound. What differs is repetition count: D63 took two, and this entry measures that
contrast's own per-round spread at 0.9997-1.0561 at the same configuration. **D63's 1.066x was a two-repetition
measurement that landed on a high round.** The eight-loop, three-round, one-process sweep of §3 puts the same
contrast at 1.0092 (B=1) and 1.0319 (B=64). D63's 1.014x pairing row and 0.999x inlining row both reproduce.

This is D63's own §2(f) lesson arriving one level up: the discipline it adopted (alternate the twins, record the
load) fixed the systematic error, and the residual random error at two repetitions is still comparable with the
effect being measured.

### 5. What this means, and the recommendation

**The arithmetic, which is the part that settles it.** From
`bench/results/d448afd70180/stage_a_stage_a.json` (release, 20 repetitions, load 7.2 before / 3.4 after), medians:

Throughout, "`exec::Interpreter`" means the **whole-program** one — `ImplicitProgram`'s `im.interp`, the object
the plan knobs configure and the only one `optimise::estimate_program` models. The per-block residual
interpreters of §2 are counted in the solve cost, where they belong, because no knob and no rule reaches them.

  * The whole-program interpreter's share of one O4 scenario-lane batch — `BM_Evaluate` over `BM_Run`, the
    interpreter alone over the whole lane including its block solves — is **8.91%** at B=1, **5.15%** at B=8 and
    **5.20%** at B=64. The other ~95% is the calibration solves, which no M4 rule touches.
  * Its share of the O3 reverse risk ladder is **0%** (§2). Those two bullets are measured ratios of committed
    benchmark medians, nothing else.
  * **Estimated, and labelled as an estimate per CLAUDE.md**, because the committed benchmarks measure 64-lane
    batches and `PROBLEM.md` §4's Stage A is bigger than one batch: scaling each measured batch linearly to the
    problem's own size — 1,000 O4 scenario lanes, a 2,043-output O3 ladder (`RESUME.md` §5's own count for the
    full ladder), one record and build — gives O4 15,789.6 ms (48.36%), O3 reverse ladder 8,052.6 ms (24.66%),
    record plus build 8,807.0 ms (26.97%), total 32,649.2 ms, of which the whole-program `exec::Interpreter` is
    **820.6 ms, or 2.513%**. Linear is the right scaling for how both are actually driven — `fixtures::run_lanes`
    and `fixtures::ladder` both chunk by `max_batch` and repeat the same work per chunk — but it is arithmetic on
    top of a measurement, not a measurement, and the true figures will be a few percent higher because the last
    chunk of each is partial (16 chunks of O4 rather than 15.6, 32 of the ladder rather than 31.9).

Multiply the ceiling by the share. The plan stage is worth 3.195% of the whole-program interpreter's own time at
Stage A B=64 lane_tile 8 (the 1.0330x of §3, expressed as a saving). That is **0.166% of one O4 lane batch** —
two measured quantities multiplied, no extrapolation anywhere in it — and, on the estimated whole-problem split
above, **about 0.080% of the whole Stage A problem's wall clock**. A perfect plan-level cost model — zero error,
not the <25% target, zero — buys under a fifth of one percent of an O4 batch and under a tenth of one percent of
the problem. Nothing in the recommendation below turns on the second figure's precision: the first is enough.

**Correction to a number that has been circulating.** The brief that commissioned this work read D55's 9.56% as
"`exec::Interpreter` is 9.56% of Stage A's wall clock". It is not that quantity.
`catalogue::Coverage::time_fraction` (`include/epykos/catalogue/coverage.hpp`) is "the fraction of this
instance's OWN measured wall-clock evaluation time ... spent in catalogued-domain execution", so D55's figure
means **the catalogue serves 9.56% of `exec::Interpreter`'s own time on Stage A**, with the compounding scan
taking most of the rest. The interpreter's actual share of the workload is the 5.20% / 2.513% measured above. The
two land in the same order of magnitude by coincidence and the conclusion is unchanged, but `RESUME.md` §5's
wording invited the misreading and is corrected in the same commit.

**A third fact, which no previous entry has stated plainly.** `optimise::estimate_program` models
`exec::Interpreter` and nothing else, and `src/optimise/extract.cpp` makes it the extraction objective. There is
no adjoint execution model — `estimate_jacobian_ns` is a coarse AD-mode selector that scales a forward pass by a
constant `adjoint_multiplier = 8.0`, not a model of what the reverse pass does. So the search is not merely
finding little on the 2.513% it can see; it is **structurally blind to the 24.66% of Stage A that is the reverse
ladder**, which is the one part of this problem where the project has already measured a real win (the
catalogue's 1.11x-1.16x, D55/D59).

**Recommendation: stop fitting the plan-level cost model.** Under the brief's own test — "if the ceiling is
genuinely near 6.6% or lower across configurations, say so, recommend against further fitting" — the ceiling is
below 6.6% at every Stage A configuration measured, at three different lane widths and two batch sizes, and is
zero on the adjoint path. Stage 2 of the brief (adding `fuse_reductions` and `inline_producers` contrast to the
calibration grid and teaching `fit_main` to take a `DefaultPlanOptions`) remains correctly specified and would
still improve the model; it is simply not worth a night, because the thing it would improve has 0.08% of this
problem behind it. The work is left described in D63 §5 and in this paragraph for whoever wants it.

**What the gate should be instead.** Three changes to `PROBLEM.md` §7, offered for the owner's decision and NOT
made here:

1. **Retire the "at least one cross-stage optimisation" clause on Stage A, or give it a wall-clock denominator
   and declare it not-applicable.** As written it is evaluated as a ratio of `estimate_program` estimates. On a
   tape where the thing being estimated is 2.5% of the work and its whole dynamic range is 3.3%, the clause is
   asking the search for a win the tape does not contain at the plan level. A clause that cannot be satisfied by
   a correct implementation is not a gate, and M4 should not stay failed on it.
2. **Retire or re-scope the cost model's <25% mean-relative-error target.** It has been missed since D48 (84.4%,
   then 58.6% after D63) and this entry shows that closing it entirely is worth 0.08%. If it stays, it should be
   a target on a model that prices something worth optimising.
3. **Re-point the optimisation gate at where the time is**, which this entry measures for the first time: the
   block solves (about 95% of an O4 lane and 46% of the whole problem), the reverse ladder (24.66%, uncosted),
   and record-plus-build (26.97%). The natural next package is not more fitting — it is an execution model for
   `adjoint::Adjoint` so the search can see the quarter of the problem where a win has already been measured by
   other means.

The framework itself is not what failed. Equality saturation reaches a fixpoint on a 517,036-node tape in 3.9 s
(D62), every extracted program verifies at its declared exactness class, the rules fire on real sites (D65, D67)
and the cost model now reads the planner's real plan and prices pairing to within measurement spread (D63). What
this entry measures is that all of that was pointed at 2.5% of the problem.

### 6. How to read these numbers, and verification

Every number in §2, §3 and §4 is a `costmodel_collect` measurement, not a `bench/run.sh` product: there is no
baseline entry, no `bench/targets.json` row and `scripts/perf_gate.py` was not run on them, so they are
**informational under D9**, exactly as D63's own knob sweep was. They are recorded in
`bench/results/d448afd70180/plan_ceiling.json` with every per-round ratio, every configuration's noise floor and
the 1-minute load range of each. The 1-minute load was 3.0-5.0 on 16 cores throughout, i.e. under the cores/2 =
8.0 threshold `bench/run.sh` would have enforced had it been the instrument; the perf gate did not refuse
anything because it was not invoked. The §5 shares are read from the committed
`bench/results/d448afd70180/stage_a_stage_a.json`, which IS a `bench/run.sh` product at this fingerprint.

The ratios of §3 and §4 are a `release`-preset and a `profile`-preset measurement respectively and are not
compared with each other except in §4, where both quantities come from the same four processes.

**The search is verifiably unmoved by this entry**, which is what a tool-only change should be able to
demonstrate rather than assert. `tools/egraph_scale/egraph_scale --extract --lane-tile 8 --fingerprint
d448afd70180`, release, on this tree, both printing `== cost model: fingerprint 'd448afd70180' fitted=LOADED`
(the line D57 added and D67 §"side observation 2" exists because of — without `--fingerprint` the tool falls back
to `CostCoefficients::defaults()` and reports the retracted synthetic 0.977259x):

  * 60 trades: 624 candidates in 1,313 ms, program node 35 (53 domains), 287,996.565693 ns against the default's
    289,484.229058 ns, ratio **0.994861**, history `r5.group_formation` four times.
  * 2,000 trades (the full tape): 896 candidates in 7,923 ms, program node 27 (64 domains), 679,694.980658 ns
    against 685,847.858213 ns, ratio **0.991029**, history `r5.group_formation` three times.

Both are byte-identical to D63 §4's own table at those sizes, as they must be — this entry touches no engine file
and no coefficient. **Nothing clears noise, and nothing was expected to**; the point of quoting them is that the
instrument added here is inert to the thing being measured.

**One portability point, caught by reading rather than by CI, and it is the third of its family.** `getloadavg`
is a BSD extension. This project sets `CMAKE_CXX_EXTENSIONS OFF`, so GCC gets `-std=c++20` and defines
`__STRICT_ANSI__`, and glibc's `<features.h>` then declines to define `_DEFAULT_SOURCE` for itself, leaves
`__USE_MISC` off and compiles the `getloadavg` declaration out of `<stdlib.h>` — while Apple clang builds the
same line without complaint, because Darwin's libc gates that declaration on `_ANSI_SOURCE`, which nothing here
defines. CI builds `tools/` on every preset (`cmake --build --preset <preset>`, no target filter), so this would
have been a red ubuntu build on all three of its jobs. Fixed twice over, because there is no GCC on the
measurement machine to test the first fix against: `_DEFAULT_SOURCE` is requested on the file's first line,
before any include can pull `<features.h>` in, **and** the Linux path does not use `getloadavg` at all — it reads
`/proc/loadavg`, which needs no feature-test macro and no declaration. After D46 (GCC's cross-TU FMA contraction)
and D61 (unsequenced operand evaluation order), this is the third GCC/Apple-clang divergence on this project and
the second that only the ubuntu jobs could have caught.

No gate and no mutant: this entry adds no engine line to mutate and changes no decision any program makes.
`ctest --preset release` **111/111** and `--preset reference` **111/111**, 0 failed each, on this tree. The
registry stays at 50 mutants over 57 gate tests, unswept locally here because no file this entry touches is under
`src/` or `include/` at all — CI's ubuntu mutation job runs the merged registry in full and did. No SwapEngine
file opened. No file under `include/epykos/`, `src/` or `tests/` touched.

**CI: `ci_green` = true, all four jobs, twice.** On `dffc0c8`, the commit carrying this entry's only code — the
tool, the `getloadavg` fix and the result record — run
<https://github.com/kbro123/EpykosEngine/actions/runs/36081853231>: ubuntu-latest/release, ubuntu-latest/reference,
ubuntu-latest/mutation and macos-latest/release all **success**. Re-confirmed on `b948244`, this package's last
commit, run <https://github.com/kbro123/EpykosEngine/actions/runs/36082611765>, **success** on all four again. The
two ubuntu compile jobs are the ones that matter for the portability point above: they are the only GCC on this
project, and they built this TU.

**One note on the ladder numbers quoted just above, because the branch moved while this entry was being written.**
`fdd07a5` ("fit at the lane width the interpreter really plans with", D63's own (d2) fix) is an ancestor of this
package's base `581b8e9`, so it was present for every measurement here; the 0.994861 / 0.991029 above are
post-`Lt`-fix figures and match D63 §4's own amended table, not D67's superseded one. Checked rather than assumed.

## D73 — P1: a rewrite replaces a TERM, not a program; e-classes over terms; the identity set; and the recurrence kind of PRINCIPLES.md §2a turns out not to need any of it (2026-09-25)

Implements `docs/PRINCIPLES.md` §8 items 2 and 3. **This entry records a DESIGN, not an
implementation**: `docs/TERM_REWRITING.md`, six interface headers
(`include/epykos/rewrite/{error_model,term,term_rule,recurrence}.hpp`,
`include/epykos/optimise/{term_egraph,term_extract}.hpp`), one `.cpp` of three `to_string` tables
(`src/rewrite/term_names.cpp`) so the enumerations are linkable, and one compile-and-shape gate
(`tests/rewrite/term_interface_test.cpp`). No rule, no cost model and not `src/optimise/egraph.cpp`
are touched. The point is to review the interface before roughly six thousand lines are written
against it. `docs/TERM_REWRITING.md` is the document; this entry records the decisions and the
findings.

**1. What replaces `Proposal`.** A `rewrite::TermRewrite`: an e-class id plus a `TermExpr` — a small
node arena whose operands may be other arena nodes, **e-classes the match bound** (pattern holes, so
a rewrite reuses structure instead of copying it), an existing `ir::Slot` verbatim, or **new data**
(`new_literals` / `new_columns` / `new_gathers` / `new_segments`). A term is defined as the sub-DAG
rooted at one `Step` of one `Group`, denoting one scalar per ROW of that group's domain; the domain
is the term's *anchor*, so a term is a row-indexed vector, not a scalar. Two consequences are
recorded as contract rather than commentary: **a term rewrite applies to every row of its anchor
domain** (that is what a domain IS), and **a rule can never rewrite a subset of rows** — a rewrite
valid for only some rows must first split the domain, which is `r2.bucket_rows`' job at the program
tier. The term layer therefore structurally cannot fragment the domain partition, which is the
failure D65 measured (R2 without its floor: 10 domains to 19,624). Side conditions are row-universal
and the IR can usually discharge them EXACTLY, because a Column is a `std::vector<double>` sitting in
the Program; the price, stated, is that such a proof is about THIS RECORDING.

**2. The `new_*` data vectors are a requirement, not a generality.** Discovered by working
PRINCIPLES.md §2a through: the arithmetic-series closed form needs a per-row step index, which is a
Column that does not exist yet. A replacement language that could only rearrange existing nodes could
express exactly one of §2a's four classes.

**3. E-classes over terms, and which of D62's mechanisms survive.** The anchor domain is PART OF A
NODE'S IDENTITY (two structurally identical terms in different domains denote different row vectors
and must not be unioned; `anchors_consistent()` is the gate). Gathers are an explicit index-map
term-former: congruence handles `Gath(g,t) = Gath(g,t')`, while `Gath(g,f(t)) = f(Gath(g,t))` is a
RULE (`r4a` generalised) and is the bridge every cross-domain identity crosses. Of D62: the
**application memo SURVIVES, re-keyed** from (program node, rule, site) to **(e-NODE, rule,
binding)** — e-nodes are immutable under hash-consing while CLASSES merge, so a class-keyed memo
would go unsound on the first union; bindings are canonicalised at lookup, so a merge costs redundant
work, never wrong work; still sound for exactly D62's reason (rule purity, which is why `TermRule` is
const and the DRIVER interns, not the rule). **Representative-only matching becomes UNNECESSARY**
(hash-consing leaves no congruent duplicate to skip), and with it D49's flagged per-node plan-tier
limitation at this tier. **`RefirePolicy::NoFreshCrossRule` becomes UNNECESSARY and should be deleted
here**: it existed solely to bound D62's cause (B), and the completeness D62 booked away ("any
genuine optimum that needs two different structural rewrites composed after both have already fired
once is no longer reachable at all") is RECOVERED, because k independent edits are k choice points,
O(k) nodes, not 2^k clones. **`PipelineOrderedPlans` survives unchanged in the PLAN tier, which this
design does not touch** — see finding 11.

**Why this is tractable at all, and it is the headline**: a term e-graph over the Stage A tape is NOT
517,036 nodes. `ir::infer` already collapses those to 67 domains (80 after R2/R1) of a few Steps
each, so the term graph is on the order of 10^3 nodes. The 517,036 : 67 collapse IS the sharing and
it happens before the optimiser is reached. (The collapse is measured, D35/D44/D65; the 10^3 is an
estimate and is labelled as one.)

**4. The identity set: 32 named axioms (`rewrite::Axiom`), three exactness buckets.** Ten
unconditionally exact in IEEE-754 (`neg_neg`, `sub_as_add_neg`, `neg_sub`, `mul_one`, `div_one`,
`select_same_arms`, `select_push_unary`, commutativity of Add/Mul/CmpEq — the last already asserted
bitwise by `op_is_commutative` and canonicalised by D61 — plus both gather commutations, which are
exact because a gather RE-INDEXES rather than computes, `r4a`'s own E0 declaration being the
precedent). Eleven exact only once a row-universal side
condition is discharged, each naming its obligation — notably **`add(x,0) = x` is NOT unconditionally
E0**: it fails at x = -0, and the engine can observe the difference through `Recip`, `Div` and
`Sqrt` (comparisons cannot, which is why it is easy to miss). The rest are identities in R and not in
float (E1), each carrying an `ErrorTerm`. Two of them — `fma_contract` and `mul_recip_as_div` — are
faster AND more accurate, i.e. inadmissible under the M1-M4 contract and preferable under
PRINCIPLES.md §4; `axiom_improves_accuracy` exists, and is gated, so that demotion cannot be quietly
reverted.

**5. `Op::Sum` and `Op::Affine` get NO associative-commutative matching, and the flag defaults off
under a test.** Three independent reasons: (i) Sum is a fixed-arity LEFT FOLD in operand order, so
reordering changes rounding — "AC on Sum" is not one E0 axiom but an unbounded family of E1 rewrites
with unbounded error; (ii) **D61 decided this already in the other direction**, canonicalising a
commutative STEP's operands and deliberately not a Sum's member order, and AC matching would silently
reverse that and reopen `task_919ea449`'s class of defect; (iii) Stage A's leg sums have hundreds of
members and n! orderings admit no bound. The one Sum rewrite allowed is `sum_factor_common`
(`Sum(c*x_i) = c*Sum(x_i)`), which removes n multiplies and reorders nothing.

**6. The recurrence kind of §2a is NOT a term rule, and this is a finding.** A scan domain's rows are
the STEPS of its chains (D41, chain-major); a closed form has one value per CHAIN. Collapsing a
recurrence therefore deletes rows, changes a row count and re-points gathers — a DOMAIN-level edit,
the kind `r5`/`r7` make, and exactly what a term rewrite is defined not to do. So it slots into the
EXISTING `rewrite::Rule` interface, and PRINCIPLES.md §6 item 1's combinatorial objection does not
apply to it: **one site per scan domain, five scan domains on the whole Stage A tape.** Sequencing
consequence, for the owner: PRINCIPLES.md §8 puts term rewriting (item 2) before telescoping (item
4), and item 4 does not depend on item 2. Telescoping can be attempted FIRST, as one rule with a
classifier, and would then be the evidence that decides whether the term layer earns its six thousand
lines.

**7. A recurrence match is PROVED by exact checks on tables the Program already holds.** Four
obligations, every one decidable without sampling or numerics: (P1) step shape; (P2) the INDEX
CONDITION `gathers[den].index[r] == gathers[num].index[r+1]` inside every chain — an equality of
`int32_t` VALUE IDS, which proves the two discount factors are literally the same recorded value and
cancel exactly in R; (P3) the COEFFICIENT CONDITION, a bitwise comparison of the `ObsDay::weight` and
`ObsDay::tau_rate` Columns; (P4) liveness, that nothing outside the domain reads an intermediate row.
`RecurrenceProof` carries each as its own boolean plus a written `account`, so D53's "state whether
your deliverable actually fires" is answerable as "it did not, and here is the obligation that
failed". (P3) is not a formality: **the two-day-lookback blueprint passes (P2) and fails (P3)**, so a
matcher checking only index structure would emit a wrong answer (that blueprint is not drawn by the
Stage A trade mix, so it is a correctness case and the classifier's best negative test rather than a
share of this book), and **the lockout blueprint
telescopes on a PREFIX** (the frozen rate dates repeat, breaking (P2) on the tail), which is why
`prefix_steps` exists. Two closures are emitted: `ChainFinal` (needs (P4); the 250:1 form) and
`PerRow` (same row count, a divide instead of a multiply, but PARALLEL instead of D41's sequential
waves) — which pays is the cost model's question, so the rule offers both.

**Also found while checking (P3): `maths/instrument/tables.hpp`'s comment is over-broad.** It says
"lookback / observation shift / lockout: the span and the weight differ, and it does not [telescope]".
Reading `src/conventions/rfr.cpp`, `ObservationShift` shifts `obs_start` and `obs_end` TOGETHER and
the loop then takes both the weight and the rate's own span from the same shifted business days, so
`w_k == tau_k` still holds and it DOES telescope; it is `Lookback` that breaks it, moving the rate
date independently of the accrual day. Worth 10 points of Stage A's trade mix that a reader of the
comment would write off. Recorded as a code-reading claim, not a measurement; the check is finding 14
(a). The comment is left unedited — this package changes no maths.

**8. Error: ulps do not compose; adjoint-weighted absolute perturbations do.** An ulp is not a scale,
and even relative error is amplified without bound by cancellation — Stage A's own recorded forward
rate is `(DF_s/DF_e - 1)/tau` with the ratio within ~1e-4 of 1, so that subtraction multiplies
incoming error by ~1e4. The model adopted: each rewrite injects an absolute perturbation at its term,
and `d(output) = sum |d(o)/d(t)| * |dt|`, which composes because it is linear and is nearly free HERE
because `d(o)/d(t)` is what the mechanical adjoint already computes (M2, D31). Stated as sharply as
possible in both the header and the document: **this is an ESTIMATE and the SEARCH heuristic; the
GATE is PRINCIPLES.md §4's oracle, run once on the extracted candidate.** Two known invalidities are
handled explicitly rather than hoped away (across a `Select`, where the first-order effect is zero
and the real effect is the arm difference; and where a term's value can vanish, so relative error is
undefined). Tolerances are per output class (`Valuation`/`Sensitivity`/`Diagnostic`), so extraction is
**constrained single objective** — minimise cost subject to error <= budget — not a Pareto search;
`ErrorBudget::exact_only()` is the default and reproduces the M1-M4 contract exactly, which is how
this can land without invalidating M1-M3. Extraction prices the WHOLE lowered, DCE'd program, not the
term, because a telescope's large win is a dead-code consequence in a different domain (about 88 of
91 discount factors die, and `exp` was 54% of the M1 book's B=64 time, D27).

**9. Saturation bounds, and what completeness they cost, in D62's style.** Four mechanisms replace
`RefirePolicy` at this tier: no AC on Sum/Affine (gives up pairwise/Kahan re-summation, a real
accuracy improvement now unreachable); bounded pattern depth 4 (a deeper rule must be split in two);
per-rule banning with exponential backoff, egg's mechanism (**this is what D65's R2 needed and did not
have** — at 10,111 sites it would have been banned in round 1 with a logged reason instead of running
700 s and blowing the bound; gives up confluence, mitigated by logging every ban so "found nothing"
and "was banned from looking" stay distinguishable); and per-group scoping with a quarantined,
single-reader-only cross-domain phase (gives up identities spanning three domains through shared
gathers — and since CSE shares discount factors by construction, this **may block the very shape the
telescope needs**, which is a second independent reason finding 6 matters).

**10. Migration: the seven layout rules are WRAPPED, not ported.** Three tiers — TERM (new), PROGRAM
(kept, for domain-restructuring rules), PLAN (kept, untouched) — with the term tier as a SUB-SOLVER
that extracts once per error tier and contributes O(tiers) program nodes, not O(matches). R1, R2,
R4b, R5 and R7 stay on `rewrite::Rule` (they change the domain partition, which is not a term
rewrite); R6 stays in the plan tier; R3 and R4a ARE term rewrites and should move later. Cost of
wrapping, stated: R3/R4a exist twice until they move (harmless — the program tier hash-conses on
`ir::serialize` — but wasteful and a confusing log, so the driver should disable the program-tier
copy), and the program tier keeps `NoFreshCrossRule` and its completeness loss for the demoted layout
rules, which is the right trade for rules PRINCIPLES.md §7 has demoted.

**11. What this design does NOT fix, recorded so it is deliberate.** The plan tier is still a
whole-`PlanAnnotations`-valued e-graph with D62's 2^k lattice (105,977 plan nodes / 10.5 GiB on 60
trades), and term-level rewriting does nothing for it because a plan is not a term. Given D68 (the
whole plan stage is 1.027x-1.042x on Stage A and a PERFECT plan-level cost model is worth ~0.08% of
its wall clock), the recommendation is not to fix it.

**12. THE LARGEST FINDING: the 250:1 lives where no rule is applied.** Measured in this repository,
this session. `src/solver/residual.cpp:73-77` builds `program_ = ir::infer(slice_.tape)` and
constructs an `exec::Interpreter` and an `adjoint::Adjoint` on it DIRECTLY — no rewrite pipeline, no
e-graph, no cost model, no plan. D68 says the same from the other end ("slice programs no rule is
applied to"). The calibration solve is ~48% of Stage A (PRINCIPLES.md §6 item 6), its instruments are
OIS swaps whose residuals are exactly the compounded-coupon product loops, and PRINCIPLES.md §0 puts
telescoping at "about 250:1 **on the calibration side**". So **the 250:1 cannot be realised by any
amount of term-rewriting work, because the programs it would apply to never pass through the
optimiser.** The fix is small (`ResidualProgram`'s constructor takes an optional optimisation pass)
and is IN SCOPE under PRINCIPLES.md §3, which excludes solver POLICY — Jacobian policy, iteration
strategy, convergence criteria — and explicitly includes "the arithmetic the engine was handed":
rewriting the residual's EXPRESSION is arithmetic, not policy, and leaves the iterates and the
convergence criterion untouched.

**13. What PRINCIPLES.md §2's discover-only stance over a FROZEN op set costs, plainly, as asked.**
It works, for the thing that matters: the telescope needs only `Div`, its proof is exact index
arithmetic, and it is a general fact about scans with no instrument knowledge in it — §2's
form/declaration boundary holds cleanly and (P2)/(P3) are that boundary made mechanical. The bill:
**three of §2a's four recurrence classes lose their asymptotic win.** Geometric and
linear-constant-coefficient need `r^n`, which must be spelled `exp(n*log r)` — legal, but E1, needing
a positivity proof, and trading n multiplies for one `exp` and one `log`, which on the measured libm
cost breaks even near n = 10 and is a small constant factor beyond it. A handful of identities are
inexpressible for want of `Abs` (`sqrt(x*x)`) and `Log1p` (the `(DF_s/DF_e - 1)` cancellation, tier 2
anyway). Twenty-eight of the thirty-two axioms need no op the engine lacks. If the owner ever
reconsiders, the single highest-value addition is `Pow`, which would move the two exp/log classes
from constant-factor to asymptotic. Noted for a future decision; not requested here.

**14. The cheapest experiments, ranked, because this package deliberately measures nothing.** (a) One
afternoon, existing tooling: over Stage A's five scan domains, print `shape_string`, check (P2)'s
index equality and report the longest prefix it holds on, bitwise-compare the coefficient columns,
scan for reads of non-final rows, and report each domain's share of measured wall clock from the
existing `EPYKOS_EXEC_PROFILE` table. Four booleans and a number per domain, and it answers — before
any interface is written — whether PRINCIPLES.md §8 item 4's worked example is available at all and
whether it is worth anything on this fixture. (b) Hours: `ir::to_string` one `ResidualProgram`'s
`program_` for the USD SOFR block and confirm finding 12. (c) Hand-construct the telescoped program
once by editing the tape, price it with `estimate_program`, then run it — two numbers that separate
"the cost model cannot see the win" from "the plumbing of finding 12 is the blocker". Doing (a) and
(b) before implementing anything is this package's own recommendation.

**Gates.** `tests/rewrite/term_interface_test.cpp`: every header compiles under every preset and both
compilers (D46's lesson applies to headers nobody has instantiated as much as to any other); every
`Axiom` and `RecurrenceClass` has a distinct name, so the enum and `TERM_REWRITING.md` §3's table
cannot drift apart silently; the exactness buckets partition; `axiom_improves_accuracy` is non-empty
(if it ever empties, §4's demotion of bit-identity has been reverted); the default budget is
exact-only, `ac_matching_on_sum` is off and `max_pattern_depth <= 4`. No mutant is registered: there
is no behaviour to mutate, and CLAUDE.md's mutation requirement attaches to a rewrite, which this
package does not ship.
