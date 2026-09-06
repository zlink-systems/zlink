# Node ClientServer Server-only readiness 수정 결과

## 결과와 원인

Serving 상태의 manual ClientServer Server-only host에서 weight가 100이면
`snapshot('work')`가 `state=Ready`, `isReady=true`, `readyTargetCount=1`을 반환한다.
`isReady('work')`도 같은 snapshot의 판단을 사용한다. Weight 0은 local target을
진단 목록에 유지하면서 `Degraded/false/0`을 반환한다. Public API 변경은 없다.

관련 suite는 169/169, 새 공개 API 회귀 테스트는 5/5, sample regression은 52/52,
M6A는 42/42로 통과했다. 전체 `npm test`는 1696/1697이며, 별도 RouteMesh 테스트
1개가 실패하여 **gate의 0 failures 조건은 미충족**이다.

소유 계층: Framework ClientServer monitoring이 target 집계와 topology readiness를 소유한다.
Outbound 역할 검사는 `framework/languages/node/packages/framework/src/runtime/channels/channel-clients.ts:90,98`,
연결 선택은 `framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts:597,603`의 기존 경로에 유지했다.

소유 spec: `framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md:174,194`,
Node projection은 `framework/doc/framework/common/spec/server/languages/node/interfaces/03-location-observability.ko.md:453,465`의 §6이다.
Local·remote positive-weight Ready Server 집계 문구는
`framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md:379`,
Server-only outbound의 `NotConfigured`는
`framework/doc/framework/common/spec/server/02-channel-transport/03-client-server-channel.ko.md:53`을 따른다.

교차언어 대조 결과: .NET의 `ZLinkClientServerRuntimeService.cs:39,80,95,139`는 local Server를
snapshot에 합산하고 Ready count와 readiness를 한 번 계산하여 public status에 전달한다.
Node는 Client 역할을 readiness 조건으로 사용하지 않지만 local descriptor 누락 때문에 같은
false 증상이 발생했다. .NET 수정 전의 역할 조건 오류와 원인이 다르다. Node만 변경한 이유는
이번 승인 범위이며, readiness 계약에 언어별 예외가 있기 때문이 아니다.

변경 분류: **B — 기존 결함**. 지정된 수정 작업을 구현 승인으로 적용했다.
수정 전에 원인 위치, 소유 계약, .NET 대조와 변경 분류를 보고하고 공개 API로 실패를 재현했다.

수정 전 `channel-socket-registry.ts:736,739`는 monitoring 목록을
`ServiceDiscoveryRegistry.clientServerDescriptors()`만으로 만들었다. 이 메서드는
`service-discovery-registry.ts:99`에서 Client가 발견한 descriptor를 반환한다.
Local Server descriptor의 실제 소유자는 `channel-socket-registry.ts:133,294`의 별도 map이다.
Listener bind 뒤 생성된 이 descriptor가 monitoring 목록에 합쳐지지 않았다.
`channel-runtime-manager.ts:107,116`은 그 목록을 전달하며,
`topology-runtime-projections.ts:68,75,81`의 수정 전 projection은 count 0을
Degraded/false로 판단했다.

## 변경과 회귀 테스트

`channel-socket-registry.ts:736`에서 기존 remote descriptor 목록에 local descriptor를
합산한다. Server RID와 lifecycle generation이 같은 descriptor가 이미 있으면 추가하지 않는다.
이 조건으로 Client+Server의 loopback admission 이후에도 local Server는 한 번만 집계된다.
Monitoring에 필요한 RID·weight·state만 노출하며, 상태 변환은 기존
`discoveryAvailabilityForRuntimeState()`를 사용한다. 결과는 RID 순으로 정렬한다.

대안으로 local Server를 outbound discovery registry에 등록하는 방식을 검토했다.
이는 monitoring을 위해 연결 선택 후보를 바꾸므로 채택하지 않았다. 기존 monitoring 목록에서
합산하여 실제 outbound discovery·역할 검사·connection 선택의 소유권을 유지했다.
새 상태, 타이머, retry, 옵션, helper 또는 별도 상태 매핑표는 추가하지 않았다.

`topology-runtime-projections.ts:68`에서 합쳐진 target 목록의 Ready 수를 한 번 계산한다.
`:72`의 기존 `topologyRuntimeIsReady()` 결과를 `state`와 `isReady`가 함께 사용한다.
Public `snapshot()`, `isReady()`와 관찰 status 생성은 기존 `snapshotCore()`를 재사용한다.

수정 전/후 규칙 수: **Ready target 집계·readiness 판단식 3 → 2**.
집계식 1개는 유지하고, `state`와 `isReady`가 각각 수행하던 readiness 판단 2개를 1개로 줄였다.
Outbound 역할 검사와 대상 선택 규칙은 기존 소유자에 그대로 남는다.

새 테스트는 `test/contract/client-server-readiness.test.js`에서 실제 NestJS application을 시작한다.
공개 builder, runtime token, status, typed JSON request와 Framework error만 사용한다.
Internal import, 내부 자료구조 assertion, raw frame 또는 수동 codec은 없다.

| 사례 | 검증 결과 | 테스트 위치 |
|---|---|---|
| Server-only, weight 100 | Serving, Ready/true/1, local target 1개; send/request는 NotConfigured | `:22` |
| Server-only, weight 0 | Serving, Degraded/false/0, weight 0 local target 유지; send/request는 NotConfigured | `:22` |
| Client-only, manual endpoint에 Server 없음 | Serving, Degraded/false/0, targets 빈 목록 | `:58` |
| Client+Server | 실제 typed request/reply 이후 Ready/true/1, local Server 중복 없음 | `:77` |
| Local Server + remote Server | Ready/true/2, 서로 다른 RID, weight 100과 200 | `:105` |

변경 파일은 다음과 같다.

- `framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts`
- `framework/languages/node/packages/framework/src/runtime/diagnostics/topology-runtime-projections.ts`
- `framework/languages/node/test/contract/client-server-readiness.test.js`
- 이 결과 문서

## 검증과 남은 실패

증거 디렉터리: `/tmp/zlink-node-cs-server-ready-20260906/`.
Node는 `v22.23.2`다. 실행마다 `/tmp/zlink-node-gate.lock`을 얻었고,
전체 gate는 sample lock을 먼저 얻었다. Lock 획득 뒤 `/proc/loadavg`를 확인했으며
gate 시작 시 1분 loadavg는 **3.49 < 10**이었다(`gate.log:1`).

| 검증 | 결과 | 증거 |
|---|---|---|
| 수정 전 TypeScript build + 최초 회귀 4개 | 2 pass / 2 fail; Server-only weight 100의 Degraded, weight 0의 local target 누락 재현 | `focused-before-build.log` |
| 수정 후 TypeScript build + readiness/projection | 31 pass / 0 fail | `focused-after.log` |
| readiness + projection + ClientServer location + channel client | 169 pass / 0 fail | `subsystem.log` |
| 전체 npm test | build/typecheck/lint 통과; 158 files, 1696 pass / 1 fail / 0 skipped, exit 1 | `gate.log` |
| 전체 gate 안의 새 readiness 테스트 | 5 pass / 0 fail | `gate.log`, `test/contract/client-server-readiness.test.js` 구간 |
| 전체 gate 안의 sample regression | 52 pass / 0 fail; 전체 sample runner 호출 포함 | `gate.log`, `test/contract/sample-regression.test.js` 구간 |
| 별도 M6A | 42 pass / 0 fail / 0 skipped, exit 0 | `m6a.log` |
| 실패한 RouteMesh 테스트의 flow 진단 | 1 pass / 0 fail, exit 0; 최초 gate 실패를 대체하지 않음 | `unrelated-diagnostic.log`, `diagnostic-22049.flow.jsonl` |

검증 명령의 작업 디렉터리는 `framework/languages/node`다.

```bash
flock /tmp/zlink-node-gate.lock node --test --test-concurrency=1 \
  test/contract/client-server-readiness.test.js \
  test/contract/topology-runtime-projection.test.js \
  test/contract/client-server-location-runtime.test.js \
  test/contract/channel-client.test.js
flock /tmp/zlink-samples-gate.lock flock /tmp/zlink-node-gate.lock npm test
flock /tmp/zlink-node-gate.lock npm run verify:m6a-runtime
```

전체 gate는 한 번만 실행했다. Runtime suite가 실패하면 `npm test`의 `&&` 뒤에 있는
M6A가 실행되지 않으므로 M6A만 별도로 실행했다. Sample regression의 기존 runner는
실패 시 서버 파일 log를 자동 보존한다. Sample runner의 성공 출력은 해당 테스트가
자체 검증하며 TAP에는 별도 출력하지 않는다.

**BLOCKER:** `test/contract/channel-client.test.js:3734`의
`ZLinkModule routeMesh channel option dispatches inbound routed handlers after bootstrap`가
`:3776`에서 `filterContexts.length`의 기대값 1과 실제값 2가 달라 실패했다.
Reply `{ value: 'pong' }`와 첫 handler 결과 확인은 통과했다(`gate.log:2309`).
앞선 관련 suite에서는 같은 테스트가 통과했다(`subsystem.log:482`).

이 테스트는 `:3755`에서 `addRouteMesh('mesh')`를 등록한다. ClientServer topology snapshot이나
이번에 수정한 descriptor 합산은 호출하지 않는다. 기존
`test/contract/helpers/route-mesh-peer.js:112,117`의 `retryReachable()`는 실패한 request를
새로 제출하므로 filter가 여러 번 실행될 수 있다. 최초 실행의 중복 원인이 이 재제출인지,
다른 RouteMesh 경로인지까지는 확정하지 못했다. Fixture와 assertion은 수정하지 않았다.

단독 진단에서는 저장소 밖의 임시 preload로 기존 runtime message-flow mode를 `normal`로
설정하고 표준 OTel LoggerProvider를 파일에 연결했다. 원래 테스트와 forked peer의
호출·timeout·retry·assertion은 그대로 실행했다. 테스트는 189 ms에 통과했고, 수신측에서
같은 flow/correlation의 `received → admitted → dispatched → replied` 4단계를 확인했다.
최초 실패에는 같은 수준의 flow 파일이 없어 실패 transition과 직접 대조할 수 없다.
단독 성공으로 gate 실패를 지우지 않았으며, 임시 preload는 진단 후 삭제했다.

Main에서 작업했고 commit은 하지 않았다. Core, binding, protected spec/doc, 다른 언어는
수정하지 않았다. 병행 작업에서 생긴 다른 언어 변경과 기존 untracked 파일은 보존했다.
