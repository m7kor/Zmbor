#!/usr/bin/env python3
"""Normalize forensic-agent reports into research/candidates/*.json records.

Usage: import_forensics.py <report.json> <source-key> <out.json> [integrations.json]

Each report is a JSON array written by a read-only source-inspection pass. Records are mapped onto
research/schema/candidate.schema.json; every original field is kept under "agent_report" so nothing
the analysis said is lost. Status rules:
  - the analysis alone never yields more than CANDIDATE (source inspection is not testing);
  - DUPLICATE becomes SUPERSEDED;
  - integrations.json (id -> evidence) upgrades a record to INTEGRATED with the commit and test results
    produced by this repository, or to SUPERSEDED when another record carries the integration.
"""
import json
import pathlib
import re
import sys

SOURCES = {
    "sotc": {"repository": "LightVelox/PS2Recomp", "branch": "main", "upstream_base": "eb8688b", "license": "GPL-3.0"},
    "dq8": {"repository": "Sinan-Karakaya/PS2Recomp", "branch": "main", "upstream_base": "d74a3ce1", "license": "GPL-3.0"},
    "roadtrip": {"repository": "silentsudin/PS2Recomp", "branch": "roadtrip", "upstream_base": "c5a9d025", "license": "GPL-3.0"},
    "srwz": {"repository": "SloppyWyn/PS2Recomp", "branch": "srwz", "upstream_base": "2c5fbb93", "license": "GPL-3.0"},
}
VENDORED = {
    "KZ": ("drakolordx7/KillzoneRecomp", "832c8adb", "GPL-3.0"),
    "BT3": ("z3xox/BT3-Recomp", "e22a34be", "GPL-3.0"),
    "GOW": ("KIexster/god-of-war-recomp", "92ece581", "GPL-3.0"),
    "UR": ("thiagordsfx/urban-reign-recomp", "2fa53c4d", "GPL-3.0"),
    "DJ": ("jcsix694/def-jam-vendetta-recomp", "ecd34971", "GPL-3.0"),
    "AC5": ("sal063/Ace-Combat-5-Static-recompilation", "078662b4", "Apache-2.0"),
}
STATUSES = {"DISCOVERED", "ANALYZED", "CANDIDATE", "REJECTED", "INCOMPATIBLE", "SUPERSEDED", "BLOCKED",
            "GAME_SPECIFIC", "ARCHITECTURE_REFERENCE"}


def text(value):
    if value is None:
        return ""
    if isinstance(value, (dict, list)):
        return json.dumps(value, ensure_ascii=False)
    return str(value)


def risk(value):
    v = text(value).lower()
    for key, out in (("very high", "high"), ("high", "high"), ("med", "medium"), ("low", "low")):
        if v.startswith(key):
            return out
    return "unknown"


def scope(record, category, status):
    if status == "GAME_SPECIFIC" or category == "F":
        return "game-specific"
    g = text(record.get("general_or_specific")).strip().lower()
    if g.startswith("general") or g.startswith("semi-general"):
        return "general"
    if "title-specific" in g or "game-specific" in g:
        return "game-specific"
    return "unknown"


def source_for(record, key, index):
    if key in SOURCES:
        src = dict(SOURCES[key])
        if key == "roadtrip" and str(record.get("id", "")).startswith("srwz"):
            src = dict(SOURCES["srwz"])
        commits = record.get("commits") or []
        src["revision"] = ", ".join(commits) if isinstance(commits, list) else text(commits)
        return src, src.pop("license")
    repo_field = text(record.get("repo"))
    abbrev = re.split(r"[ ,(]", repo_field)[0] if repo_field else ""
    repo, rev, lic = VENDORED.get(abbrev, (repo_field or "unknown/unknown", text(record.get("revision")), text(record.get("license")) or "unknown"))
    src = {"repository": repo, "revision": text(record.get("revision")) or rev, "involved": repo_field}
    return src, (text(record.get("license")) or lic)


def main():
    report_path, key, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    integrations = {}
    if len(sys.argv) > 4:
        integrations = json.loads(pathlib.Path(sys.argv[4]).read_text(encoding="utf-8-sig"))
    raw = json.loads(pathlib.Path(report_path).read_text(encoding="utf-8-sig"))
    out = []
    for index, record in enumerate(raw, start=1):
        original_id = text(record.get("id"))
        rid = original_id.lower()
        if not re.match(r"^[a-z0-9]+(-[a-z0-9]+)*-[0-9]{2,}$", rid):
            rid = f"{key}-{index:02d}"
        category = (text(record.get("category")) or "E").strip()[:1].upper()
        status = text(record.get("recommended_status")).upper() or "ANALYZED"
        if status == "DUPLICATE":
            status = "SUPERSEDED"
        if status not in STATUSES:
            status = "ANALYZED"
        source, license_id = source_for(record, key, index)
        source["files"] = record.get("files") or []
        normalized = {
            "id": rid,
            "source": source,
            "title": text(record.get("title")),
            "category": category if category in "ABCDEF" else "E",
            "subsystem": text(record.get("subsystem")),
            "problem": text(record.get("problem")),
            "approach": text(record.get("approach")),
            "scope": scope(record, category, status),
            "scope_evidence": text(record.get("general_or_specific")),
            "reference": record.get("reference") if isinstance(record.get("reference"), str) else None,
            "license": license_id,
            "upstream_equivalent": text(record.get("upstream_pr_equivalent")) or None,
            "risk": risk(record.get("risk")),
            "priority": record.get("priority") if isinstance(record.get("priority"), int) and 1 <= record.get("priority") <= 5 else None,
            "status": status,
            "evidence": {"analysis": "read-only source inspection by a forensic pass (2026-10-23); not built or tested here"},
            "agent_report": record,
        }
        if original_id != rid:
            normalized["source_id"] = original_id
        if normalized["priority"] is None:
            del normalized["priority"]
        integ = integrations.get(rid) or integrations.get(original_id)
        if integ:
            normalized["status"] = integ["status"]
            normalized["evidence"].update(integ.get("evidence", {}))
            if integ.get("note"):
                normalized["limitations"] = integ["note"]
        out.append(normalized)
    pathlib.Path(out_path).write_text(json.dumps(out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{len(out)} records -> {out_path}")


if __name__ == "__main__":
    main()
