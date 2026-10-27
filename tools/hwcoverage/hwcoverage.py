#!/usr/bin/env python3
"""Hardware coverage engine (docs/engineering/PS2_HARDWARE_VERIFICATION.md).

Maps every component of docs/status/components.json to the evidence levels below and never merges them:

  test_exists        the component cites unit tests (by name) or test files, and the validator found them
  test_executed      a cited named test ran in the recorded unit-test run
  test_passed        a cited named test passed in that run
  vectors_passed     a cited test whose expected values are real-PS2 results (hwvec) passed in that run
  hardware_verified  every cited ps2autotests program matches real-PS2 output (docs/hwtest/dashboard.json)
  timing_verified    timing checked against hardware; no component has such evidence yet

  hwcoverage.py record --tests-exe build/ps2xTest/ps2x_tests[.exe]
        run the unit tests from the repository root and write docs/status/test-results.json (per-test result,
        commit, binary SHA-256). Run this after a build; commit the file with the change it measured.
  hwcoverage.py report [--check]
        write (or check) docs/status/hardware-coverage.json and docs/status/hardware-coverage-report.md
"""
import argparse
import hashlib
import json
import platform
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
INVENTORY = ROOT / "docs/status/components.json"
HW_DASHBOARD = ROOT / "docs/hwtest/dashboard.json"
RESULTS = ROOT / "docs/status/test-results.json"
COVERAGE = ROOT / "docs/status/hardware-coverage.json"
REPORT = ROOT / "docs/status/hardware-coverage-report.md"
REQUIREMENTS = ROOT / "docs/engineering/PS2_HARDWARE_REQUIREMENTS.json"
GAP_MATRIX = ROOT / "docs/engineering/PS2_COMPONENT_GAP_MATRIX.md"
CONTRACT_FIELDS = ["block", "registers", "functional_requirements", "reset_state", "read_write_semantics",
                   "error_behavior", "interrupts", "dma", "timing", "dependencies", "acceptance", "references",
                   "known_divergences"]
TEST_SOURCES = ["ps2xTest/src", "ps2xTest/gs_cache"]
LEVELS = ["contract_specified", "test_exists", "test_executed", "test_passed", "vectors_passed", "hardware_verified", "timing_verified"]


def known_test_names():
    names = set()
    for d in TEST_SOURCES:
        for p in (ROOT / d).rglob("*.cpp"):
            s = p.read_text(encoding="utf-8", errors="replace")
            names.update(re.findall(r'tc\.Run\(\s*"([^"]+)"', s))
    return names


def parse_run(output, names):
    """Per-test result from MiniTest output. A test's name is matched against the names declared in the sources,
    because code under test may print between the name and its [Passed]/[Failed] marker."""
    ordered = sorted(names, key=len, reverse=True)
    results, current = {}, None
    for raw in output.splitlines():
        line = re.sub(r"\x1b\[[0-9;]*m", "", raw)
        if "[Run]:" in line:
            rest = line.split("[Run]:", 1)[1].strip()
            current = next((n for n in ordered if rest.startswith(n)), None)
            if current is None:
                current = "<unknown>:" + rest[:80]
        for marker, status in (("[Passed]", "PASS"), ("[Failed]", "FAIL"), ("[Error]", "ERROR")):
            if marker in line and current is not None:
                results[current] = status
                current = None
                break
    return results


def cmd_record(tests_exe):
    exe = Path(tests_exe)
    if not exe.is_absolute():
        exe = ROOT / exe
    proc = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, errors="replace")
    (ROOT / "ps2_log.txt").unlink(missing_ok=True)
    results = parse_run(proc.stdout + proc.stderr, known_test_names())
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    dirty = bool(subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT,
                                capture_output=True, text=True).stdout.strip())
    doc = {
        "commit": commit, "tree_clean": not dirty, "platform": platform.platform(terse=True),
        "binary": exe.name, "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "exit_code": proc.returncode,
        "totals": {s: sum(1 for v in results.values() if v == s) for s in ("PASS", "FAIL", "ERROR")},
        "results": dict(sorted(results.items())),
    }
    RESULTS.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    print(f"recorded {len(results)} tests: {doc['totals']} (exit {proc.returncode}, commit {commit[:7]}, clean={not dirty})")
    return 0 if proc.returncode == 0 and doc["totals"]["FAIL"] == 0 and doc["totals"]["ERROR"] == 0 else 1


def _leaves(node, path=()):
    if "children" in node:
        for c in node["children"]:
            yield from _leaves(c, path + (node.get("category") or node.get("id"),))
    else:
        yield node, path


def load_contracts(inv_ids):
    """Verification contracts, hand-written per component. Unknown component ids and missing fields are errors."""
    if not REQUIREMENTS.exists():
        return {}, []
    doc = json.loads(REQUIREMENTS.read_text(encoding="utf-8"))
    problems = []
    for cid, c in doc["contracts"].items():
        if cid not in inv_ids:
            problems.append(f"contract {cid}: no such component in docs/status/components.json")
        miss = [f for f in CONTRACT_FIELDS if f not in c]
        if miss:
            problems.append(f"contract {cid}: missing fields {miss} (write 'unknown' where nothing is known)")
    return doc["contracts"], problems


def coverage():
    inv = json.loads(INVENTORY.read_text(encoding="utf-8"))
    inv_ids = {l["id"] for l, _ in _leaves(inv["sections"][0])}
    contracts, problems = load_contracts(inv_ids)
    if problems:
        raise SystemExit("\n".join(problems))
    hw = inv["measurements"]["hardware"]["tests"]
    run = json.loads(RESULTS.read_text(encoding="utf-8")) if RESULTS.exists() else {"results": {}}
    res = run["results"]
    comps = []
    for leaf, path in _leaves(inv["sections"][0]):
        named = [r.split("::", 1)[1] for r in leaf["tests"] if "::" in r]
        vec = [r.split("::", 1)[1] for r in leaf.get("hwvec", [])]

        def lookup(needles):
            # a reference is a substring of the test name (as the validator checks it in the source)
            out = []
            for n in needles:
                hits = [k for k in res if n in k]
                out.append(res[hits[0]] if hits else None)
            return out

        r_named, r_vec = lookup(named), lookup(vec)
        hw_status = [hw[t]["status"] for t in leaf["hw"]]
        levels = {
            "contract_specified": leaf["id"] in contracts,
            "test_exists": bool(leaf["tests"] or vec),
            "test_executed": any(x is not None for x in r_named + r_vec),
            "test_passed": bool(r_named + r_vec) and all(x == "PASS" for x in r_named + r_vec),
            "vectors_passed": bool(vec) and all(x == "PASS" for x in r_vec),
            "hardware_verified": bool(hw_status) and all(s == "PASS" for s in hw_status),
            "timing_verified": False,
        }
        comps.append({
            "id": leaf["id"], "name": leaf["name"], "category": path[1] if len(path) > 1 else "",
            "status": leaf["status"], "missing": leaf["missing"], "blocker": leaf["blocker"], "target": leaf["target"],
            "levels": levels,
            "named_tests": {"cited": len(named), "executed": sum(x is not None for x in r_named),
                            "passed": sum(x == "PASS" for x in r_named)},
            "hardware_programs": {"cited": len(hw_status), "matching": sum(s == "PASS" for s in hw_status)},
        })
    totals = {lv: sum(1 for c in comps if c["levels"][lv]) for lv in LEVELS}
    by_cat = {}
    for c in comps:
        b = by_cat.setdefault(c["category"], {"components": 0, **{lv: 0 for lv in LEVELS}})
        b["components"] += 1
        for lv in LEVELS:
            b[lv] += 1 if c["levels"][lv] else 0
    return {
        "source": {"inventory": "docs/status/components.json", "hardware_results": "docs/hwtest/dashboard.json",
                   "unit_results": "docs/status/test-results.json",
                   "unit_results_commit": run.get("commit"), "unit_results_binary_sha256": run.get("binary_sha256")},
        "levels": LEVELS, "components_total": len(comps), "totals": totals,
        "by_category": dict(sorted(by_cat.items())), "components": comps,
    }


def report_md(cov):
    L = ["<!-- Generated by tools/hwcoverage/hwcoverage.py. Do not edit. -->", "# Hardware coverage", "",
         "Evidence levels per PS2 hardware and runtime component. The levels are independent: a passing unit test "
         "is not a hardware result, and no component has timing verified against hardware yet.", "",
         f"Unit-test run: commit `{(cov['source']['unit_results_commit'] or 'none')[:12]}`, binary sha256 "
         f"`{(cov['source']['unit_results_binary_sha256'] or 'none')[:16]}`.", "",
         "| category | components | " + " | ".join(f"`{lv}`" for lv in LEVELS) + " |",
         "|---|---|" + "---|" * len(LEVELS)]
    for cat, b in cov["by_category"].items():
        L.append(f"| {cat} | {b['components']} | " + " | ".join(str(b[lv]) for lv in LEVELS) + " |")
    t = cov["totals"]
    L.append(f"| **all** | **{cov['components_total']}** | " + " | ".join(f"**{t[lv]}**" for lv in LEVELS) + " |")
    L += ["", "| component | status | named tests passed / executed / cited | hardware programs matching / cited | levels |",
          "|---|---|---|---|---|"]
    for c in cov["components"]:
        lv = ", ".join(k for k in LEVELS if c["levels"][k]) or "—"
        n, h = c["named_tests"], c["hardware_programs"]
        L.append(f"| `{c['id']}` | `{c['status']}` | {n['passed']} / {n['executed']} / {n['cited']} | "
                 f"{h['matching']} / {h['cited']} | {lv} |")
    return "\n".join(L) + "\n"


def gap_matrix_md(cov):
    def esc(x):
        return (x or "—").replace("|", "\\|")

    L = ["<!-- Generated by tools/hwcoverage/hwcoverage.py from docs/status/components.json. Do not edit. -->",
         "# PS2 component gap matrix", "",
         "Every PS2 hardware and runtime component that is not `VERIFIED_COMPLETE`, with what is missing, its blocker "
         "and its target. `NOT_APPLICABLE` entries are scope proposals awaiting the owner's decision.", ""]
    for cat in sorted({c["category"] for c in cov["components"]}):
        rows = [c for c in cov["components"] if c["category"] == cat and c["status"] != "VERIFIED_COMPLETE"]
        L += [f"## {cat}", "", "| component | status | missing | blocker | target | evidence levels |",
              "|---|---|---|---|---|---|"]
        for c in rows:
            lv = ", ".join(k for k in LEVELS if c["levels"][k]) or "—"
            L.append(f"| `{c['id']}` {esc(c['name'])} | `{c['status']}` | {esc(c['missing'])} | {esc(c['blocker'])} | "
                     f"{c['target'] or '—'} | {lv} |")
        L.append("")
    return "\n".join(L)


def cmd_report(check):
    cov = coverage()
    outs = {COVERAGE: json.dumps(cov, indent=1, ensure_ascii=False) + "\n", REPORT: report_md(cov),
            GAP_MATRIX: gap_matrix_md(cov)}
    stale = [p for p, c in outs.items() if not p.exists() or p.read_text(encoding="utf-8") != c]
    if check:
        for p in stale:
            print(f"{p.relative_to(ROOT).as_posix()} is stale: run tools/hwcoverage/hwcoverage.py report")
        return 1 if stale else 0
    for p in stale:
        p.write_text(outs[p], encoding="utf-8", newline="\n")
    print(f"{cov['components_total']} components; " + ", ".join(f"{k} {v}" for k, v in cov["totals"].items()))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("--tests-exe", required=True)
    rep = sub.add_parser("report")
    rep.add_argument("--check", action="store_true")
    a = ap.parse_args(argv)
    return cmd_record(a.tests_exe) if a.cmd == "record" else cmd_report(a.check)


if __name__ == "__main__":
    sys.exit(main())
