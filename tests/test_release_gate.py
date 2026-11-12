"""Release artifacts require dependency/license acceptance, unlike diagnostic builds."""
from pathlib import Path
import unittest


class ReleaseGateTest(unittest.TestCase):
    def test_release_upload_requires_acceptance(self):
        workflow = (Path(__file__).resolve().parents[1] / '.github/workflows/ci.yml').read_text()
        self.assertIn('run: python3 scripts/upstreams.py acceptance', workflow)
        release = workflow.split('- name: Upload Release-Accepted Module Artifact', 1)[1]
        self.assertIn("if: steps.acceptance.outcome == 'success'", release)
        self.assertIn('name: kaorioshelper-diagnostic-module', workflow)


if __name__ == '__main__':
    unittest.main()
