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
