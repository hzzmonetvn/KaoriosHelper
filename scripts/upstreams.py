#!/usr/bin/env python3
"""Metadata-only provenance checks; never fetch or execute upstream programs."""
import argparse
import copy
import json
import os
from pathlib import Path
import re
import urllib.parse
import urllib.request

SHA = re.compile(r"[0-9a-f]{40}\Z")


def api(path):
    request = urllib.request.Request("https://api.github.com/" + path,
        headers={"Accept": "application/vnd.github+json", "User-Agent": "upstream-metadata-check",
                 **({"Authorization": "Bearer " + os.environ["GH_TOKEN"]} if os.environ.get("GH_TOKEN") else {})})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def entries(lock):
    for name, item in lock["upstreams"].items():
        yield name, item
        for group in ("submodules", "build_dependencies"):
            for child, dependency in item.get(group, {}).items():
                yield name + "/" + child, dependency


def validate(lock, acceptance=False):
    errors = []
    for name, item in entries(lock):
        if not SHA.fullmatch(item.get("pinned_commit", "")):
            errors.append(name + ": missing immutable commit")
        if item.get("pin_kind") == "tag" and item.get("tag_commit") != item["pinned_commit"]:
            errors.append(name + ": release tag differs from pin")
        if acceptance:
            review = item.get("license_review", {})
            if (item.get("license", "UNKNOWN") == "UNKNOWN" or
                    review.get("status") != "accepted" or not review.get("evidence")):
                errors.append(name + ": license acceptance/evidence outstanding")
            evidence = item.get("evidence", {})
            if not isinstance(evidence, dict) or evidence.get("dependency_closure") != "complete":
                errors.append(name + ": dependency/license closure outstanding")
    return errors


def check(lock, fetch=api):
    report = {"limits": "Metadata comparison only; no runtime or license acceptance implied.", "components": []}
    for name, item in entries(lock):
        row = {"component": name, "current": item["pinned_commit"], "repository": item["repository"]}
        report["components"].append(row)
        url = urllib.parse.urlparse(item["repository"])
        if url.hostname != "github.com":
            row.update(status="unsupported", reason="Non-GitHub upstream requires manual metadata review")
            continue
        repo = url.path.strip("/").removesuffix(".git")
        try:
            # Verify the immutable pin still resolves independently of a moving branch.
            pinned = fetch(f"repos/{repo}/commits/{item['pinned_commit']}")
            if pinned["sha"] != item["pinned_commit"]:
                raise ValueError("pin resolution mismatch")
            if item.get("pinned_tag"):
                tag = fetch(f"repos/{repo}/commits/" + urllib.parse.quote(item["pinned_tag"], safe=""))
                row["tag_current"] = tag["sha"]
                row["tag_moved"] = tag["sha"] != item.get("tag_commit")
            ref = item.get("default_branch")
            if not ref:
                ref = fetch(f"repos/{repo}")["default_branch"]
            latest = fetch(f"repos/{repo}/commits/" + urllib.parse.quote(ref, safe=""))["sha"]
            if not SHA.fullmatch(latest):
                raise ValueError("invalid upstream commit")
            row["candidate"] = latest
            comparison = fetch(f"repos/{repo}/compare/{item['pinned_commit']}...{latest}")
            row.update(status="checked", comparison={k: comparison.get(k) for k in
                ("status", "ahead_by", "behind_by", "total_commits", "html_url")},
                files=[{k: f.get(k) for k in ("filename", "status", "additions", "deletions")} for f in comparison.get("files", [])],
                commits=[{"sha": c["sha"], "message": c.get("commit", {}).get("message", "")} for c in comparison.get("commits", [])],
                pagination_limit="GitHub compare response may truncate files/commits; inspect comparison URL before acceptance")
        except Exception as exc:
            row.update(status="error", reason=str(exc))
    return report


def propose(lock, report):
    candidate = copy.deepcopy(lock)
    rows = {row["component"]: row for row in report["components"]}
    # Child pins derive from parent manifests/gitlinks, not dependency branch HEADs.
    # Keep them immutable until those declarations receive a separate source review.
    for name, item in candidate["upstreams"].items():
        row = rows[name]
        if row["status"] != "checked" or row.get("tag_moved") or row["candidate"] == item["pinned_commit"]:
            continue
        item["pinned_commit"] = row["candidate"]
        item["pin_kind"] = "snapshot"
        item["license_review"] = {"status": "pending", "reason": "Candidate changed; manual source and dependency review required."}
        item["evidence"] = {"commit_object": "api-resolved", "dependency_closure": "incomplete", "runtime": "unverified"}
    return candidate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("validate", "acceptance", "check", "sync"))
    parser.add_argument("--lock", default="upstreams.lock.json")
    parser.add_argument("--report", default="upstream-report.json")
    args = parser.parse_args()
    path = Path(args.lock)
    lock = json.loads(path.read_text())
    errors = validate(lock, args.mode == "acceptance")
    if errors:
        print("\n".join(errors))
        return 1
    if args.mode in ("validate", "acceptance"):
        return 0
    report = check(lock)
    Path(args.report).write_text(json.dumps(report, indent=2) + "\n")
    if args.mode == "sync":
        path.write_text(json.dumps(propose(lock, report), indent=2) + "\n")
    return int(any(row["status"] == "error" or row.get("tag_moved") for row in report["components"]))


if __name__ == "__main__":
    raise SystemExit(main())
