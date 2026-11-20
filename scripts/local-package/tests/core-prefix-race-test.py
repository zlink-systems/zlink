#!/usr/bin/env python3
"""Exercise both Core publishers offline, with a release download barrier.

Run under the heavy slot. At most two publisher commands run concurrently.
The fixture runtime implements only the version query used by verify-package.sh.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import time


ROOT = Path(__file__).resolve().parents[3]
FETCH = ROOT / 'scripts/local-package/core/fetch-release.sh'
LOCAL = ROOT / 'scripts/gate/materialize-local-core-prefix.sh'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(prefix, version):
    return (digest(prefix / 'share/zlink/core-package-provenance.json'),
            digest(prefix / f'lib/libzlink.so.{version}'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iterations', type=int, default=40)
    parser.add_argument('--scripts-root', type=Path, default=ROOT)
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error('--iterations must be positive')
    memory = subprocess.check_output(['free', '-g'], text=True)
    print(memory, flush=True)
    if int(memory.splitlines()[1].split()[6]) < 8:
        print('At least 8 GiB available memory is required', flush=True)
        return 75
    version = re.search(r'^LIBZLINK_VERSION=(.*)$',
                        (ROOT / 'VERSION').read_text(), re.M)[1]
    (ROOT / '.artifacts').mkdir(exist_ok=True)
    fetch = args.scripts_root / FETCH.relative_to(ROOT)
    local_script = args.scripts_root / LOCAL.relative_to(ROOT)
    failures = 0
    with tempfile.TemporaryDirectory(prefix='core-prefix-race-', dir=ROOT / '.artifacts') as tmp:
        base = Path(tmp)
        assets = base / 'assets'
        assets.mkdir()
        build = base / 'build'
        (build / 'lib').mkdir(parents=True)
        source = base / 'version.c'
        numbers = version.split('.')
        source.write_text('void zlink_version(int *a,int *b,int *c) {'
                          f'*a={numbers[0]};*b={numbers[1]};*c={numbers[2]};}}\n')
        runtime = build / f'lib/libzlink.so.{version}'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wl,-soname,libzlink.so.0',
                        str(source), '-o', str(runtime)], check=True)
        binary = base / 'libzlink-linux-x64'
        binary.mkdir()
        shutil.copy(runtime, binary / 'libzlink.so')
        (binary / 'include').mkdir()
        with tarfile.open(assets / 'libzlink-linux-x64.tar.gz', 'w:gz') as archive:
            archive.add(binary, arcname=binary.name)
        with tarfile.open(assets / f'zlink-{version}-source.tar.gz', 'w:gz') as archive:
            archive.add(ROOT / 'core/include', arcname='core/include')
        checksums = assets / 'checksums.txt'
        checksums.write_text(f'{digest(binary / "libzlink.so")}  ./libzlink-linux-x64/libzlink.so\n')
        (assets / 'release-provenance.txt').write_text(
            f'tag=core/v{version}\nruntime_version={version}\n'
            f'checksums_sha256={digest(checksums)}\n'
            f'source_archive_sha256={digest(assets / f"zlink-{version}-source.tar.gz")}\n'
            f'source_sha={"1" * 40}\n')
        bin_dir = base / 'bin'
        bin_dir.mkdir()
        gh = bin_dir / 'gh'
        gh.write_text('''#!/usr/bin/env python3
import os, pathlib, shutil, sys, time
args = sys.argv
ready = pathlib.Path(os.environ['RACE_READY'])
go = pathlib.Path(os.environ['RACE_GO'])
ready.touch()
deadline = time.monotonic() + 30
while not go.exists():
    if time.monotonic() > deadline: sys.exit('download barrier timed out')
    time.sleep(.01)
shutil.copyfile(pathlib.Path(os.environ['RACE_ASSETS']) / args[args.index('-p')+1], args[args.index('-O')+1])
''')
        gh.chmod(0o755)
        for iteration in range(1, args.iterations + 1):
            run = base / str(iteration)
            run.mkdir()
            env = dict(os.environ, PATH=f'{bin_dir}:{os.environ["PATH"]}',
                       GH_TOKEN='offline-fixture', ZLINK_CORE_RELEASE_VERSION=version, RACE_ASSETS=str(assets),
                       RACE_READY=str(run / 'ready'), RACE_GO=str(run / 'go'),
                       ZLINK_CORE_CACHE_DIR=str(run / 'cache'), TMPDIR=str(run))
            command = ['bash', str(fetch), '--platform', 'linux-x64']
            with (run / 'fetch.log').open('w+') as log:
                release = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 30
                    while not (run / 'ready').exists():
                        if release.poll() is not None or time.monotonic() > deadline:
                            raise RuntimeError('fetch did not reach download barrier')
                        time.sleep(.01)
                    local = subprocess.run(['bash', str(local_script), str(build)], env=env,
                                           capture_output=True, text=True, check=True)
                    prefix = Path(re.search(r'Core prefix materialized: (.*?) \(from ', (local.stdout + local.stderr))[1])
                    if args.scripts_root == ROOT:
                        assert local.stdout.strip() == str(prefix), 'local publisher stdout must be its exact prefix'
                    approved = snapshot(prefix, version)
                    (run / 'go').touch()
                    if release.wait(timeout=30) != 0:
                        log.seek(0)
                        raise RuntimeError('fetch failed:\n' + log.read())
                    log.seek(0)
                    fetched = Path(log.read().splitlines()[-1])
                    assert snapshot(prefix, version) == approved, 'approved local provenance changed after release publish'
                    approved_release = snapshot(fetched, version)
                    # An older worktree can still replace the legacy mutable prefix.
                    legacy = run / 'cache' / version / 'linux-x64'
                    legacy.mkdir(parents=True, exist_ok=True)
                    shutil.rmtree(legacy)
                    assert snapshot(prefix, version) == approved, 'legacy publisher deleted approved local prefix'
                    assert snapshot(fetched, version) == approved_release, 'legacy publisher deleted approved release prefix'
                    subprocess.run(['bash', str(local_script), str(build)], env=env,
                                   capture_output=True, check=True)
                    assert snapshot(fetched, version) == approved_release, 'approved release provenance changed after local publish'
                    cached = subprocess.check_output(command, env=env, text=True).strip()
                    assert Path(cached) == fetched, 'release cache hit selected different provenance'
                    # The read-only session diagnostic must recognize the published cache.
                    status_source = (ROOT / 'scripts/dev/session-setup.sh').read_text()
                    status_function = re.search(r'check_core_prefix\(\) \{.*?\n\}', status_source, re.S)[0]
                    status = subprocess.check_output(
                        ['bash', '-c', status_function + '\ncheck_core_prefix\nprintf "%s\\n" "$CORE_STATUS"'],
                        env=dict(env, REPO_ROOT=str(ROOT), ZLINK_CORE_RELEASE_PLATFORM='linux-x64'), text=True)
                    assert '준비됨' in status and str(fetched) in status, 'session diagnostic missed published release'
                    # Two force publishers must reuse the same complete entry, without nesting it.
                    with (run / 'force.log').open('w') as force_log:
                        first = subprocess.Popen(command + ['--force'], env=env, stdout=force_log, stderr=force_log)
                        try:
                            second = subprocess.run(command + ['--force'], env=env,
                                                    capture_output=True, text=True, timeout=30)
                            assert first.wait(timeout=30) == 0 and second.returncode == 0, 'concurrent force publish failed'
                            assert Path(second.stdout.strip()) == fetched, 'force publish changed exact prefix'
                            assert snapshot(fetched, version) == approved_release, 'force publish changed approved provenance'
                        finally:
                            if first.poll() is None:
                                first.kill()
                                first.wait()
                    print(f'PASS {iteration}: approved local/release prefixes stable; legacy replacement isolated; concurrent force reuse', flush=True)
                except (AssertionError, FileNotFoundError) as error:
                    failures += 1
                    print(f'FAIL {iteration}: {error}', flush=True)
                except subprocess.CalledProcessError as error:
                    raise RuntimeError(f'{error.cmd} failed:\n{error.stdout}\n{error.stderr}') from error
                finally:
                    if release.poll() is None:
                        release.kill()
                        release.wait()
            shutil.rmtree(run)
    print(f'iterations={args.iterations} passed={args.iterations - failures} failed={failures}')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
