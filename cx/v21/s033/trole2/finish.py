from pathlib import Path
import subprocess, os, json, resource

root=Path('/root/worktree/zlink-v21-s033/trole')
job=Path('/root/jobs/fx/cx/v21/s033/trole2')
build=root/'build-trole'
resource.setrlimit(resource.RLIMIT_AS,(26843545600,26843545600))
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
env=os.environ.copy()
for key in ['ZLINK_STREAM_FLOW_BENCH','ZLINK_ACTOR_HEADER_BENCH','ZLINK_PERF_RECEIVE_ROLE_LOG']:
    env.pop(key,None)
def run(name,command,*,cwd=root,extra=None,timeout=None,compile=False,test=False):
    if compile:
        command=['/root/jobs/fx/cx/cpp-build-slot.sh',*command]
    if test:
        command=['bash','-c','. /root/jobs/fx/cx/measure-gate.sh; measure_gate_pass; exec flock -o -s "$MEASURE_LOCK" "$@"','trole-tests',*command]
    print('START',name,flush=True)
    with (job/'logs'/f'{name}.log').open('w') as output:
        result=subprocess.run(command,cwd=cwd,env=env| (extra or {}),stdout=output,stderr=subprocess.STDOUT,timeout=timeout)
    print('END',name,result.returncode,flush=True)
    result.check_returncode()
def git(*args):
    return subprocess.check_output(['git',*args],cwd=root,text=True).rstrip()

run('build-product-bundle',['python3',str(job/'build_product.py')],compile=True)
run('test-product-bundle',['python3',str(job/'test_product.py')],test=True)
run('format-product',['bash','scripts/format/format.sh','--check','cpp'])
run('diff-check-product',['git','diff','--check'])
assert git('branch','--show-current')=='perf/1639-cpp-target-receive-role'
assert git('rev-parse','HEAD')=='76a4d7850664c2cb32e8d35837dac45d9d2a4057'
assert not git('diff','--cached','--name-only')
product=[
 'framework/languages/cpp/CMakeLists.txt',
 'framework/languages/cpp/framework/src/runtime/dispatch/offload_executor.cpp',
 'framework/languages/cpp/framework/src/runtime/dispatch/offload_executor.hpp',
 'framework/languages/cpp/framework/src/runtime/mesh/mesh_node_host_service.cpp',
 'framework/languages/cpp/framework/src/runtime/mesh/service_mailbox.cpp',
 *['framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_'+name+'.cpp' for name in ['handler_entry_permit','mailbox_owner_dispatch','offload_executor','receive_role_completion','receive_role_handoff']]]
assert {line[3:] for line in git('status','--porcelain').splitlines()}==set(product)
run('stage-product',['git','add','--',*product])
run('commit-product',['git','commit','-m','perf(cpp): assign target receive roles at rotation boundaries (#1639)',
    '-m','Executor가 회전 반환 뒤 idle worker의 역할과 정상 queue head를 한 곳에서 배정한다. 역할 credit과 기존 claim/gate/permit 경로를 유지한다.\n\n판정 위치 12→11, 배치 모듈 소유자 1→1. T01–T11 반복 검사와 음성 대조를 추가하고, 알려진 T05 same-owner 실패는 pass set에서 제외한다.'])
commits={'product':git('rev-parse','HEAD')}
(job/'logs/commits.json').write_text(json.dumps(commits,indent=2)+'\n')
assert not git('status','--porcelain')
run('apply-measurement',['python3',str(job/'add_measurement.py')])
base=root/'framework/languages/cpp/framework/src/runtime'
measurement=[str(base/'dispatch/offload_executor.cpp'),str(base/'mesh/mesh_node_host_service.cpp'),str(base/'dispatch/receive_role_measurement.hpp')]
source=(base/'dispatch/offload_executor.cpp').read_text()
for needle in ['::begin_rotation ();','::end_rotation ();','::enter_application (','().attempts.fetch_add','().eligible.fetch_add','().successes.fetch_add','::local_turn.reset ();']:
    assert source.count(needle)==1,(needle,source.count(needle))
assert (base/'mesh/mesh_node_host_service.cpp').read_text().count('::record_started (rotation);')==1
run('format-measurement-files',['clang-format-18','-i',*measurement])
run('build-measurement',['cmake','--build',str(build),'-j3','--','-k','0'],compile=True)
run('test-measurement-bundle',['python3',str(job/'test_measurement.py')],test=True)
metrics=job/'logs/receive-role-counters.tsv'
lines=metrics.read_text().splitlines()
counts=dict(zip(lines[0].split('\t'),map(int,lines[1].split('\t'))))
assert 0<counts['H']<=counts['E']<=counts['A']<=counts['R'],counts
assert 0<counts['records_started_locally']<=counts['records_started'],counts
(job/'logs/measurement-validation.json').write_text(json.dumps(counts,indent=2)+'\n')
run('format-final',['bash','scripts/format/format.sh','--check','cpp'])
run('diff-check-measurement',['git','diff','--check'])
assert {line[3:] for line in git('status','--porcelain').splitlines()}=={str(Path(p).relative_to(root)) for p in measurement}
run('stage-measurement',['git','add','--',*measurement])
run('commit-measurement',['git','commit','-m','perf(cpp): add opt-in receive-role experiment counters (#1639)',
    '-m','ZLINK_PERF_RECEIVE_ROLE_LOG가 설정된 실행에서만 A/H/E/R과 실제 local handler 시작 수를 수집한다. 정상 종료 시 TSV를 기록한다. 제품 변경과 분리한 측정 전용 커밋이다.'])
commits['measurement']=git('rev-parse','HEAD')
(job/'logs/commits.json').write_text(json.dumps(commits,indent=2)+'\n')
assert not git('status','--porcelain')
print('DONE',commits,counts,flush=True)
