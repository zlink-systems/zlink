"""llms.txt 생성 결과와 빌드 산출물 검사를 확인한다."""

import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

SCRIPT_DIR = Path(__file__).resolve().parent
HOOK_DIR = SCRIPT_DIR.parent / "hooks"
sys.path.insert(0, str(HOOK_DIR))

import check_llms_txt
import llms_txt


class LlmsTxtTests(unittest.TestCase):
    def _page(self, title, source, url, markdown):
        return SimpleNamespace(
            is_page=True,
            is_section=False,
            is_link=False,
            title=title,
            url=url,
            file=SimpleNamespace(src_uri=source, content_string=markdown),
            meta={},
            markdown=markdown,
        )

    def _section(self, title, *children):
        return SimpleNamespace(
            is_section=True,
            is_page=False,
            is_link=False,
            title=title,
            children=list(children),
        )

    def test_nav_render_preserves_locales_and_omits_spec_and_internals(self):
        english = [
            self._section(
                "C++",
                self._section(
                    "Getting started",
                    self._page(
                        "Overview",
                        "cpp/guide/server/01-overview.en.md",
                        "cpp/guide/server/01-overview/",
                        "# Overview\n\nA practical introduction to the C++ server guide.\n",
                    ),
                ),
                self._section(
                    "Reference",
                    self._page(
                        "Host lifecycle",
                        "dotnet/reference/01-host.en.md",
                        "dotnet/reference/01-host/",
                        "# Host lifecycle\n\nThe public host lifecycle API.\n",
                    ),
                ),
                self._section(
                    "Spec",
                    self._page(
                        "Private contract",
                        "common/spec/private.en.md",
                        "common/spec/private/",
                        "Must not be included.\n",
                    ),
                ),
                self._page(
                    "Internal design",
                    "cpp/internals/runtime.en.md",
                    "cpp/internals/runtime/",
                    "Must not be included.\n",
                ),
            )
        ]
        korean = [
            self._section(
                "C++",
                self._section(
                    "시작",
                    self._page(
                        "개요",
                        "cpp/guide/server/01-overview.ko.md",
                        "ko/cpp/guide/server/01-overview/",
                        "# 개요\n\nC++ 서버 가이드의 시작 페이지입니다.\n",
                    ),
                ),
            )
        ]

        content = llms_txt.render_llms_txt(
            "ZLink",
            "Messaging framework documentation.",
            "https://docs.example.test/",
            {"en": english, "ko": korean},
            [("en", "English"), ("ko", "한국어")],
        )

        self.assertIn("## English", content)
        self.assertIn("## 한국어", content)
        self.assertIn("https://docs.example.test/cpp/guide/server/01-overview/", content)
        self.assertIn("https://docs.example.test/ko/cpp/guide/server/01-overview/", content)
        self.assertIn(": A practical introduction to the C++ server guide.", content)
        self.assertNotIn("Private contract", content)
        self.assertNotIn("Internal design", content)

    def test_valid_generated_urls_resolve_and_broken_urls_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            site_dir = Path(directory)
            page_file = site_dir / "guide" / "index.html"
            page_file.parent.mkdir(parents=True)
            page_file.write_text("<html></html>", encoding="utf-8")
            llms_path = site_dir / "llms.txt"
            llms_path.write_text(
                "# ZLink\n\n> Documentation.\n\n## English\n\n"
                "- [Guide](https://docs.example.test/guide/): Guide summary.\n",
                encoding="utf-8",
            )

            self.assertEqual(check_llms_txt.validate_site(site_dir, "https://docs.example.test/"), [])

            llms_path.write_text(
                "# ZLink\n\n> Documentation.\n\n## English\n\n"
                "- [Missing](https://docs.example.test/missing/): Missing page.\n",
                encoding="utf-8",
            )
            errors = check_llms_txt.validate_site(site_dir, "https://docs.example.test/")
            self.assertTrue(any("built page does not exist" in error for error in errors))

    def test_url_outside_site_and_missing_llms_file_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            site_dir = Path(directory)
            (site_dir / "llms.txt").write_text(
                "# ZLink\n\n## English\n\n"
                "- [External](https://other.example.test/page/): External page.\n",
                encoding="utf-8",
            )
            errors = check_llms_txt.validate_site(site_dir, "https://docs.example.test/")
            self.assertTrue(any("outside site_url origin" in error for error in errors))

            (site_dir / "llms.txt").unlink()
            errors = check_llms_txt.validate_site(site_dir, "https://docs.example.test/")
            self.assertTrue(any("missing" in error for error in errors))

    def test_description_skips_contract_blockquotes(self):
        page = self._page(
            "DI container",
            "cpp/guide/server/40-di-container.en.md",
            "cpp/guide/server/40-di-container/",
            "# DI container\n\n> The formal contract is elsewhere.\n\n"
            "This chapter explains how to configure the container.\n",
        )

        content = llms_txt.render_llms_txt(
            "ZLink",
            "Documentation.",
            "https://docs.example.test/",
            {"en": [self._section("C++", page)]},
            [("en", "English")],
        )

        self.assertIn(": This chapter explains how to configure the container.", content)
        self.assertNotIn("formal contract", content)


if __name__ == "__main__":
    unittest.main()
