#!/usr/bin/env python3
"""Binding package cache. Shell entry points own version checks and real builds."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys

LANGUAGES = ('c', 'cpp', 'dotnet', 'go', 'java', 'node', 'python', 'rust')
SCOPES = ('bindings', 'scripts/local-package')


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def tree_hash(root, scope):
    # Hash actual working bytes (including staged/untracked edits), executable
    # bits and symlink targets; ignore timestamps, absolute paths and HEAD.
    names = git(root, 'ls-files', '-z', '--cached', '--others',
                '--exclude-standard', '--', scope).split(b'\0')
    result = hashlib.sha256()
    for name in sorted(set(names) - {b''}):
        path = root / os.fsdecode(name)
        if path.is_symlink():
            mode, value = b'120000', os.fsencode(os.readlink(path))
        elif path.is_file():
            mode = b'100755' if path.stat().st_mode & 0o111 else b'100644'
            value = bytes.fromhex(digest(path))
        elif not path.exists():
            mode, value = b'deleted', b''
        else:
            raise ValueError(f'Unsupported cache input: {path}')
        result.update(name + b'\0' + mode + b'\0' + value + b'\0')
    return result.hexdigest()


def version(root, filename, field):
    return dict(line.split('=', 1) for line in
                (root / filename).read_text().splitlines())[field]


def package_versions(root):
    core = version(root, 'VERSION', 'LIBZLINK_VERSION')
    return {
        language: (
            core if language == 'c' else
            version(root, f'bindings/{language}/VERSION', 'ZLINK_BINDING_VERSION')
        )
        for language in LANGUAGES
    }


def tool_id(root):
    java = Path(os.environ['JAVA_HOME']) / 'bin' if os.environ.get('JAVA_HOME') else Path('')
    commands = [
        ('CC', [*shlex.split(os.environ.get('CC', 'cc')), '--version']),
        ('CXX', [*shlex.split(os.environ.get('CXX', 'c++')), '--version']),
        ('cmake', ['cmake', '--version']), ('make', ['make', '--version']),
        ('ninja', ['ninja', '--version']), ('dotnet', ['dotnet', '--version']),
        ('node', ['node', '--version']), ('npm', ['npm', '--version']),
        ('java', [str(java / 'java'), '-version']),
        ('javac', [str(java / 'javac'), '-version']), ('go', ['go', 'version']),
        ('rustc', ['rustc', '--version']), ('cargo', ['cargo', '--version']),
        ('python', [os.environ.get('PYTHON_EXECUTABLE', 'python3'), '--version']),
    ]
    values = []
    for name, command in commands:
        try:
            run = subprocess.run(command, cwd=root, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, check=False, text=True)
            value = ' '.join(run.stdout.splitlines()) if run.returncode == 0 else 'unavailable'
        except FileNotFoundError:
            value = 'unavailable'
        values.append(f'{name}({shlex.join(command[:-1])})={value}')
    return ';'.join(values)


def cache_key(root, tools, host=None):
    host = host or f'{platform.system().lower()}-{platform.machine().lower()}'
    fields = [tree_hash(root, 'bindings'),
              json.dumps(package_versions(root), sort_keys=True, separators=(',', ':')),
              version(root, 'VERSION', 'LIBZLINK_VERSION'),
              tree_hash(root, 'scripts/local-package'), host, tools]
    # NUL separates the six fields unambiguously, in specification order.
    return hashlib.sha256('\0'.join(fields).encode()).hexdigest()[:16]


def dirty(root):
    return bool(git(root, 'status', '--porcelain=v1', '--untracked-files=all', '--', *SCOPES))


def package_files(root, versions, languages):
    """The binding output layout has a single owner, also used by verification."""
    patterns = {
        'c': [f'c/zlink-c-{versions["c"]}.tar.gz'],
        'cpp': [f'install/zlink-cpp/{versions["cpp"]}/**/*'],
        'dotnet': [f'nuget/Zlink.{versions["dotnet"]}.nupkg'],
        'go': [f'go/zlink-go-{versions["go"]}.tar.gz'],
        'java': [f'maven/systems/zlink/zlink/{versions["java"]}/**/*'],
        'node': [f'npm/zlink-systems-zlink-{versions["node"]}.tgz'],
        'python': [f'python/zlink-{versions["python"]}-*.whl', f'python/zlink-{versions["python"]}.tar.gz'],
        'rust': [f'rust/zlink-{versions["rust"]}.crate'],
    }
    files = {}
    for lang in languages:
        paths = set()
        for pattern in patterns[lang]:
            matches = {p for p in root.glob(pattern) if p.is_file()}
            if not matches:
                raise ValueError(f'Missing {lang} package: {pattern}')
            paths.update(matches)
        binding = versions[lang]
        required = {'cpp': [f'install/zlink-cpp/{binding}/include/zlink.hpp',
                            f'install/zlink-cpp/{binding}/lib/libzlink_cpp.a'],
                    'java': [f'maven/systems/zlink/zlink/{binding}/zlink-{binding}.jar',
                             f'maven/systems/zlink/zlink/{binding}/zlink-{binding}.pom']}
        for name in required.get(lang, []):
            if root / name not in paths:
                raise ValueError(f'Missing {lang} package: {name}')
        for path in sorted(paths):
            if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
                raise ValueError(f'Package must be a regular file inside its root: {path}')
            files[path.relative_to(root).as_posix()] = {'language': lang, 'sha256': digest(path)}
    return files


def verify(entry, versions, languages):
    manifest = json.loads((entry / '.complete').read_text())
    selected = {language: versions[language] for language in languages}
    actual = package_files(entry, versions, languages)
    if manifest != {'binding_versions': selected, 'files': actual}:
        raise ValueError(f'Package manifest/digest mismatch: {entry}')
    return actual


@contextmanager
def lock(path, blocking=True):
    with path.open('a') as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | (0 if blocking else fcntl.LOCK_NB))
        except BlockingIOError:
            yield False
            return
        try:
            yield True
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


def local_directory(path):
    # Migrate the first PR's directory symlink without touching its target.
    if path.is_symlink():
        path.unlink()
    path.mkdir(parents=True, exist_ok=True)


def link_packages(entry, output, files):
    local_directory(output)
    for name, record in files.items():
        target = output / name
        # Directory links from older local layouts must not redirect writes.
        for parent in reversed(target.parent.relative_to(output).parents):
            local_directory(output / parent)
        local_directory(target.parent)
        temporary = target.with_name(f'.{target.name}.link-{os.getpid()}')
        try:
            temporary.symlink_to(entry / name)
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
        if digest(target) != record['sha256']:
            raise ValueError(f'Linked package digest mismatch: {target}')
    print(f'-- verified {len(files)} binding file digests (including NuGet/npm/Maven)')
    invalidate_consumer_caches(versions_from(files))


def versions_from(files):
    """Package version per language, read back from the linked output names."""
    found = {}
    for name in files:
        match = re.search(r'nuget/Zlink\.(?P<v>[0-9][^/]*)\.nupkg$', name)
        if match:
            found['dotnet'] = match.group('v')
        match = re.search(r'npm/zlink-systems-zlink-(?P<v>[0-9][^/]*)\.tgz$', name)
        if match:
            found['node'] = match.group('v')
    return found


def invalidate_consumer_caches(versions):
    """A package rebuilt at the same version must not be served from a stale cache.

    Development rebuilds keep the version and change the content, but every consumer
    caches by version.  The side that produced the package owns invalidating what it
    just made stale; otherwise each consumer has to remember a different cleanup.
    Maven and the C++ prefix read the linked path directly and need nothing here.
    """
    version = versions.get('dotnet')
    if version:
        cached = Path.home() / '.nuget/packages/zlink' / version
        if cached.is_dir():
            shutil.rmtree(cached)
            print(f'-- invalidated stale NuGet cache: {cached}', flush=True)
    version = versions.get('node')
    if version:
        # npm keys cacache by the tarball URL; a file: install re-reads the file, but
        # a registry-style entry for the same version would shadow it.
        result = subprocess.run(
            ['npm', 'cache', 'ls', f'@zlink-systems/zlink@{version}'],
            capture_output=True, text=True, check=False)
        if result.returncode == 0 and result.stdout.strip():
            subprocess.run(['npm', 'cache', 'clean', '--force'],
                           capture_output=True, check=False)
            print(f'-- invalidated npm cache entry for @zlink-systems/zlink@{version}',
                  flush=True)


def source_copy(root, destination):
    """Native staging modifies bindings; keep it in this worktree's build tree."""
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    for source in root.iterdir():
        if source.name not in {'.git', '.artifacts', 'bindings', 'scripts'}:
            (destination / source.name).symlink_to(source, target_is_directory=source.is_dir())
    (destination / 'scripts').mkdir()
    for source in (root / 'scripts').iterdir():
        if source.name != 'local-package':
            (destination / 'scripts' / source.name).symlink_to(source, target_is_directory=source.is_dir())
    for scope in SCOPES:
        for name in git(root, 'ls-files', '-z', '--', scope).split(b'\0'):
            if not name:
                continue
            relative = os.fsdecode(name)
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / relative, target, follow_symlinks=False)
    return destination


def build(root, output, languages, source=None):
    env = dict(os.environ, ZLINK_LOCAL_PACKAGE_ROOT=str(output))
    hook = os.environ.get('ZLINK_PACKAGE_BUILD_CMD')
    if hook:
        # An executable path, not a shell expression. Intended for offline tests.
        command = [hook, *languages]
    else:
        source = source or root
        env['GIT_DIR'] = git(root, 'rev-parse', '--absolute-git-dir').decode().strip()
        env['GIT_WORK_TREE'] = str(source)
        command = ['bash', str(source / 'scripts/local-package/build-wsl.sh'),
                   '--build-packages', *languages]
    subprocess.run(command, env=env, cwd=source or root, check=True)


def prepare(root, output, cache, languages):
    tools = tool_id(root)
    print(f'-- package tool version id: {tools}', flush=True)
    local_directory(output)
    # Serialize same-worktree builds, including private output and source copies.
    with lock(output / '.package-build.lock'):
        key = cache_key(root, tools)
        versions = package_versions(root)
        core_env = dict(os.environ, ZLINK_LOCAL_PACKAGE_ROOT=str(output))
        subprocess.run(['bash', str(root / 'scripts/local-package/build-wsl.sh'),
                        '--prepare-core'], env=core_env, check=True)
        custom_flags = os.environ.get('CONFIGURATION', 'Release') != 'Release' or any(
            os.environ.get(name) for name in ('CFLAGS', 'CXXFLAGS', 'CPPFLAGS',
                'LDFLAGS', 'RUSTFLAGS', 'CARGO_ENCODED_RUSTFLAGS', 'GOFLAGS', 'CMAKE_GENERATOR'))
        # These non-default build inputs are outside the specified six-field key.
        if dirty(root) or custom_flags:
            private = root / '.artifacts/wsl-private'
            private.mkdir(parents=True, exist_ok=True)
            (private / '.complete').unlink(missing_ok=True)
            print(f'-- dirty inputs or custom build flags: private build at {private}', flush=True)
            build(root, private, languages)
            files = package_files(private, versions, languages)
            selected = {language: versions[language] for language in languages}
            (private / '.complete').write_text(json.dumps({'binding_versions': selected, 'files': files}, indent=2) + '\n')
            link_packages(private, output, verify(private, versions, languages))
            return
        cache.mkdir(parents=True, exist_ok=True)
        entry = cache / key
        with lock(cache / f'{key}.lock'):
            if dirty(root) or cache_key(root, tools) != key:
                raise ValueError('Package inputs changed while waiting for the cache lock')
            if not entry.exists():
                staging = cache / f'{key}.staging-{os.getpid()}'
                staging.mkdir()
                try:
                    print(f'-- cache miss: {staging}', flush=True)
                    build_tree = output / 'build'
                    local_directory(build_tree)
                    (staging / 'build').symlink_to(build_tree, target_is_directory=True)
                    source = source_copy(root, build_tree / 'package-source')
                    build(root, staging, LANGUAGES, source)
                    files = package_files(staging, versions, LANGUAGES)
                    # Preserve package files only: Core and build outputs are local.
                    (staging / 'build').unlink()
                    allowed = set(files)
                    for path in sorted(staging.rglob('*'), reverse=True):
                        if path.is_dir() and not path.is_symlink():
                            if not any(path.iterdir()):
                                path.rmdir()
                        elif path.relative_to(staging).as_posix() not in allowed:
                            path.unlink()
                    (staging / '.complete').write_text(json.dumps({'binding_versions': versions, 'files': files}, indent=2) + '\n')
                    verify(staging, versions, LANGUAGES)
                    if dirty(root) or cache_key(root, tools) != key:
                        raise ValueError('Package inputs changed during build; refusing publication')
                    # Never replace an existing published key, even if corrupt.
                    staging.rename(entry)
                finally:
                    if staging.exists():
                        shutil.rmtree(staging)
            else:
                print(f'-- cache hit: {entry}', flush=True)
            link_packages(entry, output, verify(entry, versions, LANGUAGES))
            print(f'-- package cache key: {key}', flush=True)


def linked_keys(root, cache, baseline):
    roots = {baseline}
    for record in git(root, 'worktree', 'list', '--porcelain', '-z').split(b'\0'):
        if record.startswith(b'worktree '):
            roots.add(Path(os.fsdecode(record[9:])))
    keys = set()
    for worktree in roots:
        if not worktree.is_dir():
            continue
        output = worktree / '.artifacts/wsl'
        paths = [output]
        for parent, dirs, files in os.walk(output):
            if Path(parent) == output and 'build' in dirs:
                dirs.remove('build')
            paths.extend(Path(parent) / name for name in dirs + files)
        for path in paths:
            if path.is_symlink():
                target = path.resolve()
                if target.is_relative_to(cache):
                    relative = target.relative_to(cache)
                    if relative.parts:
                        keys.add(relative.parts[0])
    return keys


def prune(root, cache, baseline, keep, dry_run):
    if not cache.exists():
        print(f'-- no package cache: {cache}')
        return
    entries = sorted((p for p in cache.iterdir() if re.fullmatch(r'[0-9a-f]{16}', p.name)
                      and p.is_dir() and not p.is_symlink() and (p / '.complete').is_file()),
                     key=lambda p: ((p / '.complete').stat().st_mtime_ns, p.name), reverse=True)
    for entry in entries[keep:]:
        if dry_run:
            # Read-only, including lock files: actual pruning rechecks under lock.
            action = 'protect' if entry.name in linked_keys(root, cache, baseline) else 'remove'
            print(f'[dry-run] {action} {entry}')
            continue
        with lock(cache / f'{entry.name}.lock', blocking=False) as acquired:
            if not acquired or entry.name in linked_keys(root, cache, baseline):
                print(f'-- protected/in use: {entry}')
                continue
            shutil.rmtree(entry)
            print(f'-- removed: {entry}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'key', 'prune'])
    parser.add_argument('--keep', type=int, default=5)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('languages', nargs='*', choices=[*LANGUAGES, 'core'])
    args = parser.parse_args()
    if args.keep < 0:
        parser.error('--keep must be non-negative')
    root = Path(__file__).resolve().parents[2]
    cache = Path(os.environ.get('ZLINK_PACKAGE_CACHE_ROOT', Path.home() / '.cache/zlink/packages')).resolve()
    output = Path(os.environ.get('ZLINK_LOCAL_PACKAGE_ROOT', root / '.artifacts/wsl')).absolute()
    if args.action == 'prune':
        baseline = Path(os.environ.get('ZLINK_BASELINE_ROOT', Path.home() / 'project/zlink'))
        prune(root, cache, baseline, args.keep, args.dry_run)
    elif args.action == 'key':
        tools = tool_id(root)
        print(f'-- package tool version id: {tools}', file=sys.stderr)
        print(cache_key(root, tools))
    else:
        prepare(root, output, cache, tuple(lang for lang in args.languages if lang != 'core') or LANGUAGES)


if __name__ == '__main__':
    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Package cache error: {error}', file=sys.stderr)
        sys.exit(1)
