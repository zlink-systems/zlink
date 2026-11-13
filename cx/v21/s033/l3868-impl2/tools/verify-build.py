#!/usr/bin/env python3
import hashlib
import json
import shlex
from pathlib import Path
import subprocess
import sys

root = Path('/root/worktree/zlink-v21-s033/l3868-next')
build = root / 'build-l3868-next'
step = sys.argv[1]
cache = (build / 'CMakeCache.txt').read_text()
settings = ('CMAKE_BUILD_TYPE:STRING=Release',
            'ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE:BOOL=ON',
            'ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX:PATH=/root/.cache/zlink/core-release/1.25.0/linux-x64')
for setting in settings:
    assert setting in cache, setting
all_deps = subprocess.check_output(['ninja', '-C', str(build), '-t', 'deps'], text=True)
dependencies = {}
current = None
for line in all_deps.splitlines():
    if ': #deps' in line:
        current = line.split(': #deps', 1)[0]
        dependencies[current] = set()
    elif current and line.startswith('    '):
        path = (build / line.strip()).resolve()
        if path.is_file() and path.is_relative_to(root) and path.suffix in ('.cpp', '.hpp', '.h', '.inc', '.proto'):
            dependencies[current].add(path)
records = []
for name in ('bench_cpp_client', 'bench_cpp_framework_server', 'bench_cpp_zlink_server',
             'bench_cpp_grpc_server', 'bench_cpp_ladder_contract_test'):
    binary = build / name
    inputs = subprocess.check_output(['ninja', '-C', str(build), '-t', 'inputs', name], text=True)
    sources = set()
    commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', name], text=True)
    for command in commands.splitlines():
        arguments = shlex.split(command)
        if '-o' in arguments:
            output = arguments[arguments.index('-o') + 1]
            sources.update(dependencies.get(output, ()))
    for line in inputs.splitlines():
        path = (build / line).resolve()
        if path.is_file() and path.is_relative_to(root) and path.suffix in ('.cpp', '.hpp', '.h', '.inc', '.proto'):
            sources.add(path)
    assert sources, name
    latest = max(sources, key=lambda path: path.stat().st_mtime_ns)
    assert binary.stat().st_mtime_ns > latest.stat().st_mtime_ns, (name, str(latest))
    records.append(dict(binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                        binary_mtime_ns=binary.stat().st_mtime_ns, source_count=len(sources),
                        newest_source=str(latest), source_mtime_ns=latest.stat().st_mtime_ns))
record = dict(settings=settings, step=step, head=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
              status=subprocess.check_output(['git', 'status', '--short'], cwd=root, text=True),
              records=records)
output = Path('/root/jobs/fx/cx/v21/s033/l3868-impl2/raw') / f'build-{step}-evidence.json'
output.write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
print(f'{step}: {len(records)} binaries newer than all transitive worktree sources; sha256 in {output}')
