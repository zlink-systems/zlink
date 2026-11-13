from pathlib import Path
import json,subprocess

job=Path('/root/jobs/fx/cx/v21/s033/trole2')
repo=Path('/root/worktree/zlink-v21-s033/trole')
runtime=repo/'framework/languages/cpp/framework/src/runtime'
units=repo/'framework/languages/cpp/tests/Zlink.Framework.UnitTests'
handoff=units/'test_cpp_framework_receive_role_handoff.cpp'
commits=json.loads((job/'logs/commits.json').read_text())
passes=json.loads((job/'logs/handoff-final-50.json').read_text())
negatives=json.loads((job/'negative/results.json').read_text())
assert len(passes)==17 and set(passes.values())=={50}
assert len(negatives)==14 and all(n['rejected'] for n in negatives)
def line(path,needle):
    found=[i for i,s in enumerate(path.read_text().splitlines(),1) if needle in s]
    assert found,(path,needle)
    return found[0]
def ref(path,number,label=None):
    return f'[{label or (path.name+":"+str(number))}]({path}:{number})'
def at(path,needle,label=None):return ref(path,line(path,needle),label)
def log(name,number,label=None):return ref(job/name,number,label)
executor=runtime/'dispatch/offload_executor.cpp'
header=runtime/'dispatch/offload_executor.hpp'
host=runtime/'mesh/mesh_node_host_service.cpp'
mailbox=runtime/'mesh/service_mailbox.cpp'
spec=repo/'framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md'
complete='measurement' in commits
for kind,sha in commits.items():
    files=subprocess.check_output(['git','diff-tree','--no-commit-id','--name-only','-r',sha],cwd=repo,text=True)
    (job/'logs'/f'{kind}-files.txt').write_text(sha+'\n'+files)
text=['# #1639 C++ 수신 역할 인계 — '+('완료' if complete else '계측 검증 중'),'']
text += [f'- 제품 `{commits["product"]}`; 계측 `{commits.get("measurement","아직 미커밋")}`. '+log('logs/commits.json',2),
         f'- 유지할 빌드: `{repo}/build-trole`. '+log('logs/build-inputs.txt',2),
         '- Release·tests ON·binding source ON·`-j3 -k 0`·가상메모리 25 GiB. '+log('logs/build-inputs.txt',5),
         '']
text += ['**변경·소유**','',
 '- 제품 11파일: runtime 4·unit tests 5·CMake/contract 2. '+log('logs/product-files.txt',2),
 f'- host는 한 회전 뒤 반환하고 executor가 수신·게시 stack 종료 후 한 번 try-lock하여 idle slot과 정상 queue head를 교환한다. '+at(host,'bool run () override')+'; '+at(executor,'offload_executor_t::try_handoff (worker_t &worker)'),
 f'- `_roles`·idle 개수·공용 CV를 worker assignment·idle 목록·개별 CV로 교체했다. 게시 scope는 복원되는 TLS 문맥으로 식별하며 역할·예약 권위는 assignment 하나다. '+at(header,'std::optional<work_item_t> assignment')+'; '+at(executor,'thread_local offload_executor_t *offload_executor_t::_publishing_executor'),
 f'- 실제 판정 위치 **12→11**, 배치 모듈 소유자 **1→1**. 수신 worker 선택만 **2→1**로 합쳤고 승인 표의 다른 판정 10개는 유지했다. '+at(executor,'offload_executor_t::assign_worker_locked (worker_t *current)')+'; '+ref(Path('/root/jobs/fx/cx/v21/s033/trole/design.md'),104),
 f'- 역할 이동은 기존 credit을 유지하고, owner 알림은 게시 callback이 수락한 뒤 제거한다. '+at(executor,'Moving a role preserves its credit')+'; '+at(mailbox,'ready->second (ready->first.front ());'),
 '- tooling smoke에 메인 빌드의 source binding 옵션을 전달했다. '+at(repo/'framework/languages/cpp/CMakeLists.txt','      ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE)')+'; '+at(repo/'framework/languages/cpp/tests/Zlink.Framework.ContractTests/verify_tooling_contract.cmake','    ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE)'),
 '',
 '- 소유 계층: Framework executor. '+at(executor,'offload_executor_t::assign_worker_locked (worker_t *current)'),
 '- 조항: S08 §4.4의 독점 배정·수신 종료·실패 경로·자원 상한. '+ref(spec,309),
 '- 교차언어 대조: '+ref(Path('/root/jobs/fx/cx/v21/s033/trole/logs/dotnet-6821c3b8af.cs'),307,'.NET 6821c3b8af')+'는 예약 교환, '+ref(repo/'framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java',5592,'Java')+'/'+ref(repo/'framework/languages/java/zlink-framework-kotlin/build.gradle.kts',13,'Kotlin')+'은 고정 pump, '+ref(repo/'framework/languages/node/packages/framework/src/runtime/backend/mesh-dispatch-pump.ts',94,'Node')+'는 event loop다. 물리 배치 차이를 유지하며 T12는 후속이다.',
 '- 변경 분류: **A, 기존 S08 계약 적응**. '+ref(spec,309),
 '']
text += ['**검사**','',
 '표의 음성 수는 의도한 결함을 검출한 실행 수다. T04는 예상된 SIGABRT, 나머지는 exit 1이었다. '+log('logs/negative-run.log',1),
 '', '| 항목 | 양성 | 음성 변형·검출 |', '|---|---|---|']
descriptions={
 'T01':'인계 생략', 'T02':'blocking lock', 'T03':'회전당 try-lock 65회',
 'T04':'수신 역할에서 application 직접 실행', 'T05':'FIFO 역전·독립 gate 직렬화',
 'T06':'filter 전 permit 반환', 'T07':'Spot 공유 gate 제거',
 'T08':'completion worker 1개', 'T09':'역할 credit 조기 반환·종료 queue 오판',
 'T10':'queue tail 선택·successor 이중 통지', 'T11':'수락 전 알림 제거'}
for ident in ['T01','T02','T03','T04','T05','T06','T07','T08','T09','T10','T11']:
    cases=[n for n in passes if n.startswith('ReceiveRoleHandoff.'+ident)]
    control=[(i,n) for i,n in enumerate(negatives,1) if n['id'].startswith(ident)]
    if ident=='T04':
        source=at(units/'test_cpp_framework_mailbox_owner_dispatch.cpp','int main (',ident)
        positive=log('logs/focused-driver.log',4,'TCP 50/50')
    else:
        source=at(handoff,'TEST (ReceiveRoleHandoff, '+ident,ident)
        positive=at(job/'logs/handoff-final-50.json','"ReceiveRoleHandoff.'+ident,f'{len(cases)}개 각각 50/50')
    count=len(control)
    negative=log('logs/negative-run.log',control[0][0],f'{count}/{count}')
    text.append(f'| {source} | {positive} | {descriptions[ident]} {negative} |')
text += ['',
 '- handler-entry 5개와 기존 executor·TCP mailbox·receive completion·Yield·operation registry 실행 파일도 각각 50회 통과했다. '+log('logs/focused-driver.log',2),
 '- 전체 unit/contract **302개 1회: 301 통과·tooling 1 실패**. source binding 옵션 전달 누락을 수정한 뒤 해당 검사만 **1/1 통과**했다. '+log('logs/full-suite.log',1106)+'; '+log('logs/tooling-source-forward.log',4),
 '- T05 같은 Node owner 조건은 `DISABLED_`로 pass set에서 제외했다. 별도 실행은 **50회 모두 예상 실패**였다. 기존 active-prefix 판정은 유지했다. '+at(handoff,'TEST (ReceiveRoleHandoff, DISABLED_T05KnownFailureSameNodeOwnerProgress)')+'; '+log('logs/focused-driver.log',8)+'; '+ref(mailbox,212)+'; '+ref(mailbox,235),
 '- 제품 `format.sh --check cpp`·diff check 통과. '+log('logs/finalize-driver.log',2),
 '']
measurement=runtime/'dispatch/receive_role_measurement.hpp'
text += ['**계측·남은 위험**','',
 '- `ZLINK_PERF_RECEIVE_ROLE_LOG=/절대경로/run.tsv`로 A(시도)·H(성공)·E(try-lock 획득 후 idle 확인 전 eligible head)·R(완료 회전)과 handler 시작/local 시작을 수집한다. 제품 커밋 뒤에만 적용했다. '+ref(measurement,19)+'; '+at(executor,'E counts rotations'),
 '- local은 같은 회전 record가 이전 수신자의 다음 application turn에서 실제 handler entry에 도달한 수다. H와 별도로 센다. '+at(measurement,'inline void record_started')+'; '+at(host,'receive_role_measurement::record_started (rotation);')]
metrics_path=job/'logs/measurement-validation.json'
if metrics_path.exists():
    m=json.loads(metrics_path.read_text())
    text += [f'- 계측 동작 확인: R/E/A/H={m["R"]}/{m["E"]}/{m["A"]}/{m["H"]}, local/시작={m["records_started_locally"]}/{m["records_started"]}. H≤E≤A≤R, local≤시작 및 H/local>0을 확인했다. '+log('logs/receive-role-counters.tsv',2)]
else:text += ['- 계측 빌드·env OFF/ON 동작 확인·별도 커밋은 진행 중이다. '+log('logs/finalize-driver.log',13)]
if complete:
    text += ['- 계측 변경은 별도 3파일이다. '+log('logs/measurement-files.txt',2)]
    text += ['- 계측 env OFF 관련 검사, env ON TCP 검사, 최종 형식·diff 검사 통과. '+at(job/'logs/finalize-driver.log','END test-measurement-bundle 0')+'; '+at(job/'logs/finalize-driver.log','END format-final 0')]
text += [
 '- TSV는 정상 종료 시의 프로세스 누적값이다. warmup/active 구간을 분리하지 않으며 env ON은 계측 비용을 추가한다. '+ref(measurement,19)+'; '+ref(measurement,23)+'; '+at(measurement,'std::make_shared<rotation_t>'),
 '- 벤치마크는 미실행이며 성능 효과는 미판정이다. T12와 §9 측정은 감독자 후속이다. '+ref(job/'test_product.py',11)+'; '+at(job/'finalize.py','for key in ['),
 '- 리팩터링 점검 3건 중 고정 수신 판정·idle 표현 2건 정리, 기존 prefix 1건 보류. '+at(executor,'offload_executor_t::assign_worker_locked (worker_t *current)')+'; '+at(header,'std::optional<work_item_t> assignment')+'; '+ref(mailbox,212),
 '']
(job/'report.md').write_text('\n'.join(text))
