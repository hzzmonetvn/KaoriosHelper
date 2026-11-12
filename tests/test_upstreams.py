import copy
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("upstreams", ROOT / "scripts/upstreams.py")
upstreams = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upstreams)
OLD = "a" * 40
NEW = "b" * 40


class UpstreamTests(unittest.TestCase):
    def setUp(self):
        self.lock = {"upstreams": {"fixture": {
            "repository": "https://github.com/example/fixture.git",
            "default_branch": "main", "pinned_commit": OLD,
            "pinned_tag": "v1", "tag_commit": OLD, "pin_kind": "tag"}}}

    def fetch(self, path):
        if "/compare/" in path:
            return {"status": "ahead", "ahead_by": 1, "behind_by": 0,
                    "total_commits": 1, "html_url": "https://github.com/example/fixture/compare/a...b",
                    "files": [{"filename": "LICENSE", "status": "modified", "additions": 1, "deletions": 1}],
                    "commits": [{"sha": NEW, "commit": {"message": "Fixture update"}}]}
        return {"sha": NEW if path.endswith("/main") else OLD}

    def test_check_does_not_mutate_and_records_comparison(self):
        original = copy.deepcopy(self.lock)
        report = upstreams.check(self.lock, self.fetch)
        self.assertEqual(self.lock, original)
        row = report["components"][0]
        self.assertEqual(row["current"], OLD)
        self.assertEqual(row["candidate"], NEW)
        self.assertEqual(row["files"][0]["filename"], "LICENSE")
        self.assertFalse(row["tag_moved"])

    def test_sync_preserves_tag_identity_and_requires_review(self):
        result = upstreams.propose(self.lock, upstreams.check(self.lock, self.fetch))
        item = result["upstreams"]["fixture"]
        self.assertEqual(item["pinned_commit"], NEW)
        self.assertEqual(item["tag_commit"], OLD)
        self.assertEqual(item["pin_kind"], "snapshot")
        self.assertTrue(upstreams.validate(result, acceptance=True))
        self.assertEqual(upstreams.validate(result), [])
        self.assertEqual(self.lock["upstreams"]["fixture"]["pinned_commit"], OLD)

    def test_moved_tag_is_reported_and_not_synced(self):
        def fetch(path):
            return {"sha": NEW} if path.endswith("/v1") else self.fetch(path)
        report = upstreams.check(self.lock, fetch)
        self.assertTrue(report["components"][0]["tag_moved"])
        self.assertEqual(upstreams.propose(self.lock, report), self.lock)

    def test_sync_does_not_advance_parent_declared_dependencies(self):
        item = self.lock["upstreams"]["fixture"]
        item["submodules"] = {"lib": {
            "repository": "https://github.com/example/lib.git",
            "default_branch": "main", "pinned_commit": OLD}}
        item["build_dependencies"] = {"tool": {
            "repository": "https://github.com/example/tool.git",
            "default_branch": "main", "pinned_commit": OLD}}
        report = upstreams.check(self.lock, self.fetch)
        result = upstreams.propose(self.lock, report)
        updated = result["upstreams"]["fixture"]
        self.assertEqual(updated["pinned_commit"], NEW)
        self.assertEqual(updated["submodules"], item["submodules"])
        self.assertEqual(updated["build_dependencies"], item["build_dependencies"])
        self.assertEqual(updated["evidence"]["dependency_closure"], "incomplete")

    def test_api_failure_not_silently_accepted(self):
        def fetch(path):
            raise OSError("fixture unavailable")
        report = upstreams.check(self.lock, fetch)
        self.assertEqual(report["components"][0]["status"], "error")
        self.assertEqual(upstreams.propose(self.lock, report), self.lock)

    def test_non_github_requires_manual_review(self):
        self.lock["upstreams"]["fixture"]["repository"] = "https://gitlab.com/example/fixture.git"
        report = upstreams.check(self.lock, lambda _: self.fail("unexpected API request"))
        self.assertEqual(report["components"][0]["status"], "unsupported")

    def test_dependency_pin_and_acceptance(self):
        item = self.lock["upstreams"]["fixture"]
        item["submodules"] = {"lib": {"repository": "https://github.com/example/lib", "pinned_commit": "main"}}
        self.assertTrue(upstreams.validate(self.lock))
        item["submodules"]["lib"]["pinned_commit"] = NEW
        self.assertEqual(upstreams.validate(self.lock), [])
        self.assertTrue(upstreams.validate(self.lock, acceptance=True))

    def test_repository_contract(self):
        lock = json.loads((ROOT / "upstreams.lock.json").read_text())
        self.assertEqual(upstreams.validate(lock), [])
        self.assertTrue(upstreams.validate(lock, acceptance=True))
        kow = lock["upstreams"]["kow-inject"]
        self.assertEqual(kow["pinned_commit"], "1138c6f3cebb70c00ec5573911cee745134a3e6d")
        self.assertEqual(kow["configuration"]["format"], "pif.prop")
        self.assertIn("play-integrity-fork", lock["upstreams"])
        tee = lock["upstreams"]["tee-simulator-rs"]
        self.assertEqual(next(iter(tee["submodules"].values()))["license"], "UNKNOWN")


if __name__ == "__main__":
    unittest.main()
