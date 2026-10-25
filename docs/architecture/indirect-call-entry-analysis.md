# Indirect calls and function-wide resume entries (Phase 6 measurement, not adopted)

## What the recompiler does

`ControlFlowAnalyzer::analyze` (control_flow_analyzer.cpp) marks every instruction of a function as an
`indirectFallbackEntryPoint` when the function contains a JR (non-`$ra`) or a JALR whose target it cannot resolve
to a jump table. `discoverAdditionalEntryPoints` registers those addresses as resume entries of the owner, and the
emitter puts each into the owner's `switch (ctx->pc)`. Every such instruction is then a potential entry point of
the generated C++ function, which keeps the host compiler from optimising across instructions. This is a deliberate
upstream design (PR #140; test "unresolved JALR marks internal labels as indirect fallback resume entries"): a JR to
a computed address inside the function, and a JALR that lands inside a region that function discovery merged
(e.g. `la $t9, label; jalr $t9` on a stripped ELF), can then re-enter through the dispatcher.

## Measurement (ps2autotests set, 55 programs)

| | promotion for JR and JALR (production) | promotion for JR only (experiment) |
|---|---|---|
| JALR sites / non-`$ra` JR sites in the generated code | 2569 / 385 | same |
| additional (resume) entry points | 233,881 | 96,258 (-59%) |
| fallback entries | 308,112 | 36 |
| generated `ps2_recompiled_functions.cpp`, total | 227.0 MB | 208.2 MB (-8%) |
| build of the 55 programs (`/Od`, sccache off, one run each) | 796 s | 752 s (-5.5%) |
| hardware matrix | 53/55, 8202/8250 | 53/55, 8202/8250, guest output identical on all 55 |

Experiment recompiler: `if (!foundTable && jrInst->function != SPECIAL_JALR)` in place of `if (!foundTable)`;
built separately (`build-exp`), never committed.

## Decision: not adopted

- The measured gain is small at `/Od` (one run each; noise unknown). The expected larger effect, better host
  optimisation at `/O2` across fewer labels, was not measured.
- The 55 programs do not contain the case the promotion protects (JALR into a merged region); identical output on
  them does not show the experiment is safe on stripped commercial ELFs. With the promotion removed such a call is
  reported (`reportMissingFunction`), not silently mis-executed, but a program that worked would stop working.
- A principled replacement exists but is not built yet: limit JALR promotion to *address-taken* instructions of the
  function (constants built with LUI/ADDIU or LUI/ORI, and aligned words in data sections that point into the
  function). It needs a test set of stripped ELFs with function-pointer calls into merged regions before it can be
  evaluated; the experiment above gives its upper bound on the benefit.
