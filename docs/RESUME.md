# RESUME — EpykosEngine handoff (2026-09-22)

For the next session (local, remote-control, on the Mac Pro). Read this, then `CLAUDE.md`, then `docs/DESIGN.md`.

## 1. State of the repo

- `kbro123/EpykosEngine` was created empty today. Local clone in the cloud session: `/home/user/epykosengine`, branch `main`.
- Files written in this session (design only, **no code**):
  `README.md`, `CLAUDE.md`, `.gitignore`, `docs/DESIGN.md`, `docs/ROADMAP.md`, `docs/DECISIONS.md`, `docs/PRIOR_ART.md`, `docs/RESUME.md` (this file).
- Owner decisions taken in this session (recorded in `docs/DECISIONS.md`):
  - M0 = **design doc only**; code starts at M1.
  - **Standalone** repo; SwapEngine/QuantLib are test-only oracles later, never build dependencies.
  - Initial scaffold goes to **`main`** (repo had no history); later work on branches.
- Owner intent: iterate on the design doc first; then, tonight, spin up agents to work on it until morning.

## 2. Where this came from (context you need)

SwapEngine (`kbro123/SwapEngine`, `/home/user/SwapEngine` in the cloud session, Mac Pro checkout on `main`) is the existing curve-calibration/streaming
engine. Its parked branch `research/aad-graph-kernels` (one commit, `8a16781`, cut 2026-09-15 from `e2-conventions`, 33 commits behind `main`,
DO NOT MERGE) recorded the engine's unmodified templated pricing into a scalar tape and showed:
- CSE/DCE shrinks desk from 141.8k to 15.3k ops; the affine collapse **rediscovers the hand-built W-cache** (857 rows, exactly);
- coarse lowering (level/op-type groups + fold-sum) cuts dispatch to 122–184 per eval, bit-identical under `-ffp-contract=off`;
- a cashflow-table IR lowers 408/420 ladder rows (exception: the moment path, a quadratic form);
- the scalar tape is **1.5–2.5× slower** than the hand W-cache on linear books; **7–11× faster** than the hybrid AAD path on value-dependent ones;
- desk_mixed's per-tick refresh is a Newton 2-cycle across four Hyman predicates — kinks need an active set, like band edges.
- **Never measured:** coarse-kernel timings vs the W-cache; select-vs-guard break-even; NaN-in-discarded-arm.

Since that branch, SwapEngine `main` landed the piecewise-linear W tier (per-Hyman-cell W + rank-k re-take, default routing), ONE row model
(terms × transform × row map), and `NormalOp` (one pseudo-inverse). Current baselines (fingerprint `966685f93279`): desk_mixed residual (PWL) 4.7 µs,
Jacobian ~181 µs, **stream tick 551 µs vs desk 22 µs** — the 25× gap is refresh scheduling / kink cycling, not kernel speed.

Rebuild any research probe from the SwapEngine root:
`xcrun -sdk macosx clang++ -std=c++20 -O3 -DNDEBUG -march=x86-64-v3 -fno-math-errno -w -I include -I third_party/eigen -I build/generated -I bench/fixtures <probe>.cpp -o <probe>`
(add `-ffp-contract=off` for bit-identity runs).

## 3. The design in one paragraph

Stage computation by rate of change: **structure** (record once, fold), **state** (the compiled program's inputs), **batch** (innermost SIMD axis).
Maths is written once, templated on `Scalar`; recorded with a `Rec` scalar whose **constants are leaves**; a **signature pass** (hash-cons modulo
constants) infers **domains** (Knots → Times → Subs → Coupons → Legs → Rows), columns, gather indices, segments and scan recurrences; **fusion
rewrites R1–R7** (each with an exactness class E0/E1) reproduce every hand fast path (`cpn_is_plain`, `sub_is_identity`, shared `INV`, block GEMV);
execution is **catalogue (AOT C++) → tiled interpreter → batch mode**, **no JIT**; adjoints are reversed group-by-group with pull-transposes;
value branches are `select` (mask + margin + arm gap exported), solvers are `implicit` nodes, active sets are `pin` with a solver-owned mask.
Full text: `docs/DESIGN.md`. Milestones and kill gates: `docs/ROADMAP.md`.

## 4. Open questions for the design review (resolve before M1 starts)

1. **Op set completeness.** Is `scan` enough for compounding/survival/path evolution, or does MC time-stepping need an explicit `step` op with a
   state vector? Does `quadform` earn its place or should the moment path stay a fallback?
2. **`Rec` representation.** `{double v; int id}` plus an Eigen `NumTraits`? Do we record Eigen expressions elementwise (simple, large tapes) or
   intercept `Matrix<Rec>` products as `linmap` directly (smaller tapes, more recorder code)?
3. **Signature boundaries.** Fan-out > 1 as a boundary may over-fragment; confirm on the M1 book that Times/Coupons/Legs come out as expected.
4. **Batch-axis layout.** SoA with batch innermost on every domain (row-major grids) — decide tile size (256?) and whether batch width is a
   compile-time constant per kernel or runtime.
5. **Catalogue mechanism.** Build-time tool: run reference workloads → hot group signatures → emitted C++ → compiled in. Where does the catalogue
   live in the tree, and how is coverage reported?
6. **M1 workload definition.** Flat/linear curve + compounded OIS par swaps, 1k-swap book, single state + batch 64. Fix the exact book so the
   hand-fused reference and the generic path price the same thing.
7. **Toolchain & deps.** Proposal (mirrors SwapEngine): C++20, CMake + Ninja, Eigen (header-only), GoogleTest, Google Benchmark, vendored under
   `third_party/` (gitignored, fetched by a bootstrap script), ISA auto-detect, no Homebrew deps. Confirm.
8. **License.** SwapEngine is proprietary; nothing added here yet. Decide before the first push if it matters.
9. **Numerics policy.** Reference TUs built with `-ffp-contract=off` for E0 gates; production may contract. Confirm.
10. **FMA/select NaN discipline.** `m/|m|` → `copysign`, etc. — list the safe-arm rewrites the maths must use.

## 5. Suggested overnight plan (M1 kill test) — for when the design is settled

Run as a Workflow (multi-agent) with an adversarial review pass; each package has an explicit interface so they can proceed in parallel and meet
at the gates. Nothing lands on `main` without the gates.

| package | deliverable | interface / gate |
|---|---|---|
| P0 scaffold | CMake/Ninja skeleton, `third_party` bootstrap, gtest/benchmark wiring, fingerprint script | builds on macOS (Apple clang 21) and Linux/GCC |
| P1 maths | templated `Scalar` kernel: linear curve, compounded OIS par swap, fixed/float legs; `double` instantiation = oracle | unit tests vs closed forms |
| P2 recorder | `Rec`, `RecBool`, taint, constants-as-leaves, `select`/`structural_if`, tape + CSE/DCE + fold-sum | records P1 unmodified; tape replay E0 vs `double` under `-ffp-contract=off` |
| P3 signature pass | hash-cons modulo constants → domain IR (domains, columns, gathers, segments); **round-trip identity** expander | identity on every test book |
| P4 interpreter | tiled vector interpreter over domain IR groups (no catalogue) | E0 vs P2 replay |
| P5 hand reference | hand-fused kernel for the same book (gather → fma → segment_sum, shared reciprocal, block GEMV) | E1 vs P4 (≤1e-12 rel) |
| P6 benchmark | 1k-swap book, single state and batch 64; fingerprinted; load-checked | **go: P4 ≤ 1.3× P5 single-state, ≤ 1.1× batched** |
| P7 review | adversarial reviewers on P2–P5 (missed branches, hidden allocations, non-determinism), completeness critic | findings fixed or filed |

Order: P0 → {P1, P2, P5} → P3 → P4 → P6 → P7. P5 can start from P1's `double` path immediately.

## 6. Session pointers

- Cloud session (this handoff): `https://claude.ai/code/session_01TMGoPNGUCq4KpWegJv1i4u` (SwapEngine branch `claude/generic-kernel-tape-3d8m1d`, no changes pushed there).
- Earlier remote-control session on SwapEngine today ("Curve definition with boundaries", archived 16:52Z): artifacts "SwapsEngine Speed Ledger" and "SwapEngine Generality Review".
