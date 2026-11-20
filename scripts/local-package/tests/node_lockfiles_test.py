#!/usr/bin/env python3
"""Exercise version synchronization against an isolated npm registry."""
import base64
import hashlib
import http.server
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.parse import unquote, urlsplit


SOURCE = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    'sync_version_node_lockfiles', SOURCE / 'scripts/local-package/sync-version.py'
)
sync_version = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = sync_version
spec.loader.exec_module(sync_version)
NPM = shutil.which('npm')
FRAMEWORK = '@zlink-systems/framework'
REDIS = '@zlink-systems/framework-locations-redis'
BINDING = '@zlink-systems/zlink'
DEPENDENCY = 'fixture-protobuf'
LEAF = 'fixture-protobuf-leaf'
TEMPLATE = 'framework/languages/node/templates/board-tables'
LOCK = f'{TEMPLATE}/my-game/package-lock.json'


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


class Registry:
    def __init__(self):
        self.responses = {}
        responses = self.responses

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                body = responses.get(unquote(urlsplit(self.path).path))
                if body is None:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_args):
                pass

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.url = f'http://127.0.0.1:{self.server.server_port}'
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def package(self, name, version, dependencies=None):
        manifest = {'name': name, 'version': version, 'dependencies': dependencies or {}}
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode='w:gz') as handle:
            body = json.dumps(manifest).encode()
            member = tarfile.TarInfo('package/package.json')
            member.size = len(body)
            handle.addfile(member, io.BytesIO(body))
        data = archive.getvalue()
        archive_path = f'/tarballs/{name.rsplit("/", 1)[-1]}-{version}.tgz'
        self.responses[archive_path] = data
        manifest['dist'] = {
            'tarball': self.url + archive_path,
            'integrity': 'sha512-' + base64.b64encode(hashlib.sha512(data).digest()).decode(),
        }
        self.responses['/' + name] = json.dumps({
            'name': name, 'dist-tags': {'latest': version}, 'versions': {version: manifest},
        }).encode()


@unittest.skipUnless(NPM, 'npm is required for lockfile integration tests')
class NodeLockfileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='sync-node-lockfiles.')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / TEMPLATE / 'my-game'
        self.lock_path = self.root / LOCK
        self.registry = Registry()
        self.addCleanup(self.registry.close)
        self.registry.package(FRAMEWORK, '9.9.1', {BINDING: '8.8.0', DEPENDENCY: '1.0.0'})
        self.registry.package(REDIS, '9.9.1', {FRAMEWORK: '9.9.1', BINDING: '8.8.0'})
        self.registry.package(BINDING, '8.8.0')
        self.registry.package(DEPENDENCY, '1.0.0', {LEAF: '1.0.0'})
        self.registry.package(LEAF, '1.0.0')
        self.environment = {
            'npm_config_registry': self.registry.url,
            'npm_config_cache': str(self.root / 'npm-cache'),
            'npm_config_update_notifier': 'false',
        }
        self.addCleanup(patch.stopall)
        patch.dict(os.environ, self.environment).start()
        patch.object(sync_version, 'REPO_ROOT', self.root).start()
        patch.object(sync_version, 'template_versions', return_value={
            'node': {'framework': '9.9.1', 'binding': '8.8.0'},
        }).start()
        patch.object(sync_version, 'TEMPLATE_CONFIG_FIELDS', {'node': (
            sync_version.TemplateField('package.json', 'framework',
                                       sync_version.node_internal_dependency_pattern(), 2),
            sync_version.TemplateField('package.json', 'binding',
                                       r'("@zlink-systems/zlink"\s*:\s*")(?P<version>[0-9]+\.[0-9]+\.[0-9]+)(")'),
        )}).start()
        patch.object(sync_version, 'template_directories', return_value=[TEMPLATE]).start()
        patch.object(sync_version, 'TEMPLATE_NODE_LOCKS', {TEMPLATE: (3, (FRAMEWORK, REDIS))}).start()
        patch.object(sync_version, 'TEMPLATE_DOCUMENTS', ()).start()
        patch.object(sync_version, 'TEMPLATE_DOCUMENT_MENTIONS', {TEMPLATE: ()}).start()
        manifest = {'name': 'fixture-consumer', 'version': '1.0.0', 'dependencies': {
            FRAMEWORK: '9.9.0', REDIS: '9.9.0', BINDING: '8.8.0',
        }}
        write_json(self.game / 'package.json', manifest)
        packages = {'': manifest}
        for name, dependencies in (
            (FRAMEWORK, {BINDING: '8.8.0'}),
            (REDIS, {FRAMEWORK: '9.9.0', BINDING: '8.8.0'}),
            (BINDING, {}),
            ('fixture-old', {}),
        ):
            version = '8.8.0' if name == BINDING else '9.9.0'
            artifact = name.rsplit('/', 1)[-1]
            packages[f'node_modules/{name}'] = {
                'version': version,
                'resolved': f'https://registry.npmjs.org/{name}/-/{artifact}-{version}.tgz',
                'dependencies': dependencies,
            }
        write_json(self.lock_path, {
            'name': manifest['name'], 'version': manifest['version'],
            'lockfileVersion': 3, 'requires': True, 'packages': packages,
        })

    def test_write_recomputes_new_package_dependencies(self):
        sync_version.synchronize_template_versions(True)
        packages = json.loads(self.lock_path.read_text(encoding='utf-8'))['packages']
        framework = packages[f'node_modules/{FRAMEWORK}']
        self.assertEqual(framework['version'], '9.9.1')
        self.assertEqual(framework['dependencies'].get(DEPENDENCY), '1.0.0',
                         'new package dependency is missing after version synchronization')
        self.assertIn(f'node_modules/{DEPENDENCY}', packages)
        self.assertIn(f'node_modules/{LEAF}', packages)
        self.assertIn('integrity', framework)
        self.assertFalse((self.game / 'node_modules').exists())
        result = subprocess.run(
            [NPM, 'ci', '--ignore-scripts', '--no-audit', '--no-fund'],
            cwd=self.game, text=True, capture_output=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.game / f'node_modules/{LEAF}/package.json').is_file())

    def test_check_rejects_installed_package_dependency_mismatch_without_writing(self):
        # First synchronize only the old metadata, exactly as a stale committed lock can look.
        source = self.lock_path.read_text(encoding='utf-8')
        self.lock_path.write_text(sync_version.update_template_node_lock(
            source, {'framework': '9.9.1', 'binding': '8.8.0'}, LOCK, 3, (FRAMEWORK, REDIS)
        ), encoding='utf-8')
        manifest = json.loads((self.game / 'package.json').read_text(encoding='utf-8'))
        manifest['dependencies'][FRAMEWORK] = manifest['dependencies'][REDIS] = '9.9.1'
        write_json(self.game / 'package.json', manifest)
        write_json(self.game / f'node_modules/{FRAMEWORK}/package.json', {
            'name': FRAMEWORK, 'version': '9.9.1',
            'dependencies': {BINDING: '8.8.0', DEPENDENCY: '1.0.0'},
        })
        before = self.lock_path.read_bytes()
        with self.assertRaisesRegex(sync_version.SyncError, 'dependenc'):
            sync_version.synchronize_template_versions(False)
        self.assertEqual(self.lock_path.read_bytes(), before)


if __name__ == '__main__':
    unittest.main()
