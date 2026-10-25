# Resume prompt for the next session

Paste the text below into a new Claude Code session opened at `D:\Project\Zmbor\wt-phase6`.

---

You are continuing Zmbor (https://github.com/m7kor/Zmbor), the continuation of the "PS2Recomp Ultimate" work. It is
principal-engineer work on a PlayStation 2 static recompiler and runtime built on the upstream project
ran-j/PS2Recomp.

- Communicate with me in Arabic; keep technical identifiers and code in English.
- Evidence only: never claim a result you did not measure. Emulator output, model predictions and Zmbor output are
  never hardware evidence (`docs/architecture/evidence-register.md`).
- Never edit hardware `.expected` files to make tests pass; no fitted 1-ulp corrections or tuned constants.
- Never represent unit or hardware tests as proof that a commercial game works.
- Never download or commit commercial games, BIOS files or unrelated binaries.
- Commit locally with `git -c user.name=m7kor -c user.email=alshariefk@gmail.com commit ...`.
- Zmbor `main` is protected: work on a branch and open a pull request only when I ask.
- Push only when I ask; never force-push; never rewrite history.
- Never modify m7kor/PS2Recomp or ran-j/PS2Recomp, other worktrees, or the read-only RE4 locations
  (`D:\Project\compatbilty`, `C:\Project\PS2redmption`).

Read `HANDOFF.md` first (Phase 6 section), then the architecture documents it lists. `CHANGELOG.md` has every change
with its root cause and measurements.

Work in `D:\Project\Zmbor\wt-phase6` on branch `ultimate/phase6-native-compatibility` (local, unpushed; Phase 5 is merged as PR #3).

Before changing anything:

1. Run `git status` and `git log --oneline -10`; compare with `git ls-remote https://github.com/m7kor/Zmbor.git`.
2. Build `build` and rerun `tools/acceptance/gate.ps1` (see `docs/architecture/acceptance-gate.md`): expect the numbers in `CHANGELOG.md` Phase 6, and report
   any difference.

Then continue with the next priorities in `HANDOFF.md`. Find each root cause before making architectural changes; add
a regression test for each change and mutation-check it; keep every passing hardware test passing.
