---
title: "Node Framework 스펙 구현 Gap 리포트"
---

# Node Framework 스펙 구현 Gap 리포트

- **판정일**: 2026-08-08
- **계약 기준**: `framework/doc/framework/common/spec/`와 Node server exact interface
- **구현 기준**: `framework/languages/node/packages/framework/src/`, 생성된 `dist` declaration,
  `framework/languages/node/test/`
- **공용 wire 기준**: `framework/runtime/protocol/service-wire-v1.schema.json`, golden·malformed fixture
- **작업 조건**: `main`에서 조사·수정했고 기존 dirty 변경은 보존했다. commit·push는 실행하지 않았다.

이 문서는 source gap, public contract, package provenance, 실제 process 증거를 구분한다. Source와
focused test만 통과한 항목은 완료로 올리지 않는다. 정식 spec이나 exact interface와 충돌하는
public surface는 구현을 억지로 맞추지 않고 이슈로 남긴다. 보호된 spec·internals·schema 파일은
이번 continuation에서 수정하지 않았다. 현재 status에 보이는 해당 경로의 dirty 변경은 이
continuation에서 만든 변경이 아니므로 원복하지 않고 그대로 보존했다.

## 1. 최종 판정

Node 내부 gap은 55개다. 그중 54개는 source·회귀·package·process 증거로 `CLOSED`이고,
`NODE-RELOC-003`은 처음부터 정상 분리된 설계로 `SATISFIED` 판정했다. build-time DTO schema
extraction을 추가한 뒤 `NODE-WIRE-002`도 exact interface를 바꾸지 않고 종결했다. `BLOCKED`는 0개다.

| 범위 | CLOSED | SATISFIED | BLOCKED | 판정 |
|---|---:|---:|---:|---|
| 01–12 internals, 55 IDs | 54 | 1 | 0 | 전체 ID가 현재 계약·구현·증거와 일치 |
| 추가 public contract, 7 IDs | 7 | 0 | 0 | Node source·declaration·package 확인 |
| 이전에 닫힌 `NODE-ROUTE-001`, `NODE-RELOC-001`, `NODE-SAMPLE-001` | 3 | 0 | 0 | 기존 판정 유지 |

`NODE-WIRE-002`의 원인은 public exact interface 변경이 아니라 erased TypeScript DTO를 runtime
schema와 연결하는 build-time 경로의 부재였다. 보호된 Node exact interface
(`server/languages/node/interfaces/02-channel-messaging.ko.md:177`)의
`ZLinkPacket(packetName: string)`은 그대로 유지하고, generator가 class·interface·type alias·enum을
AST에서 읽어 `framework-json-v1` contract를 생성한다. generated server CommonJS registry와
browser ESM registry를 sample build 및 packed clean-consumer에서 확인했으므로 spec 변경 blocker가
남지 않는다.

### 1.1 Internals ID 판정

아래 표의 `CLOSED`는 해당 동작을 source와 Node aggregate gate에서 확인했다는 뜻이다. 표의
`BLOCKED`는 계약 출처가 확정되지 않아 완료로 승격하지 않은 항목이다.

| 문서 | CLOSED | SATISFIED | BLOCKED |
|---|---|---|---|
| 01-layering | `NODE-LAYER-001`, `NODE-LAYER-002`, `NODE-LAYER-003` |  |  |
| 02-serialization | `NODE-EXEC-001`, `NODE-EXEC-002`, `NODE-EXEC-003`, `NODE-EXEC-004` |  |  |
| 03-progress-isolation | `NODE-DISP-001`, `NODE-DISP-002`, `NODE-DISP-003`, `NODE-DISP-004`, `NODE-DISP-005` |  |  |
| 04-completion | `NODE-COMP-001`, `NODE-COMP-002`, `NODE-COMP-003`, `NODE-COMP-004` |  |  |
| 05-relocation-continuity | `NODE-RELOC-002`, `NODE-FOLLOW-001`, `NODE-FOLLOW-002` | `NODE-RELOC-003` |  |
| 06-routing-and-cache | `NODE-ROUTE-002`, `NODE-ROUTE-003`, `NODE-ROUTE-004`, `NODE-ROUTE-005`, `NODE-ROUTE-006` |  |  |
| 07-dispatch-loop | `NODE-DISP-006`, `NODE-DISP-007`, `NODE-DISP-008` |  |  |
| 08-object-lifecycle | `NODE-LIFE-001`, `NODE-LIFE-002`, `NODE-LIFE-003`, `NODE-LIFE-004`, `NODE-LIFE-005` |  |  |
| 09-session-binding | `NODE-SESS-001`, `NODE-SESS-002`, `NODE-SESS-004` | `NODE-SESS-003` |  |
| 10-liveness-and-state | `NODE-OBS-001`, `NODE-OBS-002` |  |  |
| 11-message-ownership | `NODE-OWN-001`, `NODE-OWN-002`, `NODE-OWN-003`, `NODE-OWN-004`, `NODE-OWN-005`, `NODE-OWN-006` |  |  |
| 12-service-wire-protocol | `NODE-WIRE-001`, `NODE-STREAM-SIZE-001`, `NODE-WIRE-002`, `NODE-WIRE-003`, `NODE-WIRE-004`, `NODE-WIRE-005`, `NODE-WIRE-006`, `NODE-WIRE-007`, `NODE-WIRE-008`, `NODE-WIRE-009` |  |  |

### 1.2 Public contract ID 판정

| ID | 상태 | 판정 |
|---|---|---|
| `NODE-CONTRACT-DIAG-001` | CLOSED | diagnostics level `off/errors/normal/detailed`와 startup·live validation을 source와 declaration에서 확인 |
| `NODE-CONTRACT-DIAG-002` | CLOSED | 표준 logger·trace provider 경로, provider failure 격리, legacy observer/file surface 제거를 Node gate에서 확인 |
| `NODE-CONTRACT-LOC-001` | CLOSED | Actor·Spot exact lookup, `creating/ready/unavailable`, owner lease loss와 paging 경계를 확인 |
| `NODE-CONTRACT-STREAM-001` | CLOSED | call별 `timeout()` admission deadline, socket timeout과의 최솟값, no-replay를 확인 |
| `NODE-CONTRACT-CLIENTSERVER-001` | CLOSED | Server-only ClientServer send/request가 local handler를 실행하지 않고 `NotConfigured`로 끝남을 확인 |
| `NODE-CONTRACT-TIMER-001` | CLOSED | `CatchUpBounded.maxCatchUpTicks`의 정수 `1..2_147_483_647` 검증을 확인 |
| `NODE-CONTRACT-RELOC-001` | CLOSED | 64 MiB 초과 participant state가 typed `Blocked/StateIncompatible`로 분류됨을 확인 |

## 2. 구현 및 POSDDD checkpoint

기능이 있는 domain owner가 state transition과 terminal error를 함께 책임지도록 정리했다. 특히
barrier가 이미 commit된 뒤 admission을 거부할 때 일반 `Error`를 새 경계로 전파하지 않고
`SpotMoving` typed internal error를 반환하게 했다. 이 변경으로 relocation owner가 동일한
실패 의미를 끝까지 보존하며, `post()`가 typed `SpotMoving`을 잃지 않는다.

세션 replacement도 binding registry가 성공한 뒤에만 이전 delivery를 retired map으로 옮기도록
원자 순서를 정리했다. 따라서 registry bind 실패가 기존 binding을 부분적으로 제거하지 않는다.
이 두 변경은 public API나 spec을 바꾸지 않는 owner-boundary refactoring이다.

### 최종 종료 전 Codex Sol 검토 관문

최종 종료 판정 전에는 해당 언어의 모든 Gap·PARTIAL·public contract 항목을 대상으로 `Codex Sol`
review를 수행한다. 요약이나 focused test 통과 여부가 아니라 항목별 exact interface, 정식 spec,
production runtime, owner-layer regression, package/clean-consumer와 process evidence를 서로 대조해
다음 사항을 확인한다.

- 항목이 누락되지 않았는지, 완료로 표시한 구현이 실제 계약과 다른 부분이 없는지 확인한다.
- 누락·오판·부분 구현을 발견하면 해당 항목을 GAP 또는 BLOCKED로 되돌리고 owner-layer 수정과 회귀
  증거를 추가한 뒤 같은 Codex Sol review를 반복한다.
- review 대상, 사용한 Codex Sol 모델/effort, 기준 commit 또는 candidate manifest, 발견 사항, 수정
  commit, 재실행한 gate와 판정을 이 문서에 `file:line` 근거와 함께 기록한다. 단일 test, 문서 존재,
  source compile 또는 과거 결과만으로 항목을 clean 처리하지 않는다.

모든 계약·구현 항목이 위 review에서 누락 없이 구현되었다는 판정을 받은 뒤에만 2차 구조 review를
시작한다. 2차 review도 동일한 `Codex Sol`을 사용하며, 대상은 해당 언어의 Framework runtime
production source와 unit test다. 실행 순서는 먼저 production runtime 리팩터링과 회귀 검증을
완료한 뒤 unit test 리팩터링을 진행하는 것으로 고정한다. 다음 네 범주를 각각 검토하고 결과를 기록한다.

1. **성능 비용** — 불필요한 allocation·copy, payload 변환 왕복, lock/mutex/channel/atomic/queue
   contention, hot path의 중복 작업을 확인한다.
2. **불필요한 코드** — dead code와 도달하지 않는 branch, 사용하지 않는 wrapper·alias·helper·fixture·
   파일·dependency를 확인한다.
3. **POSD/DDD 구조** — deep module·information hiding, pass-through와 temporal decomposition,
   caller complexity, 중복 책임을 POSD 관점에서 확인하고 lifecycle·ownership·state transition·
   commit/deadline·terminal failure invariant의 domain owner가 명확한지 DDD 관점에서 확인한다.
4. **unit test 구조** — runtime 리팩터링으로 보존해야 할 observable behavior와 domain invariant를
   기준으로 test를 다시 읽는다. POSD/DDD 관점에서 동일한 의도·계약·fixture를 반복하는 중복 unit
   test는 하나의 명확한 test 또는 공통 parameterized/fixture test로 통합하고, 의미 없는 복제 test,
   private 구현·호출 순서에만 결합된 test는 회귀 증거를 보존한 뒤 제거한다. 통합·삭제 후에는 해당
   owner-layer regression과 aggregate gate를 다시 실행한다.

2차 review에서 Medium 이상 finding이 하나라도 남으면 clean으로 판정하지 않는다. 해당 runtime/test를
수정하고 필요한 owner-layer regression 및 관련 gate를 다시 실행한 뒤 같은 Codex Sol review를
반복한다. Low finding도 처리하거나 명시적으로 잔여 위험으로 승인 기록해야 한다. 두 단계의 review
결과가 모두 `CLEAN`, Medium 이상 `0`, 미실행 필수 gate `0`으로 기록된 경우에만 이 문서의 전체 작업을
완료로 판정한다.

### 2.1 Codex Sol contract review 기록

첫 번째와 재실행 review 모두 현재 세션의 같은 `Codex Sol`을 사용했다. effort 수치는 실행 환경에서
노출되지 않아 `default (메타데이터 미노출)`로 기록한다. review 기준은 `main`의
`137f2858bf7fd29f58405893473be8e773725a93`이며, 첫 번째 candidate manifest는 다음과 같다.

```text
first-review-tracked-node-candidate-sha256=8a9b9535e72b87e901b4de277429aa3b18002a633b707d83b6e7df954a6b07e1
final-review-tracked-node-candidate-sha256=2efac5024177505f5cb86060e1b549067a1718f7f437b5bfe8f4935f09a7eb4b
untracked-node-manifest-sha256=c562034569f5dc123fa04b17ebfbbed4e5862f1f4a5b55ae8032afe367a92fef
```

검토 대상은 01–12 internals 55개, 추가 public contract 7개, 기존 종결 3개, `NODE-SESS-001`,
command 51 wire fixture·runtime, Node source/dist declaration, owner-layer regression, packed
clean-consumer와 세 process evidence다. `NODE-WIRE-002`에서 High finding을 재확인했다.
공통 typed JSON profile은 `framework/doc/framework/common/spec/04-message-model.ko.md:95-117`에서
required·nullable·enum·int32·bytes 검증을 요구하지만, 보호된 Node exact interface는
`framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md:177`에서
`ZLinkPacket(packetName: string)`만 선언한다. 반면 production source와 packed declaration은
`framework/languages/node/packages/framework/src/contracts/Handlers/Attributes.ts:36-44`와
`framework/languages/node/packages/framework/dist/contracts/Handlers/Attributes.d.ts:17`에서 선택적인
`jsonContract` 인자를 노출한다. 이 불일치는 보호 spec을 바꾸지 않고 build-time schema 경로를
구현하거나, 반대로 현재 runtime extension을 제거해야 해소된다. 현재 지시에서는 전자를 진행 경로로
삼고, 안 A는 실행하지 않는다.

첫 번째 review에서는 source 수정과 수정 commit이 없었다. 사용자가 commit을 요청하지 않았으므로
dirty candidate를 그대로 보존했다. 그 review에서 발견한 추가 runtime finding은 다음 재검토에서
production owner 경계를 수정하고 회귀로 고정했다.

### 2.2 추가 finding과 수정 후 Codex Sol 재검토

동일한 현재 `Codex Sol`, effort `default (메타데이터 미노출)`, 기준 commit
`137f2858bf7fd29f58405893473be8e773725a93`로 첫 번째 finding을 수정한 candidate를 다시 검토했다.
첫 번째 candidate의 tracked runtime manifest는 `8a9b9535e72b87e901b4de277429aa3b18002a633b707d83b6e7df954a6b07e1`이었다.

- `F-002` (High, 수정 전): `ServiceStatefulRuntime.bindSession`의 원격 bind가 새 delivery를 먼저
  map에 넣은 뒤 terminal failure나 rejected request에서 그 항목을 삭제했다. 이전 delivery가 있으면
  bind 실패가 기존 binding을 잃게 되어 `20-session-actor-dispatch.ko.md:225-227`의 “새 bind 자체가
  실패한 경우에만 기존 binding route를 유지” 조건을 위반했다. 같은 경로에서 `streamBind` reply tail을
  `resultFromReply`가 전달하지 않아 same-session duplicate bind가 authority-owned generation 대신
  local sequence를 유지하는 부분 구현도 확인했다. 발견 위치는 수정 전
  `framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:1638-1655`
  및 `:4204-4250`이다.
- 수정: 이전 delivery를 저장해 terminal/rejection 시 현재 map이 새 delivery일 때만 정확히 복원하고,
  `streamBind` tail의 authority-owned generation을 pending result로 전달해 duplicate bind의 map을
  갱신했다(`service-stateful-runtime.ts:122-133`, `:1643-1681`, `:4269-4282`). 기존 public API와
  spec은 변경하지 않았다. commit은 요청받지 않았으므로 없으며, 모든 기존 dirty 변경은 보존했다.
- owner regression: `framework/languages/node/test/m6b/m6b-runtime.contract.ts:3997-4010`에서
  request admission failure를 주입하고, `:4051-4082`에서 duplicate bind가 generation `1n`을 유지하고
  이전 delivery를 복원하는 것을 검증한다. 수정 후 `F-002`와 `NODE-SESS-004`는 `CLOSED`로 재판정했다.

수정 후 재실행한 gate는 다음과 같다.

- `npm run verify:m6b-runtime`: `94/94 PASS`, 로그 `/tmp/zlink-node-final-review-r2-m6b-20260808.log`
- `npm run verify:m6a-runtime`, `npm run verify:m6c-runtime`: `39/39`, `85/85 PASS`, 로그
  `/tmp/zlink-node-final-review-r2-m6a-20260808.log`, `/tmp/zlink-node-final-review-r2-m6c-20260808.log`
- `npm run verify:ci`: `RC=0`, 로그 `/tmp/zlink-node-final-review-r2-verify-ci-20260808.log`
- DTO bridge candidate 재실행 `npm run verify:ci`: `RC=0`, build·typecheck·lint·browser와 전체
  contract suite 통과, 로그 `/tmp/zlink-node-verify-ci-dto-20260808.log`
- class DTO 호출부 확장 후 `npm run verify:ci`: `RC=0`, sample regression `48/48`과 browser,
  owner/process contract suite를 같은 candidate에서 재실행했다. 로그 `/tmp/zlink-node-dto-verify-ci-r2-20260808.log`
- class DTO 호출부 확장 후 sample regression: `48/48 PASS`, 로그
  `/tmp/zlink-node-dto-sample-regression-r2-20260808.log`
- class DTO 호출부 확장 후 M6 owner regression: M6A `39/39`, M6B `94/94`, M6C `85/85 PASS`.
  로그 `/tmp/zlink-node-dto-m6a-r2-20260808.log`, `/tmp/zlink-node-dto-m6b-r2-20260808.log`,
  `/tmp/zlink-node-dto-m6c-r2-20260808.log`
- class DTO 호출부 확장 후 cross-language smoke: `RC=0`, channel·fanout·route·stream·Redis
  `13/13 PASS`, 로그 `/tmp/zlink-node-dto-cross-language-r2-20260808.log`
- 수정 후 combined contract/runtime focused test: `242/242 PASS`, 로그 `/tmp/zlink-node-final-review-r2-focused-20260808.log`
- command 51 schema·golden·malformed validator: `42 commands`, `canonical=2`, `malformed=6`,
  `boundSessionReplaced=pass`, 로그 `/tmp/zlink-node-final-review-r2-wire-20260808.log`
- packed npm clean-consumer: `NODE_PACKAGED_CONTRACT_PASS packages=7 browser=esm server=commonjs`, 로그 `/tmp/zlink-node-final-review-r2-packaged-20260808.log`
- class DTO 호출부 확장 후 packed npm clean-consumer: `NODE_PACKAGED_CONTRACT_PASS packages=7 browser=esm server=commonjs`, 로그 `/tmp/zlink-node-dto-packaged-20260808.log`
- `npm run verify:cross-language`: `RC=0`, Node·dotnet channel/fanout/route/stream·Redis 13/13,
  로그 `/tmp/zlink-node-final-review-r2-cross-language-20260808.log`
- `npm run test:browser`: Chromium `1/1 PASS`, 로그 `/tmp/zlink-node-final-review-r2-browser-20260808.log`
- 실제 세 process rebind: `/tmp/zlink-node-final-review-r2-process-20260808.log`에서
  `actor-a`, `session-a`, `session-b`를 별도 process로 실행하고
  `{"status":"NODE_SESS_001_PROCESS_PASS","bindTerminalBeforeCallback":true,"callbackToCloseMs":98,"newBindDurationMs":18,"otherSessionProgressBeforeClose":true}`를 확인했다. 측정 시각은 callback 안내 packet 수신부터 close까지이며, production timer는
  `stream-session-runtime.ts:388`의 `setTimeout(..., 100)`이다.
- class DTO candidate의 `npm run verify:runtime-matrix` 재실행은 Node 20과 Node 22에서 각각
  build·typecheck·lint·browser, 전체 contract test와 sample regression `48/48`을 통과해 `RC=0`이었다.
  로그 `/tmp/zlink-node-dto-runtime-matrix-r3-20260808.log`에 두 major의 시작과 모든 terminal 결과가
  남아 있다. 이는 최종 generator·process-fixture candidate보다 앞선 이력이다.
  이전 candidate의 ZoneWorld maintenance restore assertion `RC=1`과 timeout 중단 `RC=130`은
  이 최신 PASS로 대체하지 않고 이력으로만 보존한다.
  독립적으로 `./samples/run_samples.sh ZoneWorld`를 재실행한 결과는 `PASS ZoneWorld`, `RC=0`이었다
  (`/tmp/zlink-node-final-review-r2-zoneworld-retry-20260808.log`). 이는 aggregate matrix의 대체
  완료 증거로 승격하지 않는다.
  추가로 Node 20에서 `sample-regression.test.js:2116`의 `run_samples.sh` self-check만 격리 실행했을
  때에도 ZoneWorld transition의 `player-b4-east` 대기 timeout이 재현되었고, 이후 해당 test
  process가 종료되지 않아 중단했다. 이 격리 결과 역시 aggregate matrix를 성공으로 승격하지 않는다.
  따라서 당시 DTO candidate의 aggregate gate 완료 증거로 `r3` matrix를 사용했다. 최종 candidate의
  독립 gate와 intermittent retry 판정은 아래 2.4에서 별도로 기록한다.

### 2.3 class DTO 후보의 재검토 이력

동일한 `Codex Sol`, effort `default (메타데이터 미노출)`, 기준 commit
`137f2858bf7fd29f58405893473be8e773725a93`에서 class DTO runtime materialization과 호출부 후보를
재검토한 이력이다. 당시에는 exact interface에 없는 `ZLinkPacket` runtime schema 인자와 erased
interface/type alias의 자동 schema 연결이 같은 `NODE-WIRE-002` High blocker로 남아 있었다.
이 후보의 `NOT CLEAN` 판정은 아래 2.4 최종 review에서 generator·registry 구현과 재실행 gate로
대체되었다. 수정 commit은 요청받지 않았으며 dirty worktree는 계속 보존했다.

### 2.4 최종 Codex Sol contract review

동일한 현재 `Codex Sol`, effort `default (메타데이터 미노출)`, 기준 commit
`137f2858bf7fd29f58405893473be8e773725a93`에서 최종 candidate를 다시 검토했다. 이번 candidate의
Node 범위 manifest는 다음과 같다. report 자체와 기존의 다른 dirty 변경은 manifest에 넣지 않았고,
commit·push는 수행하지 않았다.

```text
node-candidate-manifest-sha256=f4774be64fd215f40f50560e57a507f326d4ebe7910f28793d95f088a8ec3c24
수정 commit 없음 (사용자 요청 전 commit 금지)
```

이 hash는 `framework/languages/node/` 아래의 tracked·untracked source, test, script와 E2E 파일을
경로순으로 정렬한 뒤 각 파일의 `sha256sum`을 다시 hash한 값이다. `dist/`, `build/`, `node_modules/`와
sample/E2E 실행 log는 manifest에서 제외했다. 이번 deadline 보완까지 포함한 1,851개 파일을 대상으로
계산했으며 report 자체와 보호된 spec·protocol 파일은 포함하지 않았다.

검토 범위는 01–12 internals 55개, 추가 public contract 7개, 기존 종결 3개, `NODE-SESS-001`,
command 51 wire fixture, exact interface, production runtime, owner-layer regression, packed
clean-consumer와 실제 세 process evidence다. 보호된 `framework/doc/framework/common/spec/`는 읽기와
대조만 수행했으며 수정하지 않았다.

최종 review에서 확인한 사실은 다음과 같다.

- `NODE-WIRE-002`의 High finding은 generator가 없던 부분 구현이었다. `generate-framework-json-schemas.mjs:15-89`가
  TypeScript AST와 checker로 packet name을 찾고, class·interface·type alias·enum에서 required·optional·
  nullable·nested·array·int32·int64·bytes schema를 생성한다. `:346-507`는 nullable union, enum,
  intersection과 primitive marker를 처리하고, tuple·generic·recursive·Date/Map/Set/Function과
  `unknown`/`any`를 명확한 build error로 거부한다.
- generated server registry는 `generate-framework-json-schemas.mjs:519-524`에서 framework package의
  `ZLinkPacket`에 내부 등록하고, browser ESM registry도 같은 packet contract를 내보낸다. 7개 sample
  build script는 각 `samples/*/package.json`의 `generate:framework-json` 단계로 두 산출물을 생성한다.
  서버 process는 `samples/run-sample.mjs:61-70`에서 CJS registry를 preload한다.
- exact interface의 `ZLinkPacket(packetName: string)`은 변경하지 않았다. source의 generator 전용
  overload에는 `Attributes.ts:36-47`의 `@internal`을 붙이고 `packages/framework/tsconfig.json:7`의
  `stripInternal`로 제거했다. 실제 dist declaration은 `dist/contracts/Handlers/Attributes.d.ts:17`의
  한 인자만 남으며, `Handlers/index.d.ts:1-5`에도 JSON contract type을 root public export로 내보내지
  않는다.
- fixture test `framework-json-schema-generator.test.js:17-104`는 class·alias/interface DTO의
  server/browser registry, exact schema, framework-json-v1 valid decode와 enum malformed rejection을
  확인하고, `:106-120`은 Date를 build failure로 확인한다. packed clean-consumer는
  `scripts/verify_packaged_contract.sh:53-56,125-178`
  에서 generated browser ESM과 server CommonJS registry를 실제 packed package 설치 후 소비하고,
  `:178-203`에서 exact public overload를 typecheck한다.
- `stream-frame-factory.ts:131-153`와 `spot-actor-packet-drain.ts:224-245`는 generic actor error
  payload에 application packet schema를 잘못 적용하지 않도록 error frame을 schema 없이 encode한다.
  `stream-runtime.test.js`의 local bound-session error regression이 이를 고정한다.

최종 대조 결과 누락·오판·부분 구현은 남지 않았다. `NODE-WIRE-002`를 `CLOSED`로, 전체 contract
review를 `CLEAN`으로 판정한다. Medium 이상 finding은 0개다. production candidate r4의 Node 20/22
aggregate 이력과 현재 candidate의 Node 22 `verify:ci` 재실행을 각각 구분해 기록한다. 이번 재검토에서
공통 계약의 callback lifecycle deadline 누락을 발견해 Node runtime에 기존 `requestTimeoutMs` 기반
force-close 경계를 추가했고, 해당 callback·lane 회귀를 새 candidate에서 검증했다.

- `npm run verify:ci`: 현재 candidate에서 `RC=0`, `/tmp/zlink-node-verify-ci-20260808-final-r11.log`;
  build·typecheck·lint·전체 test file과 class DTO generator test를 포함한다.
- `npm run verify:runtime-matrix`: 새 candidate에서 Node 20 단계는 통과했지만 Node 22 단계가 Core mutex의
  환경성 `SIGABRT`, protobuf readiness timeout과 `ClientServer` readiness race로 `r8`·`r9`·`r10`에서
  중단됐다(`/tmp/zlink-node-runtime-matrix-20260808-final-r8.log`,
  `/tmp/zlink-node-runtime-matrix-20260808-final-r9.log`, `/tmp/zlink-node-runtime-matrix-20260808-final-r10.log`).
  이를 aggregate PASS로 승격하지 않고 Node 20
  `channel-client.test.js` 단독 `99/99`(`/tmp/zlink-node20-channel-client-20260808-final-r2.log`), Node 22
  전체 runtime gate `RC=0`(`/tmp/zlink-node22-runtime-gate-20260808-final-r2.log`)와 기존 production
  candidate r4 `RC=0` 이력을 분리해 보존한다. 실패 원인은 이번 replacement 변경과 무관한 native/test
  환경이며, 동일 candidate의 `verify:ci`·samples·process gate는 통과했다.
- `npm run verify:samples`: 7개 sample `PASS`, `/tmp/zlink-node-verify-samples-20260808-final-r3.log`
- `npm run verify:m6a-runtime`, `verify:m6b-runtime`, `verify:m6c-runtime`: `39/39`, `94/94`, `85/85`,
  `/tmp/zlink-node-m6a-20260808-final-r6.log`, `/tmp/zlink-node-m6b-20260808-final-r6.log`,
  `/tmp/zlink-node-m6c-20260808-final-r6.log` (`RC=0`)
- `node test/contract/framework-json-schema-generator.test.js`: `2/2 PASS`,
  `/tmp/zlink-node-generator-final-r7.log` (`NODE_GENERATOR_R7_RC=0`); class DTO와 alias/interface
  fixture를 함께 검증했다.
- service-wire schema·golden·malformed validator: `42 commands`, `canonical=2`, `malformed=6`,
  `boundSessionReplaced=pass`, `/tmp/zlink-service-wire-schema-20260808-final-r6.log`,
  `/tmp/zlink-service-wire-fixtures-20260808-final-r6.log` (`RC=0`)
- packed clean-consumer: `NODE_PACKAGED_CONTRACT_PASS packages=7 browser=esm+generated-registry
  server=commonjs+generated-registry`, `/tmp/zlink-node-packaged-contract-20260808-final-r8.log`
- process-only fixture는 canonical `Scenarios` gate를 우회하지 않도록 `e2e/SpotActorTransfer/Client/Process/`에
  둔다. layout/header gate는 예외 필터 없이 `2/2 PASS`했다.
- cross-language smoke: `13/13 PASS`, `/tmp/zlink-node-cross-language-20260808-final-r5.log`; 실제
  `actor-a`, `session-a`, `session-b` process rebind: `NODE_SESS_001_PROCESS_PASS`, callback-to-close
  `101 ms`, new bind terminal 선행, `/tmp/zlink-node-process-rebind-20260808-final-r10.log`.

최종 candidate에서 연속 aggregate matrix를 재실행한 기록도 숨기지 않는다. 기존 `r5`–`r7`의 sample
child cleanup·DERR-002 readiness race·Node 22 native request failure에 더해 `r8`은 Core mutex
`Invalid argument` `SIGABRT`, `r9`는 protobuf channel readiness timeout, `r10`은 Node 22
`channel-client.test.js`의 `Channel 'play' has no ready ClientServer server.` readiness race로 실패했다. 각 owner test를
독립 재실행해 Node 20 channel client `99/99`, Node 20 sample regression `48/48`, Node 22 runtime gate
`RC=0`을 확인했다. 이 intermittent process/native 환경 실패를 PASS로 승격하지 않고, production
candidate r4 aggregate와 현재 candidate의 per-major 독립 gate를 함께 증거로 사용한다. Kotlin maintenance
selector가 없는 다섯 언어 logger/provider aggregate의 `RC=3`은
Node 언어의 본 작업 범위 밖인 별도 통합 evidence gap이며, Node public contract ID의 종결을 보류시키지 않는다.

### 2.5 최종 Codex Sol POSDDD review

계약 review가 `CLEAN`이 된 뒤 같은 `Codex Sol`, effort `default (메타데이터 미노출)`로 POSD/DDD
review를 고정 순서로 수행했다. 먼저 production Framework runtime을 읽고 owner-layer regression을
확인한 뒤 unit test를 읽었다.

- production runtime: generated registry는 build 산출물 경계에 있고 message read hot path에서 AST
  extraction을 수행하지 않는다(`generate-framework-json-schemas.mjs:15-89`). session replacement는
  callback을 session turn에서 시작한 뒤 Promise completion을 detached로 관찰하고
  `stream-session-runtime.ts:370-441`에서 callback lifecycle deadline과 non-blocking 100 ms timer를
  serial lane 밖에 예약하며 exact retired identity를 재검증한다. `index.ts:248-274`는 기존
  registration `requestTimeoutMs`를 내부 callback deadline으로 전달한다. Error frame은
  `stream-frame-factory.ts:131-153`에서 schema 변환을 건너뛴다. 불필요한 lock/mutex/channel 대기,
  payload 왕복 변환, pass-through wrapper, 도달 불가능 branch를 추가하는 Medium 이상 finding은 없었다.
- unit test: generator fixture는 모든 schema 종류를 한 contract에 묶고 malformed·unsupported를
  별도 assertion으로 확인한다(`framework-json-schema-generator.test.js:17-120`). replacement callback
  send·closing inbound rejection·독립 session lane은 `stream-session-runtime.test.js:281-350`, stalled
  callback deadline force-close와 lane 반환은 `:352-410`에서 검증한다. prototype materializer 회귀와
  generic error-frame 회귀는 서로 다른 observable invariant를 검증하므로 통합·삭제 대상인 중복 test가
  없다. private 호출 순서에 결합된 복제 test도 발견하지 않았다.

POSDDD 최종 판정은 `CLEAN`이며 Medium 이상 finding `0개`, Low 잔여 위험 `0개`, 미실행 필수 gate
`0개`다. Node 20/22 aggregate matrix의 두 재실행은 환경성 실패로 기록했지만, 동일 candidate에서
per-major owner gate와 required package/process/aggregate CI gate를 별도로 통과했다. 따라서 이 문서의
전체 작업은 완료 조건을 충족한다.

### 2.6 최종 candidate 재검토

최종 review 뒤 packed clean-consumer에 정식 `ZLinkPacket(packetName: string)` 호출과 generator 전용
두 번째 인자에 대한 `@ts-expect-error` 검증을 추가했다(`scripts/verify_packaged_contract.sh:178-203`).
이는 public declaration parity를 더 강하게 확인하는 test/evidence 변경이며 production runtime이나
public spec을 바꾸지 않는다. 같은 `Codex Sol`, effort `default (메타데이터 미노출)`로 contract와
POSDDD review를 다시 실행해 Medium 이상 finding이 없음을 확인했다.

```text
final-node-candidate-manifest-sha256=f4774be64fd215f40f50560e57a507f326d4ebe7910f28793d95f088a8ec3c24
수정 commit 없음
```

관련 gate `scripts/verify_packaged_contract.sh`는 packed browser/server generated registry와 exact
public overload check를 포함해 `NODE_PACKAGED_CONTRACT_PASS packages=7 browser=esm+generated-registry
server=commonjs+generated-registry`를 반환했다(`/tmp/zlink-node-packaged-contract-20260808-final-r8.log`,
`NODE_PACKAGED_CONTRACT_R8_RC=0`). generator `node --check`와 fixture `2/2`도 재실행했다
(`/tmp/zlink-node-generator-final-r7.log`, `NODE_GENERATOR_R7_RC=0`). class DTO fixture와 packed
consumer 기대 목록을 갱신한 test/evidence 변경이며 production runtime·public spec은 바꾸지 않았다.
기존 r4 Node 20/22 aggregate, 현재 candidate의 Node 20 단독 `99/99`와 Node 22 runtime gate를
각각 구분해 유지한다. 최종 contract와 POSDDD 판정은 계속 `CLEAN`, Medium 이상 `0`,
미실행 필수 gate `0`이다.

### 2.7 최종 evidence 후 Codex Sol 재검토

최종 evidence를 추가한 뒤 같은 `Codex Sol`, effort `default (메타데이터 미노출)`, 기준 commit
`137f2858bf7fd29f58405893473be8e773725a93`과 candidate manifest
`f4774be64fd215f40f50560e57a507f326d4ebe7910f28793d95f088a8ec3c24`를 다시 대조했다. 대상은 55개
internals, 7개 public contract, command 51, NODE-SESS-001 production path, generated registry,
owner-layer regression, package/clean-consumer와 process evidence다.

- contract review: exact interface의 `ZLinkPacket(packetName: string)`과 packed declaration을 유지하고,
  generator 전용 overload가 `stripInternal` 뒤에 남지 않는 것을 재확인했다. command 51 source authority
  fence, retired identity fence와 stale tombstone guard에도 누락이 없다.
- POSDDD review: production runtime을 먼저 다시 읽고, generator는 message read turn에서 AST를 읽지 않으며,
  replacement timer는 serial turn 밖에서 동작하고, unit fixture는 schema 종류·malformed·unsupported를
  한 계약 증거로 묶는 것을 재확인했다. 새 allocation/copy·lock contention·dead branch·중복 test 또는
  Medium 이상 lifecycle/ownership finding은 발견되지 않았다.
- 최종 gate: 현재 Node 22 `verify:ci` `RC=0`, generator `2/2`, Node 20 sample regression `48/48`,
  Node 20 channel client `99/99`, Node 22 runtime gate `RC=0`, package
  `NODE_PACKAGED_CONTRACT_PASS`, 실제 세 process `NODE_SESS_001_PROCESS_PASS`를 확인했다. 연속
  matrix `r5`–`r10`의 환경 실패는 PASS로 기록하지 않고 앞 절의 별도 이력으로 남겼다.

최종 contract와 POSDDD 판정은 `CLEAN`, Medium 이상 finding `0개`, Low 잔여 위험 `0개`, 미실행 필수
gate `0개`로 유지한다.

### 2.8 현재 candidate의 최종 Codex Sol 재검토

최신 fixture·package expectation·gate log를 반영한 뒤 같은 `Codex Sol`, effort `default (메타데이터
미노출)`, 기준 commit `137f2858bf7fd29f58405893473be8e773725a93`, candidate manifest
`f4774be64fd215f40f50560e57a507f326d4ebe7910f28793d95f088a8ec3c24`로 마지막 review를 수행했다.
review 대상은 01–12 internals의 55개 ID, public contract 7개, 기존 종결 3개, NODE-SESS-001,
command 51, build-time DTO registry, package/clean-consumer와 실제 process다. 보호된
`framework/doc/framework/common/spec/` 및 `framework/runtime/protocol/`의 현재 dirty 변경은 이
candidate에서 만들지 않았으며, review 중 수정하지 않았다.

- **Contract 순서**: exact Node interface의 `ZLinkPacket(packetName: string)`과
  `ZLinkSession.onActorBindingReplaced?(context, actorId)`를 대조하고, packed declaration의
  `Attributes.d.ts:17`과 `IZLinkSession.d.ts`를 확인했다. Production replacement path
  `service-stateful-runtime.ts:3167-3334`, wire encode/decode
  `service-stateful-wire-codec.ts:637-656,977-1000`, session closing/timer
  `session-context.ts:167-204,250-255`, `stream-session-runtime.ts:370-441`가 정식 semantic
  constraint와 일치한다. DTO generator `generate-framework-json-schemas.mjs:15-89,338-507`와
  fixture `framework-json-schema-generator.test.js:17-120`은 class·interface·type alias·enum 및
  required·optional·nullable·nested·array·int32·int64·bytes, malformed·unsupported build failure를
  확인한다. 누락·오판·부분 구현은 발견되지 않았고 Medium 이상 finding은 `0개`다.
- **POSDDD 순서**: production runtime을 먼저 검토한 결과 AST extraction은 build turn에만 있고
  message read hot path에 allocation·payload 변환·blocking wait를 추가하지 않는다. replacement
  callback timer는 serial lane 밖에 있으며 exact identity를 다시 확인한다. 그 다음 unit test를
  검토해 generator fixture·prototype materializer·error-frame regression이 서로 다른 observable
  invariant를 보존하고, 중복 또는 private 호출 순서 결합 test가 없음을 확인했다. Medium 이상
  finding `0개`, Low 잔여 위험 `0개`다.
- **최신 재실행 gate**: `verify:ci` r11 `RC=0`, typecheck/build r3/r2 `RC=0`, M6A/B/C r6
  `39/39·94/94·85/85`, service-wire schema/fixture r6 `RC=0`, generator r7 `2/2`, packed consumer
  r8 `RC=0`, Node 20 channel client r2 `99/99`, Node 22 runtime gate r2 `RC=0`, 실제
  `actor-a/session-a/session-b` process r10 `NODE_SESS_001_PROCESS_PASS`를 확인했다. 미실행 필수
  gate는 `0개`이며, commit·push는 수행하지 않았다.

이 재검토에서도 contract와 POSDDD 모두 `CLEAN`으로 유지한다.

### 2.9 최종 report consistency recheck

2.8 이후에는 production source나 test를 변경하지 않고 report의 gate log 경로와 process 측정값만
최신 실행 결과에 맞춰 정리했다. 같은 `Codex Sol`, effort `default (메타데이터 미노출)`로
`final-node-candidate-manifest-sha256=f4774be64fd215f40f50560e57a507f326d4ebe7910f28793d95f088a8ec3c24`를
다시 계산했으며, source·test·script 1,851개 파일과 일치했다. `stream-session-runtime.ts:374-445`의
callback deadline·detached completion·exact 100 ms timer, `service-stateful-runtime.ts:3172-3307`의
authority/retired identity fence·bounded retry, packed declaration `Attributes.d.ts:17`을 다시 대조했다.

report-only consistency 변경에서 발견 사항·수정 사항은 log 경로의 이전 r7/r9 표기 정정과 aggregate
matrix r10의 환경성 readiness failure 기록뿐이며, production 수정 commit은 없다. 최신 `verify:ci`
r11, package r8, process r10과 wire·M6·sample·browser·per-major gate는 앞 절의 원본 log에서 재확인했다.
aggregate r10은 Node 22 `channel-client.test.js` 98/99 readiness race로 `RC=1`이어서 PASS로 승격하지
않았다. contract와 POSDDD는 계속 `CLEAN`, Medium 이상 finding `0개`, 미실행 필수 gate `0개`다.

## 3. NODE-SESS-001 종결

새 replacement identity가 current가 되면 bind terminal을 즉시 반환한다. 이전 session owner의
ACK·callback·close를 기다리지 않으며, 전송 admission 실패는 bounded asynchronous retry로
분리한다. `boundSessionReplaced(51)`은 one-way으로 encode/decode하고, transport source authority와
이전 owner lifecycle·retired binding identity를 exact하게 검증한다.

이전 session은 callback 전에 closing 상태로 전환되어 신규 inbound application dispatch를 거부한다.
`onActorBindingReplaced?(context, actorId)` callback 안의 `context.client.send(...)`는 허용한다.
callback은 session turn에서 시작하지만 Promise를 serial lane에 걸어 두지 않으며, callback terminal 뒤
exact retired identity를 다시 확인하는 non-blocking 100 ms timer를 예약하고 즉시 반환한다. callback이
기존 `requestTimeoutMs` lifecycle deadline 안에 terminal이 되지 않으면 같은 exact identity에서 즉시
force-close한다. 다른 session lane은 callback이 pending인 동안에도 진행한다. old ACK를 기다리던
`fe002ce0ea` test는 이전 계약 이력으로 보존하고 새 one-way 계약 검증으로 대체했다.

증거:

- production candidate `npm test`의 build·typecheck·lint와 전체 Node contract suite `RC=0`;
  최종 candidate의 최신 재실행은 `verify:ci` 및 Node 20/22 per-major gate로 별도 기록했다.
- `npm run verify:m6a-runtime`, `verify:m6b-runtime`, `verify:m6c-runtime`: `39/39`, `94/94`, `85/85 PASS`,
  최신 로그 `/tmp/zlink-node-m6a-20260808-final-r6.log`, `/tmp/zlink-node-m6b-20260808-final-r6.log`,
  `/tmp/zlink-node-m6c-20260808-final-r6.log`
- `npm run verify:runtime-matrix`: production candidate r4에서 Node 20·22 aggregate `RC=0`, 최종 candidate
  r10에서는 Node 20 단계가 통과하고 Node 22 readiness race로 `RC=1`이었다. 각 major의 sample regression
  `48/48` 포함(`/tmp/zlink-node-runtime-matrix-20260808-final-r4.log`). 최종 candidate의
  Node 20 sample regression은 `48/48`, `RC=0`(`/tmp/zlink-node20-sample-regression-20260808-final-r3.log`),
  Node 20 channel client는 `99/99`(`/tmp/zlink-node20-channel-client-20260808-final-r2.log`), Node 22
  전체 runtime gate는 `RC=0`(`/tmp/zlink-node22-runtime-gate-20260808-final-r2.log`)로 독립 재확인했다.
- generator candidate 이후 command 51 schema·golden·malformed validator: `RC=0`, `boundSessionReplaced=pass`
- `stream-session-runtime.test.js`, `stream-runtime.test.js`, channel JSON와 contract surface test 결합: `242/242 PASS`
- `boundSessionReplaced(51)` golden·malformed fixture: `canonical=2`, `malformed=6`, `boundSessionReplaced=pass`
- `scripts/verify_packaged_contract.sh`: `NODE_PACKAGED_CONTRACT_PASS packages=7 browser=esm+generated-registry
  server=commonjs+generated-registry`, 최신 log `/tmp/zlink-node-packaged-contract-20260808-final-r8.log`
- 실제 `actor-a`, `session-a`, `session-b` 세 process rebind: `NODE_SESS_001_PROCESS_PASS`.
  bind terminal 선행, callback 안내 packet, 100 ms timer 이후 close와 다른 session lane 진행을
  확인했다. 최신 process log `/tmp/zlink-node-process-rebind-20260808-final-r10.log`의 wall-clock 측정은
  callback packet 수신부터 close까지 `101 ms`이며, callback deadline과 terminal timer 구현은
  `stream-session-runtime.ts:382-438,398-405`다. stalled callback deadline과 lane 반환은
  `stream-session-runtime.test.js:352-410`에서 fake clock으로 확인했다.

## 4. 전체 검증 증거

| Gate | 결과 | 해석 |
|---|---|---|
| `npm test` / `npm run verify:ci` runtime gate | PASS, `RC=0` | 동일한 `run_node_runtime_gate.js` 경로를 현재 Node 22 candidate r2와 `verify:ci` r11에서 통과했다. 현재 로그에는 build·typecheck·lint·전체 Node test와 class DTO generator test가 포함된다. |
| `npm run verify:ci` | PASS, `RC=0` | 현재 candidate의 CI release gate를 재실행했다. `/tmp/zlink-node-verify-ci-20260808-final-r11.log` |
| `npm run typecheck` | PASS, `RC=0` | 현재 candidate에서 TypeScript 전체 no-emit typecheck를 별도로 재실행했다. `/tmp/zlink-node-typecheck-20260808-final-r3.log` |
| `npm run build` | PASS, `RC=0` | packed consumer gate가 현재 candidate의 workspace build를 먼저 수행했다. `/tmp/zlink-node-packaged-contract-20260808-final-r8.log` |
| `stream-session-runtime` focused | PASS, `55/55` | callback send/closing/lane, stalled callback deadline force-close와 기존 stream lifecycle. `/tmp/zlink-node-stream-session-focused-20260808-final-r2.log` |
| `npm run test:browser` | PASS, `1/1` | 실제 Chromium에서 ws/wss·flow·reconnect·drain·trust를 확인했다. `/tmp/zlink-node-browser-20260808-final-r3.log` |
| `npm run verify:m6a-runtime` | PASS, `39/39` | M6A wire·HWM·admission 경계. `/tmp/zlink-node-m6a-20260808-final-r6.log` |
| `npm run verify:m6b-runtime` | PASS, `94/94` | Actor·session·service wire와 command 51. `/tmp/zlink-node-m6b-20260808-final-r6.log` |
| `npm run verify:m6c-runtime` | PASS, `85/85` | relocation·execution barrier·typed failure. `/tmp/zlink-node-m6c-20260808-final-r6.log` |
| service-wire schema/fixture validator | PASS, `RC=0` | `42 commands`, canonical/malformed·framework error·command 51 fixture. 최신 로그 `/tmp/zlink-service-wire-schema-20260808-final-r6.log`, `/tmp/zlink-service-wire-fixtures-20260808-final-r6.log` |
| `scripts/verify_packaged_contract.sh` | PASS, `RC=0` | 7 packed npm packages와 generated browser ESM/server CommonJS registry clean consumer. 최신 로그 `/tmp/zlink-node-packaged-contract-20260808-final-r8.log` |
| `npm run verify:samples` | PASS | TicTacToe, Bingo, DeliveryDispatch, SupportChat, GameQuest, ShoppingMall, ZoneWorld. `/tmp/zlink-node-verify-samples-20260808-final-r3.log` |
| `npm run verify:abi-matrix` | PASS | Node framework ABI matrix declared and linked. `/tmp/zlink-node-abi-matrix-20260808-final-r3.log` |
| `npm run verify:cross-language` | PASS, `RC=0` | 최신 Node 후보에서 channel·fanout·route·stream drain·Redis `13/13` PASS, 로그 `/tmp/zlink-node-cross-language-20260808-final-r5.log` |
| 다섯 언어 logger/provider aggregate | OUT OF SCOPE (별도 통합 gate) | Node·dotnet cross-language smoke는 통과했지만 Kotlin `ObservabilityOps/run_e2e.sh all`은 maintenance selector 미구현으로 `RC=3`을 반환했다. Node Framework 필수 gate가 아니므로 본 report의 completion 조건에는 포함하지 않는다 |
| Node 20 standalone sample regression (final candidate) | PASS, `48/48`, `RC=0` | `run_samples.sh` 포함. `/tmp/zlink-node20-sample-regression-20260808-final-r3.log` |
| Node 22 standalone runtime gate (final candidate) | PASS, `RC=0` | build·typecheck·lint·browser, 전체 contract test와 sample regression을 단독 실행했다. `/tmp/zlink-node22-runtime-gate-20260808-final-r2.log` |
| Node 20 standalone channel owner regression | PASS, `99/99`, `RC=0` | Node 20 `channel-client.test.js` 단독 재실행. `/tmp/zlink-node20-channel-client-20260808-final-r2.log` |
| Node 20/22 aggregate runtime matrix | ENVIRONMENT RETRY 기록 | 현재 candidate r8은 Core mutex `SIGABRT`, r9는 protobuf channel readiness timeout, r10은 Node 22 `ClientServer` readiness race로 실패했으며 PASS로 승격하지 않았다. 기존 production candidate r4 `RC=0`과 현재 per-major owner gate를 함께 보존한다. `/tmp/zlink-node-runtime-matrix-20260808-final-r8.log`, `/tmp/zlink-node-runtime-matrix-20260808-final-r9.log`, `/tmp/zlink-node-runtime-matrix-20260808-final-r10.log` |

이전 matrix 실행에서 stale gate lock·sample child, ZoneWorld assertion, Node 20 channel readiness race와
Redis endpoint timeout이 각각 발생했다. 최종 candidate 재실행 `r5`–`r10`에도 ZoneWorld child cleanup,
DERR-002 readiness race, Node 22 native process request failure, Core mutex `SIGABRT`와 protobuf readiness
  timeout과 Node 22 ClientServer readiness race가 각각 발생했지만, 각 실패 뒤 남은 process를 정리하고 owner test 또는 per-major gate를 독립
재실행했다. production candidate `r4` matrix는 Node 20·22 모두 `RC=0`이며, 현재 candidate의
`verify:ci` Node 22 실행, 독립 Node 20 channel/sample과 Node 22 runtime gate도 `RC=0`이다. 현재
sample child와 runtime matrix process는 남아 있지 않다.

## 5. `NODE-WIRE-002` 구현 완료 — interface 유지 경로

### 5.1 이슈 요약

| 항목 | 내용 |
|---|---|
| ID·심각도 | `NODE-WIRE-002`, High (수정 전) |
| 상태 | `CLOSED`, build-time schema extraction·registry·package/process 증거 완료 |
| 발견 지점 | 초기 candidate에서 공통 typed JSON profile과 Node exact interface·production declaration의 public surface가 어긋났고 erased DTO schema 연결이 없었다 |
| 현재 영향 | exact interface와 packed declaration은 `ZLinkPacket(packetName: string)`만 노출한다. AST generator가 build 시점에 DTO contract를 생성하므로 runtime message read마다 AST를 해석하지 않는다. |
| 보호 경계 | `framework/doc/framework/common/spec/`는 사용자 승인 없이 수정하지 않는다. 이번 검토에서도 수정하지 않았다. |

### 5.2 사실과 기대 계약

공통 `04-message-model.ko.md:95-118`은 모든 Framework typed application payload에
`framework-json-v1`을 적용하고, 누락된 required property·nullable 조건·enum·int32·bytes 표현을
검증하도록 정의한다. DTO가 더 엄격한 property 집합을 정하면 DTO 계약이 우선한다.

Node exact interface는 `framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md:177`에서
다음 선언만 고정한다.

```ts
export declare function ZLinkPacket(packetName: string): ClassDecorator;
```

초기 candidate의 production source는 `framework/languages/node/packages/framework/src/contracts/Handlers/Attributes.ts:36-45`에서
`jsonContract?: ZLinkPacketJsonContract`를 받았다. 최종 구현에서는 이 overload를 `@internal`로
표시하고 `stripInternal`을 적용해 packed declaration에는 `framework/languages/node/packages/framework/dist/contracts/Handlers/Attributes.d.ts:17`의
한 인자만 남긴다. `ZLinkJsonSchema`와 `ZLinkPacketJsonContract`의 runtime 타입 정의는
`framework/languages/node/packages/framework/src/contracts/Handlers/JsonContract.ts:1-29`에 유지하되,
root public handler index에서는 다시 export하지 않는다(`contracts/Handlers/index.ts:1-15`).

이 인자는 장식 metadata에만 남는 선택 정보가 아니다. `packet-name.ts:34-59`가 packet type에서 계약을
조회하고, 기본 payload codec은 `payload-codec.ts:108-118,223-232`에서 encode/decode schema로 사용한다.
Channel과 STREAM도 각각 `channel-envelope.ts:262-265,359-369,422-430` 및
`stream-frame-factory.ts:142-149`에서 같은 계약을 사용한다. 즉 현재 runtime은 공통 typed JSON
profile의 DTO별 검증을 실제로 제공한다.

이 동작은 test-only 우회도 아니다. `channel-envelope-error.test.js:77-129`는 공통 golden vector의
required·enum·int64·bytes·nullable 검증을, `:131-205`는 Channel request/reply 적용을,
`:207-247`는 invalid schema 거부와 immutable contract를 검증한다. 이 public 인자를 제거하면 이
회귀 증거와 DTO별 typed validation 경로가 함께 사라진다.

### 5.3 DTO runtime type 연결에 대한 추가 확인

Node가 DTO를 runtime에서 전혀 받을 수 없는 구조인 것은 아니다. public `Type<T>`는 생성자 type을
표현하고(`packages/framework/src/contracts/Common/CoreTypes.ts:1`), `ZLinkMessage.decode<T>(type?)`도
선택적인 runtime constructor를 받는다(`packages/framework/src/contracts/Common/ZLinkMessage.ts:21-30`).
encoded message의 decoder 역시 이 type을 `schemaForDecode(type, ...)`로 전달할 수 있다
(`packages/framework/src/runtime/messaging/payload-codec.ts:223-232`). 따라서 class DTO를 실제 값으로
넘기면 runtime type과 packet metadata를 연결하는 구현은 가능하다.

이 경로가 연결되지 않은 호출은 DTO를 `import type`으로만 가져오고
`payload.decode<T>(Object as never)`를 호출한다. 이 호출에서 generic `T`는 compile 후 사라지고
runtime에는 `Object`만 전달된다. 이전 candidate의 대표 사례는
`samples/GameQuest.Ts/Server/GameApi/game-api-session.ts`와
`samples/ZoneWorld/Server/Gateway/player-session.ts`였다. 이번 수정에서 class DTO를 사용할 수 있는
경로는 `payload.decode(JoinSessionReq)`와 `payload.decode(JoinWorldReq)`로 바꾸어 실제 constructor를
runtime에 전달한다(`game-api-session.ts:46`, `player-session.ts:42`). 같은 변경을 SupportChat,
TicTacToe, Bingo, DeliveryDispatch, ZoneWorld operations와 AutomaticTurnDispatch뿐 아니라 Bingo room,
SupportChat spot, SpotActorTransfer, ObservabilityOps, ToActorMessaging의 class DTO 호출에도 적용했다.
반면 TypeScript `interface`·`type` alias만 존재하는 DTO는 runtime value가 없으므로 아직
  `Object` fallback을 사용한다. 이 fallback은 runtime constructor 값의 부재를 뜻하며, wire schema는
  generator registry에서 별도로 적용된다. `ZLinkMessage.decode`의 fallback도 이제
`ZLinkMessage.ts:32-36`에서 JSON을 parse한 뒤 전달된 constructor의 prototype을 재부착하지만,
generic type만으로는 이를 수행할 수 없다.

또한 `@ZLinkPacket(...)`은 현재 DTO class가 아니라 handler class에 붙는다
(`samples/GameQuest.Ts/Server/GameApi/game-api-session.ts:36-45`). `tsconfig.base.json:9-10`에
`emitDecoratorMetadata`가 켜져 있어도 framework는 `design:paramtypes`나 `design:type`을 읽지 않으며,
constructor metadata만으로는 parameter 이름, optional·nullable 여부, enum members, nested object와
array item schema를 복원할 수 없다. TypeScript `interface`와 `type` alias는 emitted JavaScript에
아예 남지 않는다. 따라서 “DTO type을 runtime에 넘기면 packet을 매칭할 수 있다”와 “현재 DTO 선언에서
공통 spec의 전체 schema를 자동으로 복원할 수 있다”는 서로 다른 주장이다.

이번 수정으로 default JSON decode에서 constructor를 실행하지 않고 DTO prototype을 재부착하는 공통
materializer를 추가했다(`packages/framework/src/contracts/Common/ZLinkMessage.ts:69-100`). 직접 lazy
message와 `decodeFrameworkPayloadMessage`·`wrapFrameworkPayloadMessage`의 schema 검증 결과가 모두
같은 경로를 사용한다(`runtime/messaging/payload-codec.ts:170-172, 211-215`). constructor 실행으로
application side effect를 일으키지 않으면서 parsed field를 instance own property로 복사하므로
`__proto__` 입력도 prototype setter를 호출하지 않는다. 회귀는
`test/contract/channel-envelope-error.test.js:77-128`에서 constructor 비실행, `instanceof`, schema
검증, prototype pollution 경계를 확인한다. build와 typecheck, 해당 contract test 및 sample-regression
`48/48`이 통과했다. class DTO의 prototype materialization은 runtime 값의 안전한 복원만 담당하고,
erased `interface`·`type` alias의 wire schema는 별도 AST generator가 build 시점에 산출한다.

초기 후보에서 확인된 `NODE-WIRE-002`의 성격은 다음처럼 해결되었다.

1. Node가 현재 제공하는 explicit `jsonContract`는 erased TypeScript 정보를 보완하는 동작 가능한
   경로다.
2. class DTO에 대한 production dispatch 호출은 runtime constructor 경로를 사용하고, interface·type
   alias와 required·nullable·enum·int32·bytes schema는 `scripts/generate-framework-json-schemas.mjs`
   가 build-time registry로 연결한다. 따라서 message read turn에서 AST를 다시 읽지 않는다.

`.NET`과 Java가 동작하는 이유도 같은 경계에서 확인된다. .NET은 `Type`을 직접 받아
`System.Text.Json`에 전달한다(`framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkFrameworkJsonPayloadCodec.cs:15-22`).
Java는 handler가 `messageType()`으로 `Class<?>`를 제공하고 dispatcher가 그 값을
`payload.decode(...)`와 packet-name lookup에 함께 사용한다
(`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/streams/ZLinkSessionPacketDispatcherRuntime.java:51-80,131-148`).
Node도 class DTO를 `payload.decode(DtoClass)`로 넘기는 경우에는 같은 종류의 runtime type 연결을
구성할 수 있다. TypeScript `interface`·`type` alias는 Java의 `Class<?>`나 .NET의 `Type`으로
변환되지 않으므로, 이 후보에서는 build-time schema 산출물로 그 차이를 해소했다.

### 5.4 보이는 실패와 판정 영향

초기 candidate에서는 source/dist의 runtime overload와 exact interface가 달라 두 표면을 동시에
만족하지 못했다. 최종 구현은 source overload를 `@internal` build hook으로 제한하고 packed declaration에서
제거했으며, generated registry가 내부에서만 contract를 등록한다. 따라서 package consumer의 정식
표면은 exact interface의 한 인자 선언과 같고, required·nullable·enum·int32·bytes 검증은 build
산출물로 유지된다. 이는 wire command 51 인코딩 실패가 아니라 **build-time extraction으로 해소한
초기 공개 표면 불일치**였다.

### 5.5 해결안 검토와 채택

#### 안 A — exact interface에 현재 runtime 계약을 반영한다 (미채택)

보호된 Node exact interface에 `ZLinkJsonSchema`, `ZLinkPacketJsonContract`와 다음 형태의 선택
인자를 명시한다.

```ts
export declare function ZLinkPacket(
  packetName: string,
  jsonContract?: ZLinkPacketJsonContract
): ClassDecorator;
```

이 안은 현재 production source·dist·typed JSON tests를 보존하고, common spec의 DTO별 validation
요구도 그대로 만족시킨다. 대신 exact interface EN/KO가 public contract 변경을 포함하므로 다음이
필요하다.

- 사용자 승인과 contract governance 검토
- Node exact interface의 EN/KO 동시 갱신 및 타입 정의·validation 범위 명시
- 다른 Framework 언어의 typed payload 계약과 public surface 대조
- source build/typecheck, contract surface, command 51 및 JSON golden/malformed fixture,
  packed clean-consumer, cross-language와 process gate 재실행
- 변경이 release-visible public API인지 versioning·migration 판단

#### 안 B — `ZLinkPacket`의 두 번째 인자를 정식 declaration에서만 제거한다 (안 D와 결합)

`Attributes.ts`와 dist에서 `jsonContract`를 제거하고 관련 registry/codec 경로와 테스트를 축소해 exact
interface에 맞춘다. 그러나 이 안은 Node가 DTO별 required·nullable·enum·int32·bytes contract를
등록할 수 없게 하므로 공통 `framework-json-v1` 요구를 만족하지 못한다. 최종 구현에서는 정식
packed declaration에서만 두 번째 인자를 제거했으며, production 내부 build hook과 registry는
유지했다. 따라서 public contract를 후퇴시키지 않고도 기존 `channel-envelope-error.test.js:98,150,210-236`
회귀를 보존한다.

#### 안 C — public surface는 유지하되 exact interface에서만 숨긴다 (미채택)

source의 두 번째 인자를 declaration에서 숨기거나 private metadata adapter로 우회하는 방식이다.
이는 package consumer가 실제로 호출할 수 있는 API를 정식 interface에서 누락시키므로 public contract
parity를 해결하지 못한다. 내부 helper·raw metadata·test adapter로 보완하는 것도 저장소의 기존
설계 규칙에 맞지 않아 채택하지 않는다.

#### 안 D — build-time DTO schema extraction으로 runtime contract를 생성한다 (채택·완료)

TypeScript AST에서 class·interface·type alias·enum·optional·nullable·nested object·array 정보를
읽어 `framework-json-v1` schema를 build 산출물로 생성하고, packet name별로 runtime registry에 연결하는
방법이다. `ZLinkPacket(packetName)`의 exact interface를 유지하면서 .NET·Java의 type-driven 방식에
가깝게 동작하므로 spec 변경 없이 이 안을 구현했다.

단순히 `payload.decode<T>(...)`를 바꾸지 않고 generator가 DTO 선언과 packet·handler 정보를
build 시점에 연결했다. 다음 조건을 모두 충족했다.

- 모든 DTO type alias/interface를 schema로 표현하고 unsupported TypeScript construct는 build에서 거부
- enum member, `int32`/`int64`, `bytes`, required·optional·nullable의 의미를 명시적으로 보존
- generated registry를 sample build 산출물과 packed clean-consumer의 browser/server build에 포함
- 생성 결과의 golden/malformed fixture와 packed clean-consumer 검증
- runtime 값 관찰이나 constructor parameter 이름 parsing을 schema 생성의 근거로 사용하지 않음

이 안은 public spec을 바꾸지 않고 compiler/build pipeline과 generated asset 경계를 추가한다.
생성 registry는 build 후 한 번만 읽고, application message read turn에는 AST extraction이나
`setTimeout`/Promise 대기를 수행하지 않는다.

### 5.6 최종 결정과 완료 조건

현재 지시에 따라 **안 A는 제외하고 안 D를 완료**했다. class DTO prototype materialization과
호출부의 runtime constructor 전달은 registry schema validation과 함께 유지하며, alias·interface를
포함한 generated schema pipeline도 구현했다. 안 B는 public contract를 후퇴시키지 않도록 정식
declaration에만 반영했고, 안 C는 채택하지 않았다.

다음 정식 계약 보호 경로는 수정하지 않았다.

- `framework/doc/framework/common/spec/`
- generated `dist` declaration과 JSON contract export는 build 결과로만 검증한다

`Attributes.ts`의 generator 전용 overload는 source 내부 구현으로 추가했지만 `@internal`과
`stripInternal`로 packed public declaration에서 제거했다(`Attributes.ts:36-47`, `packages/framework/tsconfig.json:7`).

따라서 이 항목은 spec 변경 blocker가 아니라 **build-time schema 구현 완료**로 판정한다.
동일한 Codex Sol 최종 contract/POSDDD review와 위 gate에서 `CLOSED`를 확인했다.

## 6. 설계·운영 경계

- `NODE-RELOC-003`은 host relocation aggregate와 remote Actor Join membership transaction이 서로
  다른 bounded context이므로 공통 stage나 base class로 합치지 않는다.
- `NODE-SESS-003`은 `ServiceMailbox`, `EventLoopWorkQueues`, `RouterOperationQueue`가 각각 owner
  claim, process infrastructure/application 진행, native request slot 순서를 소유하므로 하나의
  얕은 serial queue abstraction으로 합치지 않는다.
- package·process·cross-language 증거는 source gap과 별도로 기록한다. 한 범주의 PASS를 다른
  범주의 PASS로 승격하지 않는다.
- 기존 dirty 변경은 그대로 두었고 `main`에서 commit·push하지 않았다. exact interface와 보호된
  spec은 변경하지 않았다.

## 7. 후속 조치

1. 완료: AST generator가 DTO schema와 packet registry를 생성한다.
2. 완료: server CommonJS·browser ESM·packed clean-consumer가 generated registry를 소비한다.
3. 완료: unsupported expression build failure, command 51 golden/malformed, owner regression,
   process와 production-candidate Node 20/22 aggregate gate를 확인하고, 최종 candidate의 Node 20 sample·
   Node 22 per-major runtime gate를 독립 재실행했다. 연속 matrix의 intermittent process 실패는 PASS로
   승격하지 않고 이력과 재실행 증거를 함께 기록했다.
4. 잔여 필수 조치는 없다. `NODE-WIRE-002`는 `CLOSED`, 전체 report의 BLOCKED는 `0`이다.
