# tools/coverage/ — the generated coverage matrix between this engine and SwapEngine.
#
#   coverage [--out <path>] [--no-out] [--stdout] [--blueprints <dir>]
#
# Emits `bench/compare/COVERAGE.md`: three matrices (calibration instruments, problem families,
# capabilities around the problem) and the headline fraction of their surface we can compare on
# today. Our side of every matrix is GENERATED — the shipped enums are discovered at run time and
# `blueprints/` is loaded through the engine's own loaders, so adding a product moves the output
# without anyone editing it. Their side is a dated, checked-in SNAPSHOT that cites a file and a
# symbol per row, which the artefact says plainly on every run.
#
# This tool takes NO timings and makes no performance claim — it is a capability inventory — so
# unlike tools/ladder/, tools/pullprobe/ and tools/exotics/ it does not read the load and does not
# refuse a loaded box. It is safe to run anywhere.
#
# It links `epykos` only, exactly like the other tools here. In particular it does NOT compile
# against the other checkout: D11's relaxation (2026-10-09) permits READING that code, and the
# snapshot table in coverage_main.cpp is the result of reading it, but nothing is copied or
# ported and no header of theirs is included. `tools/h2h/` remains the only target in this
# repository that compiles against their facade, and it stays behind -DEPYKOS_H2H=ON.
#
# Built whenever EPYKOS_BUILD_TOOLS is (default ON); like tools/catalogue/ and bench/, never run
# by ctest or CI. It reads the fixture-only headers under include/epykos/fixtures/ (D28), which is
# allowed for a tool and not for engine API.
add_executable(coverage coverage_main.cpp)
target_link_libraries(coverage PRIVATE epykos epykos_warnings)
