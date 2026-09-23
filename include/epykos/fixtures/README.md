Test-only fixtures, not engine API: seeded books (`m1_book`), their oracle pricers and tables (`m1_price`, `m1_reference`), record helpers (`record_m1`) and forward-mode helpers (`m1_tangent`: tangent, Jacobian by Dual<1> passes and by one Dual<12> pass), namespace `epykos::fixtures`.
Everything is generated from the seed in `docs/WORKLOADS.md`; there are no data files. Milestone names appear here only as fixture names.
No engine header (`include/epykos/<component>/`) may include a header from this directory; tests, benches and `bench/hand/` may.
