# EpykosEngine — Prior art and measured evidence

## 1. SwapEngine research (`research/aad-graph-kernels`, parked 2026-09-15)

A recording scalar was run through SwapEngine's **unmodified** templated pricing. Measured on a Xeon W-3223, engine
flags `-O3 -DNDEBUG -fno-math-errno -march=x86-64-v3`; timings were taken under load (3.5–11) and are ±30 %.

| finding | number |
|---|---|
| desk bundle: recorded ops → live after CSE+DCE | 141,813 → 15,269 (85–89 % duplicates) |
| affine collapse recovers the hand-built W-cache | 857 rows = the compiled engine's DF times, exactly |
| replay allocations | 0 |
| interpreted tape vs templated `double` (`-ffp-contract=off`) | bit-identical on 9 rungs |
| coarse lowering (level/op-type groups + fold-sum) dispatches per desk eval | 15,269 → 122–184 |
| coarse kernel parity, default flags | ≤ 6.6e-15 values, ≤ 3.9e-15 Jacobian |
| cashflow-table IR coverage | 408 / 420 ladder rows; the exception is the moment path (a quadratic form) |
| scalar-tape eval vs hand W-cache, linear books | **1.5–2.5× slower** (per-op dispatch) |
| collapsed plan vs hybrid AAD path, value-dependent books | 7–11× faster eval, 5–7× faster value+Jacobian |
| collapsed fwd+J vs hand analytic Jacobian, desk | 111–116 µs vs 195–264 µs |
| record + collapse cost | 1–25 ms per structure |
| straight-line codegen via clang | 1.4–7 s per structure (background only) |
| Hyman guards per 7-node MonotoneCubic region | 78; linear schemes 0 |
| desk_mixed streaming stall | a Newton 2-cycle across four Hyman predicates (25Y clamp, 30Y end sign) |
| reciprocal rewrite side effect | 5 degenerate-tie guard flips (~1e-20 margins, equal arms) |

**Never measured:** coarse-kernel timings vs the hand W-cache; the select-vs-guard break-even; NaN in discarded arms.
Those are exactly what M1 must settle.

## 2. SwapEngine hand-fused baselines (fingerprint `966685f93279`, single state, one thread)

| metric | SwapEngine | QuantLib | ratio |
|---|---|---|---|
| 1,000-swap OIS book reprice | 25.8 µs | 23.7 ms | ~920× |
| G10 2,000-swap book | 259 µs | 52.0 ms | ~200× |
| UST 5,000-bond curve book | 414 µs | 29.8 ms | ~72× |
| risk ladder vs bump-and-recalibrate | 113 µs | 16.8 ms | ~150× |

These are the numbers the generic path has to match (M1/M3). The fast paths behind them — plain-coupon fusion,
identity sub-periods, shared reciprocals, block GEMV — are what rewrites R1–R7 must rediscover.

## 3. External systems

Licences and product claims are from public material; verify before any dependency or competitive statement.

| system | approach | relevance |
|---|---|---|
| MatLogica AADC | record once; emit x86 machine code during recording; forward + adjoint kernels vectorised across samples; `iIf` for stochastic branches | the closest production analogue; wins on irregular MC payoffs; weak at batch-1 and for very large straight-line books |
| XAD | operator-overloading tape; JIT graph with `If` nodes; native codegen commercially licensed | reference for `select`-style branch nodes |
| CoDiPack / ADOL-C | primal-value tapes (re-evaluable at new inputs); ADOL-C guard/retape, `condassign` | precedent for guards and selects |
| CasADi | symbolic graphs → C with sparse Jacobians; `if_else` | design reference; wrong fit (re-express maths in its DSL) |
| Enzyme | LLVM-level AD of compiled code | no structure specialisation without partial evaluation |
| JAX / XLA | trace, specialise, cache by shape; `lax.cond`/`select` | the right mental model for record/specialise |
| Halide / TVM / XLA fusion | algebraic scheduling and fusion of array programs | fusion rules and cost models |
| MonetDB/X100, DuckDB | vectorised (tile-at-a-time) interpretation | the tier-2 interpreter model |
| ORE | open-source QuantLib-based XVA; I believe recent versions include a computation graph with AAD/GPU for AMC — unverified | the likely open-source competitor on XVA |
| A+ / k / q | array-language verbs over whole vectors; conditional as a vector op | the lineage of "every op is an array op, branches are selects" |
