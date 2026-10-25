# VU0 macro mode: architectural state audit (Phase 5 merge gate)

What each COP2 macro instruction class does to VU0 state in Zmbor today, from the recompiler's translation
(`ps2xRecomp/src/lib/vu_translator.cpp`, `vu_translation_helpers.cpp`) and the runtime helpers
(`ps2xRuntime/include/ps2_runtime_macros.h`). "Tested" names the test that checks it; "hardware" says whether a
real-PS2 row (evidence register category A) covers it. **Macro and micro mode are not equivalent while the gaps below
remain.**

| class | VF / ACC written | MAC | STATUS (Z/S/U/O, I/D, sticky) | CLIP | tested | hardware |
|---|---|---|---|---|---|---|
| VADD/VSUB/VMUL (+ bc/q/i forms) | vd under the dest mask (`_mm_blendv_ps`); value from `ps2_vu_fmac::add4/sub4/mul4` | **not updated** | **not updated** | – | e2e `vu_macro`, PS2VuFmac unit tests | no (cpu/vu0_macro covers integer ops and transfers only) |
| VMADD/VMSUB (+ bc/q/i), VOPMSUB | vd under the dest mask; product rounded, then the add (contract MA) | **not updated** | **not updated** | – | e2e `vu_macro` (VMADD), code generator test (VOPMSUB lanes) | no |
| VADDA/VSUBA/VMULA/VMADDA/VMSUBA (+ forms), VOPMULA | ACC under the dest mask | **not updated** | **not updated** | – | e2e `vu_macro` (VMULA), code generator test (OPMULA) | no |
| VMAX/VMINI (+ forms) | vd; host `_mm_max_ps`/`_mm_min_ps` | – | – | – | **none** | no; exponent-255 / denormal operands compare as host NaN/denormal, unlike the micro interpreter (normalised compare): **divergence, unmeasured** |
| VABS, VMOVE, VMR32 | vd / ft under the dest mask (bit operations) | – | – | – | VABS decode only; VMOVE/VMR32 **none** | no |
| VITOF*/VFTOI* | ft under the dest mask; host conversion | – | – | – | **none** in macro mode (micro ITOF has one) | no |
| VCLIPw | – | – | – | shifted in, 6 new bits from host compares | **none** in macro mode (micro CLIP timing has one) | no; exponent-255 operands compare as host NaN: unmeasured |
| VDIV/VSQRT/VRSQRT | Q | – | I/D and IS/DS set (`ps2_vu_div/sqrt/rsqrt`) | – | "VU0 macro DIV/SQRT/RSQRT follow the FDIV-unit rules and set I/D" | rules from PCSX2 VUops (emulator, category C) and the EE FPU rows: provisional |
| VWAITQ | none (Q is resolved immediately) | – | – | – | – | timing not modelled |
| VRINIT/VRNEXT/VRGET/VRXOR | R / ft | – | – | – | generated-code formatting only; "VU0 microprogram preserves the architectural RNG state" | R read-back mask: one row (transfer) |
| CFC2/CTC2 control registers | vi16-vi31 per the write masks | MAC read-only | sticky bits writable only | writable | hwtest `cpu_vu0_macro_transfer` | **yes** (transfer.expected) |
| VCALLMS/VCALLMSR | runs the micro interpreter; VF/VI/ACC/Q/I/R/MAC/CLIP/STATUS copied in and back | from micro | from micro | from micro | "VU0 microprogram executes against VU0 code and data memory" | partly (vu tests run microprograms) |

## Gaps and why they stay open

- **MAC/STATUS after macro FMAC operations.** A `CFC2 vi17` (MAC) or `vi16` (STATUS) after a macro VADD returns the
  value left by the last microprogram or CTC2. Implementing "same as micro" would be an assumption: no real-PS2
  row here observes macro-mode flags, and the only implementation available to compare with is PCSX2's (emulator
  output is not hardware evidence). Probe needed: macro FMAC on boundary operands, then CFC2 vi16/vi17
  (`HANDOFF.md` next milestone, item 2).
- **MAX/MINI/CLIP on exponent-255 or denormal operands** differ between macro (host compare) and micro (normalised
  compare). Which one the hardware does is unmeasured; both stay as they are.
- **Flag timing** (the micro interpreter's four-cycle flag pipeline) has no macro-mode counterpart.

Nothing in this list was changed at the merge gate: each correction needs evidence first.
