# PS2 release gates

The authoritative gate is `tools/acceptance/gate.ps1` (`docs/architecture/acceptance-gate.md`); this page lists what
each engineering batch must pass and where each check runs.

| level | check | where |
|---|---|---|
| 1 build | MSVC (Windows), GCC and Clang (Linux) | CI `build.yml`; local gate (MSVC) |
| 2 unit | `ps2x_tests`, ctest (incl. Z3), `ps2xIOP` | CI; local gate |
| 3 formal | `z3_int_semantics` (integer properties only) | ctest |
| 4 sanitizers, fuzz | ASan+UBSan unit/e2e, 60 s fuzz smoke per harness | CI `linux-clang-asan-ubsan`, `linux-clang-fuzz-smoke` |
| 5 differential | VU1 lockstep vs reference mode, decoder/codegen contract | unit tests |
| 6 hardware | 55 ps2autotests programs vs real-PS2 output | local gate only (ps2autotests not vendored) |
| 7 integration | `PS2_SYSTEM_INTEGRATION.md` | unit/e2e where they exist |
| 8 homebrew | compatibility stages 5-9 | not available yet |
| 9 stability | long runs | not available yet |
| 10 performance | `ps2x_bench_vu1` A/B (`docs/architecture/benchmark-methodology.md`) | local; CI checks correctness only |
| dashboard | `validate_dashboard.py`, `hwcoverage.py report --check` | CI `status-dashboard` |

A batch is acceptable when levels 1-6 show no regression against the previous gate, every new behaviour has tests that
fail on its mutants, and the inventory changes only with matching evidence. Passing level 2 never implies level 6.
