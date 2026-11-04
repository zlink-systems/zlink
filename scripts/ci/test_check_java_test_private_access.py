"""Regression tests for forbidden Java and Kotlin test reflection paths."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


CHECKER_PATH = Path(__file__).with_name("check-java-test-private-access.py")
SPEC = importlib.util.spec_from_file_location("private_access_checker", CHECKER_PATH)
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class PrivateAccessCheckerTests(unittest.TestCase):
    def assertRejected(self, source, suffix=".kt"):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / f"ExampleTest{suffix}").write_text(source, encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "private-access bypass"):
                CHECKER.check(root, {})

    def test_kotlin_accessible_assignment_is_rejected(self):
        self.assertRejected("field.isAccessible = true\n")

    def test_kotlin_set_accessible_method_reference_is_rejected(self):
        self.assertRejected("val setter = Field::setAccessible\n")

    def test_reflective_set_accessible_lookup_is_rejected(self):
        self.assertRejected(
            'type.getMethod("setAccessible", Boolean::class.java)\n', suffix=".java"
        )

    def test_reflection_text_in_comment_or_string_is_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "ExampleTest.kt").write_text(
                '// field.isAccessible = true\n'
                'val note = "Field::setAccessible and getMethod(\\"setAccessible\\")"\n',
                encoding="utf-8",
            )
            CHECKER.check(root, {})


if __name__ == "__main__":
    unittest.main()
