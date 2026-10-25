# Control-flow invariants of the recompiler

Phase 5 made **no** change to the recompiler's control-flow analysis, IR or emitter. This file states the invariants
that the existing code relies on and the tests that hold them, so that a later change to the IR can be checked
against them first. Evidence classes as in `evidence-register.md` (section 3 there covers hardware behaviour).

| # | invariant | why | held by (ps2x_tests names, abbreviated) |
|---|---|---|---|
| I1 | Every branch and jump executes exactly one delay-slot instruction (zero for an annulled likely branch), taken from the ELF even when it lies past the decoded range or the function end | MIPS semantics (A: cpu/ee/branch) | "a delay slot past the decoded range is translated from the ELF section", "a delay slot past the function's end is still translated", "branch-likely places delay slot only in taken path" |
| I2 | A delay slot that is itself a branch target gets its own label; entering there executes only the slot | branch into a delay slot is legal guest code | "labels delay slot when it is a branch target" |
| I3 | A control transfer inside a delay slot follows register entry 3.2 (provisional) | three hardware rows | `cpu_ee_branchdelay`, `cpu_ee_fpu_branchdelay` hardware tests |
| I4 | Every address the guest can return to (JAL/JALR fallthrough, SYSCALL fallthrough, loop heads that return to the dispatcher) is a resumable entry | the dispatcher may resume any of them after a callback, thread switch or exception | "JAL return addresses get resumable entry wrappers", "SYSCALL fallthrough is a resumable entry", "backward BEQ returns to the dispatcher through a resumable loop head", "control-flow analysis promotes JALR fallthrough as a resumable entry" |
| I5 | A jump whose target is unknown at recompile time sets `pc` and returns to the dispatcher; it never falls through | correctness over speed | "jump to unknown target sets pc", "branches outside function still set pc", "unresolved JALR uses runtime dispatch without broad local switch" |
| I6 | Internal targets stay labels (goto), external known targets become calls by name followed by a pc check | keeps functions closed; cross-function jumps are entries | "JAL to internal target becomes goto", "jumps to known symbols call by name", "JAL to known function emits call and check", "control-flow analysis reports cross-function mid-function jumps as external entry candidates" |
| I7 | Branch conditions use the full 64-bit EE register (bit 63 for signed compares) | EE GPRs are 64-bit (A) | "signed branches compare the full 64-bit EE register" |
| I8 | Jump tables are only used when detected by the canonical pattern or configured; otherwise I5 applies | a wrong table is silent corruption | "jump-table detection finds canonical sltiu/bne/lw/jr pattern", "configured jump table addresses drive JR dispatch targets" |

Known gaps (register 3.3): no precise-exception restart model inside delay slots beyond `in_delay_slot`/`branch_pc`;
register jumps and calls inside a delay slot fall back with a warning.

Rule for IR changes: state which invariants the change touches, keep or extend their tests, and rerun the
`cpu/ee/*` hardware tests plus the e2e programs before and after.
