#!/usr/bin/env bash
# POC 1 runner: Ghidra headless + EE extension -> Zmbor-format function map.
# Prerequisites (install first; see README.md):
#   - JDK 21 on PATH (Ghidra 12.x requirement)
#   - Ghidra 12.x extracted to $GHIDRA_HOME (or ./ghidra)
#   - ghidra-emotionengine-reloaded installed in that Ghidra (File > Install
#     Extensions in the GUI once, or copy the extension zip's contents into
#     Ghidra/Extensions/); the r5900 language id must exist.
#   - a PS2 ELF to analyse: $1 (default: fixtures/synthetic_stripped.elf)
# Usage: ./run_poc1.sh [elf] [outdir]
# Outputs: <outdir>/<name>.ghidra.csv  (Name,Start,End,Size -- the exact
#   format ElfParser::loadGhidraMap consumes) and a validation report
#   printed by validate_poc1.py. NEVER invent output: if any step fails,
#   the script stops and the failure is the result.
set -euo pipefail
cd "$(dirname "$0")"
ELF="${1:-fixtures/synthetic_stripped.elf}"
OUTDIR="${2:-results}"
GHIDRA_HOME="${GHIDRA_HOME:-./ghidra}"
ANALYZE="$GHIDRA_HOME/support/analyzeHeadless"
command -v java >/dev/null || { echo "BLOCKER: no java on PATH (need JDK 21)"; exit 2; }
java -version 2>&1 | head -2
[ -x "$ANALYZE" ] || { echo "BLOCKER: analyzeHeadless not found at $ANALYZE"; exit 2; }
mkdir -p "$OUTDIR" ghidra_projects
NAME="$(basename "$ELF" .elf)"
# NOTE: ExportPS2FunctionsHeadless.java is a NON-interactive variant of the
# in-tree script (see its header). The in-tree script cannot run headless
# (it calls askFile). If you want the full in-tree behaviour (labels,
# materialised addresses, data pointers), port that collection into the
# headless script first and re-run; do not hand-edit the CSV.
"$ANALYZE" ghidra_projects "poc1_$NAME" \
  -import "$ELF" -overwrite \
  -processor r5900:LE:32:default \
  -scriptPath "$PWD" \
  -postScript ExportPS2FunctionsHeadless.java "$PWD/$OUTDIR/$NAME.ghidra.csv"
echo "--- validation (Zmbor promotion rules) ---"
if [ -f "fixtures/synthetic_stripped.truth.json" ] && [ "$NAME" = "synthetic_stripped" ]; then
  python3 validate_poc1.py "$ELF" "$OUTDIR/$NAME.ghidra.csv" \
    --truth fixtures/synthetic_stripped.truth.json
else
  python3 validate_poc1.py "$ELF" "$OUTDIR/$NAME.ghidra.csv"
fi
