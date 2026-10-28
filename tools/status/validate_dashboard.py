#!/usr/bin/env python3
"""Validate docs/status/components.json and the files generated from it (docs/status/README.md, "Methodology").

Rejects: duplicate ids; unknown statuses; stored group statuses (they are derived); missing mandatory hardware
categories; source, test or proof references that do not exist; hardware program ids that are not in the hardware
results; completion claims without the evidence the methodology requires; contradictory statuses (a complete
component with a blocker, a missing one citing passing hardware programs, an HLE component marked complete);
measurements that differ from what docs/hwtest/dashboard.json and the decode tables give, or whose source changed
without being re-measured; and generated SVG/Markdown that is stale.

usage: validate_dashboard.py          exit 0 if valid, 1 with one line per problem otherwise
       validate_dashboard.py --selftest   check that each rule rejects a crafted bad inventory
"""
import copy
import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("generate_dashboard", HERE / "generate_dashboard.py")
gen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen)

ROOT = gen.ROOT
MANDATORY_CATEGORIES = set("ABCDEFGHIJK")
TARGETS = {"", "8", "unscheduled"}
LEAF_FIELDS = {"id", "name", "status", "implemented", "missing", "src", "tests", "hw", "blocker", "target",
               "verified_at"}
OPTIONAL_LEAF_FIELDS = {"short", "implementation", "isa", "proof", "hwvec"}
GROUP_FIELDS = {"id", "title", "children", "category", "scope", "short"}


def _ref_ok(ref, problems, where):
    path, _, needle = ref.partition("::")
    p = ROOT / path
    if not p.exists():
        problems.append(f"{where}: reference {path} does not exist")
        return
    if needle:
        if not p.is_file():
            problems.append(f"{where}: reference {path} is not a file")
        elif needle not in p.read_text(encoding="utf-8", errors="replace"):
            problems.append(f"{where}: '{needle}' not found in {path}")


def _isa_sets(inv, cls):
    classes = inv["measurements"]["isa_coverage"]["classes"]
    decoded, confirmed = set(), set()
    for c in cls.split("+"):
        if c not in classes:
            return None, None
        decoded |= set(classes[c]["decoded"])
        confirmed |= set(classes[c]["confirmed"])
    return decoded, confirmed


def validate(inv, check_generated=True, check_measurements=True):
    problems = []
    ids = set()
    hw = inv.get("measurements", {}).get("hardware", {})
    # A component may cite EE programs and, once measured, IOP programs (measurements.iop_hardware).
    iop_measurement = inv.get("measurements", {}).get("iop_hardware", {})
    iop_tests = iop_measurement.get("tests", {})
    iop_commit = iop_measurement.get("source", {}).get("evidence_commit")
    hw_tests = dict(hw.get("tests", {}))
    hw_tests.update(iop_tests)
    evidence_commit = inv.get("evidence_commit")

    def visit_group(g, section, path):
        where = "/".join(path + [g.get("id", "?")])
        extra = set(g) - GROUP_FIELDS
        if "status" in g:
            problems.append(f"{where}: group stores a status; group states are derived from their components")
        elif extra:
            problems.append(f"{where}: unknown group fields {sorted(extra)}")
        if g.get("id") in ids:
            problems.append(f"{where}: duplicate id {g['id']}")
        ids.add(g.get("id"))
        kids = g.get("children") or []
        if not kids:
            problems.append(f"{where}: group without components")
        if len({("children" in k) for k in kids}) > 1:
            problems.append(f"{where}: mixes groups and components")
        for k in kids:
            if "children" in k:
                visit_group(k, section, path + [g["id"]])
            else:
                visit_leaf(k, section, path + [g["id"]])

    def visit_leaf(leaf, section, path):
        lid = leaf.get("id", "?")
        where = "/".join(path + [lid])
        missing_fields = LEAF_FIELDS - set(leaf)
        unknown = set(leaf) - LEAF_FIELDS - OPTIONAL_LEAF_FIELDS
        if missing_fields:
            problems.append(f"{where}: missing fields {sorted(missing_fields)}")
            return
        if unknown:
            problems.append(f"{where}: unknown fields {sorted(unknown)}")
        if lid in ids:
            problems.append(f"{where}: duplicate id {lid}")
        ids.add(lid)
        st = leaf["status"]
        if st not in gen.STATUSES:
            problems.append(f"{where}: unknown status {st}")
            return
        if leaf["target"] not in TARGETS:
            problems.append(f"{where}: target must be one of {sorted(TARGETS)}")
        for ref in leaf["src"]:
            if "::" in ref:
                problems.append(f"{where}: source references name files, not lines ({ref})")
            _ref_ok(ref, problems, where)
        for ref in leaf["tests"] + leaf.get("proof", []) + leaf.get("hwvec", []):
            _ref_ok(ref, problems, where)
        for t in leaf["hw"]:
            if t not in hw_tests:
                problems.append(f"{where}: hardware program {t} is not in the hardware results")
        statuses_hw = [hw_tests[t]["status"] for t in leaf["hw"] if t in hw_tests]
        # hwvec: unit tests whose expected values are real-PS2 results (hardware-derived vectors)
        has_hw = bool(leaf["hw"]) or bool(leaf.get("hwvec"))
        has_proof, has_tests = bool(leaf.get("proof")), bool(leaf["tests"])
        hardware_section = section == "hardware"
        verified = st in ("VERIFIED_COMPLETE", "VERIFIED_PARTIAL")
        # The IOP programs were measured at their own commit: a component whose hardware evidence is only IOP
        # programs is verified at that commit, every other one at the inventory's evidence commit.
        expected_commit = evidence_commit
        if leaf["hw"] and all(t in iop_tests for t in leaf["hw"]):
            expected_commit = iop_commit
        if verified and leaf["verified_at"] != expected_commit:
            problems.append(f"{where}: verified_at must be the evidence commit {expected_commit}")
        if not verified and leaf["verified_at"] is not None:
            problems.append(f"{where}: verified_at must be null for {st}")
        if "isa" in leaf:
            decoded, confirmed = _isa_sets(inv, leaf["isa"]["class"])
            if decoded is None:
                problems.append(f"{where}: unknown instruction class {leaf['isa']['class']}")
            else:
                stray = set(leaf["isa"]["mnemonics"]) - decoded
                if stray:
                    problems.append(f"{where}: mnemonics not in the decode tables: {sorted(stray)}")
                if st == "VERIFIED_COMPLETE":
                    unconf = set(leaf["isa"]["mnemonics"]) - confirmed
                    if unconf:
                        problems.append(f"{where}: VERIFIED_COMPLETE but not hardware-confirmed: {sorted(unconf)}")
        if st == "VERIFIED_COMPLETE":
            if hardware_section and not (has_hw or has_proof):
                problems.append(f"{where}: VERIFIED_COMPLETE hardware component without hardware evidence or proof")
            if hardware_section and "isa" not in leaf and not has_proof:
                problems.append(f"{where}: VERIFIED_COMPLETE hardware component needs an enumerated scope (isa list or proof)")
            if not hardware_section and not (has_tests or has_proof or has_hw or leaf["src"]):
                problems.append(f"{where}: VERIFIED_COMPLETE without evidence")
            if any(s != "PASS" for s in statuses_hw):
                problems.append(f"{where}: VERIFIED_COMPLETE cites a hardware program that does not match")
            if leaf["blocker"] or leaf["missing"]:
                problems.append(f"{where}: VERIFIED_COMPLETE with a blocker or missing functionality")
            if leaf.get("implementation") == "hle":
                problems.append(f"{where}: an HLE component cannot be VERIFIED_COMPLETE")
        if st == "VERIFIED_PARTIAL":
            if hardware_section and not (has_hw or has_proof):
                problems.append(f"{where}: VERIFIED_PARTIAL hardware component without hardware evidence or proof")
            if not hardware_section and not (has_tests or has_proof or has_hw):
                problems.append(f"{where}: VERIFIED_PARTIAL without tests, proofs or hardware programs")
        if st in ("IMPLEMENTED_UNVERIFIED", "HLE_OR_STUB"):
            if not leaf["src"]:
                problems.append(f"{where}: {st} without a source reference")
            if has_hw and st == "IMPLEMENTED_UNVERIFIED":
                problems.append(f"{where}: cites hardware programs but is IMPLEMENTED_UNVERIFIED (should be VERIFIED_PARTIAL)")
        if st in ("MISSING", "UNKNOWN", "NOT_APPLICABLE"):
            if has_hw or has_proof:
                problems.append(f"{where}: {st} cites hardware evidence or proofs")
        if st in ("MISSING", "UNKNOWN") and not leaf["missing"]:
            problems.append(f"{where}: {st} must say what is missing or unknown")
        if st == "NOT_APPLICABLE" and not leaf["implemented"]:
            problems.append(f"{where}: NOT_APPLICABLE must state the scope reason in 'implemented'")

    categories = set()
    for sec in inv.get("sections", []):
        for g in sec["children"]:
            if g.get("category"):
                if g["category"] in categories:
                    problems.append(f"{g['id']}: duplicate category {g['category']}")
                categories.add(g["category"])
            visit_group(g, sec["id"], [sec["id"]])
    for c in sorted(MANDATORY_CATEGORIES - categories):
        problems.append(f"mandatory category {c} is missing")

    if check_measurements:
        src = inv["measurements"]["hardware_source"]
        dash_path = ROOT / src["path"]
        if gen.sha256(dash_path) != src["sha256"]:
            problems.append(f"{src['path']} changed since it was measured: run generate_dashboard.py measure and review")
        dash = json.loads(dash_path.read_text(encoding="utf-8"))
        if gen.measure_hw(dash) != hw:
            problems.append("hardware measurements differ from docs/hwtest/dashboard.json")
        iop = inv["measurements"].get("iop_hardware")
        if iop:
            if not iop["source"].get("evidence_commit"):
                problems.append("measurements.iop_hardware.source.evidence_commit is not set")
            iop_path = ROOT / iop["source"]["path"]
            if not iop_path.exists() or gen.sha256(iop_path) != iop["source"]["sha256"]:
                problems.append(f"{iop['source']['path']} changed since it was measured: run generate_dashboard.py measure and review")
            elif gen.measure_hw(json.loads(iop_path.read_text(encoding="utf-8"))) != {k: v for k, v in iop.items() if k != "source"}:
                problems.append("IOP hardware measurements differ from " + iop["source"]["path"])
        isa_now = gen.decoded_isa()
        classes = inv["measurements"]["isa_coverage"]["classes"]
        if set(classes) != set(isa_now):
            problems.append("instruction classes differ from the decode tables")
        for c, m in classes.items():
            if c in isa_now and m["decoded"] != isa_now[c]:
                problems.append(f"isa {c}: decode tables changed since measurement: re-measure")
            conf, fail, dec = set(m["confirmed"]), set(m["exercised_by_mismatching_program"]), set(m["decoded"])
            if not conf <= dec or conf & fail:
                problems.append(f"isa {c}: confirmed set inconsistent")
            if set(m["not_exercised"]) != dec - conf - fail:
                problems.append(f"isa {c}: not_exercised inconsistent")
            if m["numerator"] != len(conf) or m["denominator"] != len(dec) or m["numerator"] > m["denominator"]:
                problems.append(f"isa {c}: invalid percentage {m['numerator']}/{m['denominator']}")
        if not (0 <= hw.get("lines_match", -1) <= hw.get("lines", -1)) or \
                not (0 <= hw.get("programs_pass", -1) <= hw.get("programs", -1)):
            problems.append("hardware: invalid percentage")

    if check_generated and not problems:
        try:
            for path, content in gen.outputs(inv).items():
                current = path.read_text(encoding="utf-8") if path.exists() else None
                if current != content:
                    problems.append(f"{path.relative_to(ROOT).as_posix()} is stale: run tools/status/generate_dashboard.py")
        except gen.LayoutError as e:
            problems.append(f"layout: {e}")
    return problems


def selftest(inv):
    """Each mutation must be rejected; the unmodified inventory must pass the structural rules."""
    def first_leaf(d, pred=lambda l: True):
        for sec in d["sections"]:
            for leaf in gen.leaves_of(sec):
                if pred(leaf):
                    return leaf
        raise AssertionError("no leaf")

    cases = []

    def case(name, mutate):
        d = copy.deepcopy(inv)
        mutate(d)
        cases.append((name, d))

    case("duplicate id", lambda d: first_leaf(d).update(id=list(gen.leaves_of(d["sections"][0]))[1]["id"]))
    case("complete without evidence", lambda d: first_leaf(d, lambda l: l["status"] == "MISSING").update(status="VERIFIED_COMPLETE", missing="", blocker="", verified_at=d["evidence_commit"]))
    case("complete citing mismatching program", lambda d: first_leaf(d, lambda l: "cpu_ee_fpu_sqrt" in l["hw"]).update(status="VERIFIED_COMPLETE", missing="", blocker=""))
    case("complete with unconfirmed mnemonic", lambda d: first_leaf(d, lambda l: l["id"] == "ee.alu")["isa"]["mnemonics"].append("teq"))
    case("missing file reference", lambda d: first_leaf(d, lambda l: l["src"])["src"].append("no/such/file.cpp"))
    case("missing test name", lambda d: first_leaf(d, lambda l: l["tests"])["tests"].append("ps2xTest/src/ps2_memory_tests.cpp::no such test name"))
    case("unknown hardware program", lambda d: first_leaf(d, lambda l: l["hw"])["hw"].append("cpu_ee_nonexistent"))
    case("stored group status", lambda d: d["sections"][0]["children"][0].update(status="VERIFIED_COMPLETE"))
    case("missing category", lambda d: d["sections"][0]["children"].pop())
    case("hle marked complete", lambda d: first_leaf(d, lambda l: l.get("implementation") == "hle").update(status="VERIFIED_COMPLETE", missing="", blocker=""))
    case("tampered measurement", lambda d: d["measurements"]["hardware"].update(lines_match=d["measurements"]["hardware"]["lines"]))
    # Off by one from the measured value, so the case stays a tamper whatever the real result is (23/23 now).
    case("tampered IOP measurement", lambda d: d["measurements"]["iop_hardware"].update(programs_pass=d["measurements"]["iop_hardware"]["programs_pass"] - 1))
    case("invalid percentage", lambda d: d["measurements"]["isa_coverage"]["classes"]["mmi"].update(numerator=999))
    case("complete without enumerated scope", lambda d: first_leaf(d, lambda l: l["id"] == "vif.stcycl").update(status="VERIFIED_COMPLETE", missing="", blocker=""))
    case("missing with hardware evidence", lambda d: first_leaf(d, lambda l: l["status"] == "MISSING")["hw"].append("cpu_ee_alu"))
    failed = []
    base = validate(inv, check_generated=False)
    if base:
        failed.append("unmodified inventory rejected: " + base[0])
    for name, d in cases:
        if not validate(d, check_generated=False):
            failed.append(f"not rejected: {name}")
    for f in failed:
        print("SELFTEST FAIL " + f)
    print(f"selftest: {len(cases) - len([f for f in failed if f.startswith('not rejected')])}/{len(cases)} bad inventories rejected")
    return 1 if failed else 0


def main(argv):
    inv = gen.load()
    if "--selftest" in argv:
        return selftest(inv)
    problems = validate(inv)
    for p in problems:
        print(p)
    s = gen.summary(inv)
    if problems:
        print(f"{len(problems)} problem(s)")
        return 1
    print(f"valid: {s['leaves']} components; " + ", ".join(f"{k} {v}" for k, v in s["by_status"].items()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
