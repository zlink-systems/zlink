# Stage 1 Node gate integrity and SupportChat timing

## 결과

Node runtime gate는 더 이상 `--test-force-exit`로 test process를 성공 종료하지 않는다.
각 test file은 기존 600,000 ms 예산을 그대로 사용하는 parent watchdog 아래에서 실행하며,
top-level TAP의 발표 수, 완료 수, plan과 `# tests` 요약이 모두 일치해야 통과한다.

최종 `npm test`는 144개 test file에서 **1,560 tests = 1,559 pass + 1 fail**을
끝까지 완료했다. TAP 무결성 집계는 `announced=1560 completed=1560`이었다. 따라서
이전 1,536 pass 결과에서 잘렸던 `contract-surface.test.js`의 12개와
`sample-regression.test.js`의 12개가 모두 실행됐으며, false-green은 제거됐다.

## Gate diff

| 항목 | 이전 | 변경 후 |
|---|---|---|
| test process 종료 | `--test-force-exit` | 정상 handle 종료를 기다림 |
| 정지 감지 | test별 `--test-timeout=600000`만 사용 | 같은 600,000 ms parent watchdog도 적용하고 만료 시 file 실패 |
| file 무결성 | child exit code만 확인 | `# Subtest`, top-level `ok`/`not ok`, `1..N`, `# tests N`을 교차 검증 |
| 전체 무결성 | 없음 | 현재 실행한 file에서 도출한 발표 수 합계와 완료 수 합계가 다르면 실패 |
| 여러 실패 | 첫 실패에서 gate 종료 | 모든 test file을 실행한 뒤 실패 file을 함께 보고 |

고정된 총 test 수를 runner에 넣지 않았다. 각 file이 실제로 발표한 top-level test 수를
합산해 해당 실행의 expected total을 만들며, 최종 실행에서는 그 값이 1,560이었다.

## 실제 test 수

| 범위 | 이전 완료 | 최종 완료 | 차이 | 최종 결과 |
|---|---:|---:|---:|---|
| `contract-surface.test.js` | 28 | 40 | +12 | 40 pass |
| `sample-regression.test.js` | 39 | 51 | +12 | 50 pass, 1 fail |
| 나머지 142개 file | 1,469 | 1,469 | 0 | 1,469 pass |
| 합계 | 1,536 | 1,560 | +24 | 1,559 pass, 1 fail |

## Failure table

| Test | 분류 | 증거 | 조치 |
|---|---|---|---|
| `sample-regression.test.js` 48번, `node run_samples.sh executes every sample self-check` | 요청 범위 밖의 간헐적인 DeliveryDispatch runtime/sample 실패 | `courier-b`는 session에서 bind되고 courier-node-1에 relay됐지만, dispatch의 다음 actor call은 `Actor route 'courier-b' was not found.`로 끝나 `candidates-exhausted`가 됐다. Browser는 뒤이어 `DeliveryStatusNotify arrived out of the expected sequence`와 `Operation canceled`를 보고했다. | timeout, retry, fixture 또는 assertion을 바꾸지 않았다. Node runtime 파일은 동시 진행 중인 별도 job의 소유 범위라 이 작업에서 수정하지 않았다. |

공개 API 재현은 Node 디렉터리에서 `bash samples/run_samples.sh`를 실행해 두 courier actor의
bind 완료 뒤 두 번째 delivery를 배정하는 흐름이다. 실패 실행에서는 공개 actor request가 이미
bind-relay된 `courier-b`를 `NotFound`로 반환했다. 현재 증거만으로 Core/binding 결함이라고 판정하지
않았고, 이 작업은 Core 또는 binding을 수정하지 않았다.

## SupportChat timing 비교

`framework/languages/shared_sample/**`에는 현재 SupportChat용 `SampleTimings` 파일이 없으며,
공통 SupportChat README는 숫자를 고정하지 않고 typed message로 idle 기준을 갱신하고 별도 bounded
wait로 idle/grace를 확인하도록 규정한다. 검토 판정의 공통값은 .NET/Java/Kotlin이 공유하는
3초/2초/10초이며, C++의 10초/2초/20초는 별도 확장값이다.

| 구현/근거 | Request | Idle | Close grace | Notification wait | 판정 |
|---|---:|---:|---:|---:|---|
| 공통 SupportChat README §7.3 | 숫자 미고정 | 숫자 미고정 | 숫자 미고정 | bounded wait | budget 연장 대신 typed keepalive와 waiter 선등록 요구 |
| .NET | 5 s | 3 s | 2 s | idle + grace + request = 10 s | 공통값 |
| Java | 5 s | 3 s | 2 s | idle + grace + request = 10 s | 공통값 |
| Kotlin | 5 s | 3 s | 2 s | idle + grace + request = 10 s | 공통값 |
| C++ | 해당 client request 설정 | 10 s | 2 s | 20 s | 별도 확장값 |
| Node 변경 전 | 5 s | 10 s | 2 s | 20 s | 검토 판정 C의 timing inflation |
| Node 최종 | 5 s | 3 s | 2 s | 10 s | 공통값 복원 |

Node 근거는 `samples/SupportChat.Ts/Server/Configuration/sample-names.ts:12-15`와
`samples/SupportChat.Ts/Client/main.ts:26-28`이다. 비교 근거는 .NET
`samples/SupportChat/Server/Configuration/SampleNames.cs:33-36`, Java
`samples/java/SupportChat/Server/Configuration/.../SampleTimings.java:6-9`, Kotlin
`samples/kotlin/SupportChat/Server/Configuration/.../SampleTimings.kt:6-9`, C++
`samples/SupportChat/Server/Support/Domain/SupportChat/conversation.hpp:149-153` 및
`samples/SupportChat/Client/supportchat_client_scenario.hpp:39`다.

## 검증

모든 `npm` 실행은 `TMPDIR=/dev/shm/zlink-tmp-node`, `ZLINK_LIBRARY_PATH` 해제,
`flock -w7200 /tmp/zlink-node-gate.lock` 조건으로 실행했다.

| 실행 | 결과 |
|---|---|
| `node --test test/contract/node-test-gate.test.js` | 2/2 pass |
| 강제 종료 없는 `contract-surface.test.js` | 40/40 pass |
| 강제 종료 없는 `sample-regression.test.js` focused 실행 | 51/51 pass |
| `bash samples/run_samples.sh SupportChat.Ts` 1회차 | pass |
| `bash samples/run_samples.sh SupportChat.Ts` 2회차 | pass |
| `bash samples/run_samples.sh TicTacToe.Ts Bingo.Ts DeliveryDispatch.Ts SupportChat.Ts` | 4/4 pass |
| 최종 `npm test` | exit 1; 1,560 complete, 1,559 pass, 1 DeliveryDispatch failure |

## BLOCKERS

- 표준 `npm test`는 false-green 없이 정확히 실패한다. 남은 blocker는 위 DeliveryDispatch actor-route
  실패 한 건이다. 요청 범위와 동시 Node runtime 작업의 소유권 때문에 이 작업에서는 원인을 수정하지
  않았다.
- literal `framework/languages/shared_sample/**` 아래에는 SupportChat timing source가 없다. 수치 비교는
  공통 README의 행동 계약과 .NET/Java/Kotlin의 일치값을 기준으로 했다.
