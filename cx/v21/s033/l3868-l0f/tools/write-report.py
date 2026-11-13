#!/usr/bin/env python3
import datetime,json,pathlib,subprocess
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868-l0f');R=J/'raw';REPO=pathlib.Path('/root/worktree/zlink-v21-s033/l3868')
summary=json.loads((R/'summary.json').read_text()) if (R/'summary.json').exists() else {'matrix':[],'cross':{},'incomplete':['측정 전']}
manifest=json.loads((R/'build-provenance.json').read_text()) if (R/'build-provenance.json').exists() else {}
complete=len(summary['matrix'])==6 and len(summary['cross'])==3 and all(c['pass_0_95'] and c['g5_pass'] for c in summary['matrix']) and all(c['g5']=='pass' for c in summary['cross'].values())
head=subprocess.check_output(['git','-C',str(REPO),'rev-parse','HEAD'],text=True).strip()
dirty=bool(subprocess.check_output(['git','-C',str(REPO),'status','--porcelain'],text=True).strip())
def loc(path,needle):
 p=REPO/path;line=next(i for i,t in enumerate(p.read_text().splitlines(),1) if needle in t)
 return f'[{p.name}:{line}]({p}:{line})'
client='framework/bench/grpc/cpp/client/bench_cpp_client.cpp';driver='framework/bench/grpc/cpp/ladder/ladder_driver.inc';owner='framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp';raw='framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp';wire='framework/bench/grpc/cpp/ladder/ladder_wire.hpp'
text=['# #4350 C++ L0 — '+('완료' if complete else '완료 기준 미충족'),'',('전체 셀의 L0/binding ≥0.95와 G5를 충족했다.' if complete else '전체 완료 기준은 아직 충족하지 못했다. 아래 수치 판정과 G5 재현성 판정을 함께 확인해야 한다.'),'',f'로컬 커밋 `{head}`, dirty={int(dirty)}, branch `exp/3868-cpp-ladder`. Push·PR·merge는 하지 않았다. [빌드 출처·바이너리 SHA-256](raw/build-provenance.json). 이전 [l3868 raw](../l3868/raw/)는 그대로 보존했다.','',
f'수신 대기는 raw {loc(raw,"const int rc = router.recv")}와 L0 {loc(owner,"const int rc = _router->recv (received, zlink::recv_flags_t::none);")}에서 같은 direct blocking recv를 사용한다. 같은 stop.load와 recv 오류 처리 뒤 record 하나를 처리하며 별도 poller wait·DONTWAIT drain을 제거했다. Source는 raw {loc(client,"submit_raw (_socket")}와 L0 {loc(driver,"submit_raw (*_socket")}가 공유하는 {loc(client,"void submit_raw (")}로 payload encode·Message 생성·submit·callback을 실행한다. 기존 raw submit 본문과 같은지는 매 실행 전에 검사했다.','',
f'빈 Message 생성·move 대입·반환 array 이동은 실제 L0 source 경로에서 제거했다. L0에서 쓰지 않는 별도 context·poller와 per-stream encoder도 생성하지 않는다. Source POLLCOMPLETION wait도 raw {loc(client,"_poller.wait (&event")}와 L0 {loc(driver,"_owner->ablation_poller ().wait")}가 일치한다. Native send buffer 복사는 양쪽 공통 Core {loc("core/src/runtime/protocol/encoder.hpp","memcpy (buffer + pos")}의 동작이다. 같은 Core library를 사용하므로 Framework 추가 복사로 분류하지 않는다. [매 실행의 수신·응답·source 비용 대조](progress.md), [수정 후 대조 원본](raw/parity-source-resources/reply-parity.md).','',
f'응답은 raw {loc(raw,"const char *envelope =")}와 L0 {loc(wire,"const char *envelope =")}에서 새 envelope part·새 BenchPayload body 복사·protobuf encode·새 body part를 만들고 같은 2-part reply submit을 사용한다. Target counter는 같은 server_metrics_t::record다. 요청 part를 재사용하지 않는다.','',
'측정 조건은 kairos 서버의 exclusive measure lock, source CPU 0–7 / target CPU 8–15, warmup 30초 / active 5초다. Send-saturation 4K는 처음부터 5회, 나머지는 3회이며 G5 실패 시 5회로 늘리고 첫 실행도 중앙값에 포함했다. 유효 실행은 errors=0, submitted=completed=received인 결과만 수락한다. L0와 binding을 매 반복에서 연이어 측정한다. [원시 결과 전체·counts·중앙값](raw/summary.json), [lock 전후 상태](raw/lock-windows/).','']
text += ['변경 파일:', '',
    f'- [{client}]({REPO/client}): raw source submit을 공통 driver로 옮겼다.',
    f'- [{driver}]({REPO/driver}): L0 source가 공통 submit·completion wait를 사용하며 미사용 자원을 생성하지 않는다.',
    f'- [{owner}]({REPO/owner}): L0는 직접 blocking recv를 사용하며 미사용 submit helper를 제거했다.',
    f'- [raw_mesh_node_owner.hpp]({(REPO/owner).with_suffix('.hpp')}): 실험용 poller 접근자를 선언하고 미사용 helper 선언을 제거했다.', '']
if (R/'tables.md').exists():text.append((R/'tables.md').read_text())
else: text.append('새 측정은 아직 시작하지 않았다. 위 측정·검증 절차의 실행 결과는 진행 파일에서 확인한다.')
if summary.get('incomplete'):text+=['','남은 측정: '+', '.join(summary['incomplete'])+'.']
text+=['','관련 검증: [Release 빌드](raw/build.log), [바이너리 최신성·wire 양성/음성 검사](raw/verify-build.txt), [bench 검사](raw/bench-tests.txt), [결과 count 검사 양성/음성](raw/checker-fixtures/validation.txt), [run-group 복사 범위 검사](raw/wrapper-validation.txt).',
'', 'C++ build 조건은 지정 build-slot·-j3·Core 1.25.0 prefix·bindings source ON이다. 바이너리는 이 worktree의 build-l3868로 제한하며, 각 실행 전 commit·dirty·base·경로·SHA-256과 source 최신성을 검사한다.','']
if (R/'cost-findings.md').exists():text +=[(R/'cost-findings.md').read_text(),'']
under=[c for c in summary['matrix'] if not c['pass_0_95']]
if under and not (R/'cost-findings.md').exists():text+=['미달 셀 비용 귀속은 아직 작성되지 않았다. 완료로 판정하지 않는다.','']
text +=['소유 계층: 물리 receive·admission·HWM retry·native completion은 Core/binding이다.','',
'스펙 근거: messaging hot path §1·§3·§4. 이번 L0는 사용자가 승인한 실험이며 제품 계약 준수를 주장하지 않는다.','',
'교차언어 대조: 사용자 지정 C++ 단독 L0 실험이다. 타 언어 runtime·public spec은 수정하지 않았다.','',
'변경 분류: C(승인된 실험 전용 기능 제외). 제품 A/B 수정으로 분류하지 않는다.','',
'규칙 수: source encode·submit 알고리즘 판정 위치 2→1. 물리 admission·retry 판정 위치는 1→1(Core/binding). 새 per-message 상태·queue·lock은 추가하지 않았다.','',
'호출 경로 점검: L0 source의 임시 Message 수명 작업과 target의 별도 poller/DONTWAIT 수신을 제거했다. Native Core buffer 복사는 양쪽 공통 작업으로 확인했다. 범위 밖 제품 수정을 넘긴 항목은 없다.','',
'절차 이탈: 공통 규칙을 읽기 전에 기본 저장소 지침의 internal-docs.sh를 한 번 실행했다. 이후에는 실행하지 않았다.','',
f'기록 시각: {datetime.datetime.now(datetime.timezone.utc).isoformat()}.']
if not manifest:
    text += ['', '현재 새 바이너리·새 성능 측정은 없다. 빌드·wire·bench 실행 검증은 미완료이며 관련 원시 검증 파일이 생성된 뒤에만 통과로 판정한다.']
(J/'report.md').write_text('\n'.join(text)+'\n')
print(J/'report.md')
