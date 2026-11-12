import tempfile
import unittest
import warnings
import zipfile
from pathlib import Path

from scripts.verify_artifacts import verify_zip


class ArchiveValidationTest(unittest.TestCase):
    def check_rejected(self, names, message, symlink=False):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "module.zip"
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                with zipfile.ZipFile(path, "w") as archive:
                    for name in names:
                        info = zipfile.ZipInfo(name)
                        if symlink:
                            info.create_system = 3
                            info.external_attr = 0o120777 << 16
                        archive.writestr(info, "fixture")
            with self.assertRaisesRegex(ValueError, message):
                verify_zip(path)

    def test_duplicate_members(self):
        self.check_rejected(["module.prop", "module.prop"], "Duplicate")

    def test_unsafe_paths(self):
        for name in ("../module.prop", "/module.prop", "a/../module.prop",
                     "a//module.prop", "./module.prop", "a\\module.prop"):
            with self.subTest(name=name):
                self.check_rejected([name], "Unsafe path")

    def test_symlink_member(self):
        self.check_rejected(["module.prop"], "Symbolic link", symlink=True)
