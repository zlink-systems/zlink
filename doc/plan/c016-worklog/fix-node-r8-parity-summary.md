# Node Framework R8 오류 분류와 Redis 크기 경계 수정

감독이 승인한 R8 진단의 F-R8-18, F-R8-15, F-R8-14를 현재 코드와 공통 계약에
다시 대조하고 Node 구현을 수정했다. 감독이 각 원인별 diff와 검증 결과를 판단하기 위한 기록이다.
`main`에서 작업했으며 commit과 push는 수행하지 않았다. 세 결함의 회귀 검증은 통과했지만,
전체 gate의 실패 0건 조건은 충족하지 못했다. 남은 두 실패는 아래에 분리해서 기록했다.

## 결과와 변경 파일

각 행의 runtime과 test는 다른 두 원인의 변경 없이 분리해서 적용할 수 있다.
독립 patch는 `/tmp/zlink-node-r8-parity.mVQCTy/F-R8-18.patch`, `F-R8-15.patch`,
`F-R8-14.patch`에 있으며 각각 `git apply --reverse --check`를 통과했다.
아래 경로는 저장소 루트 기준이다.

| 항목 | 결과 | Runtime 파일 | 회귀 테스트 파일 |
|---|---|---|---|
| F-R8-18 | Typed HTTP status ≥ 400을 `InternalFailure`로 분류한다. | `framework/languages/node/packages/http-client/src/request-builder.ts:220` | `framework/languages/node/test/contract/http-client.test.js:422` |
| F-R8-15 | Actor create/get-or-create 중복 옵션을 기존 `InvalidOperation`으로 분류한다. | `framework/languages/node/packages/framework/src/runtime/actors/index.ts:919` | `framework/languages/node/test/contract/actor-create-call.test.js:23` |
| F-R8-14 | Redis provider가 encoded blob `64 MiB + 23 bytes`까지 수락한다. | `framework/languages/node/packages/framework-locations-redis/src/relocation-store.ts:17` 및 `:126` | `framework/languages/node/test/contract/relocation-redis-blob-bound.test.js:6` |

### F-R8-18

원인은 `request-builder.ts:218`의 typed status 검사에서 `Unavailable`을 만들던 분류값이다.
기존 typed response 처리에서 kind만 수정했다. 전송 오류 처리, raw response, retry 정책은
같은 소유 모듈에서 기존 동작을 유지한다.

회귀 테스트는 400·404·500·599의 정확한 public kind와 status가 포함된 오류 메시지,
raw response의 status·body 보존, 399의 typed 성공을 확인한다. Retry를 설정해도 status
실패를 재전송하지 않는지 요청 수로 확인한다. 별도 TCP listener가 연결을 끊는 테스트는
typed 제출의 전송 연결 실패가 `Unavailable`인지 확인한다.

기존 HTTP 테스트의 `Unavailable` assertion은 계약과 반대였다. 이를 `InternalFailure`로
바꾸고 경계·전송 실패 assertion을 추가했으며, 수정 전 runtime에서 실패함을 확인했다.

### F-R8-15

원인은 공통 create/get-or-create call의 `throwDuplicateOption()`이 internal
`InvalidConfiguration`을 선택하고 기존 매핑표가 이를 public `NotConfigured`로 변환하던
것이다. 이 call에서 기존 internal `InvalidOperation`을 사용한다. 매핑표와 검증 순서는
수정하지 않았다.

재제출은 재확인 시점부터 `actors/index.ts:909`의 `AlreadySubmitted`와
`framework/languages/node/packages/framework/src/runtime/framework-errors-internal.ts:81`의
매핑을 통해 `InvalidOperation`이었다. 따라서 재제출 구현 변경은 필요하지 않았다.

회귀 테스트 14건은 두 call에 대해 다음을 확인한다.

- `inMesh`, `request`, `timeout` 중복 설정 6건: `InvalidOperation`, Mesh 조회와 placement
  호출 0회, 원래 옵션 보존, 이후 정상 제출 1회. `request(undefined)`도 이미 설정한 옵션이다.
- `submit/submit`, `submit/yield`, `yield/submit`, `yield/yield` 재호출 8건:
  최초 operation 완료 전과 완료 후 모두 `InvalidOperation`, placement 호출은 총 1회.
  `yield`는 실제 Framework serial turn 안에서 검증한다.

### F-R8-14

원인은 provider 입력을 application data chunk의 64 MiB 상한으로 검사하던 상수다.
상수 이름을 `MAX_ENCODED_BLOB_BYTES`로 명확하게 하고 값을 `64 * 1024 * 1024 + 23`으로
수정했다. 상한 초과 오류 메시지도 encoded blob과 실제 상한을 나타낸다.

회귀 테스트는 공식 `ZLinkRedisRelocationStore.put()`에 실제 크기의 Buffer를 전달한다.
기존 `client` 옵션으로 가짜 Redis client를 주입하며 Redis 서버를 시작하거나 연결하지 않는다.
`64 MiB + 23`에서 provider의 저장 결과, 전체 bytes와 retention 전달을 확인하고,
`64 MiB + 24`에서는 Redis client 접근 전에 `RangeError`로 거부하며 command가 0회인지 확인한다.

## 소유권과 교차언어 대조

소유 계층: F-R8-18은 Framework HTTP typed response builder, F-R8-15는 Framework Actor
creation call, F-R8-14는 Framework Redis relocation-store provider다.

Spec 조항: F-R8-18은 `framework/doc/framework/common/spec/http-client/09-error-model.ko.md`
§9.1, F-R8-15는 `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md`
§6.2, F-R8-14는 `framework/doc/framework/common/spec/server/05-location-relocation/03-relocation-store-redis.ko.md`
§3이다. 재확인한 규범 문장은 각각 `:12`·`:15`, `:353`, `:121`·`:122`에 있다.

교차언어 대조 결과: HTTP의 C++·.NET·Java는 `InternalFailure`이며 Kotlin은 Java 결과를
전달한다. Actor의 C++·.NET은 중복 옵션과 재제출에 `InvalidOperation`을 사용한다.
현재 checkout의 Java도 같은 분류이며 Kotlin Actor wrapper는 Java call에 위임한다.
Redis는 .NET의 `64 MiB + 23`과 일치시켰고, 현재 checkout의 C++·Java도 같은 상한이다.
Node 차이는 언어 구조의 차이가 아닌 분류값·경계값의 기존 결함이다. 다른 언어는 코드만
대조했고 실행하거나 수정하지 않았다.

변경 분류: 세 항목 모두 **B — 기존 결함**이다. 감독의 D-114 승인 진단과 수정 지시에
따라 구현했다. 별도 public API, 보상 retry, 상태, timer, 매핑표를 추가하지 않았다.

수정 전/후 규칙 수: Node와 공통 계약 사이의 동일 입력 판정은 F-R8-18 **2 → 1**,
F-R8-15 **2 → 1**, F-R8-14 **2 → 1**이다. 중복·재제출의 상태 소유자는 기존 call 하나다.

코드 대조 근거는 다음과 같다. 위치는 수정 전에 실제 코드로 확인했다.

| 항목 | 대조 코드 |
|---|---|
| HTTP | `framework/languages/cpp/http-client/include/zlink/http_client/contracts/client.hpp:205`; `framework/languages/dotnet/src/Zlink.HttpClient/ZLinkHttpRequestBuilder.cs:223`; `framework/languages/java/zlink-http-client/src/main/java/systems/zlink/httpclient/ZLinkHttpRequestBuilder.java:187`; `framework/languages/java/zlink-http-client-kotlin/src/main/kotlin/systems/zlink/httpclient/kotlin/HttpClientCoroutines.kt:40` |
| Actor | `framework/languages/cpp/framework/src/runtime/actors/actor_client.cpp:103` 및 `:156`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorManagerService.cs:732` 및 `:743`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorRuntime.java:2632`; `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkOneWayCalls.kt:321` |
| Redis | `framework/languages/dotnet/src/Zlink.Framework.Locations.Redis/ZLinkRedisRelocationStore.cs:18` 및 `:339`; `framework/languages/cpp/extensions/framework-locations-redis/include/zlink/locations/redis.hpp:971`; `framework/languages/java/zlink-framework-locations-redis/src/main/java/systems/zlink/framework/locations/redis/ZLinkRedisRelocationStore.java:28` |

## 검증

로그 디렉터리: `/tmp/zlink-node-r8-parity.mVQCTy/`. 실행 환경은 Node `v22.23.2`다.

| 검증 | 결과 | 로그 |
|---|---|---|
| 수정 전 runtime으로 회귀 테스트 실행 | 18건 중 예상 결함 8건 실패, 10건 통과 | `red.log` |
| 수정 후 관련 TypeScript build와 집중 테스트 | build 성공, 18/18 통과 | `focused-build.log`, `focused.log` |
| 회귀 테스트 5회 반복 | 매회 18/18, 합계 90/90 통과, 실패·skip 0 | `repeat-1.log`부터 `repeat-5.log` |
| 전체 `npm test` | build·typecheck·lint 성공. Runtime 157개 파일, 1,690/1,691 통과, 실패 1, skip 0; exit 1 | `gate.log` |
| 실패한 timer 항목 단독 실행 | 1/1 통과; 전체 gate의 실패를 대체하지 않음 | `timer-isolated.log` |
| 미실행 M6A 단계 별도 실행 (`npm run verify:m6a-runtime`) | 40/41 통과, 실패 1, skip 0; exit 1 | `m6a.log` |
| `git diff --check` | 통과 | 명령 출력 없음 |

집중 테스트 명령은 다음과 같다.

```bash
cd framework/languages/node
node --test \
  --test-name-pattern='actor (create|getOrCreate) rejects|typed HTTP (status|transport)|Redis relocation encoded blob' \
  test/contract/actor-create-call.test.js \
  test/contract/http-client.test.js \
  test/contract/relocation-redis-blob-bound.test.js
```

실제 반복 검증과 전체 gate는 아래 두 lock을 획득한 shell 안에서 집중 테스트를
5회 실행한 뒤 `npm test`를 실행했다. Timer 단독 확인과 M6A 별도 실행에도 같은 두 lock을
적용했다. 전체 gate의 재현 명령은 다음과 같다.

```bash
cd framework/languages/node
flock -w7200 /tmp/zlink-samples-gate.lock \
  flock -w7200 /tmp/zlink-node-gate.lock npm test
```

## 남은 실패

1. `framework/languages/node/test/contract/spot-manager.test.js:4064`:
   `spot managed timer overrun policies follow dotnet skip catch-up and fixed-delay semantics`가
   pending timer 목록 `[10]`을 기대했으나 `[10, 5]`를 관찰했다
   (`gate.log:9339`). 이 테스트와 `runtime/spots/spot-timer.ts`에는 변경이 없다.
   해당 항목만 실행하면 통과했다. `spot-manager.test.js:4184`의 helper가 global
   `setTimeout`을 대체하여 다른 callback의 timer도 수집하는 경계까지 확인했으며,
   추가 5ms timer의 발행자는 확정하지 않았다. Assertion을 완화하거나 테스트를 수정하지 않았다.
2. `framework/languages/node/test/m6a/m6a-runtime.contract.ts:1958`:
   `bilateral endpoint-only manual connections learn peer RIDs and converge`에서 양쪽 peer의
   Ready와 initiator 판정 후 요청을 보냈으나 target의 `pumpOne() === 'application'` 대기가
   timeout됐다 (`m6a.log:236`, compiled test `:1479`). M6A는 raw service mesh/backend를
   검사하는 별도 경로이며 이 테스트와 해당 foundation/backend 코드에는 변경이 없다.
   Binding·transport 원인은 이 기록만으로 확정하지 않는다.

`npm test`는 첫 runtime gate 실패로 `&& npm run verify:m6a-runtime`을 실행하지 않았다.
위 M6A 결과는 남은 gate 단계를 같은 lock 아래에서 한 번 별도로 실행한 결과다.
실패 원인에 변화가 없는 전체 gate를 반복하지 않았으며, timeout·retry·expectation을
변경하지 않았다. **전체 gate 실패 0건은 미달**이며 감독의 별도 실패 조사 범위로 남긴다.

## 범위

변경 파일은 위의 Node runtime 3개, test 3개와 이 전달 문서다. 기존 사용자 변경과
`core/**`, `bindings/**`, 다른 언어, spec, `framework/doc/**`, `doc/site/**`를 수정하지 않았다.
요청한 `framework/languages/node/AGENTS.md`는 존재하지 않아 루트와 Framework 규칙을 적용했다.

별도 감독 문서 범위: `framework/doc/framework/common/spec/server/languages/node/interfaces/08-location-maintenance.ko.md:219`의
“Blob 하나는 최대 64 MiB” 문구는 공통 Redis Store §3의 encoded blob 상한과 불일치한다.
해당 문구는 공통 크기 경계로 위임하는 수정이 필요하며, 이번 작업의 문서 변경 금지 범위에 따라
수정하지 않았다.
