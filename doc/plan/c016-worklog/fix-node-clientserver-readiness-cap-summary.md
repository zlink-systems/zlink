# Node ClientServer readiness cap 진단

ClientServer readiness 대기의 소유자는 `client-server-location-runtime.ts`가 아니라
`framework/languages/node/packages/framework/src/runtime/channels/channel-socket-registry.ts:645`의
`awaitClientDealerForOutbound`다. 이 메서드는 진입 시 deadline을 만들고 monitor drain과
target 선택을 동기적으로 수행한다. 선행 resolver 또는 lease 조회 await는 없다.

## 원인과 시간 측정

`channel-socket-registry.ts:649,654`는 경과 시간 제한에 `Date.now()`를 사용한다.
같은 registry의 ClientServer liveness는 `performance.now()`를 사용한다.
wall clock이 앞으로 이동하면 readiness 대기가 일찍 끝나고, 뒤로 이동하면 실제 대기가
늘어난다. 테스트 `client-server-location-runtime.test.js:1801,1803`도 `Date.now()`로
경과 시간을 측정하므로 같은 시계 변화에 영향을 받는다.

| 측정 | Wall-clock 경과 | Monotonic 경과 | 해석 |
|---|---:|---:|---|
| 기존 gate7 cap 테스트 | 8585 ms | TAP 테스트 전체 3575.399 ms | 약 5010 ms 차이 |
| 기존 gate8 cap 테스트 | 8510 ms | TAP 테스트 전체 3472.575 ms | 약 5037 ms 차이 |
| 별도 Node 프로세스 clock sample | +5152 ms | 103.991 ms | 테스트와 무관한 wall-clock 전진 |
| 별도 Node 프로세스 clock sample | −5439 ms | 101.071 ms | 테스트와 무관한 wall-clock 후퇴 |
| 파일 전체 실행, 계측 없음 | assertion 통과 | TAP cap 테스트 전체 2927.919 ms | 36/36 통과도 실제 5초 대기를 보장하지 못함 |
| 파일 전체 실행, 구간 계측 | 9899 ms | wait owner 4857.913 ms | cap 테스트 실패, 동일 원인 재현 |

독립 clock sample 200개의 합은 wall clock 18574 ms, monotonic 20576.206 ms다.
30 ms 이상 차이가 난 구간은 10개다. 원본은
`/tmp/zlink-clientserver-cap/clock-samples.jsonl`에 보존한다.
이는 기존 로그의 8510–8585 ms가 실제 8.5초 대기였다는 해석을 반박한다.
host wall clock을 바꾸는 외부 원인은 이 측정만으로 특정하지 않았다.

대상 테스트는 fake dealer, no-op monitor와 registry를 직접 사용한다. 아직 application
message를 제출하지 않은 target 선택 단계이므로 spec 26의 message-flow transition은
없다. 기존 gate 로그를 먼저 확인한 뒤, 저장소 밖의 임시 preload로 monitor drain,
target 선택, poll 간격과 wall/monotonic 시간을 함께 측정했다. 계측 코드는 조사 후 삭제했다.

| 구간 | 측정값 |
|---|---:|
| Admission이 진행 중인 대기 | wall 43 ms / monotonic 43.002 ms |
| Channel request timeout 120 ms 대기 | wall 123 ms / monotonic 122.672 ms |
| 5초 cap 대기 전체 | wall 9899 ms / monotonic 4857.913 ms |
| Cap 대기의 monitor drain | 922회, 합계 2.690 ms, 최대 0.049 ms |
| Cap 대기의 target 선택 | 합계 2.524 ms, 최대 0.031 ms |
| Cap 대기의 최대 poll 간격 | 5.851 ms |
| 마지막 poll 간격 | wall 5047 ms / monotonic 5.613 ms |

마지막 poll은 시작 후 monotonic 4857.855 ms에 실행되었다. 그 직전 wall clock만 약
5041 ms 전진하여 deadline 검사가 참이 되었다. 대기 내부에 수초짜리 선행 작업이나
event-loop 정체가 없었으며, 테스트가 관측한 초과 시간은 wall-clock 점프였다.
이 결과는 파일 단독 실행에서도 얻었으므로 전체 suite나 이전 테스트의 상태 누수가
이 실패에 필수라는 가설도 성립하지 않는다.

원본 결과는 `/tmp/zlink-clientserver-cap/baseline-file.log`, `segments-file.log`,
`segments.jsonl`에 보존한다. 실행 환경은 `framework/languages/node`,
`TMPDIR=/dev/shm/zlink-tmp-node`, `ZLINK_LIBRARY_PATH` 해제이며 모든 테스트 명령에
`flock -w7200 /tmp/zlink-node-gate.lock`을 적용했다.

## 수정안

기존 deadline을 `performance.now()`로 계산하고 기존 polling timer의 지연을
`min(5 ms, deadline까지 남은 시간)`으로 제한한다. 두 번째 timer나 deadline 상태를
추가하지 않는다. admission이나 reconnect 정책은 이 수정의 대상이 아니다.

별도 cap timer를 경합시키는 대안은 완료·취소 소유자를 추가하므로 선택하지 않는다.
기존 deadline 하나로 전체 readiness 대기를 제한하는 방식을 선택한다.

회귀 검증은 wall clock의 전진과 후퇴에도 같은 readiness 기간을 지키는지 확인한다.
기존 실제 5초 테스트는 monotonic clock으로 측정하고 `elapsed >= 5000` 및
`elapsed < 8000` assertion을 유지한다.

- 소유 계층: Framework ClientServer target selector, `ZLinkChannelSocketRegistry.awaitClientDealerForOutbound`.
- Spec 조항: `02-channel-messaging.ko.md:170–174`의 호출 시점부터 `min(request timeout, 5초)` 대기; `03-client-server-channel` §4의 ready/weight/drain 선택; `05-transport-liveness` §2·§3·§7의 별도 liveness 시간과 종료 책임.
- 교차언어 대조: Java `ZLinkChannelSocketRegistry.java:265–282`는 `System.nanoTime()`과 남은 시간을 사용한다. .NET `ZLinkClientServerClientRuntime.cs:592–605`는 `DateTime.UtcNow`이므로 같은 시계 변화 취약점이 있으며 이번 허용 범위 밖이다. Node만 수정하는 이유는 Node 범위 제한이며 물리 transport 차이가 아니다.
- 변경 분류: B — 기존 경과 시간 계산 결함. 구현은 루트 `AGENTS.md` §3에 따른 감독의 B 승인 대기.

수정 전/후 규칙 수(제안): registry 시간 기준 2개(wall readiness, monotonic liveness) →
1개(monotonic 경과 시간); readiness deadline 소유자 1개 → 1개; polling timer 1개 → 1개.

## 검증 결과

- 별도 clock probe: 완료. 실제 wall-clock 전진과 후퇴 확인.
- 수정 전 `node --test test/contract/client-server-location-runtime.test.js`: 36/36 통과, exit 0.
- 수정 전 같은 파일에 임시 구간 계측 적용: 35/36 통과, cap 테스트만 9899 ms assertion 실패, exit 1.
- `test/contract` 전체 재현은 파일 전체 실행에서 원인이 확인되어 추가하지 않았다.
- 회귀 테스트, 수정 후 파일 3회, `npm test` 1회: 구현 승인 대기로 미실행. 전체 gate와 sample-regression의 최종 통과 여부를 주장하지 않는다.

## BLOCKERS

- 구현 승인: 원인·소유 계층·spec·교차언어 대조와 B 분류를 보고했고 감독의 승인을 기다린다.
- Node gate lock 대기는 해소되었고 재현 실행은 모두 종료했다.
- 다른 언어의 wall-clock deadline은 보고만 하며 변경하지 않는다.

저장소 변경 파일은 이 작업 기록뿐이다. Runtime과 테스트 수정안은 아직 적용하지 않았다.
Core, binding, 보호 문서와 다른 언어 파일을 변경하지 않았고 commit하지 않았다.
