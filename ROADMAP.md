# Zmbor roadmap

Order is by priority. `HANDOFF.md` has the details and the current measured state.

1. **Remaining PS2 hardware accuracy.** Two hardware programs still mismatch: FPU RSQRT.S rounding
   (`cpu_ee_fpu_sqrt`) and the last bits of the EFU series (`vu_lower_efu`). Both need new hardware measurements;
   the six other failures of the original list were fixed in Phase 4. The next concrete steps are in
   [docs/architecture/phase8-roadmap.md](docs/architecture/phase8-roadmap.md) and the per-component state in
   [docs/status](docs/status/README.md).
2. **Native recompilation.** Make function discovery more robust on stripped ELFs, generate faster code (optimized
   builds of generated code), and reduce how often execution falls back to runtime dispatch.
3. **Broader VU and IOP execution.** Cover more VU1 microprogram paths (XGKICK, the timing VIF and GIF depend on) and
   more IRX modules executed natively rather than through HLE.
4. **Advanced rendering backends.** Separate the GS frontend from the renderer and add hardware-accelerated backends
   that keep the GS cache regression tests passing.
5. **Automated PS2 game analysis.** Automatic configuration of stubs, function maps and per-game overrides from ELF
   analysis.
6. **Compatibility testing with legally supplied game data.** Only with game data the user supplies themselves. No
   game data or BIOS files are ever committed.
7. **Native port generation.** Produce packaged native builds from a verified recompilation.
8. **Professional developer GUI.** Build on `ps2xStudio` to inspect recompilation, runtime state and test results.
9. **Long-term compatibility.** Track regressions per game build and keep the hardware matrix at or above its
   current level.
