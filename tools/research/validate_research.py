#!/usr/bin/env python3
"""Validate the community research database under research/.

Checks every research/candidates/*.json record (a JSON array of candidates) against the rules in
research/schema/candidate.schema.json, plus the honesty rules the schema cannot express:
  - TESTED/INTEGRATED/VERIFIED need tests_added and a regression result;
  - INTEGRATED/VERIFIED need an integration_commit;
  - VERIFIED additionally needs observed game-level or hardware-test evidence;
  - game-specific (category F) work cannot be INTEGRATED into the general engine.
Standard library only, so it runs anywhere Python 3.8+ does. Exit code 1 on any error.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
RESEARCH = ROOT / "research"
REQUIRED = ["id", "source", "title", "category", "subsystem", "problem", "approach", "scope", "status", "evidence", "license"]
CATEGORIES = set("ABCDEF")
SCOPES = {"general", "game-specific", "unknown"}
STATUSES = {"DISCOVERED", "ANALYZED", "CANDIDATE", "TESTED", "INTEGRATED", "VERIFIED", "REJECTED",
            "INCOMPATIBLE", "SUPERSEDED", "BLOCKED", "GAME_SPECIFIC", "ARCHITECTURE_REFERENCE"}
ID_RE = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*-[0-9]{2,}$")


def check(record, where, errors, seen):
    def err(msg):
        errors.append(f"{where}: {record.get('id', '?')}: {msg}")

    missing = [key for key in REQUIRED if key not in record]
    for key in missing:
        err(f"missing '{key}'")
    if missing:
        return
    if not ID_RE.match(record["id"]):
        err("id must look like 'project-01'")
    if record["id"] in seen:
        err(f"duplicate id (also in {seen[record['id']]})")
    seen[record["id"]] = where
    source = record["source"]
    if not isinstance(source, dict) or not source.get("repository") or not source.get("revision"):
        err("source needs repository and revision (provenance)")
    if record["category"] not in CATEGORIES:
        err("category must be one of A-F")
    if record["scope"] not in SCOPES:
        err(f"scope must be one of {sorted(SCOPES)}")
    status = record["status"]
    if status not in STATUSES:
        err(f"unknown status {status}")
    evidence = record["evidence"] if isinstance(record["evidence"], dict) else {}
    if status in {"TESTED", "INTEGRATED", "VERIFIED"}:
        if not evidence.get("tests_added"):
            err(f"{status} requires evidence.tests_added")
        if not evidence.get("regression"):
            err(f"{status} requires evidence.regression")
    if status in {"INTEGRATED", "VERIFIED"} and not evidence.get("integration_commit"):
        err(f"{status} requires evidence.integration_commit")
    if status == "VERIFIED" and not evidence.get("game_level"):
        err("VERIFIED requires observed evidence.game_level (source inspection is not verification)")
    if record["category"] == "F" and status in {"INTEGRATED", "VERIFIED"} and record["scope"] != "general":
        err("game-specific work must stay in a compatibility profile, not be INTEGRATED into the engine")
    priority = record.get("priority")
    if priority is not None and not (isinstance(priority, int) and 1 <= priority <= 5):
        err("priority must be an integer 1-5")


def main():
    errors = []
    seen = {}
    count = 0
    files = sorted((RESEARCH / "candidates").glob("*.json"))
    for path in files:
        try:
            records = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            errors.append(f"{path.name}: invalid JSON: {exc}")
            continue
        if not isinstance(records, list):
            errors.append(f"{path.name}: top level must be an array")
            continue
        for record in records:
            count += 1
            check(record, path.name, errors, seen)

    manifest = RESEARCH / "manifests" / "projects.json"
    if manifest.exists():
        projects = json.loads(manifest.read_text(encoding="utf-8"))
        for project in projects:
            for key in ("repository", "license", "relationship", "status"):
                if key not in project:
                    errors.append(f"projects.json: {project.get('repository', '?')}: missing '{key}'")

    for line in errors:
        print("error:", line)
    print(f"{count} candidate record(s) in {len(files)} file(s); {len(errors)} error(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
