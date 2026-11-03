#!/usr/bin/env bash
# POC 2 runner: angr CFGFast as a secondary analysis oracle.
# Usage: ./run_poc2.sh [elf]   (default: fixtures/synthetic_stripped.elf)
# Steps: create .venv (if missing), pip install angr, run run_angr.py.
# Honest cost warning: angr + pyvex + claripy + z3 is a LARGE install
# (hundreds of MB, native build for pyvex, several minutes even on a fast
# machine) and CFGFast on a real PS2 ELF needs GBs of RAM. If the install
# fails here, that failure -- with its exact error -- is the POC result;
# do not substitute invented numbers. See README.md.
set -euo pipefail
cd "$(dirname "$0")"
ELF="${1:-fixtures/synthetic_stripped.elf}"
if [ ! -d .venv ]; then
  python3 -m venv .venv
fi
# shellcheck disable=SC1091
. .venv/bin/activate
pip install --quiet --upgrade pip
pip install angr
if [ "$ELF" = "fixtures/synthetic_stripped.elf" ]; then
  python3 run_angr.py "$ELF" --truth fixtures/synthetic_stripped.truth.json
else
  python3 run_angr.py "$ELF"
fi
