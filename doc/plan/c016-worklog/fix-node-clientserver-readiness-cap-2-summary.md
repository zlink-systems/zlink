# Node ClientServer readiness 및 기간 시간원 수정 결과

감독이 D-095(B)의 구현 범위와 남은 작업을 판정하기 위한 기록이다.
`awaitClientDealerForOutbound`는 `performance.now()`로 deadline을 계산하고 기존 polling
지연을 `min(5 ms, remaining)`으로 제한한다. 전진·후퇴 회귀는 실제 5초 하한과 8초 미만
상한을 유지한다. 나머지 Node runtime의 로컬 기간 계산도 같은 시간원을 사용하도록 수정했다.
STREAM 병행 작업과 겹칠 수 있는 파일의 미수정 duration은 아래 표와 BLOCKERS에 명시한다.

## 소유권과 변경 분류

- 소유 계층: Framework의 readiness selector, Actor/Spot 작업 deadline, idle/timer scheduler, Location retry와 host relocation/shutdown owner. Core·binding의 연결·completion·재전송 결정은 변경하지 않는다.
- Spec 조항: `server/02-channel-transport/02-channel-messaging.ko.md:170–174`(readiness), `03-spot-actor/01-spot-model.ko.md` §6.2(idle), `10-spot-timer.ko.md` §2·§3(overrun/monotonic admission), `05-location-relocation/04-relocation-flow.ko.md` §4.5·§9(Restore 원래 deadline·Message Follow), `01-execution/03-cancellation-and-shutdown.ko.md` §5(shutdown). 시간원 판정은 `decisions.ko.md` D-095.
- 교차언어 대조: Java `runtime/channels/ZLinkChannelSocketRegistry.java:268,274`는 `System.nanoTime()`과 remaining을 사용하고, `runtime/spots/ZLinkSpotTimerSchedule.java:54–69`는 보고용 `Instant`와 경과 시간용 `System.nanoTime()`을 구분한다. .NET readiness의 UTC 취약점은 기존 진단에 확인되어 별도 D-095 작업 대상이다. Node만 수정한 이유는 요청 범위이며 물리 transport의 차이가 아니다.
- 변경 분류: B — 감독이 승인한 기존 경과 시간 계산 결함. wire/store timestamp 계약은 유지한다.

수정 전/후 규칙 수: 변경한 기간 계산 owner마다 duration 시간원 2개(wall clock/monotonic) →
1개(`performance.now()` 및 기존 상대 지연 timer). readiness deadline owner 1 → 1,
polling timer 1 → 1. 보고용 timestamp는 duration 규칙에 포함하지 않는다.

기존 deadline의 시간원만 바꾸는 안을 적용했다. 두 번째 cap timer를 경합시키는 안은
완료·취소 소유자를 추가하므로 선택하지 않았다. Timestamp와 duration을 겸하던 Timer와
Spot 주소 요청은 보고·wire용 Unix 값과 로컬 기간 기준을 구분한다. 독립적인 새 timer나
재시도 정책을 추가하지 않는다.

## Date.now 호출 지점 감사

감사 기준은 수정 전 `ea0d47e1b8`이다. 아래 `file:line`의 공통 prefix는
`framework/languages/node/packages/framework/src/runtime/`이며, 숫자는 **수정 전 line**이다.
같은 파일·용도의 지점을 한 행에 모았다. 주석을 제외한 `Date.now()` 114개 호출을 모두
분류했다. duration 변경 73개, timestamp 유지 31개, 병행 작업으로 보류한 duration 10개다.
`유지`는 프로세스 간 비교 가능한 절대 시각이 필요하다는 뜻이며, monotonic으로 이미
변환한 로컬 duration을 다시 wall clock으로 측정한다는 뜻이 아니다.

| file:line (수정 전) | 구분 | 변경/유지 | 용도·근거 |
|---|---|---|---|
| `actors/actor-authority-publication.ts:102` | timestamp | 유지 | authority store에 게시하는 operationDeadline; store 시간과 비교하는 절대 시각 |
| `actors/actor-client.ts:213` | timestamp | 유지 | message metadata/Message Follow에 전송하는 Unix deadline |
| `actors/actor-client.ts:552` | duration | 변경 | 요청의 로컬 남은 시간 |
| `actors/actor-context.ts:485,502` | duration | 변경 | deferred join의 남은 시간 |
| `actors/actor-handoff.ts:907,955,1255` | duration | 변경 | Message Follow route의 로컬 보관 기간 |
| `actors/actor-handoff.ts:1400,1693,1783` | timestamp | 유지 | 수신·보관한 Message Follow의 Unix deadline 검증 또는 timer 진입 시 남은 시간 변환 |
| `actors/actor-local-native-join.ts:694,700` | duration | 변경 | join 연결 대기 한도 |
| `actors/index.ts:698,766` | duration | 변경 | Actor creation 대기 한도 |
| `channels/channel-envelope.ts:106` | timestamp | 유지 | envelope에 전송하는 ISO deadline |
| `channels/channel-socket-registry.ts:649,654` | duration | 변경 | ClientServer readiness 한도 및 polling 잔여 시간 |
| `channels/channel-transports.ts:589,590` | duration | 변경 | submit 전 남은 timeout |
| `diagnostics/flow-context.ts:79` | timestamp | 유지 | UUIDv7의 Unix timestamp 비트 |
| `foundation/service-stateful-runtime.ts:1147,1632,4035,4081,4094,4762,4805,4826,5218` | duration | 보류 | STREAM 병행 작업과 겹칠 수 있어 파일 수정 제외; BLOCKERS 참조 |
| `foundation/service-stateful-runtime.ts:1425,2284,2611,2803,2900,3007,3916,4222,5206,5257` | timestamp | 유지 | wire record의 Unix deadline 생성·검증·중복 record 유효성 검사 또는 timer 진입 시 변환 |
| `host/actor-packet-relay.ts:242` | timestamp | 유지 | 수신 Message Follow context의 Unix deadline 유효성 검사 |
| `host/actor-packet-relay.ts:504,515,556,607` | duration | 변경 | authority fence 및 session bind 제출 대기 한도 |
| `host/actor-placement-coordinator.ts:79` | timestamp | 유지 | 원격 creation record 및 store terminal publication의 Unix deadline |
| `host/actor-placement-coordinator.ts:162` | duration | 변경 | 원격 Actor creation 제출 전 남은 timeout |
| `host/actor-transfer-runtime.ts:1597,1633,1943` | duration | 변경 | authority claim/commit 재시도 한도 |
| `host/index.ts:949,969` | timestamp | 유지 | runtime status와 closing context에 보고하는 Date deadline |
| `host/index.ts:990,991,1086,1810,1825` | duration | 변경 | maintenance readiness와 shutdown의 로컬 한도 |
| `host/instance-activation-authority.ts:478,505,605` | duration | 변경 | activation/closing join 대기 한도 |
| `host/route-mesh-runtime.ts:355,555` | duration | 변경 | 숫자 timeout은 기존 timer에 직접 전달; Date 입력만 경계에서 변환 |
| `host/route-mesh-runtime.ts:461,495` | timestamp | 유지 | mesh status에 보고하는 Date deadline |
| `host/service-relocation-host-runtime.ts:1128,1152,1206,1970,2029,2316,2344,2514,2937,3257,3267,3277,3309,3310,4888,4904` | duration | 변경 | seal/ACK/Restore/reconciliation 한도 및 route convergence 경과 시간 |
| `host/spot-address-transport.ts:947,958,979` | duration | 변경 | Spot 주소 요청의 로컬 deadline와 남은 timeout |
| `host/user-spot-creation-coordinator.ts:103` | timestamp | 유지 | local/remote creation record에 전달하는 Unix deadline |
| `host/user-spot-creation-coordinator.ts:838` | duration | 변경 | 원격 User Spot creation 제출 전 남은 timeout |
| `locations/location-store-repository.ts:785,963,3061` | duration | 변경 | aggregate commit의 로컬 retry window |
| `spots/index.ts:795,924` | duration | 변경 | Instance Spot idle sweep |
| `spots/index.ts:1975,2356` | timestamp | 유지 | 수신 Actor join record의 Unix deadline 검사 및 authority commit 경계에 전달할 Unix deadline |
| `spots/spot-activation-state.ts:383` | duration | 변경 | idle eviction 판정 |
| `spots/spot-actor-packet-dispatch.ts:438` | timestamp | 유지 | handler admission에서 wire request의 Unix deadline 검사 |
| `spots/spot-actor-packet-relay-dispatch.ts:113` | timestamp | 유지 | relay admission에서 Message Follow의 Unix deadline 검사 |
| `spots/spot-closing.ts:16` | timestamp | 유지 | onClosing context에 보고하는 Date deadline |
| `spots/spot-closing.ts:17` | duration | 변경 | 숫자 cleanup timeout은 timer에 직접 전달; Date 입력만 경계에서 변환 |
| `spots/spot-manager-public.ts:137` | timestamp | 유지 | 원격 Spot close record의 Unix deadline |
| `spots/spot-manager-public.ts:237,238` | duration | 변경 | creation 제출 전 남은 timeout |
| `spots/spot-node-runtime-manager.ts:1275,1308` | duration | 변경 | publish slot 대기 한도 |
| `spots/spot-routed-frame-dispatch.ts:151,183` | duration | 변경 | route drain 재시도 한도 |
| `spots/spot-serial-turn-executor.ts:31,137,181,196,207,259` | duration | 변경 | idle 판정의 마지막 활동 시점 |
| `spots/spot-timer.ts:260,444` | duration | 변경 | timer 주기/overrun 경과 시간; 별도 Unix 값은 보고·relocation 전용 |
| `streams/session-actor-coordinator.ts:583` | timestamp | 유지 | binding token 식별 문자열의 시각 성분; 기간 계산이 아님 |
| `streams/stream-session-runtime.ts:90` | duration | 보류 | STREAM 병행 작업과 겹칠 수 있어 파일 수정 제외; BLOCKERS 참조 |

## Timestamp 경계와 보고값

아래는 현재 source line이다. Unix timestamp를 `performance.now()` 값으로 전송하지 않는다.
외부 timestamp에서 로컬 대기를 시작하는 경계는 한 번 남은 시간을 계산하고, 이후 대기는
monotonic deadline 또는 기존 `setTimeout`으로 진행한다. 수신 timestamp의 유효성 검증은
epoch 비교를 유지한다. 서로 다른 프로세스의 monotonic 원점은 직접 비교할 수 없다.

| 현재 file:line (`src/runtime/` 기준) | 유지 이유 |
|---|---|
| `host/spot-address-transport.ts:948,971` | wire deadline 생성에 필요한 시작 Unix 시각. 로컬 deadline은 monotonic이고 getter `deadlineUnixMs`만 epoch를 반환한다. |
| `spots/spot-timer.ts:261,301,304,407,408` | 시작·예약 시각과 relocation cursor는 timestamp다. 실제 tick 시작은 `new Date()`로 보고한다. |
| `spots/spot-timer.ts:331` | 복원된 Unix 시작 시각을 로컬 elapsed 기준으로 변환하는 입구. 이후 주기·overrun은 `performance.now()`만 사용한다. |
| `host/actor-transfer-runtime.ts:1361` | wire에서 전달한 authority commit deadline을 입구에서 로컬 monotonic deadline으로 변환한다. |
| `host/instance-activation-authority.ts:478,506` | 외부 activation deadline을 각 join 대기 진입 시 변환하고 polling은 monotonic으로 계산한다. |
| `spots/spot-closing.ts:18`, `host/route-mesh-runtime.ts:553` | 외부 Date deadline을 기존 timer delay로 변환한다. 숫자 timeout에는 wall clock 차감을 하지 않는다. |
| `actors/actor-client.ts:216`, `host/actor-placement-coordinator.ts:80`, `host/user-spot-creation-coordinator.ts:104` | wire/store Unix deadline은 보존한다. 각각 로컬 submit 전 남은 timeout은 monotonic으로 계산한다. |
| `diagnostics/message-flow.ts:343`, `diagnostics/index.ts:167,177,321,329,337,345,367,410` | flow·runtime·Location event의 발생 timestamp. 로그를 프로세스 간에 연관하기 위한 보고값이다. |
| `diagnostics/topology-runtime-projections.ts:85,122,210,247` | topology를 관측한 시각을 보고한다. |
| `locations/runtime.ts:503,552` | lease 남은 TTL은 store가 반환한 `leaseExpiresAt - storeNow`로 계산하며, descriptor `updatedAt`은 게시 시각이다. |
| `locations/location-store-repository.ts:190,1380,1533,1580,3387,3400,3435,3643,3769,3901` | store clock, terminal retention, lease expiry, descriptor serialization은 store의 epoch 시각을 따른다. 로컬 aggregate retry는 monotonic이다. |
| `locations/in-memory-location-store.ts:93,834,841,877,882`, `locations/in-memory-provider-location-store.ts:33,77,141`, `locations/in-memory-authority-store.ts:97,477,488` | in-memory store가 반환하는 storeNow와 lease/terminal expiry의 공통 wall-clock 영역이다. 외부 store 계약을 모사하므로 epoch를 유지한다. |

## 검증 결과

실행 디렉터리는 `framework/languages/node`, 환경은 `TMPDIR=/dev/shm/zlink-tmp-node`,
`ZLINK_LIBRARY_PATH` 해제다. build·typecheck·테스트는 모두
`flock -w7200 /tmp/zlink-node-gate.lock` 안에서 실행했다. 원본 log는 `/tmp/zlink-node-d095/`에 보존한다.

| 검증 | 결과 | 원본 log |
|---|---|---|
| build / `npm run typecheck` | 최종 source 통과 | `final-build.log`, `final-typecheck.log` |
| readiness focused | 5/5 통과; 전진·후퇴 각각 약 5001 ms | `readiness-focused.log` |
| `client-server-location-runtime.test.js` ×3 | 각각 38/38 통과. 기본 cap/전진/후퇴 TAP duration 5000.994–5002.276 ms; 실제 wait assertion은 `5000 ≤ elapsed < 8000` | `client-server-{1,2,3}.log` |
| 관련 contract 20개 파일 | 504/504 통과 | `focused-results.log`, 각 파일명 `.log`; Actor client/Spot manager는 별도 focused log |
| `sample-regression.test.js` (전체 gate 안에서 실행) | 52/52 통과, 약 229.5초. DeliveryDispatch의 기존 Core 조사 해결을 뜻하지 않는다. | `sample-regression.log` |
| `npm test` 1회 | exit 1. build/typecheck/lint 통과. 실패 파일은 STREAM runtime과 topology projection. 전체 runner의 announced/completed는 1596/1596이나 STREAM 파일은 watchdog 600초 뒤 TAP plan/summary 누락. 세부 사항은 BLOCKERS 참조. | `npm-test.log` |
| M6C relocation / authority runtime contract | 24/24, 48/48 통과 | `m6c-build.log`, `m6c-host-relocation.log`, `m6c-authority-runtime.log` |
| 최종 topology projection + drain contract | 52/52 통과; 최초 shutdown Date 보존과 최종 cleanup 한도 포함 | `final-topology-drain.log` |
| 최종 host 변경 파일 lint | 통과 | `final-host-lint.log` |

회귀 테스트 변경 파일은 `test/contract/client-server-location-runtime.test.js`,
`actor-client.test.js`, `spot-manager.test.js`, `topology-runtime-projection.test.js`,
`test/m6c/m6c-host-relocation.contract.ts`다. readiness 측정은 monotonic으로 바꾸고
wall-clock ±10,000 ms 주입을 추가했다. Actor client는 resolver 중 시계 점프 후에도
남은 timeout이 양의 정수이며 원래 한도를 넘지 않는지 검증한다. Timer는 전진·후퇴에도
schedule index와 elapsed가 유지되고 capture/restore의 cursor가 Unix timestamp인지 검증한다.
기존 fake timer 도우미도 timer와 같은 monotonic source를 제어한다. M6C와 topology 테스트가
내부 owner에 전달하는 deadline, elapsed 측정과 cleanup clock도 그 owner의 monotonic 기준을
따른다. Host는 처음 만든 보고용 Date 객체를 그대로 보존하며 coordinator에 별도의 로컬
remaining duration을 전달한다. Public shutdown 계약과 기존 timer 수는 유지한다.

## BLOCKERS

- `runtime/streams/stream-session-runtime.ts:90`의 liveness 기간 계산은 wall clock이다.
  사용자가 피하도록 지정한 STREAM 병행 작업 범위이므로 수정하지 않았다.
- `runtime/foundation/service-stateful-runtime.ts:1147,1632,4035,4081,4094,4762,4805,4826,5218`의
  Message Follow 보관 기간, STREAM bind를 포함한 durable operation deadline와 replay retention도
  duration이다. STREAM 공용 구현과 겹칠 수 있어 파일 소유 범위를 질문했고 현재 수정하지 않았다.
  이 지점들은 timestamp 유지 예외가 아니며 D-095의 후속 변경이 필요하다.
- 따라서 `src` 전체의 duration 시간원 통일이 완료됐다고 판정하지 않는다.
- 전체 gate의 `test/contract/stream-runtime.test.js:1187`은 내부
  `retryRemoteSessionBindingSend`에 `Date.now() - 1`을 주입한다. 해당 owner는
  `host/actor-packet-relay.ts:556,607`에서 monotonic deadline을 받으므로, epoch 값은
  이미 만료된 deadline이 아니다. `:1193`의 error sink 1건 assertion이 0건으로 실패하고
  retry timer가 남아 파일이 600초 watchdog까지 종료되지 않았다. 수정 제안은 `:1187`의
  입력을 `performance.now() - 1`로 바꾸는 것이다. Assertion과 runtime budget은 그대로다.
  STREAM 병행 작업의 테스트 파일도 수정하지 않았으므로 이 fixture 수정과 재검증은 남아 있다.
- 전체 gate에서 함께 실패한 topology projection의 시간원 비교·cleanup mock과 shutdown
  Date 보존은 수정했고 최종 파일+drain 재검증 52/52가 통과했다. 전체 `npm test`는 요청대로
  한 번만 실행했으므로 최종 전체 gate 통과를 주장하지 않는다.
- `sample-regression.test.js`는 별도 52/52 통과다. 남은 STREAM 테스트 fixture 실패를
  DeliveryDispatch/Core 원인으로 분류하지 않는다.

변경 파일은 위 표에서 `변경`으로 표시한 runtime 파일, 회귀 테스트와 이 요약 문서다.
Core, binding, 보호 문서, 다른 언어와 기존 사용자 변경은 수정하지 않았고 commit하지 않았다.
