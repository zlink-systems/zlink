#!/usr/bin/env python3
"""Seal a successful local build, then verify it immediately before every cell."""
import datetime, hashlib, json, pathlib, subprocess, sys
REPO=pathlib.Path('/root/worktree/zlink-v21-s033/cfx')
JOB=pathlib.Path('/root/jobs/fx/cx/v21/s033/lfx-cpp')
BUILD=REPO/'build-cfx'
MANIFEST=JOB/'raw/build-provenance.json'
def git(*args): return subprocess.check_output(['git','-C',str(REPO),*args],text=True).strip()
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def snapshot():
    roots=['framework/bench/grpc/cpp','framework/languages/cpp/framework','framework/languages/cpp/common','bindings/cpp','framework/runtime/protocol/generated/cpp']
    files={p for root in roots for p in (REPO/root).rglob('*') if p.is_file() and (p.suffix in ('.cpp','.cc','.hpp','.h','.inc','.cmake') or p.name in ('CMakeLists.txt','CMakePresets.json')) and 'build' not in p.relative_to(REPO/root).parts}
    files.add(REPO/'framework/languages/cpp/CMakeLists.txt')
    files.add(REPO/'framework/bench/grpc/proto/bench.proto')
    files.update(p for p in (BUILD/'proto_gen').glob('*') if p.suffix in ('.cc','.h'))
    return {str(p.relative_to(REPO)):sha(p) for p in sorted(files)}
def binaries():
    return {str((BUILD/name).resolve()):sha(BUILD/name) for name in ['bench_cpp_client','bench_cpp_framework_server','bench_cpp_zlink_server','bench_cpp_grpc_server']}
head=git('rev-parse','HEAD'); base=git('merge-base','HEAD','origin/main')
assert git('branch','--show-current')=='exp/3868-cpp-ladder', 'wrong branch'
dirty=bool(git('status','--porcelain'))
if sys.argv[1]=='--seal':
    source_times = [(REPO / path).stat().st_mtime_ns for path in snapshot()]
    for binary in binaries():
        assert pathlib.Path(binary).stat().st_mtime_ns > max(source_times), (binary, 'binary older than source')
    cache=(BUILD/'CMakeCache.txt').read_text()
    for expected in ['CMAKE_BUILD_TYPE:STRING=Release','ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE:BOOL=ON','ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX:PATH=/root/.cache/zlink/core-release/1.25.0/linux-x64',f'CMAKE_HOME_DIRECTORY:INTERNAL={REPO}/framework/bench/grpc/cpp']:
        assert expected in cache, expected
    commands=json.loads((BUILD/'compile_commands.json').read_text())
    for command in commands:
        path=pathlib.Path(command['file']).resolve()
        assert path.is_relative_to(REPO),(path,'source outside this worktree')
        assert '/root/zlink/' not in command['command'],command['file']
    owner_commands=[c for c in commands if c['file'].endswith('/raw_mesh_node_owner.cpp')]
    assert len(owner_commands)==1 and '-DZLINK_CPP_ABLATION_LADDER=1' in owner_commands[0]['command']
    assert '-O3' in owner_commands[0]['command'] and '-DNDEBUG' in owner_commands[0]['command']
    for name in ['bench_cpp_client','bench_cpp_framework_server','bench_cpp_zlink_server']:
        linked=subprocess.check_output(['ldd',str(BUILD/name)],text=True)
        assert 'not found' not in linked,linked
        assert '/root/.cache/zlink/core-release/1.25.0/linux-x64/lib/libzlink.so' in linked,linked
    value={'commit':head,'dirty':dirty,'base':base,'worktree':str(REPO),'built_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'sources':snapshot(),'binaries':binaries(),'compile_commands_sha256':sha(BUILD/'compile_commands.json'),'core_library':str(pathlib.Path('/root/.cache/zlink/core-release/1.25.0/linux-x64/lib/libzlink.so').resolve()),'core_sha256':sha(pathlib.Path('/root/.cache/zlink/core-release/1.25.0/linux-x64/lib/libzlink.so'))}
    MANIFEST.write_text(json.dumps(value,indent=2)+'\n')
    print(f'sealed {head} dirty={dirty}: all compiler source paths belong to {REPO}; cached Core prefix verified')
else:
    manifest=json.loads(MANIFEST.read_text())
    assert sha(pathlib.Path(manifest['core_library']))==manifest['core_sha256'],'native Core changed since build'
    assert snapshot()==manifest['sources'],'source changed since successful build'
    assert binaries()==manifest['binaries'],'binary changed since successful build'
    assert sha(BUILD/'compile_commands.json')==manifest['compile_commands_sha256'],'compile commands changed'
    assert head==manifest['commit'] and dirty==manifest['dirty'],'commit/dirty changed since successful build'
    latest_source=max((REPO / path).stat().st_mtime_ns for path in manifest['sources'])
    assert all(pathlib.Path(binary).stat().st_mtime_ns > latest_source for binary in manifest['binaries']), 'binary older than source'
    cell=pathlib.Path(sys.argv[1]); cell.mkdir(parents=True,exist_ok=True)
    value={k:v for k,v in manifest.items() if k!='sources'}
    value['verified_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    (cell/'binary-provenance.json').write_text(json.dumps(value,indent=2)+'\n')
    line=f"- {value['verified_utc']} | {cell} | built commit={head} dirty={int(dirty)} | base={base} | THIS worktree={REPO} | "+' ; '.join(f'{p} sha256={s}' for p,s in value['binaries'].items())+'\n'
    with (JOB/'progress.md').open('a') as handle: handle.write(line)
    # Source hashes above preserve the parity analysis performed outside the lock.
    parity=(JOB/'raw/preflight/reply-parity.md').read_text()
    (cell/'reply-parity.md').write_text(parity)
    with (JOB/'progress.md').open('a') as handle: handle.write(parity)

    print(f'verified binaries from THIS worktree: {cell}')
