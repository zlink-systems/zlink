# #1675 Node 메시지당 비용 — R-a / P-a

## 결과와 커밋

- 작업 브랜치: `framework-node/1675-per-message-cost-2`.
- `git fetch origin && git merge origin/main`: 충돌 없음. 병합 `e344580dae`, origin/main `325ff25d09`.
- R-a: `52980c72ce` — request admission 상태를 `RequestAdmission`과 prototype 메서드로 옮겼다.
- P-a: `3895827e6c` — application worker가 drain loop를 직접 실행한다.
- push·PR 없음. spec·공개 API 변경 없음. 시간 측정 없음.

## R-a

변경 파일: `framework/languages/node/packages/framework/src/runtime/messaging/request-result.ts`.

`confirm`·`fail`·`observe`는 상태 객체의 prototype 메서드다. 요청마다 만드는 callback은 5 → 4개(start, then 성공·실패, admitted getter)다. admitted를 먼저 읽을 때 만드는 Promise executor는 전후 같다. 승인된 R-a와 같이 일반 reply Promise에 getter를 붙이는 `Object.defineProperty`와 descriptor는 1 → 1개로 남는다. Promise subclass·prototype 교체·lazy reply 관찰은 적용하지 않았다.

- `start` 호출, then 등록, delivery wrapper 호출 순서와 admission 정착 순서가 같다.
- reply의 `constructor === Promise`, prototype, wrapper 반환 identity, admitted identity를 실행 확인했다(`logs/ra-public-behavior.log`).
- 관련 기존 시험 22/22 통과(`logs/ra-test.log`). 단언 변경 없음.
- heap sampler: 기존 `/root/jobs/fx/cx/m1675d/work/request-result-alloc.cjs`, sampling interval 32B, warmup 2,000, sample 20,000회. 시간 측정 없음.

| heap/요청 | 전 | 후 |
|---|---:|---:|
| 동일 await 구조 floor | 346.8 B | 347.2 B |
| createZLinkRequest 전체 | 861.9 B | 769.1 B |
| floor 차감 추가 heap | 515.1 B | 421.9 B |

추가 heap 감소는 약 93.2B/요청이다. sampling 값이므로 정확한 V8 object 크기와 같다고 주장하지 않는다. 원자료는 `m1675e/logs/ra-{before,after}-heap.log`다.

## P-a

변경 파일: `framework/languages/node/packages/framework/src/runtime/backend/mesh-dispatch-pump.ts`.

application worker가 자기 loop에서 ready batch를 얻고 `drainClaim`을 await한다. `drainDomain`은 infrastructure만 처리하며 optional receive batch·claim budget과 domain 선택 분기를 없앴다. `drainClaim`, `dispatchRecord`, `finishRecord`, `failRecord`는 두 실행 영역이 기존 소유자를 함께 사용한다.

`waitForApplicationReady`의 await는 유지했다. 새 worker가 첫 handler를 수신 stack에서 실행하지 않는다. `beginDrain`과 `yieldIfNeeded`가 기존 공유 예산·pendingYield를 계속 소유한다. handler 완료 → terminal completion → permit 반환 순서와 실패 보고 경로는 바꾸지 않았다. 종료 검사는 drain 정리 뒤 outer loop에서 한다. 종료 중 깨운 worker는 inner disposed 검사로 record를 시작하지 않고 batch를 정리·close한다.

비동기 handler 한 record의 실제 pump Promise는 3 → 2개다(idle worker 대기, drainClaim). worker lifetime Promise는 요청별 할당이 아니다. application 경로의 `drainDomain` async Promise 하나와 그 await 경계를 없앴다.

계측 runner는 `/root/jobs/fx/cx/m1675d/coefficients.cjs` 그대로 사용했다. 두 public NestJS host, 1024B, pattern마다 128회, 횟수만 수집했다. async_hooks의 PROMISE에는 await용 hook 객체가 포함된다.

| pattern | pump PROMISE hook/메시지 전 → 후 | 전체 PROMISE hook 전 → 후 | Buffer 개수/bytes 전 = 후 |
|---|---:|---:|---:|
| send | 1.852 → 1.555 | 6.523 → 6.117 | 5 / 2379 |
| request-serial | 7.219 → 5.602 | 28.813 → 27.336 | 12 / 5298 |
| request-backpressure | 2.086 → 2.156 | 18.945 → 19.156 | 12 / 5298 |

serial pump의 공통 yield then은 0.070 → 0.188회/메시지였다. 이 yield의 hook 3개를 빼면 pump hook은 7.009 → 5.038이다. macrotask 계수는 실행 부하에 따라 변하므로 raw hook 총량만으로 실제 Promise 수를 판정하지 않았다. backpressure에서는 여러 record에 worker turn이 분산된다. 구조상 실제 Promise 3 → 2와 serial hook 약 2개 감소를 함께 확인했다. 원자료: `m1675e/{before,after}-count.json`.

## 소유와 분류

- 소유 계층: Framework request-result(admission 관찰), dispatch application worker와 infrastructure pump(실행 turn). Core·binding의 수락·completion 결정은 바꾸지 않았다.
- spec: `01-execution/01-submit-and-completion.ko.md:63-70`(admitted·reply), `01-execution/08-messaging-hot-path.ko.md:219-225`(W1–W5·공유 worker), `:336-347`(Node await·macrotask 양보).
- 교차언어 대조: .NET `Runtime/Messaging/ZLinkRequestAdmission.cs:15-43`와 Java `runtime/internal/calls/ZLinkRequestStages.java:36-44`는 reply 성공·실패를 admission 상태 객체로 전달한다. .NET `Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:355,414-450`은 공유 worker에서 claim을 처리한다. C++ `runtime/mesh/mesh_record_dispatcher.cpp:29-71`은 handler 후 같은 dispatch에서 reply한다. Java `runtime/channels/ZLinkMeshApplicationDispatcher.java`의 handler completion과 record 정리 경로(:393,:653)를 대조했다. Kotlin은 Java runtime을 공유한다. 이번 변경은 Node의 할당·실행 경계 내부 정리이며 공개 관찰 결과의 차이를 추가하지 않았다.
- 변경 분류: B(기존 비용 결함). 승인 근거: `/root/jobs/fx/cx/review-1675.md` 「2차 리뷰」의 R-a·P-a 승인.

## 검증

| 검사 | 결과 | 로그 |
|---|---|---|
| `npm run build` | 통과(첫 build 및 최종 npm test 내부 build) | `m1675e/logs/ra-build.log`, `final-test.log` |
| `npm run lint` | 통과(packages·samples 전체) | `m1675e/logs/final-lint.log` |
| `scripts/format/format.sh --check node` | 통과 | `m1675e/logs/final-format.log` |
| `npm test` | exit 0, runtime 206파일·2712/2712 완료, 실패 0 | `m1675e/logs/final-test.log` |
| `npm run verify:m6a-runtime` | npm test가 이어서 실행, 44/44 | 같은 로그 끝 절 |

알려진 main 간헐 #1819는 이번 최종 gate에서 발생하지 않았다. 최종 실패가 없어 main-base와 실패 대조는 필요하지 않았다. 확인한 main-base HEAD는 `61893d9ae3`이며 이번 fetch의 origin/main보다 이전이다. 이를 최신 main 검증 결과로 사용하지 않았다.

최종 소스·시험 단언 변경은 두 runtime 파일뿐이다. build가 갱신한 Unity `.jspre`는 원래 HEAD 내용으로 정리했다. 작업 트리는 clean이다. 임시 음성 시험 preload는 삭제했고 보고서·원자료·재현 runner와 로그는 `/root/jobs/fx/cx/m1675e/`에 보관했다.

- focused: admission 22/22, backend-contract + node-dispatch-turn 68/68.
- backend-contract 24번: 단독 20/20, 10개 병렬 프로세스에서 20회씩 총 200/200. 최종 lint 수정 뒤 같은 반복도 단독 20/20·병렬 200/200 통과했다. 최종 node-dispatch-turn은 4/4 통과했다(`logs/final-dispatch-turn.log`). `m1675e/logs/yield-summary.txt`.
- 음성 시험: 모듈 로드 때만 첫 worker await 경계를 제거했다. node-dispatch-turn 첫 시험이 `handler ran inside the receive rotation` 단언으로 실패(exit 1). 소스·단언을 바꾸지 않았다. `m1675e/logs/negative-boundary.log`.
- 첫 전체 gate는 lint에서 중단됐다. worker loop로 옮긴 disposed 검사를 TS가 직전 검사로 narrowing하여 항상 참으로 판정했다. 종료 검사를 drain finally 뒤로 옮겨 같은 정리·종료 동작을 유지했다. `m1675e/logs/first-gate-lint-failure.log`.

## 판정 위치와 리팩토링

수정 전/후 규칙 수: admission confirm·fail·observe 각 1 → 각 1, record dispatch 1 → 1, permit 반환 2 → 2, 공유 yield 예산 판정 1 → 1, idle budget 초기화 1 → 1. application/infrastructure drain 선택 분기 1 → 0. 새 queue·lock·timer·권한 플래그 없음. closure가 소유하던 admission 상태를 한 객체로 옮겼다.

리팩토링 점검: 성능 2건·POSDDD 0건·불필요 코드 1건 발견. R-a closure 할당·P-a async wrapper와 혼합 domain 분기를 고쳤다. 새로 발견하여 넘긴 항목 0건. 이전 보고서 §5-3 공개 payload API 결정과 범위 밖 항목은 이번 작업에서 바꾸지 않았다.

## 결정 필요

이번 승인 범위 안에서 새 설계 결정은 없다. 이전 E-a/E-b/E-c의 공개 API·패키지 결정은 미해결 그대로다.
