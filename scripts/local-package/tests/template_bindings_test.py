#!/usr/bin/env python3
"""scripts/templates/check-template-bindings.py tests on a fixture tag repository."""
import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    'check_template_bindings', SOURCE / 'scripts/templates/check-template-bindings.py'
)
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)

# The binding each fixture Framework tag requires, and the packages tagged at it.
TAGS = {
    '0.31.0': ('1.21.0', ('dotnet', 'java')),
    '0.30.3': ('1.20.0', ('node',)),
}
OWNERS = {
    'cpp': ('0.31.0',),
    'dotnet': ('0.31.0', '1.21.0'),
    'java': ('0.31.0', '1.21.0'),
    'node': ('0.30.3', '1.20.0'),
}


def git(root, *arguments):
    subprocess.run(
        ['git', '-c', 'user.name=test', '-c', 'user.email=test@example.invalid', '-C', str(root), *arguments],
        check=True, capture_output=True,
    )


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8', newline='\n')


class TemplateBindingTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='template-bindings-test.'))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        git(self.root, 'init', '-q')
        for framework, (binding, languages) in TAGS.items():
            write(self.root / 'framework/languages/dotnet/contract/packages/Zlink.Framework.package.txt',
                  f'dependency targetFramework=net8.0 exclude=Build,Analyzers id=Zlink version={binding}\n')
            write(self.root / 'framework/languages/java/gradle/libs.versions.toml',
                  f'[versions]\nzlinkBindings = "{binding}"\n')
            write(self.root / 'framework/languages/node/packages/framework/package.json',
                  f'{{\n  "dependencies": {{\n    "@zlink-systems/zlink": "{binding}"\n  }}\n}}\n')
            git(self.root, 'add', '-A')
            git(self.root, 'commit', '-q', '-m', framework)
            for language in languages:
                git(self.root, 'tag', f'framework-{language}/v{framework}')
        for language, pins in OWNERS.items():
            self.owner(language, *pins)
        previous = check.sync_version.REPO_ROOT
        check.sync_version.REPO_ROOT = self.root
        self.addCleanup(setattr, check.sync_version, 'REPO_ROOT', previous)

    def owner(self, language, framework, binding=None):
        lines = [f'ZLINK_TEMPLATE_FRAMEWORK_VERSION={framework}']
        if binding:
            lines.append(f'ZLINK_TEMPLATE_BINDING_VERSION={binding}')
        write(self.root / f'framework/languages/{language}/templates/TEMPLATE_VERSIONS', '\n'.join(lines) + '\n')

    def run_main(self):
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = check.main()
        return code, stdout.getvalue(), stderr.getvalue()

    def test_pins_matching_their_tags_pass(self):
        code, stdout, stderr = self.run_main()
        self.assertEqual((code, stderr), (0, ''))
        self.assertIn('match their Framework release tags', stdout)

    def test_binding_that_differs_from_the_framework_tag_fails(self):
        self.owner('java', '0.31.0', '1.20.0')
        code, _stdout, stderr = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn(
            'ZLINK_TEMPLATE_BINDING_VERSION=1.20.0, but Framework 0.31.0 requires binding 1.21.0 '
            '(framework-java/v0.31.0:framework/languages/java/gradle/libs.versions.toml)',
            stderr,
        )

    def test_missing_framework_tag_fails(self):
        self.owner('node', '0.29.9', '1.20.0')
        code, _stdout, stderr = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn('tag framework-node/v0.29.9 not found; fetch tags (git fetch --tags)', stderr)


if __name__ == '__main__':
    unittest.main()
