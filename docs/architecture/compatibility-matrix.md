# Compatibility matrix

`tools/compat/compat.py collect` turns the results of a program set run through the pipeline (today: the hardware
test programs prepared by `tools/hwtest/hwtest.py`) into a machine-readable matrix; `validate` and `selftest` (ctest
`compat_matrix_selftest`) enforce what an entry may claim.

## Stages

| stage | name | evidence the collector requires |
|---|---|---|
| 0 | input_recognized | `prepare.json` written (the recompiler parsed the ELF) |
| 1 | recompilation | no `RECOMPILE_FAIL` |
| 2 | native_compilation | the program executable exists (its SHA-256 is recorded) |
| 3 | runtime_initialised | the process started and produced guest output |
| 4 | stable_checkpoint | the program's end marker was reached and the process exited 0 |
| 5-9 | interactive, main_workload, extended_execution, persistence, broad_compatibility | **never credited**: no input scripts, renderer checks or save tests exist yet; entries list them under `untested_stages` |

Correctness against a reference (real-PS2 `.expected` output) is a separate field (`reference`), with the first
diverging output line. Rendering, input, audio and save/load are `untested` unless an entry carries evidence; the
validator rejects `pass`/`fail` without it, stages above 4, failure class `none` without a reference PASS, unknown
failure classes, compilation credit without a binary hash, and private paths.

## Failure classes

`recompiler_crash`, `code_generation_failure`, `host_compiler_failure`, `unsupported_instruction`,
`unresolved_guest_call`, `guest_exception`, `memory_ownership_failure`, `scheduler_deadlock`, `iop_service_failure`,
`dma_vif_gif_failure`, `graphics_mismatch`, `audio_failure`, `performance_stall`, `host_crash`, `output_mismatch`,
`test_harness_failure`, `external_dependency_missing`, `unclassified`. Runtime messages map to classes through the
rules at the top of `compat.py`; the first matching rule wins and each rule names the message it relies on.

## Determinism

`--repeat-runner` runs every built program a second time and compares the guest output with the first run.

## Results

Phase 6 baseline (`0bac702` build, ps2autotests set, 55 programs): every program reached stage 4; 53 match the
real-PS2 output, 2 are `output_mismatch` (`cpu_ee_fpu_sqrt`, `vu_lower_efu`); all 55 deterministic over two runs.
These are homebrew hardware-test programs (ps2autotests, ISC), not commercial software: they say nothing about game
compatibility. No commercial or user-supplied program was run.
