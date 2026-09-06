# Node M6A runtime contract suite 수정 결과

2026-09-06. 상시 실패 3건은 fixture의 계약 이탈을 수정했다. Test 34의 간헐 실패는
NotRequired handshake가 끝나기 전에 outbound intent를 제거하던 기존 runtime 결함이었다.
R6 host seal 변경은 유지했다. Commit과 branch 변경은 하지 않았다.

원인 위치는 작업 시작 시 보존한 R6 tree 기준이다. 아래 구현·test 경로의 기준은
`framework/languages/node/`, spec 경로의 기준은
`framework/doc/framework/common/spec/server/`다. 지정된 Node `AGENTS.md`는 존재하지 않아
root·Framework 지침을 적용했다. Runtime 수정은 이번 작업 지시의 기존 결함 수정 범위에서
원인·소유 계층·교차언어 대조·B 분류를 먼저 보고한 뒤 진행했다.

## F-M6A-1 — Monitor fixture의 필수 drain 누락

- **판정: fixture 이탈, A — 계약 적응.**
- Spec: `05-transport-liveness` §2의 binding public raw API 사용,
  `core/doc/spec/core/06-monitoring.ko.md` §2의 monitor 소비 계약.
  Framework의 정확한 port는 `runtime/backend/raw-binding-port.ts`의
  `ZLinkRawMonitorPort.drain(handler): number`다.
- 원인: `test/m6a/m6a-runtime.contract.ts:254`의 monitor double이 `statusReady`와 `close`만
  구현했다. Runtime의 `raw-service-mesh-runtime.ts:663`은 필수 `drain`으로 event를 소비한다.
- 수정: event가 없는 fixture에 `drain: () => 0`을 구현했다. Bind host와 advertised host,
  Hello 수·대상·wire descriptor에 대한 기존 assertion은 그대로다.
- 검증: 해당 focused test 통과. 최종 M6A 10회에서도 통과했다.

## F-M6A-2 — 폐기된 transport-pair 종료 API 기대

- **판정: fixture 이탈, A — 계약 적응.**
- Spec: `05-transport-liveness` §5의 descriptor RID·security identity·lifecycle fence,
  `core/doc/spec/core/socket/README.ko.md` §4 「rid 중복 정책」의 REJECT/HANDOVER 소유권
  (D-094), `03-mesh-node` §7.1의 connection intent 소유권.
- 원인: `test/m6a/m6a-runtime.contract.ts:718,746,802,813`이 현재 port에 없는
  `disconnectTransportPair`와 폐기된 pair ID·generation field를 만들고 정확한 pair 종료
  호출을 기대했다. Physical replacement는 Core가 소유한다.
- 수정: fixture를 현재 monitor·router port에 맞췄다. 완전한 discovery fence에 맞는
  replacement만 admission되고, 이전 lifecycle의 늦은 descriptor는 거부되는 assertion을
  유지했다. 이전 connection의 liveness가 해제되고 replacement는 아직 ready가 아니며,
  stale intent 제거와 늦은 descriptor가 현재 endpoint·RID 등록을 종료하지 않는지도 검증한다.
  Endpoint와 RID disconnect 호출이 없고 현재 expectation이 보존됨을 명시적으로 확인한다.
- 검증: 해당 focused test 통과. 최종 M6A 10회에서도 통과했다.

## F-M6A-3 — Logical monitor event와 폐기된 lane fixture

- **판정: fixture 이탈, A — 계약 적응.**
- Spec: `core/doc/spec/core/06-monitoring.ko.md` §3.1·§3.2·§9. Core가 ROUTER의 두 lane을
  logical peer 하나로 집계하고 READY edge는 한 번만 낸다. Flag 없는 READY는 count snapshot이다.
  성립한 physical connection의 종료는 DISCONNECTED로 관찰하며 CLOSED나 application
  receive drain을 기다리지 않는다(D-092). Framework ready 제거는 `05-transport-liveness` §5다.
- 원인: `test/m6a/m6a-runtime.contract.ts:845–928`은 현재 public monitor에 없는
  transport-pair ID·generation을 사용했다. READY의 connection ID 101·102와 DISCONNECTED의
  104·103을 폐기된 pair field로 연결하고, runtime이 그 관계를 복원할 것으로 기대했다.
- 수정: 현재 `ZLinkRawMonitorRecord`를 직접 사용한다. Logical READY edge 하나와 count 1인
  snapshot을 넣고 candidate가 하나인지 확인한다. 성립한 connection의 DISCONNECTED에서
  즉시 peer가 없어지고, 뒤따른 Completion lane event에도 없는 상태를 유지한다.
  기존 두 `peer === undefined` assertion과 snapshot 무시 assertion을 유지했으며 candidate
  정리 assertion을 추가했다. 폐기된 pair field는 제거하고 `value`는 port의 `bigint`로 맞췄다.
- 검증: 해당 focused test 통과. 최종 M6A 10회에서도 통과했다.

## F-M6A-4 — NotRequired handshake 도중의 조기 intent 제거

- 소유 계층: **Framework raw mesh**가 descriptor handshake와 logical intent 종료를 소유한다.
  Core의 send submit 성공은 상대 application의 descriptor 수신 완료를 뜻하지 않는다.
- Spec: `05-transport-liveness` §3·§6·§10, `01-channel-topology` §13의 양쪽 Object Client와
  Server membership 부재 판정·NotRequired monitoring·ready/liveness 제외·동일 설정 재연결 금지,
  `03-mesh-node` §7.1의 outbound intent 제거와 binding disconnect 단일 소유·terminal 규칙(D-098),
  `06-wire-protocol` §4의 Hello → Admit/Reject와 descriptor 교환.
- 교차언어: C++ `raw_mesh_node_owner.cpp:2912–2937`은 NotRequired Hello에 Admit descriptor를
  응답하고 응답을 받은 쪽에서 종료한다. .NET `ZLinkManagedMeshNode.cs:8176–8240`도 Hello 응답과
  outbound Admit 수신 시 disconnect를 구분한다. Java `ZLinkJavaRawMeshNode.java:6425–6437`은
  Hello에 Reject(4)를 보내지만 그 송신 자리에서 outbound transport를 끊지 않고,
  `:4447–4456`에서 Reject 수신 후 종료한다. Node만 Hello 거부 송신과 intent 제거를 즉시
  결합했다. C++·.NET과 같은 descriptor 교환을 재사용하며 기존 Reject(4) 수신 호환도 유지한다.
- 변경 분류: **B — 기존 결함.** R6 seal에서 새로 발생한 결함이 아니다.
- 원인: `runtime/foundation/raw-service-mesh-runtime.ts:777–795`가 NotRequired Hello를
  Reject로 응답한 직후 `retireNotRequiredExpectedPeer`를 호출하고, `:1557`에서 outbound
  transport를 종료했다. 상대가 아직 빈 연결 알림만 소비한 순서에서는 Hello·Reject가
  전달되기 전에 연결이 닫혀 한쪽에만 NotRequired가 남았다.
- 재현: 기존 control-plane 진단 로그를 먼저 읽고 `/tmp`의 외부 preload로 send·receive·
  intent 제거·monitor event를 파일에 기록했다. 원래 test 34의 body와 2초 상한을 바꾸지 않고
  같은 process에서 순차 반복하여 **53번째 실패(52 pass, 1 fail)**를 재현했다.
  `same-process-trace.jsonl`의 마지막 실행은 left Hello submit 실패 → Hello submit 성공 →
  right 빈 알림 수신·Hello 송신 → left Hello 수신·Reject submit 성공·즉시 disconnect →
  right DISCONNECTED다. Right에는 descriptor 수신 전이가 없다.
- R6 대조: committed HEAD의 raw runtime만 외부 module loader로 대입하고 나머지 실행 환경과
  설치된 binding을 유지한 동일 test가 **47번째에 같은 원인으로 실패(46 pass, 1 fail)**했다.
  이는 전체 pre-R6 tree gate가 아니라 원인 module 대조다. Callback이 없는 fixture에서는
  `peerAdmissionSealed?.() === true`가 false이며, 해당 seal 코드는 변경하지 않았다.
- 수정: 정상 NotRequired 결과를 오류 Reject 경로에서 분리했다. Hello에는 기존 Admit 인코더로
  자신의 descriptor를 응답하고, Admit을 받은 쪽에서 기존 intent 제거 경로를 호출한다.
  Topology의 NotRequired 판정·기록과 liveness 제외는 기존 소유자가 계속 담당한다.
- 대안: 전송 완료 뒤 별도 대기·flush 확인을 두는 방식은 새 상태와 종료 규칙이 필요하므로
  채택하지 않았다. 기존 Hello/Admit으로 양쪽 descriptor 교환을 완료하는 방식을 선택했다.
- 수정 전/후 규칙 수: **NotRequired intent 종료 계기 2 → 1** — Hello 수신과 terminal 응답
  수신에서 각각 종료하던 것을 terminal 응답 수신으로 통합했다. 새 상태·timer·retry·budget은 없다.
- 회귀: `test/contract/raw-mesh-not-required-admission.test.js` 5건. RID 지정/endpoint-only ×
  host callback 없음/열린 seal 4조합에서 connector가 상대 Hello를 먼저 받는 순서를 강제한다.
  양쪽 descriptor·generation·revision 보존, NotRequired 양쪽 기록, liveness 0, endpoint 종료 1회,
  늦은 READY와 announce의 terminal intent 재활성화 금지를 확인한다. 별도 1건은 Hello/Admit만
  교환하여 peer descriptor가 합성 값으로 바뀌지 않는지 검증한다.
- 검증: 신규 5건은 수정 전 R6와 committed raw module 모두 **0/5**, 수정 후 **5/5**다.
  기존 host admission seal 회귀 2건도 통과했다. 원래 test 34는 수정 후 같은 process에서
  **100/100**, 외부 tracing 없는 별도 process에서 **30/30** 통과했다. 원래 test 34의 body,
  assertion과 timeout은 변경하지 않았다. Repository에는 임시 tracing을 추가하지 않았다.

## Gate 결과

- 상시 실패 3건 focused: **3/3**.
- 신규 NotRequired 회귀 5건 + 기존 R6 host seal 회귀 2건: **7/7**.
- Test 34 단독: **30/30**, 추가 동일 process 대조 **100/100**.
- 최종 `flock -w7200 /tmp/zlink-node-gate.lock npm run verify:m6a-runtime`:
  **41/41 × 10회, failure·cancelled·skip 0**, 각 실행에 TypeScript build 포함.
- 전체 `flock /tmp/zlink-samples-gate.lock flock /tmp/zlink-node-gate.lock npm test`:
  **1회 실행, exit 0, 1,665/1,665**, failure·cancelled·skip 0.
  Build·typecheck·lint·sample-regression과 sample runner가 모두 통과했다.
- `git diff --check`: 통과. 최종 M6A 이전에 관찰한 별도 test 39 실패는 아래 BLOCKERS에 보존했다.

로그·native trace·원인 대조 harness는 `/tmp/zlink-node-m6a/`에 있다. 주요 파일은
`same-process-repro.log`, `same-process-trace.jsonl`, `pre-r6-same-process-repro.log`,
`pre-r6-same-process-trace.jsonl`, `regression-{before,pre-r6,after}.log`,
`fixed-same-process.log`, `single-final-{1..30}.log`, `m6a-final-{1..10}.log`,
`m6a-final-results.json`, `npm-test.log`, `npm-test-result.json`이다.

## Gate drift 판정

**`verify:m6a-runtime`을 `npm test`의 필수 검증에 포함할 것을 권고한다.**
`scripts/run_node_runtime_gate.js:170`은 `.test.js`만 수집한다. M6A의 TypeScript contract는
별도 tsconfig로 compile한 `.contract.js`여서 현재 npm gate에 들어가지 않는다. Monitor port,
descriptor fence, native admission/liveness 검증 41건이 빠져 기존 npm gate가 통과해도 이번
세 fixture 이탈과 간헐 실패를 검출하지 못했다. `package.json`과 gate script는 변경하지 않았다.
편입 방식은 감독이 결정한다.

## 변경 파일과 원인별 patch

Node 변경은 아래 3개 파일이다. 이 보고서는 별도 산출물이다.

- `packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts` — F-M6A-4.
- `test/m6a/m6a-runtime.contract.ts` — F-M6A-1·2·3.
- `test/contract/raw-mesh-not-required-admission.test.js` — F-M6A-4 신규 회귀.

통합 patch는 **`/tmp/zlink-node-m6a/fix.patch`**다. 작업 시작 시의 R6 tree 위에 적용하며
Node 3개 파일만 포함한다. 원인별 patch는 같은 디렉터리에 있고 다음 순서로 적용한다.

1. `F-M6A-1-monitor-port.patch`
2. `F-M6A-2-core-replacement-ownership.patch`
3. `F-M6A-3-logical-monitor-events.patch`
4. `F-M6A-4-not-required-handshake.patch`

통합 patch와 원인별 patch를 각각 보존한 R6 baseline에 적용하여 최종 3개 파일이 byte 단위로
일치함을 확인했다. 나머지 보존한 Node 파일 1,913개는 그대로다. 기존 R6 patch 파일,
Core·binding·다른 언어·보호된 문서 경로는 수정하지 않았다.

## BLOCKERS

요청한 네 원인의 잔여 실패는 없다. 다만 **별도 test 39의 간헐 실패 1회는 원인 미확정**이다.
최종 fixture 검토 뒤 첫 M6A 실행에서 `bilateral endpoint-only manual connections learn peer
RIDs and converge`가 request 제출 후 application 수신을 기다리다 실패했다
(`test/m6a/m6a-runtime.contract.ts:1958`, `m6a-test39-failure-1.log`). Test 34는 이 실행에서도
통과했다. 두 peer는 Object Server여서 이번 NotRequired 변경 분기를 사용하지 않는다.

Test 39는 현재 코드의 동일 process 100회와 별도 process 100회, committed raw module 대조
100회에서 모두 통과했으며 최종 M6A 10회에서도 통과했다. 이 결과만으로 간헐 실패의 원인이
해결됐거나 R6 이전부터 존재했다고 단정하지 않는다. 해당 test의 assertion·timeout·runtime은
수정하지 않았다. 추가 원인 조사가 필요하면 이 실패를 별도 범위로 배정해야 한다.
