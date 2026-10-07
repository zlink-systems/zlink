"""다른 언어 장의 링크와 언어 전용 장의 생성 범위를 검증한다."""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import generate_language_guides as guides


class LanguageGuideGenerationTests(unittest.TestCase):
    def test_external_language_chapter_stays_external(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            framework = root / "framework"
            common = framework / "common/guide/stream-connector"
            common.mkdir(parents=True)
            name = "40-protobuf.ko.md"
            (common / name).write_text("# Protobuf\n\nNode tutorial.\n", encoding="utf-8")
            for language in guides.LANGUAGES.values():
                target = framework / language / "guide/stream-connector"
                target.mkdir(parents=True)
                link = name if language == "node" else f"../../../node/guide/stream-connector/{name}"
                (target / "README.ko.md").write_text(
                    f"# Guide\n\n| 1 | [Protobuf]({link}) | Node |\n", encoding="utf-8"
                )
            with patch.object(guides, "FRAMEWORK", framework), patch.object(guides, "REPO_ROOT", root):
                written, stale = guides.generate_locale(suffix="ko", section="stream-connector", check_only=False)
            self.assertEqual(written, 1)
            self.assertEqual(stale, [])
            self.assertTrue((framework / "node/guide/stream-connector" / name).exists())
            for language in ("cpp", "dotnet", "java", "kotlin"):
                self.assertFalse((framework / language / "guide/stream-connector" / name).exists())

    def test_language_specific_chapter_has_no_language_switch(self):
        self.assertEqual(guides.language_switch("node", "40-protobuf.ko.md", "ko", "stream-connector"), "")

    def test_shared_chapter_keeps_language_switch(self):
        switch = guides.language_switch("node", "05-receiving.ko.md", "ko", "stream-connector")
        self.assertIn("**Node/TypeScript**", switch)
        for language in ("cpp", "dotnet", "java", "kotlin"):
            self.assertIn(f"../../../{language}/guide/stream-connector/05-receiving.ko.md", switch)


if __name__ == "__main__":
    unittest.main()
