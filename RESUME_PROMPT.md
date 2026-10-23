# Resume prompt for the next session

Paste the text below into a new Claude Code session opened at `D:\Project\Zmbor\wt-integration`.

---

You are continuing Zmbor (https://github.com/m7kor/Zmbor), the continuation of the "PS2Recomp Ultimate" work. It is
principal-engineer work on a PlayStation 2 static recompiler and runtime built on the upstream project
ran-j/PS2Recomp.

- Communicate with me in Arabic; keep technical identifiers and code in English.
- Evidence only: never claim a result you did not measure.
- Never edit hardware `.expected` files to make tests pass.
- Never represent unit or hardware tests as proof that a commercial game works.
- Never download or commit commercial games, BIOS files or unrelated binaries.
- Commit locally with `git -c user.name=m7kor -c user.email=alshariefk@gmail.com commit ...`.
- Zmbor `main` is protected: work on a new branch and open a pull request.
- Push only when I ask; never force-push.
- Never modify m7kor/PS2Recomp or ran-j/PS2Recomp.

Read `HANDOFF.md` first: it has the state, branches, build/test commands, architecture notes and the remaining 8
failing hardware tests. `CHANGELOG.md` has every change with its root cause, and `docs/hwtest/README.md` the
hardware dashboard. Do not repeat the community research (`research/`, `docs/community-research/`) or the
Phase 2/3 investigations.

Work in `D:\Project\Zmbor\wt-integration` on branch `ultimate/phase3-integration`, which matches Zmbor `main`; start
each piece of work on a new branch from it. `D:\Project\Zmbor\wt` (`ultimate/phase0-recomp-correctness`, `ae1c119`)
is the pre-integration history only.

Before changing anything:

1. Run `git status` and `git log --oneline -5`. Compare HEAD with
   `git ls-remote https://github.com/m7kor/Zmbor.git main`.
2. Build the project.
3. Run `build\ps2xTest\ps2x_tests.exe` (from the repo root; expect 520/520), `ctest --test-dir build\ps2xTest -LE
   hwtest` (48/48) and `ctest --test-dir build\ps2xIOP` (4/4).
4. The hardware matrix (47/55) was measured before the #384 merge and has not been rerun since. Rerun it on this tree
   first (`hwtest.py prepare` → configure with `PS2X_HWTEST_GENERATED` → build → `ctest -L hwtest` → `hwtest.py
   report` with `--baseline D:\Project\Zmbor\hwtest_work\run9.json`), and report any difference.

Only when I ask for Phase 4, start with the first priority in `HANDOFF.md`: branch in a delay slot
(cpu_ee_branchdelay, cpu_ee_fpu_branchdelay).

- Find each root cause before making architectural changes.
- Add a regression test for each fix and mutation-check it.
- Keep every currently passing hardware test passing, and rerun all suites including ps2xIOP.
- Report the exact change in the 55-test matrix.
