# Resume prompt for the next session

Paste the text below into a new Claude Code session opened at `D:\Project\Zmbor\wt`.

---

You are continuing "PS2Recomp Ultimate": principal-engineer work on a fork of ran-j/PS2Recomp, published at
https://github.com/m7kor/PS2Recomp.

- Communicate with me in Arabic; keep technical identifiers and code in English.
- Evidence only: never claim a result you did not measure.
- Never edit hardware `.expected` files to make tests pass.
- Never represent unit or hardware tests as proof that a commercial game works.
- Never download or commit commercial games, BIOS files or unrelated binaries.
- Commit locally with `git -c user.name=m7kor -c user.email=alshariefk@gmail.com commit ...`.
- Push only when I ask; never force-push and never overwrite `main`.

Read `HANDOFF.md` first: it has the state, branches, build/test commands, architecture notes and the remaining 8
failing hardware tests. `CHANGELOG.md` has every change with its root cause, and `docs/hwtest/README.md` the
hardware dashboard. Do not repeat the community research (`research/`, `docs/community-research/`) or the
Phase 2/3 investigations.

Before changing anything:

1. Run `git status` and `git log --oneline -5` on `ultimate/phase0-recomp-correctness`.
2. Build the project.
3. Run `build\ps2xTest\ps2x_tests.exe` (from the repo root), `ctest --test-dir build\ps2xTest -LE hwtest` and
   `ctest --test-dir build\ps2xIOP`.
4. Confirm the hardware matrix is still 47/55 (`hwtest.py prepare` → build → `ctest -L hwtest` → `hwtest.py report`
   with `--baseline` set to the last run JSON).

Then start Phase 4 with the first priority in `HANDOFF.md`: branch in a delay slot (cpu_ee_branchdelay,
cpu_ee_fpu_branchdelay).

- Find each root cause before making architectural changes.
- Add a regression test for each fix and mutation-check it.
- Keep every currently passing hardware test passing, and rerun all suites including ps2xIOP.
- Report the exact change in the 55-test matrix.
