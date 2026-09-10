#!/usr/bin/env python3
"""Offline integration tests: real Git worktrees/flock/rename, fake binding builds."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('cache', SOURCE / 'scripts/local-package/package-cache.py')
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)

BUILDER = r'''#!/usr/bin/env python3
import os
from pathlib import Path
import sys
import time
root = Path(os.environ['ZLINK_LOCAL_PACKAGE_ROOT'])
root.mkdir(parents=True, exist_ok=True)
with open(os.environ['BUILD_LOG'], 'a') as log:
    log.write(str(root) + '\n')
if os.environ.get('BUILD_WAIT'):
    Path(os.environ['BUILD_WAIT'] + '.started').touch()
    deadline = time.monotonic() + 10
    while not Path(os.environ['BUILD_WAIT']).exists():
        if time.monotonic() > deadline:
            sys.exit(9)
        time.sleep(0.02)
if os.environ.get('BUILD_FAIL'):
    (root / 'partial').touch()
    sys.exit(3)
files = {
    'c': ['c/zlink-c-1.0.0.tar.gz'],
    'cpp': ['install/zlink-cpp/1.0.0/include/zlink.hpp',
            'install/zlink-cpp/1.0.0/lib/libzlink_cpp.a'],
    'dotnet': ['nuget/Zlink.1.0.0.nupkg'],
    'go': ['go/zlink-go-1.0.0.tar.gz'],
    'java': ['maven/systems/zlink/zlink/1.0.0/zlink-1.0.0.jar',
             'maven/systems/zlink/zlink/1.0.0/zlink-1.0.0.pom'],
    'node': ['npm/zlink-systems-zlink-1.0.0.tgz'],
    'python': ['python/zlink-1.0.0-py3-none-any.whl', 'python/zlink-1.0.0.tar.gz'],
    'rust': ['rust/zlink-1.0.0.crate'],
}
for language in sys.argv[1:]:
    if language == os.environ.get('BUILD_OMIT'):
        continue
    for name in files[language]:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(language + ': fixture bytes\n')
(root / 'build').mkdir(exist_ok=True)
(root / 'build/native.o').write_text('private build output')
core = root / 'install/zlink-core/1.0.0/core.fixture'
if not core.parent.is_symlink():
    core.parent.mkdir(parents=True, exist_ok=True)
    core.write_text('core release copy')
# Real native staging mutates binding inputs: clean builds must isolate this.
Path('bindings/native.fixture').write_text('derived native input')
if os.environ.get('BUILD_EDIT_ORIGINAL'):
    Path(os.environ['BUILD_EDIT_ORIGINAL']).write_text('concurrent user edit')
'''


class CacheTests(unittest.TestCase):
    def setUp(self):
        parent = Path('/tmp/zlink-sol-work-sh-cache')
        parent.mkdir(exist_ok=True)
        self.temp = Path(tempfile.mkdtemp(prefix='cache-test.', dir=parent))
        self.addCleanup(shutil.rmtree, self.temp)
        self.root = self.temp / 'repo'
        self.root.mkdir()
        self.home = self.temp / 'home'
        self.home.mkdir()
        self.entries = self.home / '.cache/zlink/packages'
        self.log = self.temp / 'build.log'
        self.checks = self.temp / 'checks.log'
        self.bin = self.temp / 'bin'
        self.bin.mkdir()
        fake_tool = self.bin / 'fake-tool'
        fake_tool.write_text('#!/bin/sh\nprintf "fixture-tool 1.0\\n"\n')
        fake_tool.chmod(0o755)
        for name in ['cc', 'c++', 'cmake', 'make', 'ninja', 'dotnet', 'node', 'npm',
                     'java', 'javac', 'go', 'rustc', 'cargo']:
            (self.bin / name).symlink_to(fake_tool)
        builder = self.temp / 'builder'
        builder.write_text(BUILDER)
        builder.chmod(0o755)
        self.env = {**os.environ, 'HOME': str(self.home), 'JAVA_HOME': '',
                    'PATH': f'{self.bin}:{os.environ["PATH"]}',
                    'ZLINK_PACKAGE_CACHE_ROOT': str(self.entries),
                    'ZLINK_PACKAGE_BUILD_CMD': str(builder),
                    'BUILD_LOG': str(self.log), 'CHECK_LOG': str(self.checks),
                    'ZLINK_BASELINE_ROOT': str(self.temp / 'baseline')}
        for key in ['GIT_DIR', 'GIT_WORK_TREE', 'ZLINK_LOCAL_PACKAGE_ROOT',
                    'ZLINK_CORE_RELEASE_VERSION', 'CC', 'CXX']:
            self.env.pop(key, None)
        (self.root / 'bindings').mkdir()
        (self.root / 'bindings/input').write_text('original\n')
        for language in cache.LANGUAGES:
            if language == 'c':
                continue
            version_dir = self.root / 'bindings' / language
            version_dir.mkdir()
            (version_dir / 'VERSION').write_text('ZLINK_BINDING_VERSION=1.0.0\n')
        scripts = self.root / 'scripts/local-package'
        scripts.mkdir(parents=True)
        for name in ['build-wsl.sh', 'package-cache.py', 'cache-prune.sh']:
            shutil.copy2(SOURCE / 'scripts/local-package' / name, scripts / name)
        (scripts / 'sync-version.py').write_text('''import os, sys
from pathlib import Path
with open(os.environ['CHECK_LOG'], 'a') as log:
    log.write(str(Path(__file__).resolve()) + ' ' + ' '.join(sys.argv[1:]) + '\\n')
if os.environ.get('CHECK_FAIL'):
    sys.exit(4)
''')
        core = self.temp / 'core-release'
        (core / 'share/zlink').mkdir(parents=True)
        (core / 'share/zlink/core-package-provenance.json').write_text('{\n  "version": "1.0.0"\n}\n')
        (scripts / 'core').mkdir()
        (scripts / 'core/fetch-release.sh').write_text('#!/bin/sh\nprintf "%s\\n" "' + str(core) + '"\n')
        (self.root / '.gitignore').write_text('.artifacts/\n')
        (self.root / 'VERSION').write_text('LIBZLINK_VERSION=1.0.0\n')
        self.git('init', '-b', 'main')
        self.git('config', 'user.name', 'cache fixture')
        self.git('config', 'user.email', 'cache@example.test')
        self.git('add', '.')
        self.git('commit', '-m', 'fixture: initial')

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], stderr=subprocess.PIPE)

    def run_script(self, name='build-wsl.sh', args=(), root=None, extra=None, ok=True):
        command = ['bash', str((root or self.root) / 'scripts/local-package' / name), *args]
        result = subprocess.run(command, env={**self.env, **(extra or {})},
                                capture_output=True, text=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def worktree(self, name='other'):
        other = self.temp / name
        self.git('worktree', 'add', '--detach', str(other), 'HEAD')
        return other

    def published(self):
        return [p for p in self.entries.glob('*') if len(p.name) == 16 and p.is_dir()]

    def builds(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_01_miss_atomic_publication_and_file_links(self):
        self.run_script()
        entry, = self.published()
        self.assertRegex(self.builds()[0], rf'{entry.name}\.staging-\d+$')
        self.assertFalse(list(self.entries.glob('*.staging-*')))
        manifest = json.loads((entry / '.complete').read_text())
        self.assertEqual(manifest['binding_versions'], {language: '1.0.0' for language in cache.LANGUAGES})
        self.assertEqual({v['language'] for v in manifest['files'].values()}, set(cache.LANGUAGES))
        self.assertFalse((entry / 'build').exists())
        self.assertFalse((entry / 'install/zlink-core').exists())
        self.assertEqual(self.git('status', '--porcelain'), b'')
        output = self.root / '.artifacts/wsl'
        self.assertFalse(output.is_symlink())
        self.assertTrue((output / 'build/native.o').is_file())
        for name, record in manifest['files'].items():
            self.assertTrue((output / name).is_symlink())
            self.assertEqual(cache.digest(output / name), record['sha256'])

    def test_02_second_worktree_hit_checks_versions_and_digests(self):
        self.run_script()
        other = self.worktree()
        self.checks.write_text('')
        self.run_script(root=other)
        self.assertTrue((other / '.artifacts/wsl/install/zlink-core/1.0.0/share/zlink/core-package-provenance.json').is_file())
        self.assertEqual(len(self.builds()), 1)
        self.assertEqual(len(self.checks.read_text().splitlines()), 2)
        self.assertTrue((other / '.artifacts/wsl/npm/zlink-systems-zlink-1.0.0.tgz').is_symlink())
        self.run_script(root=other, extra={'CHECK_FAIL': '1'}, ok=False)
        self.assertEqual(len(self.builds()), 1)
        entry, = self.published()
        for name in ['npm/zlink-systems-zlink-1.0.0.tgz',
                     'maven/systems/zlink/zlink/1.0.0/zlink-1.0.0.jar',
                     'nuget/Zlink.1.0.0.nupkg']:
            package = entry / name
            original = package.read_bytes()
            package.write_text('corrupt')
            failure = self.run_script(root=other, ok=False)
            self.assertIn('digest mismatch', failure.stderr)
            self.assertEqual(len(self.builds()), 1)
            package.write_bytes(original)

    def test_03_dirty_staged_unstaged_untracked_use_private(self):
        for scope in cache.SCOPES:
            for state in ['staged', 'unstaged', 'untracked']:
                with self.subTest(scope=scope, state=state):
                    other = self.worktree(f'{scope.replace("/", "-")}-{state}')
                    file = other / scope / ('input' if scope == 'bindings' else 'build-wsl.sh')
                    if state == 'untracked':
                        file = other / scope / 'new-file'
                    with file.open('a') as stream:
                        stream.write('\n# dirty\n')
                    if state == 'staged':
                        subprocess.run(['git', '-C', str(other), 'add', str(file)], check=True)
                    self.run_script(root=other, args=('node',))
                    self.assertEqual(Path(self.builds()[-1]), other / '.artifacts/wsl-private')
                    self.assertFalse(self.published())
                    self.assertFalse(self.entries.exists())
                    self.assertTrue((other / '.artifacts/wsl/npm/zlink-systems-zlink-1.0.0.tgz').is_symlink())

    def test_04_concurrent_worktrees_have_one_writer(self):
        other = self.worktree()
        release = self.temp / 'release'
        env = {**self.env, 'BUILD_WAIT': str(release)}
        processes = []
        try:
            first = subprocess.Popen(['bash', str(self.root / 'scripts/local-package/build-wsl.sh')],
                                     env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(first)
            deadline = time.monotonic() + 10
            while not Path(str(release) + '.started').exists():
                self.assertIsNone(first.poll())
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.02)
            self.assertFalse(self.published())
            second = subprocess.Popen(['bash', str(other / 'scripts/local-package/build-wsl.sh')],
                                      env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(second)
            time.sleep(0.2)
            self.assertIsNone(second.poll())
            release.touch()
            for process in processes:
                out, err = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 0, out + err)
        finally:
            release.touch()
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.communicate()
        self.assertEqual(len(self.builds()), 1)
        self.assertEqual(len(self.published()), 1)
        for root in [self.root, other]:
            self.assertTrue((root / '.artifacts/wsl/nuget/Zlink.1.0.0.nupkg').is_symlink())

    def test_05_failure_cleans_staging_and_does_not_publish(self):
        self.run_script(extra={'BUILD_FAIL': '1'}, ok=False)
        self.assertFalse(self.published())
        self.assertFalse(list(self.entries.glob('*.staging-*')))
        self.run_script(extra={'BUILD_OMIT': 'node'}, ok=False)
        self.assertFalse(self.published())
        self.assertFalse(list(self.entries.glob('*.staging-*')))
        # Failure at the rename itself, after successful validation.
        with patch.dict(os.environ, self.env), patch.object(Path, 'rename', side_effect=OSError('publish failed')):
            with self.assertRaisesRegex(OSError, 'publish failed'):
                cache.prepare(self.root, self.root / '.artifacts/wsl', self.entries, cache.LANGUAGES)
        self.assertFalse(self.published())
        self.assertFalse(list(self.entries.glob('*.staging-*')))

    def test_06_prune_keeps_recent_linked_and_baseline_keys(self):
        self.run_script()
        linked, = self.published()
        os.utime(linked / '.complete', (1, 1))
        baseline = self.temp / 'baseline'
        (baseline / '.artifacts/wsl/npm').mkdir(parents=True)
        for i in range(1, 10):
            entry = self.entries / f'{i:016x}'
            entry.mkdir()
            (entry / '.complete').write_text('{}')
            (entry / 'fixture').write_text('fixture')
            os.utime(entry / '.complete', (i + 1, i + 1))
        (baseline / '.artifacts/wsl/npm/binding').symlink_to(self.entries / f'{1:016x}' / 'fixture')
        other = self.worktree()
        (other / '.artifacts/wsl').mkdir(parents=True)
        (other / '.artifacts/wsl/binding').symlink_to(self.entries / f'{2:016x}' / 'fixture')
        before = sorted(str(p) for p in self.entries.rglob('*'))
        self.run_script('cache-prune.sh', ('--keep', '5', '--dry-run'))
        self.assertEqual(before, sorted(str(p) for p in self.entries.rglob('*')))
        self.run_script('cache-prune.sh', ('--keep', '5'))
        self.assertEqual({p.name for p in self.published()},
                         {linked.name, *(f'{i:016x}' for i in [1, 2, 5, 6, 7, 8, 9])})
        self.run_script('cache-prune.sh', ('--keep', '-1'), ok=False)

    def test_07_key_tracks_working_content_versions_platform_and_tools(self):
        original = cache.cache_key(self.root, 'tools-1', 'linux-x64')
        other = self.worktree()
        self.assertEqual(cache.cache_key(other, 'tools-1', 'linux-x64'), original)
        for name in ['bindings/cpp/VERSION', 'bindings/java/VERSION', 'VERSION', 'bindings/input',
                     'scripts/local-package/build-wsl.sh']:
            path = self.root / name
            before = path.read_bytes()
            path.write_bytes(before.replace(b'1.0.0', b'2.0.0') if name.endswith('VERSION') else before + b'\n# edit')
            self.assertNotEqual(cache.cache_key(self.root, 'tools-1', 'linux-x64'), original, name)
            path.write_bytes(before)
        self.assertNotEqual(cache.cache_key(self.root, 'tools-1', 'linux-arm64'), original)
        self.assertNotEqual(cache.cache_key(self.root, 'tools-2', 'linux-x64'), original)
        self.git('commit', '--allow-empty', '-m', 'unrelated: history only')
        self.assertEqual(cache.cache_key(self.root, 'tools-1', 'linux-x64'), original)
        # Staging changes cannot be hidden by HEAD tree hashing.
        (self.root / 'bindings/input').write_text('staged')
        self.git('add', 'bindings/input')
        self.assertNotEqual(cache.cache_key(self.root, 'tools-1', 'linux-x64'), original)
        result = self.run_script(args=('--cache-key',))
        self.assertRegex(result.stdout.strip(), r'^[0-9a-f]{16}$')
        self.assertIn('package tool version id:', result.stderr)

    def test_08_legacy_link_migration_and_framework_output_preservation(self):
        baseline = self.temp / 'baseline/.artifacts/wsl'
        baseline.mkdir(parents=True)
        (baseline / 'untouched').write_text('baseline')
        output = self.root / '.artifacts/wsl'
        output.parent.mkdir()
        output.symlink_to(baseline, target_is_directory=True)
        self.run_script()
        self.assertEqual(list(baseline.iterdir()), [baseline / 'untouched'])
        package = output / 'nuget/Zlink.HttpClient.1.0.0.nupkg'
        package.write_text('framework output')
        self.run_script()
        self.assertEqual(package.read_text(), 'framework output')
        self.assertFalse(package.is_symlink())
        self.assertEqual(len(self.builds()), 1)

    def test_09_input_changes_during_build_refuse_publication(self):
        result = self.run_script(extra={'BUILD_EDIT_ORIGINAL': str(self.root / 'bindings/input')}, ok=False)
        self.assertIn('inputs changed during build', result.stderr)
        self.assertFalse(self.published())
        self.assertFalse(list(self.entries.glob('*.staging-*')))

    def test_10_real_shell_build_boundary_with_fake_language_builders(self):
        scripts = self.root / 'scripts/local-package'
        (scripts / 'native').mkdir()
        sync = scripts / 'native/sync-local-core-libs.sh'
        sync.write_text('#!/bin/sh\nprintf "native staging\\n" > "$GIT_WORK_TREE/bindings/native.fixture"\n')
        sync.chmod(0o755)
        for lang in cache.LANGUAGES:
            (scripts / lang).mkdir()
            (scripts / lang / 'build-wsl.sh').write_text(
                '#!/bin/sh\nexec "' + str(self.temp / 'builder') + '" ' + lang + '\n')
        self.git('add', 'scripts')
        self.git('commit', '-m', 'fixture: fake language builders')
        self.run_script(extra={'ZLINK_PACKAGE_BUILD_CMD': ''})
        self.assertEqual(len(self.builds()), 8)
        self.assertEqual(self.git('status', '--porcelain'), b'')
        output = self.root / '.artifacts/wsl'
        self.assertTrue((output / 'build/package-source/bindings/native.fixture').exists())
        self.assertEqual((output / 'install/zlink-core/1.0.0').resolve(), self.temp / 'core-release')
        other = self.worktree()
        self.run_script(root=other, extra={'ZLINK_PACKAGE_BUILD_CMD': ''})
        self.assertEqual(len(self.builds()), 8)

    def test_11_dirty_inputs_bypass_existing_shared_entry(self):
        self.run_script()
        entry, = self.published()
        before = (entry / '.complete').read_bytes()
        (self.root / 'bindings/input').write_text('dirty after hit')
        self.run_script(args=('dotnet',))
        self.assertEqual(len(self.builds()), 2)
        self.assertEqual(Path(self.builds()[-1]), self.root / '.artifacts/wsl-private')
        self.assertEqual((entry / '.complete').read_bytes(), before)
        self.assertEqual(self.published(), [entry])
        target = self.root / '.artifacts/wsl/nuget/Zlink.1.0.0.nupkg'
        self.assertTrue(target.resolve().is_relative_to(self.root / '.artifacts/wsl-private'))

    def test_12_custom_build_flags_do_not_reuse_release_cache(self):
        self.run_script()
        entry, = self.published()
        other = self.worktree()
        self.run_script(root=other, args=('cpp',), extra={'CONFIGURATION': 'Debug'})
        self.assertEqual(Path(self.builds()[-1]), other / '.artifacts/wsl-private')
        self.assertEqual(self.published(), [entry])


if __name__ == '__main__':
    unittest.main(verbosity=2)
