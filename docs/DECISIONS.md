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
