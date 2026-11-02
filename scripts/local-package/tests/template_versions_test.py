#!/usr/bin/env python3
"""Template version sync tests on a temporary copy of the templates."""
import importlib.util
from pathlib import Path
import re
import shutil
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    'sync_version', SOURCE / 'scripts/local-package/sync-version.py'
)
sync_version = importlib.util.module_from_spec(spec)
# dataclasses resolves annotations through sys.modules.
sys.modules[spec.name] = sync_version
spec.loader.exec_module(sync_version)

TEMPLATE_PARENTS = [f'framework/languages/{language}/templates' for language in ('cpp', 'dotnet', 'java', 'node')]
CONFIG_FILES = {
    'Directory.Packages.props', 'build.gradle.kts', 'package.json', 'package-lock.json', 'bootstrap.cmake',
}


class TemplateVersionTests(unittest.TestCase):
    def setUp(self):
        self.temp = Path(tempfile.mkdtemp(prefix='template-versions-test.'))
        self.addCleanup(shutil.rmtree, self.temp, ignore_errors=True)
        self.root = self.temp / 'repo'
        for parent in TEMPLATE_PARENTS:
            shutil.copytree(SOURCE / parent, self.root / parent)
        self.original = self.snapshot()
        previous = sync_version.REPO_ROOT
        sync_version.REPO_ROOT = self.root
        self.addCleanup(setattr, sync_version, 'REPO_ROOT', previous)

    def snapshot(self):
        # Bytes: templates also carry binary files such as gradle-wrapper.jar.
        return {
            path.relative_to(self.root).as_posix(): path.read_bytes()
            for parent in TEMPLATE_PARENTS
            for path in sorted((self.root / parent).rglob('*'))
            if path.is_file()
        }

    def edit(self, relative, old, new):
        path = self.root / relative
        text = path.read_text(encoding='utf-8')
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1), encoding='utf-8', newline='\n')

    def owner(self, language, framework, binding=None):
        lines = [f'ZLINK_TEMPLATE_FRAMEWORK_VERSION={framework}']
        if binding:
            lines.append(f'ZLINK_TEMPLATE_BINDING_VERSION={binding}')
        (self.root / f'framework/languages/{language}/templates/TEMPLATE_VERSIONS').write_text(
            '\n'.join(lines) + '\n', encoding='utf-8', newline='\n'
        )

    def changed(self, write):
        _templates, changed = sync_version.synchronize_template_versions(write)
        return sorted(path.relative_to(self.root).as_posix() for path in changed)

    def test_current_templates_match_their_owner_files(self):
        self.assertEqual(self.changed(write=False), [])

    def test_check_fails_when_one_template_pins_another_framework(self):
        relative = 'framework/languages/java/templates/kotlin/starter/my-game/build.gradle.kts'
        self.edit(relative, 'extra["frameworkVersion"] = "0.31.0"', 'extra["frameworkVersion"] = "0.30.0"')
        self.assertEqual(self.changed(write=False), [relative])
        self.assertEqual(self.changed(write=True), [relative])
        self.assertEqual(self.snapshot(), self.original)

    def test_write_moves_every_managed_template_pin(self):
        old = {'cpp': ('0.31.0',), 'dotnet': ('0.31.0', '1.21.0'), 'java': ('0.31.0', '1.21.0'),
               'node': ('0.30.3', '1.20.0')}
        new = ('0.32.0', '1.22.0')
        for language, pins in old.items():
            self.owner(language, *new[:len(pins)])
        changed = self.changed(write=True)
        self.assertEqual(self.changed(write=False), [])
        after = self.snapshot()

        managed = {
            path for path in self.original
            if path.partition('/my-game/')[2] in CONFIG_FILES | set(sync_version.TEMPLATE_DOCUMENTS)
        }
        self.assertEqual(len({path.split('/my-game/')[0] for path in managed}), 60)
        mentioned = {
            f'{template}/my-game/{document}'
            for template, counts in sync_version.TEMPLATE_DOCUMENT_MENTIONS.items()
            for document, count in zip(sync_version.TEMPLATE_DOCUMENTS, counts) if sum(count)
        }
        # Every configuration file (60 manifests and one lockfile) and every document
        # that names a pin moved.
        self.assertEqual(changed, sorted({path for path in managed if Path(path).name in CONFIG_FILES} | mentioned))

        for path, before in self.original.items():
            if path.endswith('/TEMPLATE_VERSIONS'):
                continue
            if path not in managed:
                self.assertEqual(after[path], before, path)
                continue
            pins = old[path.split('/')[2]]
            text = after[path].decode('utf-8')
            values = '|'.join(re.escape(value) for value in pins)
            # No pin is left at the previous value, apart from milestone mentions.
            self.assertEqual(re.findall(
                rf'(?<![0-9.])(?<!planned for )(?<!scheduled for )(?<!fixed in )({values})'
                rf'(?!\.?[0-9])(?! validation)',
                text,
            ), [], path)
            # Only those pins moved; a lockfile also drops each moved package's integrity.
            strip = lambda lines: [line for line in lines if not line.lstrip().startswith('"integrity"')]
            before_lines = strip(before.decode('utf-8').splitlines())
            after_lines = strip(text.splitlines())
            self.assertEqual(len(before_lines), len(after_lines), path)
            for previous, line in zip(before_lines, after_lines):
                if previous != line:
                    for old_value, new_value in zip(pins, new):
                        line = line.replace(new_value, old_value)
                    self.assertEqual(line, previous, path)

        # Values that look like versions but are not Framework or binding pins stay.
        def text(path):
            return after[f'framework/languages/{path}'].decode('utf-8')
        self.assertIn('redis:7.2.5', text('cpp/templates/starter/my-game/docker-compose.yml'))
        self.assertIn('eclipse-temurin:25.0.2', text('java/templates/java/starter/my-game/Dockerfile'))
        self.assertIn('"rxjs": "7.8.2"', text('node/templates/starter/my-game/package.json'))
        self.assertIn('kotlin("jvm") version "2.3.21"', text('java/templates/kotlin/starter/my-game/build.gradle.kts'))
        readme = text('cpp/templates/board-tables/my-game/README.md')
        self.assertIn('planned for 0.31.0', readme)
        self.assertIn('not observed in 0.31.0 validation', readme)
        self.assertIn('fixed in 0.32.0', text('node/templates/mmo-megaserver/my-game/README.md'))

    def test_unregistered_template_fails(self):
        shutil.copytree(
            self.root / 'framework/languages/node/templates/starter',
            self.root / 'framework/languages/node/templates/extra',
        )
        with self.assertRaisesRegex(sync_version.SyncError, r"node/templates/extra"):
            self.changed(write=False)


if __name__ == '__main__':
    unittest.main()
