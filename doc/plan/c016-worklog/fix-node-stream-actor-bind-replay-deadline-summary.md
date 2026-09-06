# Node STREAM actor bind replay deadline 검증 결과

감독이 간헐 실패의 원인과 수정 범위를 판정하기 위한 기록이다. Runtime은 원래 monotonic
deadline에서 각 attempt의 남은 시간을 차감한다. 실패 원인은 테스트가 별도 wall-clock
시각으로 제출 budget을 검증하는 데 있다. 수정 범위는 Node contract test와 이 보고서다.
단독 재현은 20회 중 12회, CPU 부하 재현은 10회 중 3회 실패했다. 수정 후에는 단독과
CPU 부하에서 각각 20/20회 통과했다. 전체 `npm test`도 exit 0이며 1,613 tests 모두 통과했다.

## 원인과 소유권

- 소유 계층: Framework `ServiceStatefulRuntime`의 durable sender가 operation deadline·replay identity·admission 이력을 소유한다. 테스트가 이 deadline을 측정하는 방법만 수정했다.
- Spec 조항: `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:668–679` §5의 같은 `OperationId`, attempt마다 남은 deadline 전부 사용, admission 여부에 따른 `Unavailable`/`DeadlineExceeded`. [D-093](decisions.ko.md)의 sender 단일 소유권과 [D-095](decisions.ko.md)의 monotonic duration 규칙을 함께 적용했다.
- 교차언어 대조: Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaDurableRequest.java:39,77–82`는 `System.nanoTime()` deadline에서 남은 시간을 계산해 제출한다. .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkDurableRequest.cs:23–28`은 `Stopwatch.GetElapsedTime(startTimestamp)`를 차감해 제출한다. Node도 같은 규칙이다. 이번 차이는 Node 테스트의 wall-clock 관측이며 다른 언어 runtime 변경이 필요한 증거는 없다.
- 변경 분류: **B — 기존 테스트 결함**. 사용자가 요청한 계약에 맞는 assertion 수정 범위다. Framework runtime 변경이나 spec 완화는 없다.

수정 전/후 규칙 수: deadline 검증의 시간원 2개(runtime monotonic + test wall clock) →
1개(monotonic); deadline budget assertion 위치 1 → 1; runtime deadline 소유자 1 → 1.
추가 runtime 상태·timer·retry 정책은 없다.

원인 위치는 수정 전 `framework/languages/node/test/contract/stream-actor-bind-replay.test.js:35,76,95`다.
Fixture는 service wrapper에서 `Date.now() + timeoutMs`를 만들고, transport stub에 진입한 뒤
별도로 `Date.now()`를 읽어 `abs(at + timeoutMs - deadline) <= 1`을 요구했다.
이 시각들은 runtime의 deadline 생성·remaining 계산 시각과 다르고 시간원도 다르다.
Wall clock의 정수 밀리초 양자화, 시간원 사이의 경과 차이, wrapper 진입부터 deadline 생성까지의
동기 실행과 remaining 계산 뒤 관측까지의 지연이 assertion에 함께 포함된다. 이번 표본만으로
wall-clock jump가 실제 발생했다고 단정하지 않는다.

Runtime 경로의 prefix는 `framework/languages/node/packages/framework/src/runtime/`다.
`foundation/service-stateful-runtime.ts:1632`에서 `performance.now() + timeoutMs`를 한 번 만들고,
`:1730–1731`에서 같은 값을 durable sender에 전달한다. `:4084`는 매 attempt마다
`Math.ceil(deadlineMs - performance.now())`를 계산하며 `:4089–4090`의 제출 전에는
`await`가 없다. `:4097–4101`의 retry 대기는 실패 뒤에 있고 다음 attempt에서 다시 remaining을
계산한다. Deadline 재설정이나 attempt별 예산 분할은 없다.

`foundation/raw-service-mesh-runtime.ts:542–547`은 timeout을 그대로 전달한다.
`backend/node/node-raw-binding-port.ts:285–294`도 `.timeout(timeoutMs).submit()`을 동기적으로
호출한 다음 결과를 await한다. 이 구간에 추가 `ceil`이나 deadline 재계산은 없다.
정수 밀리초 변환은 D-095의 [기존 변경 기록](fix-node-monotonic-durations-3-summary.md)에
명시된 sender 제출 경계에서 한 번 적용된다.

남은 기간을 계산한 monotonic 시각을 `s`, deadline을 `D`라 하면 D-095의 올림 변환을 한 정수 timeout `T`의 범위는
`0 <= s + T - D < 1 ms`다. 나중에 측정한 transport 진입 시각 `a`에는 `a - s`도 포함되므로
실제 clock으로 읽은 `a + T - D < 1 ms`를 무조건 보장할 수 없다. 측정된 monotonic drift는
전체 439 attempts에서 0.030553–1.038692 ms였다. 따라서 단순히 `Date.now()`를
`performance.now()`로 바꾸고 1 ms 비교를 유지해도 관측 지연 문제는 남는다.

## 수정과 검증 범위

수정 파일은 `framework/languages/node/test/contract/stream-actor-bind-replay.test.js`다.
실제 timer를 사용하는 기존 테스트는 오류 종류, admission 이력, ingress 부재, header·correlation·binding
identity, 완료 1회와 terminal 수신 후 replay 중단을 검증한다. 정밀 deadline assertion은 같은
fixture를 사용하는 제어된 monotonic clock 테스트로 옮겼다. 비교 허용치를 늘리지 않았다.

새 테스트는 `performance.now()`와 `setTimeout`을 Node test mock으로 제어하고, 첫 attempt 뒤
wall clock을 각각 +60,000 ms와 −60,000 ms 이동한다. `bind(80)`의 deadline은 1080.25 ms다.
테스트가 시각을 전진시킨 뒤 pending microtask를 처리하므로 CPU scheduling 지연이
계약의 시간 값에 섞이지 않는다. 각 attempt의 timeout은 양의 정수이고
`0 <= at + timeoutMs - deadline < 1`인지 검사한다. Failure message에 `at`, `timeoutMs`,
`deadline`, `drift`를 포함한다.

| Monotonic at (ms) | 남은 기간 (ms) | 제출 timeout (ms) | Drift (ms) |
|---:|---:|---:|---:|
| 1000.25 | 80 | 80 | 0 |
| 1020.50 | 59.75 | 60 | 0.25 |
| 1040.75 | 39.50 | 40 | 0.50 |
| 1061.00 | 19.25 | 20 | 0.75 |

1080.25 ms에는 추가 attempt 없이 `Unavailable`로 종결한다. Deadline을 매번 새로 만들거나,
예산을 분할하거나, 소수 밀리초를 내림하면 고정 budget 배열 또는 drift 검증이 실패한다.

검토한 대안은 실제 clock의 허용 오차를 늘리는 방법과 계약 시간을 제어하는 방법이다.
전자는 scheduling 지연의 상한을 정할 근거가 없고 짧은 budget 오류를 감출 수 있다.
후자는 기존 실제 timer 시나리오를 유지하면서 별도 시간 허용치를 없애므로 채택했다.

## 재현 drift

실행 위치는 `framework/languages/node`, 환경은 `TMPDIR=/dev/shm/zlink-tmp-node`,
`ZLINK_LIBRARY_PATH` 해제다. 모든 반복 실행은 `flock -w7200 /tmp/zlink-node-gate.lock`에서
실행했다. CPU 부하는 별도 Node process의 `for (;;) { Math.sqrt(Math.random()); }` 한 개이며
각 부하 batch가 끝나면 종료했다. CPU affinity는 고정하지 않았다.

첫 재현부터 TAP과 모든 attempt 값을 `/tmp/zlink-node-bind-replay-deadline/`에 보존했다.
해당 fixture는 raw transport stub과 completion diagnostics를 사용하며 application dispatch·OTel
flow provider를 구성하지 않는다. Stub의 deadline 수치는 message-flow에 포함되지 않으므로
기존 `attempts` 수집을 process preload로 출력하고 실제 sender deadline을 함께 관측했다.
Runtime source·dist에 임시 로깅을 추가하지 않았다. 계측 preload는 수정 전 재현에만 사용했다.
원본은 `before-idle-01.log`…`20.log`, `before-load-01.log`…`10.log`, 위반값 모음은
`drifts.json`이다. 통과 로그와 실패 로그 모두 보존했다.

아래 표는 실패한 15회에서 원래 assertion을 위반한 모든 attempt다. 모두 원래 첫 테스트인
“route absent for the whole deadline”에서 실패했다. `at`와 `deadline`은 원래 fixture의 Unix ms,
`drift = at + timeoutMs - deadline`이다. 마지막 열은 별도 측정한 monotonic transport 진입
시각과 실제 runtime deadline으로 계산한 값이며, remaining 계산 후 관측까지의 지연을 포함한다.

| 실행 | Attempt | at | timeoutMs | deadline | Drift (ms) | Monotonic drift (ms) |
|---|---:|---:|---:|---:|---:|---:|
| idle-02 | 3 | 1788653048979 | 38 | 1788653049019 | -2 | 0.073225 |
| idle-03 | 1 | 1788653049459 | 80 | 1788653049536 | +3 | 0.455704 |
| idle-03 | 2 | 1788653049480 | 59 | 1788653049536 | +3 | 0.284869 |
| idle-03 | 3 | 1788653049499 | 40 | 1788653049536 | +3 | 0.678070 |
| idle-03 | 4 | 1788653049519 | 20 | 1788653049536 | +3 | 0.980459 |
| idle-03 | 5 | 1788653049538 | 1 | 1788653049536 | +3 | 0.235889 |
| idle-06 | 2 | 1788653051034 | 58 | 1788653051090 | +2 | 0.768592 |
| idle-06 | 4 | 1788653051075 | 17 | 1788653051090 | +2 | 0.676995 |
| idle-08 | 3 | 1788653052091 | 39 | 1788653052128 | +2 | 0.912858 |
| idle-09 | 1 | 1788653052566 | 80 | 1788653052644 | +2 | 0.460757 |
| idle-09 | 2 | 1788653052586 | 60 | 1788653052644 | +2 | 0.546208 |
| idle-09 | 3 | 1788653052607 | 39 | 1788653052644 | +2 | 0.073692 |
| idle-09 | 4 | 1788653052626 | 20 | 1788653052644 | +2 | 0.471511 |
| idle-09 | 5 | 1788653052645 | 1 | 1788653052644 | +2 | 0.854195 |
| idle-10 | 2 | 1788653053112 | 59 | 1788653053169 | +2 | 0.483481 |
| idle-10 | 3 | 1788653053132 | 39 | 1788653053169 | +2 | 1.013786 |
| idle-10 | 5 | 1788653053170 | 1 | 1788653053169 | +2 | 0.710268 |
| idle-11 | 1 | 1788653053610 | 80 | 1788653053687 | +3 | 0.461638 |
| idle-11 | 2 | 1788653053631 | 59 | 1788653053687 | +3 | 0.485426 |
| idle-11 | 3 | 1788653053651 | 39 | 1788653053687 | +3 | 0.990715 |
| idle-11 | 4 | 1788653053672 | 18 | 1788653053687 | +3 | 0.248292 |
| idle-11 | 5 | 1788653053689 | 1 | 1788653053687 | +3 | 0.462735 |
| idle-13 | 2 | 1788653054672 | 59 | 1788653054729 | +2 | 0.839494 |
| idle-14 | 1 | 1788653055169 | 80 | 1788653055247 | +2 | 0.653406 |
| idle-14 | 2 | 1788653055190 | 59 | 1788653055247 | +2 | 0.674897 |
| idle-14 | 3 | 1788653055211 | 38 | 1788653055247 | +2 | 0.175764 |
| idle-14 | 4 | 1788653055231 | 18 | 1788653055247 | +2 | 0.509590 |
| idle-14 | 5 | 1788653055249 | 1 | 1788653055247 | +3 | 0.923298 |
| idle-17 | 3 | 1788653056771 | 40 | 1788653056809 | +2 | 0.939554 |
| idle-18 | 3 | 1788653057290 | 39 | 1788653057327 | +2 | 0.787198 |
| idle-19 | 4 | 1788653057825 | 19 | 1788653057842 | +2 | 0.858088 |
| load-05 | 1 | 1788653066316 | 80 | 1788653066394 | +2 | 0.518065 |
| load-05 | 3 | 1788653066357 | 39 | 1788653066394 | +2 | 0.634212 |
| load-05 | 5 | 1788653066395 | 1 | 1788653066394 | +2 | 0.541847 |
| load-08 | 1 | 1788653067879 | 80 | 1788653067956 | +3 | 0.460756 |
| load-08 | 2 | 1788653067900 | 59 | 1788653067956 | +3 | 0.664049 |
| load-08 | 3 | 1788653067921 | 38 | 1788653067956 | +3 | 0.073355 |
| load-08 | 4 | 1788653067941 | 18 | 1788653067956 | +3 | 0.329524 |
| load-08 | 5 | 1788653067958 | 1 | 1788653067956 | +3 | 0.634171 |
| load-09 | 1 | 1788653068398 | 80 | 1788653068476 | +2 | 0.511388 |
| load-09 | 2 | 1788653068418 | 60 | 1788653068476 | +2 | 0.970340 |
| load-09 | 4 | 1788653068459 | 19 | 1788653068476 | +2 | 0.674584 |
| load-09 | 5 | 1788653068477 | 1 | 1788653068476 | +2 | 0.971740 |

## 실행 결과

Installed Core의 실제 파일
`framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/libzlink.so.0.17.0`의
SHA-256은 `083588b48faaf5e1e640961802cfd94847396ea52770b10c2b94216258b79dce`이며 package
provenance와 일치한다. Core·binding package 재빌드나 교체는 하지 않았다.

| 검증 | 결과 | 로그 |
|---|---|---|
| 수정 전 파일 ×20, 부하 없음 | 8 pass / 12 fail | `before-idle-*.log` |
| 수정 전 파일 ×10, CPU busy-loop | 7 pass / 3 fail | `before-load-*.log` |
| 수정 후 파일 ×20, 부하 없음 | 20/20회, 각 7/7 tests; 총 140 pass / 0 fail | `after-idle-*.log` |
| 수정 후 파일 ×20, CPU busy-loop | 20/20회, 각 7/7 tests; 총 140 pass / 0 fail | `after-load-*.log` |
| 전체 `npm test` 1회 | exit 0; build·typecheck·lint 통과; 151개 파일, 1,613 pass / 0 fail; announced=completed=1613; skip·cancel 0 | `npm-test.log` |
| 전체 gate 안의 해당 파일 / sample 회귀 | 각각 7/7, 52/52; fail 0 | `npm-test.log` |
| `git diff --check` | 통과 | 출력 없음 |

전체 gate는 Node gate 잠금에 `/tmp/zlink-samples-gate.lock`도 함께 적용했다.
보호 경로, 다른 언어와 기존 사용자 변경은 수정하지 않았고 commit하지 않았다.

## BLOCKERS

없음. 요청한 반복 검증과 전체 gate가 모두 통과했다.

테스트·보고서의 코드 부합과 한국어 문서 원칙을 독립 검토했으며 finding은 없었다.
최종 gate 결과는 process exit code와 원본 TAP의 pass·fail·skip·cancel·aggregate를 별도로 대조했다.
