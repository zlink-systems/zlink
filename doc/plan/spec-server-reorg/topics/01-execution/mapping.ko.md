# spec/server 재구성 — 01-execution 매핑표

> 캠페인: `framework/doc/framework/common/spec/server`를 주제별로 재구성하고
> [스펙 문서 작성 가이드](../../../../principal/documentation/spec-writing-guide.ko.md)대로 다시 쓴다.
> 이 문서는 `01-execution` 주제의 작업 계획이다. 양식은
> [04-session 매핑표](../04-session/mapping.ko.md)를 따른다.

관련: [spec-gap 대장](../../spec-gap.ko.md) · [주제 구분 초안](../../topic-map.ko.md) ·
[04-session 매핑표](../04-session/mapping.ko.md)(shared permit 이관 출처)

## 1. 대상과 규모

| 현재 문서 | 줄 수 | 성격 | 외부 anchor 링크 수 |
|---|---|---|---|
| `05-async-execution-policy` | 525 | 계약 — submit terminator, request completion, handler turn, cancellation, Spot timer | 36 |
| `33-core-hwm-application-job-flow` | 181 | 계약 — Core byte HWM과 Framework Application Job Queue의 분리, ordinary ingress permit 순서 | 0 |
| `46-internal-dispatch-loop` | 337 | 구현 스펙 — 수신·dispatch 루프, owner 준비 상태, 배타권, batch, timer scheduler, shared permit | 2 |
| `42-internal-progress-isolation` | 248 | 구현 스펙 — application/infrastructure 실행 분리, backpressure 3단계 | 0 |
| `43-internal-completion` | 172 | 구현 스펙 — operation 완료 확정, dispatcher, OperationId/ReplyRouteId | 0 |
| `41-internal-serialization` | 343 | 구현 스펙 — Spot·Actor queue/gate 분리, lane 정책, 실행권 함정 | 9 |
| `50-internal-message-ownership` | 312 | 구현 스펙 — payload 소유권·복사, 역직렬화 시점, codec 선택 cache | 11 |
| 합계 | 2,118 | | 94 |

외부 참조 파일(anchor 없는 문서-대-문서 링크 포함) 다수 — 언어별 guide·e2e·sample·interface 문서.
**코드에서 이 문서를 경로로 여는 곳 1건**: `scripts/verify-framework-submit-api.sh`가
`framework/doc/framework/common/spec/server/05-async-execution-policy.ko.md`를 읽어 다음 문구
4개를 needle로 검색한다 — `동기 \`TrySubmit\` 계열을 제공하지 않는다`,
`반환 데이터 없이 완료`, `` `DeadlineExceeded` ``, `` `ShuttingDown` ``. 재작성 뒤 경로와 이
문구를 함께 갱신해야 한다(§6).

그 외 `mesh-dispatch-pump.ts`(node)·`m6a-runtime.contract.ts`(node)·`app.cpp`(cpp)·
`test_cpp_framework_application_job_queue.cpp`(cpp)·`ApplicationJobQueueTests.cs`(dotnet)·
`ServiceRuntimeFoundationTests.cs`(dotnet)는 옛 문서 번호(`33-…`, `46-…`, `05-…`)를 **주석에서
근거로만 인용**하고 경로로 열지 않는다 — 이동 시점에 주석 갱신 대상이지 needle 갱신 대상은
아니다.

## 2. 독자 질문 — 주제 README가 답할 것

| 질문 | 답이 있어야 할 자리 |
|---|---|
| Submit·send·request는 각각 언제 완료되는가 | README 개요 + `submit-and-completion` terminator 절 |
| Backpressure에 걸리면 caller는 무엇을 받는가, 언제 기다리고 언제 즉시 실패하는가 | `submit-and-completion` admission 경계 + `application-job-queue-and-backpressure` |
| Handler가 실행되는 동안 같은 Spot의 다른 handler는 왜 끼어들 수 없는가 | `handler-turn-and-execution-gate` 개요 |
| `Yield`와 `Defer()`는 각각 무엇을 반납하고 무엇을 유지하는가 | `handler-turn-and-execution-gate` |
| handler가 원격 응답을 기다리는 동안 timeout·종료·새 연결은 왜 계속 진행하는가 | `handler-turn-and-execution-gate` application/infrastructure 분리 |
| Core byte HWM과 Framework job 수 제한은 왜 다른 값인가, 어디서 만나는가 | `application-job-queue-and-backpressure` 개요 |
| Cancellation은 이미 시작된 작업에 무엇을 하는가, 무엇을 못 하는가 | `cancellation-and-shutdown` |
| Spot timer는 밀리면 어떻게 되는가, 몇 개까지 등록해도 자원이 늘지 않는가 | `spot-timer` |
| message가 socket에서 handler까지 가는 동안 byte를 몇 번 복사하는가 | `payload-ownership-and-codec` |
| 응답·timeout·취소·종료가 동시에 오면 무엇이 이기는가 | `submit-and-completion` 완료 자리 절 |
| 제한은 무엇인가 (`MaxQueuedApplicationJobs`, pause/resume %, lane 상한, dispatcher 4,096, timer 기본값) | 각 문서 수치 절, README 요약표 |

## 3. 새 구조

```
spec/server/01-execution/
  README.ko.md                                주제 진입 1장
  01-submit-and-completion.ko.md              05 §1.1(일부)·§1.2-1.4·§2·§6 + 43 전체
  02-handler-turn-and-execution-gate.ko.md     05 §1.1(Yield 부분)·§3·§3.1 + 41 전체 + 42 §1-4·§7
  03-cancellation-and-shutdown.ko.md           05 §4
  04-spot-timer.ko.md                          05 §5 + 46 §7
  05-application-job-queue-and-backpressure.ko.md  33 전체 + 46 §1·§2·§5(일부)·§6·§8 + 42 §5·§6
                                                (+ 이관: 19 §10, 48 말미, 05 §10 shared permit)
  06-payload-ownership-and-codec.ko.md         50 전체
```

en 짝 문서는 동일 구조. 문서 이름에서 전역 번호를 뺀다 — 읽기 순서는 README가 정한다.

### 3.1 `README.ko.md`

| 새 절 | 내용 | 서술 종류 |
|---|---|---|
| 1. Execution이 다루는 범위 | submit부터 완료까지, 어디서 어디까지가 이 주제인가(관련 주제와 경계) | 계약 |
| 2. 역할과 책임 | Application / Framework / Core / provider 구분 요약표 | 계약 |
| 3. 두 capacity authority — 한눈에 | Core byte HWM vs Application Job Queue 그림 1개(33 §1·§2 요약) | 계약 |
| 4. 질문과 문서 | §2의 질문표 | — |
| 5. 문서 목록과 읽기 순서 | §3의 파일 목록 | — |
| 6. 수치 요약표 | 각 문서가 소유하는 수치를 한 곳에 모아 포인터만(값은 각 문서가 소유) | 계약 |

### 3.2 `01-submit-and-completion` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Messaging·Worker call 개요와 적용 범위 | 05 서문, §1.1 적용 범위 | 계약 |
| 2. Terminator별 완료 의미와 언어별 이름 | 05 §1.1 terminator 표, 언어별 terminal 이름 | 계약 |
| 3. Worker offload | 05 §1.2 | 계약 |
| 4. One-way submit — admission 경계 | 05 §1.3 admission boundary | 계약 |
| 5. Backpressure와 오류 분류 | 05 §1.3 backpressure/오류분류, pending admission target | 계약 |
| 6. Logical Multicast와 Classic fanout | 05 §1.3 | 계약 |
| 7. Admission deadline — owner와 값 규칙 | 05 §1.4 deadline owner, send timeout 값 규칙, STREAM send call별 timeout | 계약 |
| 8. STREAM reply token | 05 §1.4 | 계약 |
| 9. Request completion — 완료 경쟁과 timeout budget | 05 §2 완료경쟁, timeout budget, 늦은 결과 처리 (같은 turn 대기는 →02로 이동, S 참고) | 계약 |
| 10. Operation identity와 완료 자리 (구현) | 43 §1, §3 | 구현 |
| 11. 완료 callback의 execution turn (구현) | 43 §2 | 구현 |
| 12. 수락 후에는 다시 보내지 않는다 | 43 §4 | 계약+구현 |
| 13. 응답을 기다리지 않는 호출의 완료 지점 | 43 §5 | 계약 |
| 14. 실패를 문자열로 분류하지 않는다 (구현) | 43 §6 | 구현 |
| 15. 언어별 표현 | 05 §6 | 선언 |
| 16. 검증 요구 | 05(신규 — 옛 문서에 없었음, S7), 43 §7 | 검증 |

### 3.3 `02-handler-turn-and-execution-gate` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Queue와 gate 분리 원칙 | 41 §1, 42 §1 표 | 계약 |
| 2. Execution gate — owner 처리 순서 | 05 §3 execution gate | 계약 |
| 3. `Yield` 시 gate와 claim | 05 §1.1 "Yield 시 claim과 gate" + §3 "Yield 시 gate와 claim" (같은 규칙 중복 서술 통합, S8) | 계약 |
| 4. 같은 turn에서의 대기와 반납 | 05 §2 "같은 turn에서의 대기" + 41 §6 "기다릴 때 실행 권한 반납" (중복 통합, S8) | 계약 |
| 5. Actor Join과 `Defer()` 완료 경계 | 05 §3.1 전체 | 계약 |
| 6. 처리 권한 획득의 함정 (구현) | 41 §2 함정 1·2·3·5, 46 §3 배타권과 fencing 번호 | 구현 |
| 7. Lane 분리와 우선순위 (구현) | 41 §2 lane 표·우선순위·양보 부채, 42 §7 FIFO 분리(중복, 41로 흡수·S9) | 구현 |
| 8. 뒤처리와 turn 경계 (구현) | 41 §2 함정 4 | 구현 |
| 9. 시간 예산과 batch 처리 (구현) | 46 §4 | 구현 |
| 10. 실행 자원은 Spot 수에 비례하지 않는다 (구현) | 41 §3, 42 §4(process 기준 배분, 언어별 표) | 구현 |
| 11. 두 동기화 지점을 싸게 만든다 (구현) | 41 §4 | 구현 |
| 12. 넘겨가며 실행할 때 캐시 비용 (구현) | 41 §5 | 구현 |
| 13. Application·infrastructure 진행 분리 | 05 §3 application/infrastructure domain, 42 §1(표는 §1로 흡수)·§3 관측 | 계약 |
| 14. Object placement와 activation | 05 §3 | 계약 |
| 15. 오류 처리 | 05 §3 | 계약 |
| 16. 검증 요구 | 05 §3 관련 확인 항목(신규 작성) + 41 §7 + 42 §8(handler-turn 관련 행만) | 검증 |

### 3.4 `03-cancellation-and-shutdown` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. 협력적 cancellation | 05 §4 협력적 cancellation | 계약 |
| 2. Pre-cancelled call | 05 §4 | 계약 |
| 3. Cancellation의 경쟁 처리 | 05 §4 | 계약 |
| 4. Logical Multicast cancellation | 05 §4 | 계약 |
| 5. MeshNode relocation과 drain | 05 §4 | 계약 |
| 6. 검증 요구 | 신규 작성(옛 문서에 §9 자체가 없었음, S7과 동일 결함) | 검증 |

### 3.5 `04-spot-timer` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. Timer generation과 cancel | 05 §5 timer generation/cancel, tick 정보 필드 | 계약 |
| 2. Overrun policy | 05 §5 | 계약 |
| 3. Owner lease와 admission | 05 §5 | 계약 |
| 4. 공유 scheduler — 자원은 등록 수에 비례하지 않는다 (구현) | 46 §7 timer 자원 | 구현 |
| 5. 늦은 tick 처리의 내부 구현 (구현) | 46 §7 늦은 tick 처리(§2의 공개 option을 internals가 어떻게 지키는지) | 구현 |
| 6. tick이 실행 권한으로 들어가는 경로 (구현) | 46 §7, →02 링크 | 구현 |
| 7. 고빈도 timer의 batch 처리 | 05 §5 + 46 §7 언급(중복, 46으로 흡수) | 계약+구현 |
| 8. 검증 요구 | 05 §5 관련 항목 + 46 §9의 timer 관련 3행 | 검증 |

### 3.6 `05-application-job-queue-and-backpressure` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. 두 독립된 capacity authority | 33 §1, §2 | 계약 |
| 2. 설정과 profile 경계 | 33 §3 | 계약 |
| 3. Ordinary ingress permit 순서 | 33 §4 + 46 §1(준비된 owner 집합)·§2(넣을지 판단과 넣기의 원자성) + **이관 3건**(19 §10, 48 말미, 05 §10 — §4.1 참고) | 계약+구현(방향 구분) |
| 4. 소켓에서 여러 건 읽기 (구현) | 46 §6 | 구현 |
| 5. 수신 처리와 상태 변경 분리 (구현) | 46 §8 | 구현 |
| 6. Pressure 상태와 socket 제어 | 33 §5·§6 + 46 tail "Permit 변경과 pressure 평가"·42 tail "Pressure 전이와 송신 완료"(3중 중복 통합, S2) | 계약+구현 |
| 7. Send completion과의 합성 | 33 §7 | 계약 |
| 8. Backpressure 3단계와 한도 종류 | 42 §5·§6 | 계약 |
| 9. 큰 payload와 운영값 | 33 §8 | 계약 |
| 10. 검증 요구 | 33 §9 + 46 §9(permit 관련 행) + 42 §8(backpressure 관련 행) | 검증 |

#### 3.6.1 이관 규칙이 놓이는 정확한 자리

세 이관 규칙(§4.1 아래)은 모두 "ordinary ingress가 receive/claim 전에 permit을 얻는다"는
같은 명제를 각자의 문맥(STREAM packet, cross-node Session record, 일반 job queue 진입)에서
반복한다. `05-application-job-queue-and-backpressure` §3 "Ordinary ingress permit 순서"가
**하나의 계약 문장**으로 통합해 소유하고, 세 옛 출처가 각자 이 규칙을 인용했던 자리 — 새
`04-session/01-stream-session.ko.md` §9, `04-session/02-session-actor-binding.ko.md` §10, 그리고
이 주제 자신의 옛 `05-async-execution-policy.ko.md` §10 — 는 모두 이 문장 하나로 흡수된다.
`session/`의 두 문서는 이미 "재구성 중 — execution 주제로 이관"이라는 pointer 문구를 달아
두었으므로, 이동 완료 시점에 그 pointer의 대상 경로만
`01-execution/04-application-job-queue-and-backpressure.ko.md#3-ordinary-ingress-permit-순서`로
바꾸면 된다(§6).

### 3.7 `06-payload-ownership-and-codec` 절 구성

| 새 절 | 옛 출처 | 서술 종류 |
|---|---|---|
| 1. 두 종류의 복사 | 50 §1 | 계약 |
| 2. 없앨 수 있는 복사 (구현) | 50 §2 | 구현 |
| 3. 이동 기록을 hot path에서 만들지 않는다 (구현) | 50 §3 | 구현 |
| 4. 큐에 있는 동안의 소유자 | 50 §4 | 계약 |
| 5. Handler에 무엇을 넘기는가 | 50 §5 | 계약 |
| 6. 역직렬화를 언제 하는가 | 50 §6 | 계약 |
| 7. Codec 선택 — 계약과 internals의 경계 | 50 §7 | 계약+구현(절 안에서 방향 표시) |
| 8. Retained Core lease와 1:N child 소유권 | 50 tail(중복 없음 — 이 문서에만 있던 내용, 번호만 부여) | 구현 |
| 9. 검증 요구 | 50 §8 | 검증 |

## 4. 읽으면서 발견한 구조 문제 (재작성에서 처리)

| # | 문제 | 처리 |
|---|---|---|
| S1 | **`40-52` 다섯 문서 전부가 머리말에 "공개 규범 스펙이 아닌 내부 설계 문서 … Application이 관찰하는 동작을 추가하거나 변경하지 않는다"는 blockquote를 달고 있다**(41·42·43·46·50). 이는 캠페인이 이미 확정한 방향(README §1-1: "40~52는 '내부 설계(비규범)'가 아니라 구현 스펙이다")과 정면으로 어긋난다 | 이 blockquote를 제거한다. 대신 가이드 §4.4대로 계약 소유 문서를 링크하고, "이 절은 구현 서술이다 — 코드가 사실이면 문서를 고친다"는 방향만 표시한다 |
| S2 | **shared permit/HWM 규칙이 옛 문서 안에서만 7곳에 거의 같은 문장으로 반복된다** — 33 §4·§5·§6(원 소유), 46 tail 2개 절, 42 §5·§6 및 tail 2개 절, 43 tail, 41 tail, 50 tail, 05 §10. 여기에 이관될 19 §10·48 말미까지 더하면 총 9곳 | `05-application-job-queue-and-backpressure`가 단독 소유(§3.1)한다. 나머지 문서(01·02·04·06)는 인용이 필요한 자리에서 그 절을 링크만 한다. 재작성 시 "포화됐을 때 permit이 없으면 receive/claim하지 않는다"류 문장이 새 문서에 두 번 나오면 리뷰 실패로 본다 |
| S3 | **05 문서의 절 번호가 `## 6.`에서 `## 10.`으로 건너뛴다.** §7~9가 없고 §10 "Application job queue 비동기 경계"만 번호 없이 나중에 붙었다(S2의 05측 사본) | 재작성 시 이 절은 사라지고 §3의 통합 규칙으로 흡수되므로 번호 문제 자체가 없어진다 |
| S4 | **내부 설계 문서 5개가 모두 본문 끝에 번호 없는 절을 tail로 달고 있다**(46 "Shared supply permit…"·"Permit 변경과…", 42 "포화 상태의…"·"Pressure 전이와…", 43 "Completion과 shared capacity", 41 "Shared queue와 owner 직렬화", 50 "Retained Core lease와…") — session 파일럿 S3와 같은 패턴("33 HWM 작업의 흔적") | S2의 통합 결과 대부분 사라진다. 50 tail만 다른 문서에 사본이 없는 고유 내용이므로 §3.7의 §8로 번호를 붙여 정식 절로 흡수한다 |
| S5 | **"결정" 라벨이 41·42·43·46·50에 총 36회** 나온다(46:11, 50:14, 42:4, 41:4, 43:3). 가이드 §2.4/§4.4는 라벨을 없애고 굵은 규칙 문장 + 이유로 쓰라고 한다 | 전부 제거하고 굵은 규칙 문장 + 이유 불릿으로 재작성한다. "언어별 재량" 표시는 46에 1회만 있던 것을 유지·확장한다(예: 41 §5 "언어가 제약하는 선택" — 관찰 결과가 같은 이유와 확인 기준이 있으면 재량으로, 없으면 "아직 정하지 않은 것"으로 spec-gap 후보에 올린다) |
| S6 | **41이 "함정 1~5" 서사 구조를 쓴다** — 문제 제기 → 함정 설명 → 결정 순서로, 규칙+이유 불릿과 다른 형식이다 | 각 함정을 "굵은 규칙 문장 + 왜 이렇게 하지 않으면 깨지는가" 불릿으로 바꾼다. "이 구조를 놓치면 생기는 일" 비교표는 표 형식 그대로 유지해도 §7.1 조건을 만족한다 |
| S7 | **`05-async-execution-policy`와 `42-internal-progress-isolation` 모두 문서 마지막에 "검증 요구" top-level 절이 없다.** 05는 §6에서 끝나고(§10은 tail), 42는 "확인할 결과"는 있지만 §4.4가 요구하는 white-box/관찰 구분 없이 평평하게 나열한다 | 05에서 갈라지는 3개 새 문서(01·02·03·04) 각각에 §9.3 형식의 검증 요구 절을 신규 작성한다. 42가 흩어지는 두 문서(02·05)도 마찬가지로 재작성한다 |
| S8 | **같은 원본 안에서도 규칙이 두 번 서술된다** — (a) 05 §1.1 "Yield 시 claim과 gate"와 05 §3 "Yield 시 gate와 claim"이 거의 같은 문장을 두 자리에서 반복. (b) 05 §2 "같은 turn에서의 대기"와 41 §6 "기다릴 때 실행 권한 반납"이 같은 반납-재개 규칙을 다른 각도로 반복 | (a)는 `02-handler-turn-and-execution-gate` §3 한 곳으로 합친다. (b)는 같은 문서 §4 한 곳으로 합친다. `01-submit-and-completion`에서는 완료 의미만 남기고 두 규칙 모두 §02 링크로 대체한다 |
| S9 | **42 §7의 owner당 두 FIFO(application/lifecycle) 분리 서술이 41 §2의 lane 표와 같은 구조를 숫자 없이 반복**한다. 42는 존재만 말하고, 41이 정확한 수치(1,024건·64 MiB, 128건·4 MiB, 10 ms, 8 turn)를 소유한다 | 41이 유일한 소유 문서가 되고, `02-handler-turn-and-execution-gate` §13(application/infrastructure 절)에서는 "두 FIFO가 owner마다 분리된다"는 결과만 한 문장으로 쓰고 §7(lane 절)을 링크한다 |
| S10 | **46 §5의 "MeshNode socket option은 송·수신 방향을 따로 전달한다" 하위 절이 주제 범위를 벗어난다.** `SendHighWaterMark`/`ReceiveHighWaterMark`/`SendTimeout`/`ReceiveTimeout` 배선은 dispatch loop의 "즉시 깨움" 규칙과 무관하고, 계약 소유는 이미 [07-channel-topology](../../../../../framework/doc/framework/common/spec/server/02-channel-transport/01-channel-topology.ko.md)와 [13-mesh-node](../../../../../framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md)다 | 이번 주제에서는 옮기지 않는다(session 파일럿 S1과 같은 처리 — 다른 주제 차례에). `05-application-job-queue-and-backpressure`에서 이 하위 절 전체를 들어내고, 자리에는 "이 설정의 배선은 [02-channel-transport] 문서가 다룬다"는 한 문장만 남긴다. `02-channel-transport` 매핑표 작성 시 이 이관을 반영해야 한다 |
| S11 | **평서문 표(관측 대상)와 white-box 전용 조건이 "확인할 결과" 절에 섞여 있다** — 예: 46 "한 owner의 처리 권한이 동시에 하나만 존재한다", "이전 권한 시점의 늦은 완료가 현재 처리에 섞이지 않는다"는 fencing 번호 내부 구현 없이는 공개 표면에서 관찰할 수 없다 | 가이드 §4.4·§9.3대로 이런 항목은 규칙 문단이 "내부 확인 조건"으로 소유하도록 옮기고, 검증 절에는 공개 표면으로 관찰 가능한 항목만 남긴다 |
| S12 | **"정본" 표현이 41 §2에 1회** 남아 있다(가이드 §3.3/원칙 7.3 위반) | "그 판단의 기준이 되는 표는" 또는 "이 규칙을 소유하는 표는"으로 바꾼다 |
| S13 | **05 §3.1(Actor Join)이 문서 순서상 §3(handler turn) 바로 뒤에 있지만, §1.1의 "Actor Join" 하위 절(80-87행)과 내용이 겹친다** — 두 자리 모두 "Join call에는 terminator를 제공하지 않는다"·"Defer는 target I/O를 시작하지 않는다"는 취지를 반복 | `02-handler-turn-and-execution-gate` §5 한 곳에 합치고, `01-submit-and-completion`에서는 "Actor Join은 이 절의 terminator 대상이 아니다"라는 범위 제외 문장만 남긴다 |
| S14 | **43 §2의 dispatcher 동시 callback 상한 `4,096`이 적용 범위(process 전체인지 host instance 단위인지)를 밝히지 않는다** | 재작성 시 원문 그대로 옮기되(가이드 §2.5 — 새 보장을 만들지 않는다), 이 모호성은 새 규칙을 만들지 않고 §5의 spec-gap 후보로 올린다 |
| S15 | **46 §4의 "owner 점유 시간 예산" 설명에는 숫자가 없고, 41 §2의 "owner 점유 상한"에는 `10 ms` 기본값이 있다.** 두 절이 같은 메커니즘을 가리키는지, 46의 일반 원칙이 `PerActor`에도 적용되는지 문면상 불명확하다 | 재작성 시 46 쪽 절(→02 §9)이 41 §2(→02 §7)를 링크해 같은 메커니즘임을 명시한다. 그래도 남는 모호성은 spec-gap 후보로 올린다(§5) |

## 5. 규칙 등가성 대장

재작성 뒤 이 표의 모든 행이 새 문서의 정확히 한 절에 있어야 한다. 행이 없으면 누락, 표에
없는 보장이 새 문서에 있으면 추가 보장(둘 다 대조 실패). "새 위치"는 재작성 뒤 grep으로 실물
확인한 다음에만 채운다(§2.5).

### 5.1 `05-async-execution-policy` (R1–R45)

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R1 | Naming 규칙 적용 대상(Messaging call builder + `RunCpuWorker`·`RunIoWorker`) vs 비적용(topology·endpoint·설정·등록·object lifecycle builder, `RelayAsync` 같은 직접 method) | 05 §1.1 적용 범위 | |
| R2 | Terminator 3종(one-way 비동기 terminal / 일반 비동기 terminal / `Yield`)의 완료 의미와 owner turn 규칙 | 05 §1.1 표 | |
| R3 | 언어별 terminal 이름(.NET `Async`, Java·Node·C++ `submit`, Kotlin `await`; 즉시 제출은 `Submit`; gate 반납 terminal만 `Yield`) | 05 §1.1 | |
| R4 | `Yield` 제공 대상(`SpotWide`·Instance Spot의 Channel·Spot·Actor request, CPU·I/O worker, Actor·Spot create/get-or-create 특례)과 미제공 대상(Entry Spot·`PerActor`·Entry Actor·Node·Channel handler, owner turn 밖 `InvalidOperation`, Actor join·send·publish·timer 등록·close·destroy) | 05 §1.1 표 | |
| R5 | `Yield` 시 claim과 gate — Actor FIFO claim 유지, Spot gate만 반납; 다른 Actor·Spot·timer는 진행 가능하나 같은 Actor 다음 record는 대기; continuation이 gate 재획득 후 job 종료·claim 해제; 대기 전 읽은 mutable state는 재확인 필요 | 05 §1.1 | |
| R6 | Actor Join은 terminator naming 대상이 아니다; 동기 `Defer()` 1회로 barrier 등록, target I/O 미시작, gate·claim 미반납; `SpotWide`가 먼저 `Yield`했다면 continuation 종료 시점이 barrier terminal; `PerActor`/Entry Yield 금지 불변; Join call에 일반 terminal·`Yield`·one-way 미제공 | 05 §1.1 "Actor Join" | |
| R7 | CPU/I/O worker는 bounded scheduler 제출; CPU slot은 callback 실행 중만 점유, I/O 대기 중 미점유; I/O admission이 별도 bounded, CPU queue full이 이미 제출된 I/O completion을 `CapacityExceeded`로 바꾸지 않음; public I/O thread-count 설정 불필요 | 05 §1.2 | |
| R8 | Worker 실패 분류(`CapacityExceeded`/`DeadlineExceeded`/`InternalFailure`); timeout·cancel 뒤 늦게 끝난 작업은 두 번째 terminal을 만들지 않음 | 05 §1.2 | |
| R9 | Send·publish·bound session send·session Actor relay·명시 STREAM send·reply는 비동기 submit terminator 하나만, `TrySubmit` 없음; 정상 완료=source-local admission 성공(표: Remote target→local transport queue, Local target→mailbox/relay queue, Classic fanout·STREAM→socket queue); remote 수락·실행·완료는 기다리지 않음; Global Spot·Actor send는 Ready authority resolve부터 이 admission까지 기다림 | 05 §1.3 admission boundary | |
| R10 | Local capacity 부족 시 family send timeout까지 대기(Core HWM은 Core가 재시도·completion 소유, Framework는 readiness callback·retry waiter·binding adapter 없음); `Backpressured`는 public terminal 아님; capacity 확보 시 정확히 1회 제출 정상 완료; timeout·cancel·shutdown 먼저 확정 시 late admission 없이 예외 1회 완료; bounded waiter도 가득 차면 `DeadlineExceeded` 즉시 완료 | 05 §1.3 backpressure | |
| R11 | 오류 분류 표 7행(Actor/Spot authority 없음→`NotFound`, Mesh·Server 없음→`NotFound`, route 없음→`Unavailable`, admission deadline 만료→`DeadlineExceeded`, shutdown 중→`ShuttingDown`, terminal 중복 실행→`InvalidOperation`) | 05 §1.3 | |
| R12 | Pending admission이 유지하는 값(Node RID, global Spot·Actor ID, session binding token); RouteMesh·ClientServer select-one은 첫 binding operation 시작 직전에만 eligible member 재선택 가능; binding operation 시작 뒤 target 확정, capacity 이유로 재선택·재제출 안 함; 완료 뒤 자동 재제출 없음 | 05 §1.3 | |
| R13 | Logical Multicast — target snapshot 고정 후 각 1회 시도; capacity 확보 시 반환 데이터 없이 정상 완료, target별 제출은 내부 계속; 시작 후 개별 target 실패는 rollback·exceptional completion 아님; target별 결과는 public 반환값·monitoring 값 아님; target 0개도 정상 완료 | 05 §1.3 Logical Multicast | |
| R14 | Classic fanout — subscriber 없어도 publisher socket queue 수락 시 정상 완료; subscriber 수·수신 완료는 public result 아님 | 05 §1.3 | |
| R15 | Admission deadline owner 표 6행(RouteMesh·Spot·Actor→선택 MeshNode ROUTER send timeout 기본 1초, ClientServer→client DEALER 기본 1초, Logical Multicast→선택 MeshNode ROUTER target별, classic fanout→publisher socket 기본 1초, bound session·relay→Framework socket 동일 deadline 유지, STREAM send·reply→해당 STREAM socket) | 05 §1.4 deadline owner | |
| R16 | Send timeout 값 규칙(ms 올림 `1..INT_MAX`; 양수 sub-ms는 1ms; `0`·음수·무한대·상한초과는 startup 거부; 미지정 시 family 1초 기본값; setter는 잘못된 값 즉시 거부) | 05 §1.4 | |
| R17 | STREAM send call별 admission timeout modifier(선택적, transport 수락 대기 최대 시간); 생략 시 socket timeout, 지정 시 먼저 도달하는 deadline; 값 검증 동일; deadline 먼저 끝나면 `DeadlineExceeded` 1회, 이후 admission·replay 없음; STREAM reply call엔 미적용 | 05 §1.4 | |
| R18 | One-shot reply token — request sequence·token 보존; 첫 유효 terminator invocation이 admission 시도 전 원자적 claim·소비; 실패 완료돼도 재사용 불가; 같은 token 두 call 경쟁 시 claim 성공만 admission 시작; caller request timeout은 admission deadline으로 미사용; 늦게 수락된 reply가 correlation 불일치해도 결과를 바꾸지 않음 | 05 §1.4 STREAM reply token | |
| R19 | Request는 reply·remote 오류·timeout·cancel·shutdown 중 먼저 확정된 결과로 1회 완료; timeout·cancel은 caller 대기만 끝내고 원격 handler 업무를 rollback하지 않음; 늦은 reply는 handler에 재전달하지 않고 correlation state만 정리 | 05 §2 완료경쟁 | |
| R20 | Global object request timeout은 Ready authority resolve+outbound admission+handler+reply 전체 포함; source는 잔여 시간만 다음 단계에 전달; 미수락 증명 receipt 없으므로 자동 재제출 안 함 | 05 §2 timeout budget | |
| R21 | 같은 handler turn에서 보낸 request를 기다릴 수 있음; reply completion·binding operation completion은 infrastructure에서 진행돼 다음 application message 실행 없이 turn 재개 가능; `Async`는 원래 turn 유지, `SpotWide`의 `Yield`는 반납 후 completion 확정 시 원래 Spot queue에 재개 record 삽입; reply payload를 새 Spot packet으로 dispatch하지 않음 | 05 §2 같은 turn에서의 대기 | |
| R22 | 먼저 확정된 terminal 결과 하나만 사용; Spot 종료·같은 ID 새 generation 시 이전 activation의 늦은 reply 미전달; target 연결 종료·timeout 뒤 다른 경로로 자동 재전송 안 함 | 05 §2 늦은 결과 처리 | |
| R23 | Node·ChannelName handler, 각 Spot·Actor는 자신의 execution gate 순서로 처리; `Async` 대기는 완료 continuation 끝날 때까지 같은 gate의 다음 record 미실행; `SpotWide` `Yield`는 gate 반납 후 다음 record 실행 가능, completion은 새 turn으로 재개; Entry Spot Actor·`PerActor` Actor는 Actor별 gate, `Yield` 미제공; 같은 gate의 두 turn 동시 실행 없음 | 05 §3 execution gate | |
| R24 | `SpotWide` member Actor `Yield` 시 User Spot gate만 반환, Actor queue claim은 continuation 끝날 때까지 유지; 같은 Actor 다음 job은 먼저 실행 불가; 자신에게 보낸 request도 재진입·inline 실행으로 바꾸지 않음 | 05 §3 Yield 시 gate와 claim | |
| R25 | Application/infrastructure domain 독립 진행; payload decoding·user callback·exception mapping은 application turn; request completion과 bounded liveness·admission·relocation·reply recovery는 기존 Completion connection에서 수신, Core HWM 재시도 결과는 binding operation별 completion으로 수신하며 service-wire `SendReady`와 구분; peer 상태변경·shutdown barrier도 infrastructure task; application 대기 중에도 infrastructure 진행 가능해야 함; lifecycle의 user callback job은 application turn 포함 | 05 §3 application/infrastructure domain | |
| R26 | Object placement·activation은 infrastructure task; Location Store reservation이 확정한 owner만 factory 실행; `AuthorityOwnerGeneration`·owner lease는 Store·fencing 전용, `ObjectGeneration`은 public ref·session bind에도 사용; cold activation은 durable inbox first record 확정+recovery root·cursor 포함 `Ready` commit; owner lease로 계산한 admission deadline 적용해 first record 복원 후 activation barrier 개방 | 05 §3 object placement/activation | |
| R27 | Handler 예외 시 send handler는 logger·telemetry·metric 기록; request handler는 framework 오류 reply 생성; provider failure는 원래 dispatch 결과 불변, 별도 public error observer 없음 | 05 §3 오류 처리 | |
| R28 | `Defer()`는 즉시 시작 API 아닌 동기 terminal — 현재 handler 정상 종료 뒤 Join 실행할 intent+비활성 barrier 등록; 모든 언어에서 결과 없는 일반 함수, awaitable·promise·coroutine 반환 안 함 | 05 §3.1 Defer 성격 | |
| R29 | `Yield`는 결과 대기 중 shared gate 반납(Actor claim 유지); `Defer()`는 target 조회·Store I/O 없이 intent+barrier만 등록(gate·claim 모두 유지, handler 계속 실행) | 05 §3.1 Defer vs Yield | |
| R30 | Handler가 `Yield` 전·후 continuation에서 Join 등록 가능하나 마지막 awaited continuation 정상 종료 시점에만 barrier 활성화; exception·cancel·reply encoding 실패로 끝나면 등록한 비활성 barrier 모두 폐기; Join 결과는 원래 handler 재개값이 아닌 이동 대상 Actor의 completion callback으로 전달 | 05 §3.1 barrier 활성화/폐기 | |
| R31 | `Defer()`는 handler가 연 registration scope 안에서만 허용, scope 닫힌 뒤 호출 시 `InvalidOperation`; 기다리지 않은 detached task에서 호출은 application contract 위반이며 모든 언어에서 검출 보장 안 함 | 05 §3.1 호출 가능 범위 | |
| R32 | One-way terminal과 `Defer()` 모두 single-use나 완료 시점 다름 — one-way는 source-local outbound admission 대기, `Defer()`는 local registration 검증 뒤 즉시 반환; registration 오류는 target I/O 전 동기 발생; target 미발견·capacity 부족·relocation policy·callback 실패는 handler 종료 뒤 Actor completion으로 전달 | 05 §3.1 완료 시점 | |
| R33 | Cancellation은 협력적 요청; 완료된 결과·수락된 one-way 전달은 취소 안 됨; 언어별 표면(.NET `CancellationToken`, Java `cancel(false)`, Kotlin coroutine cancellation, Node `AbortSignal`); C++ one-way submit엔 별도 cancellation 입력 없음; task·stage 미사용만으로 취소 보장 안 됨 | 05 §4 협력적 cancellation | |
| R34 | Pre-cancelled call — argument·handle·one-shot state 먼저 검증; .NET pre-cancelled token·Node 이미 abort된 signal은 admission 시작 안 하고 cancelled awaitable로 완료; Java·Kotlin submit엔 cancellation 입력 없음; JVM call은 첫 non-blocking 시도 뒤 stage 반환하므로 `cancel(false)`가 첫 시도를 취소 못 함(표: 언어별 cancellation 입력·첫 시도 취소 가능 여부) | 05 §4 Pre-cancelled call | |
| R35 | Cancellation은 exceptional completion; admission 시작 후 cancel·timeout·shutdown·수락 경쟁 시 원자 terminal state 먼저 확정한 하나만 완료; 취소된 pending admission은 나중에 수락되면 안 됨 | 05 §4 경쟁 처리 | |
| R36 | Logical Multicast cancellation — executor direct handoff+publish transaction 원자적 확정 전에만 시작을 막을 수 있음; 시작 뒤 cancellation은 commit된 snapshot 중단 안 함, target별 관측정보·monitoring 값 안 만듦; Java `cancel(false)`·Kotlin cancellation은 `false` 반환·미취소; drain·shutdown도 시작된 transaction 완료를 기다리며 host drain deadline 넘긴 경우만 bounded force stop | 05 §4 Logical Multicast cancellation | |
| R37 | MeshNode `Relocating` 전환 시 새 ChannelName 선택·Logical Multicast target 제외; permit 못 얻은 unit의 application claim은 계속 진행, permit 얻은 turn 경계에서만 seal; `Draining` 뒤엔 이미 수락한 record·completion·relocation·STREAM barrier만 shutdown deadline까지 진행; deadline 뒤 남은 claim revoke, 대기 operation을 shutdown 결과로 완료 | 05 §4 MeshNode relocation과 drain | |
| R38 | Draining MeshNode는 새 object placement 후보에서도 제외; pending activation은 drain deadline과 activation deadline 중 먼저 도달한 경계에서 request 1회 terminal 완료+one-way payload drop; 경쟁해도 pending operation·payload reservation 1회만 정리 | 05 §4 | |
| R39 | 같은 timer key 재등록 시 generation 증가; 이전 generation queue record는 callback 미실행; cancel은 해당 generation 이후 callback 시작만 차단(이미 시작한 건 중단 안 함); 반복 timer가 handler보다 빨리 만료돼도 동시 실행 안 함, 중복 만료를 1개 pending record로 병합 가능 | 05 §5 timer generation/cancel | |
| R40 | Overrun policy 3종(`SkipLateTicks` 기본값/`CatchUpBounded`/`DelayNextTick`); `MaxCatchUpTicks` 기본 `1`, `CatchUpBounded`에서 `1..INT_MAX`; relocation encoding은 무시값을 유효 기본값으로 normalize 가능 | 05 §5 overrun policy | |
| R41 | `DeliveryIndex`(callback마다 정확히 1 증가)·`ScheduledIndex`(감소 안 함, `>= DeliveryIndex`)·`SkippedTicks`(계산식) 필드 의미; wall-clock 오차·exact nanosecond는 public 결과 아님 | 05 §5 tick 정보 | |
| R42 | Spot timer는 owner lease+admission deadline 확인 뒤에만 admission; lease 갱신 멈춰 monotonic deadline 넘으면 process 재개 후에도 새 tick 미투입·callback 미시작; 이전 owner authority의 pending tick도 미실행 | 05 §5 owner lease/admission | |
| R43 | 고빈도 timer도 관리형 언어에서 native callback 경계를 매 tick 왕복 안 함; platform timer가 scheduler에 wakeup 신호를 보내면 batch 처리 | 05 §5 고빈도 timer | |
| R44 | 공통 계약은 특정 async type 이름 강제 안 함(완료 순서·cancellation·오류 의미는 이 문서 소유, 반환 type·오류 표현은 exact interface 소유); 언어별 표(일반 비동기 완료/spot turn 반납/exact interface owner) | 05 §6 | |
| R45 | C++ `task_t`는 호출 시 operation 시작하므로 `submit()`을 결과 사용 여부로 다르게 씀; 반환형만 다른 overload 없음; blocking `submit()`과 coroutine terminal을 함께 제공 안 하고 `task_t<T> submit()` 하나, callback overload는 `submit(callback)`으로 별도 제공 가능 | 05 §6 언어별 표현 | |

### 5.2 `33-core-hwm-application-job-flow` (R46–R71)

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R46 | Core는 transport queue의 physical-frame byte로 burst 제한, Framework는 handler 시작 전 대기 job 수로 유입 제한; 값을 서로의 counter로 복사 안 함; 설정·profile·단위·계상경계·관측값 공유 안 함 | 33 §1 | |
| R47 | Core HWM은 전송 경로 마지막 안전장치(Framework pressure가 줄여도 이미 들어간 data 존재); Core queue 비어도 handler job이 오래 대기 가능; 두 보호 모두 필요하나 같은 보호 아님 | 33 §1 | |
| R48 | 두 authority 표(Core byte HWM: physical-frame charge, frame 소유 시 계상/dequeue 시 반환; Application Job Queue: job permit 수, receive·claim 직전 reservation+turn 전환 시 계상/callback 첫 instruction 직전 또는 callback 전 terminal에서 반환) | 33 §2 | |
| R49 | Core frame charge는 payload+metadata byte 포함; record가 binding에 넘어가면 그 charge 종료; Framework는 retained-credit lease 요청·byte charge 연장 안 함 | 33 §2 | |
| R50 | Receive 뒤 payload storage는 payload ownership 일반 규칙을 따름(→06); 복사·이동·해제는 lifetime 관리이며 HWM credit·두 번째 job permit·별도 authority 아님 | 33 §2 | |
| R51 | Framework pressure count = reserved supply permits + queued application jobs; capacity waiter는 미포함; reservation→queued 전환 시 합계 불변, permit 반환 시만 감소; 1 record가 1:N callback 만들면 실제 callback turn마다 permit 1개 사용 | 33 §2 | |
| R52 | 설정군 표(`CoreHwmMemoryLimitBytes` 등→Core 소유, 기본 `Balanced`; `ApplicationJobQueueProfile` 등→Framework host 소유, 기본 `Balanced`) | 33 §3 | |
| R53 | Framework는 startup에서 Core 설정값을 binding context option으로 전달 가능하나 profile 비율 계산·budget을 connection 수로 나누기 안 함; Core snapshot을 status에 투영은 읽기전용 관측이며 pressure 계산 입력 아님 | 33 §3 | |
| R54 | Framework job pressure가 Core에 주는 feedback은 `RUNNING`·`PAUSED` receive-flow 절대 상태 하나뿐; Framework는 Core HWM 설정·queued-byte counter를 전이에 맞춰 변경 안 함 | 33 §3 | |
| R55 | Ordinary ingress 5단계(가장 오래 기다린 live source 순 permit 대기 → permit 얻은 뒤에만 receive·claim → application record는 permit을 owner mailbox·serial queue handler turn으로 이전 → control·malformed record는 유한 처리 뒤 reservation 반환 → callback 실제 첫 instruction 직전 permit 반환) | 33 §4 | |
| R56 | Handler 시작 뒤 await·suspension·continuation·reply 대기는 같은 permit 재획득 안 함; relocation의 durable backlog는 관련 spec이 정한 owner로 handoff 후 initial reservation 반환, runnable turn마다 새 permit 획득 | 33 §4 | |
| R57 | Pre-receive에 terminal reply·error completion으로 식별되는 supply만 permit 우회; ordinary connection에서 receive 후 분류해 우회 소급 적용 안 함; 이 분리로 ordinary queue 포화돼도 이미 시작한 operation의 terminal completion 진행 | 33 §4 | |
| R58 | Framework heartbeat·topology·relocation과 service-wire `SendReady` kind `12`는 completion supply 아님; 이 control record는 application data line에 남아 기존 FIFO·liveness 계약 따름 | 33 §4 | |
| R59 | 금지 4항(permit 없이 먼저 receive 후 counter만 증가 / retained-credit lease·byte-HWM으로 Core HWM 재구현 / 포화를 reject·drop·polling·busy spin으로 바꿈 / 소유하지 않는 unbounded·hidden backlog 보관) | 33 §4 | |
| R60 | Pause/resume count 계산식(`ceil(M*P/100)`, `floor(M*R/100)`); `P`는 `1..100` 기본 `80`, `R`은 `0..99` 기본 `60`, `R < P`; `running`에서 permits≥pause면 `paused`, `paused`에서 permits≤resume면 `running`, 경계 사이는 현재 상태 유지 | 33 §5 | |
| R61 | 전이한 절대 상태는 RouteMesh·ClientServer의 paired DEALER/ROUTER socket에만 적용; PUB/SUB·Classic fanout·STREAM은 연동 범위 아님, 기존 Core byte HWM·구조적 queue 상한 유지 | 33 §5 | |
| R62 | `PAUSED`는 Core HWM 값 불변(Core는 remote-pause·local byte-HWM blocker 독립 합성); `RUNNING`은 remote-pause 원인만 제거하므로 local HWM full이면 send 계속 대기; pressure 상태 자체는 route ready·transport liveness 불변 | 33 §5 | |
| R63 | Host queue owner는 permit count 변경 등 synchronization 경계에서 pressure 상태 계산; 상태 바뀌면 지원 socket snapshot에 새 절대 상태 적용, 같은 상태 반복 적용 안 함, stale transition이 최신 상태를 못 덮어씀 | 33 §6 | |
| R64 | 새 socket은 현재 pressure 상태 적용 뒤 registry 게시; close는 먼저 registry에서 제거한 뒤 진행; binding 호출은 queue·registry·user callback lock 밖에서 수행; close와 경쟁한 lifecycle 결과 외 설정 실패는 진단·metric 기록 | 33 §6 | |
| R65 | 이 receive-flow state API가 Framework pressure와 Core send flow 사이 유일한 runtime 제어 지점; raw flow frame 생성·Core control lane을 범용 channel로 사용 안 함 | 33 §6 | |
| R66 | Core·binding은 HWM 대기·재시도·operation별 completion 소유; Framework는 exact target 선택 뒤 binding operation 하나만 시작, PAUSE·HWM 이유로 재선택·두 번째 operation 안 만듦 | 33 §7 | |
| R67 | Framework는 제거된 `send_ready` callback·readiness waiter·retry adapter 없음; deadline·cancel·detach·shutdown은 기존 state machine 첫 terminal 규칙 따름; `SendReady` kind `12`는 다른 계약 | 33 §7 | |
| R68 | Application Job Queue는 job count 제한, payload byte 가중 안 함(빈/큰 payload 각각 job 1개); Framework queue 상한은 process memory byte hard cap 아님 | 33 §8 | |
| R69 | 큰 payload workload는 production 분포·permits in use·process memory·throughput·latency 함께 측정해 `MaxQueuedApplicationJobs` 조정; 단일 message는 `MaxMessageSize`로 별도 제한; Core profile 연결·retained-credit lease 복구로 해결 안 함 | 33 §8 | |
| R70 | Core HWM은 Core queue memory의 마지막 안전장치로 계속 동작 — Framework가 permit 이유로 receive 멈추면 local Core receive queue에 byte가 쌓이고 finite Core HWM+TCP backpressure가 sender 진행 제한 | 33 §8 | |
| R71 | Contract test 요구 8항목(Core/Application profile 독립 설정과 기본값, permit 없으면 다음 record 미수신, reservation·queued·callback permit count 일치, 80/60 hysteresis 정확성, socket 동기화·close 경쟁·stale transition 안전성, 지원 paired socket에만 적용, completion supply는 permit 포화와 독립, retained receive·waiter·retry 미사용) | 33 §9 | |

### 5.3 `46-internal-dispatch-loop` (R72–R87)

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R72 | 지금 처리할 일이 있는 owner를 상태로 유지(missed wakeup 없음, 같은 owner 중복 등재 없음); 빈 queue에 첫 항목 시 집합 추가, 실행 자원이 하나 가져가 처리, 처리 뒤 남으면 다시 넣고 비면 뺌, **깨어난 뒤 항상 이 집합을 다시 확인** | 46 §1 | |
| R73 | Owner 대기열에 넣을지 판단하는 확인들과 실제 넣기가 같은 구간 안에서 끝남(확인·넣기 사이 owner 안 바뀜); 한 commit으로 처리하는 5단계(수락 상태·객체 존재/owner 유효·봉인 아님·lane 건수·byte 동시 예약·sequence 확정+FIFO enqueue·비었으면 즉시 signal) | 46 §2 | |
| R74 | 확인 실패한 message는 대기열에 안 나타남(넣었다 빼는 방식 아님); 예약·enqueue 실패해도 건수·byte·sequence는 이전 값 유지; 실패한 시도가 다음 정상 작업의 순서·admission 여부를 바꾸지 않음 | 46 §2 | |
| R75 | **언어별 재량** — 확인·넣기 구간을 잠금으로 만들지 다른 방법으로 만들지 자유(관찰 결과: 확인만 하고 넣는 것까지만 이 구간에 있고, 역직렬화·handler 조회는 구간 밖) | 46 §2 | |
| R76 | 가져오기는 배타권 획득을 겸함(한 owner당 처리 권한 동시 1개); 처리 권한마다 재사용 없는 번호를 붙여 늦은 완료가 자기 것인지 판단(내부 확인 조건) | 46 §3 | |
| R77 | 한 번 가져오면 정해진 시간 예산 안에서 여러 건 이어 처리, 한 건 끝날 때마다 예산 확인 후 남으면 다음 건 아니면 반납; 건수가 아닌 시간으로 잼(handler별 처리시간 다름); 실행 중 handler는 안 끊음; byte 합계는 보조로만(처리시간이 payload 크기에 비례할 때만) | 46 §4 | |
| R78 | 작업 도착이 실행을 직접 깨움 — 비어 있던 FIFO가 채워지는 순간 process 공유 실행 자원에 signal(4언어 구현 예시는 재량); 주기적 polling은 정상 처리의 시작 조건이 아님; 깨운 뒤 §1 규칙대로 재확인 | 46 §5 | |
| R79 | 한 번 깨어났을 때 소켓에서 건수·byte·경과시간 중 먼저 닿는 한도까지 여러 건 이어 읽음; 다음 회전은 이번에 멈춘 연결 다음부터 시작(cursor 유지); 이 규칙은 fanout·RouteMesh·ClientServer·service connection·STREAM 등 모든 multi-connection 수신 경로에 적용; 한도 걸려 남은 것은 다음 깨어날 때 이어 읽음 | 46 §6 | |
| R80 | Timer는 공유 scheduler 하나가 마감시각 우선순위 대기열로 관리, 등록마다 전용 자원을 만들지 않음(1만 Spot×timer 2개 예시로 2만 자원 vs 스레드 1개+대기열 2만 항목 비교) | 46 §7 | |
| R81 | 늦은 tick 처리는 공개 option(→04 소유)이며 internals가 고정하지 않음; 기본 동작은 밀린 tick을 하나로 합침, 따라잡기 option은 그 option이 정한 개수까지가 상한; tick 통계를 timer 수명 동안 무한정 누적 안 함 | 46 §7 | |
| R82 | Timer callback은 그 Spot의 실행 권한을 거쳐 실행(`SpotWide`는 공유 권한, `PerActor`는 timer 이름별 권한); 권한을 못 얻으면 그 tick은 보관 자리에 남았다가 재시도 | 46 §7 | |
| R83 | 수신 콜백은 데이터 소유권을 옮기고 바로 반환, 그 안에서 handler 호출·Spot 상태 변경 안 함(내부 확인 조건); 형식 검사는 handler 호출 전 끝냄, 형식 안 맞는 입력은 handler에 미도달(응답 기다리는 호출은 `ProtocolError`, 안 기다리면 기록만) | 46 §8 | |
| R84 | 확인할 결과 17항(§9 목록) | 46 §9 | |
| R85 | Pre-receive에 terminal completion으로 식별되는 supply만 shared permit 우회, 나머지 application·control·malformed record는 receive/claim 전 host-instance permit 획득; source마다 outstanding waiter 1개, oldest waiter 순 handoff; batch 처리한 source는 tail 이동, batch·1:N은 확보 permit보다 많은 job 미게시 | 46 tail "Shared supply permit" (33 §4/§2와 중복 — 통합 대상) | |
| R86 | Application permit은 실제 exact-target callback 첫 instruction에서 반환, control·malformed는 내부 처리 직후; cancel·source close·shutdown은 waiter·handoff permit을 정확히 1회 정리; same-host relay·fanout·serial owner·relocation 경로는 permit 반환에 필요한 자원을 쥔 채 새 permit acquire를 기다리면 안 됨(지속 wait/capacity cycle은 protocol/runtime bug) | 46 tail (중복) | |
| R87 | Reserved permit 획득·queued 전환·release는 같은 queue owner 경계에서 pressure 상태 갱신; `running`/`paused` 전이 조건은 33 §5와 동일; shutdown은 마지막 상태 적용을 무기한 기다리지 않음 | 46 tail "Permit 변경과 pressure 평가" (33 §5·§6과 중복 — 통합 대상, S2) | |

### 5.4 `42-internal-progress-isolation` (R88–R102)

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R88 | Runtime 작업은 application(handler·message·timer·session callback, Spot 소유자별 순서)과 infrastructure(peer 수락·binding completion·완료확정·owner 갱신·이동·종료, application 대기와 무관 진행)로 나뉨; infrastructure는 application handler가 점유할 수 없는 실행 영역에서 진행(spec 요구는 "독립 진행"보다 강함) | 42 §1 | |
| R89 | 같은 executor 안에 infrastructure 전용 자리만 떼면 queue 자리는 남아도 실행 주체가 없을 수 있음(그래서 예약이 아닌 분리); 서로 다른 목적의 한도(Core byte HWM/Application job queue/owner별 count·byte queue/outbound admission waiter)는 합치지 않음, 같은 label·단위가 있어도 type·계산·error 의미 공유 안 함 | 42 §2 | |
| R90 | 상태 구독자·metric 수집기는 어느 영역의 진행 권한도 점유 안 함; 구독자 자리는 한도 있고 넘치면 source별 최신 status로 합쳐 따라잡음, 자리 가득 찼다고 stream 안 끊고 반대로 message 처리도 안 늦춤 | 42 §3 | |
| R91 | 자원은 process 하나 기준으로 배분, topology·Spot 수에 따라 안 늘림(infrastructure는 대부분 짧고 대기 없어 적은 자원으로 충분) | 42 §4 | |
| R92 | 계약은 "전용 thread"가 아닌 "application이 전부 대기 중일 때 infrastructure가 진행"(언어별 표: C++ 전용 worker/.NET 별도 lane/Java 전용 executor/Node는 물리적 분리 불가·lane만 분리); 보장(await로 양보한 뒤엔 infrastructure 진행) vs 미보장(양보 없이 CPU 붙잡은 동안의 진행, application 책임); 판정 기준은 자료형이 아닌 양보 후 진행하는가 | 42 §4 | |
| R93 | Backpressure 3단계는 send·publish·one-way 계열에만 적용(제출 거절 시 시간까지 대기 → 공간 생기면 1회 제출 → 시간 먼저 끝나면 `DeadlineExceeded`); Request 계열은 안 기다림(같은 runtime→`CapacityExceeded`, 다른 node→`Unavailable`, 결과를 받아 판단 가능하므로) | 42 §5 | |
| R94 | 3단계는 public 결과 미확정 구간에만 적용(완료된 호출 뒤 실패는 관측으로만); 기다리는 동안 실행 권한을 쥐지 않음; 기다리는 자리도 한도 있어 가득 차면 즉시 `DeadlineExceeded`(`Backpressured`는 public terminal 아님) | 42 §5 | |
| R95 | Ordinary source는 host-shared permit readiness 뒤 receive·claim, application job은 callback start까지 permit 유지; receive 뒤 payload owner는 native storage 수명 관리하나 Core HWM budget 계속 점유 안 함; control·malformed도 같은 permit 얻되 내부 처리 직후 반환 | 42 §5 (33 §4/46 tail과 중복) | |
| R96 | StreamNode client→server complete-message `MaxMessageSize`는 이 capacity와 독립된 wire guard(6-byte prefix 제외 header+payload, 기본 `64 KiB`, server→client 미적용) | 42 §5 | |
| R97 | 한도 종류별 terminal 의미 구분(owner structural 한도·outbound waiter→owner error/`DeadlineExceeded`; host shared job queue capacity 부족→public reject/drop 아닌 cancellable oldest-waiter wait; Core byte HWM→transport backpressure); 4행 한도표(무엇으로 재는가·포화 의미) | 42 §6 | |
| R98 | 실행 중인 영역은 문맥 표시로 확인, 잘못된 조합이면 기다리지 않고 즉시 실패(대기하면 교착, 통과시키면 분리가 무너짐) | 42 §7 | |
| R99 | 두 영역은 owner마다 물리적으로 다른 FIFO(application FIFO·lifecycle FIFO는 예약·admission 상태 공유 안 함, application 가득 차도 이미 수락한 lifecycle은 실행 가능); 우선순위·굶주림 방지는 41 소유(S9); owner가 늘어도 실행 thread·executor를 owner마다 안 만듦, 비어있던 FIFO에 첫 작업이 들어오면 즉시 signal | 42 §7 (41 §2와 구조 중복, S9) | |
| R100 | 현재 실행 영역은 문맥 표시로 구분(thread 종류에만 연결하면 Node 단일 event loop·`.NET` 경로 표현 못 함); 언어별 실행 수단은 달라도 잘못된 영역 호출은 같은 방식으로 실패해야 함 | 42 §7 | |
| R101 | 확인할 결과 15항(§8 목록) | 42 §8 | |
| R102 | Host queue owner는 permit count 변경 경계에서 80% pause·60% resume hysteresis 평가; 상태 바뀌면 지원 socket snapshot에 새 절대 상태 적용, socket별 적용 직렬화·stale sequence 무시; binding 호출은 queue·registry·user lock 밖, close는 registry 제거 먼저; Core·binding이 HWM 재시도·completion 소유하므로 Framework infrastructure domain은 별도 `send_ready` waiter·retry adapter 없음 | 42 tail "Pressure 전이와 송신 완료" (33 §5·§6, 46 tail과 3중 중복 — S2) | |

### 5.5 `43-internal-completion` (R103–R112)

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R103 | 호출마다 완료 자리 하나, 여러 경로가 경쟁, 차지한 경로만 caller 완료; 완료 권한 확정 방식은 진행 중 호출 표에서 항목을 atomic하게 꺼내는 연산(성공한 경로만 권한 획득, 나머지는 이미 완료됐음을 확인하고 끝남); 이 연산이 완료 권한 확정+정리를 함께 수행, 별도 완료 표시·두 번째 자리 예약 불필요; 모든 완료 경로가 같은 방식 사용 | 43 §1 | |
| R104 | 완료 확정 시 잡은 잠금 안에서 application callback을 실행하지 않음(교착 방지); 잠금 놓은 직후 같은 stack에서 바로 실행도 불충분(transport 처리가 끝나기 전 재진입 가능); 완료 callback은 process 공유 completion dispatcher에 넣고 새 execution turn에서 실행 — 순서(권한 확정→잠금 해제→dispatcher 투입→새 turn 실행) | 43 §2 | |
| R105 | Terminal winner가 항목을 꺼낸 뒤 dispatcher admission 실패하면 completion을 잃으므로, operation 수락 시 dispatcher 자리도 함께 예약해 callback 반환 시까지 유지; 진행 중 operation과 dispatcher 대기·실행 callback의 합은 **4,096개**를 넘지 않음(적용 범위는 spec-gap 후보 §5) | 43 §2 | |
| R106 | 예약할 자리 없으면 request 보내기 전 `CapacityExceeded`로 거부; 한 번 수락한 completion enqueue엔 거부·버리는 경로 없음; dispatcher는 callback마다 thread를 안 만들고 공유 lane 사용, shutdown에서 수락한 callback을 모두 실행; 한 callback의 exception은 뒤 callback 실행을 막지 않음 | 43 §2 | |
| R107 | Service wire request는 `OperationId`(`{high:u64, low:u64}`, terminal deduplication identity, relocation·relay 거쳐도 유지)와 `ReplyRouteId`(non-zero `u64`, source lifecycle 안 대기 항목 연결, identity 대신 아님) 두 값을 함께 보존, 둘 다 내부값이며 application에 미노출; `OperationId`는 두 word 모두 0 불가; registry·durable record는 두 word 전체 보존(`low`만 key면 다른 operation을 같은 항목으로 오판) | 43 §3 | |
| R108 | 보내는 runtime은 `OperationId`·`ReplyRouteId`를 먼저 만들고 pending completion entry·dispatcher 자리·reply 경로를 등록한 다음에만 transport에 submit; 같은 process 즉시 응답이어도 등록보다 reply가 먼저 처리되지 않음(별도 보관 map·교차 확인 불필요) | 43 §3 | |
| R109 | 전송이 message를 수락한 뒤엔 대상 실행 여부를 알 수 없으므로 runtime이 자동으로 다시 보내지 않음(연결이 끊겨도 동일); application이 새 호출을 시작할 수는 있고 중복 실행 위험은 application이 판단; 표(수락 전 실패→다시 보내도 됨, 수락 후 실패→안 됨) | 43 §4 | |
| R110 | 응답을 기다리지 않는 호출은 이 process의 송신 경로가 message를 수락한 시점에 정상 완료; 원격 queue 수신·handler 실행 여부는 이 결과로 모름; "로컬 수락"과 "전송 수락"은 같은 완료 경계(문서·주석은 send acceptance 한 표현만 사용) | 43 §5 | |
| R111 | 완료 경로는 취소·시간초과·종료를 타입이나 전용 값으로 분류, 오류 메시지 문자열에 정규식을 걸어 판정하지 않음 | 43 §6 | |
| R112 | 확인할 결과 10항(§7 목록) | 43 §7 | |

### 5.6 `41-internal-serialization` (R113–R131)

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R113 | 정식 spec의 두 요구 — Actor 앞 payload는 실행 모드와 무관하게 항상 그 Actor queue에 제출; `SpotWide`에서는 그 Spot의 Actor handler·Spot handler·timer·lifecycle callback이 전체에서 한 번에 하나만 실행. 두 문장을 합치면 "queue는 Actor마다, gate는 Spot이 공유"하는 구조 | 41 §1 | |
| R114 | 이 구조를 놓치면(실행 권한까지 Actor마다 두면 SpotWide에서 두 handler 동시 실행=경쟁 상태; SpotWide에서 queue를 하나로 합치면 이동 시 Actor별 남은 작업을 못 갈라냄) | 41 §1 | |
| R115 | `PerActor`는 Actor별·Spot별 분리로 안 끝나고 timer마다도 따로(timer 2개를 Spot 줄에 함께 넣으면 서로 다른 timer가 서로 기다림) | 41 §1 | |
| R116 | 작업을 thread에 넘겨가며 실행하는 구현에서 thread-local storage로 작업 사이 상태를 전달하면 다음 작업이 다른 thread에서 실행될 때 상태를 잃음(방식 자체는 문제 없음 — thread 고정 전제 코드를 그대로 옮기는 것이 문제) | 41 §2 함정1 | |
| R117 | 대기열이 가득 찼을 때 그 자리에서 바로 실행하면 직렬성이 깨짐; 결과는 제출 계열·대기열 위치·public 결과 확정 여부로 갈림(표 9행 — send/one-way 같은 runtime→send timeout 대기, 다른 node→결과 없음; publish 시작 전→기다림, 시작 후 local target→건너뜀; request 같은 runtime→`CapacityExceeded`, 다른 node→`Unavailable`; control claim 같은/다른 node; 송신 backpressure→Core가 처리); 그 자리에서 실행은 어느 경우도 선택지 아님 | 41 §2 함정2 | |
| R118 | 대기열 앞쪽에 넣는 새치기 경로를 두지 않음(먼저 처리할 작업은 별도 대기열+우선순위 명시); owner마다 두 FIFO lane(application lane=업무 payload·timer callback, 건수·byte 두 축; lifecycle lane=join·leave·relocation·lifecycle control, 별도 한도) | 41 §2 함정3 | |
| R119 | 기본 admission — application lane `1,024`건·`64 MiB`, lifecycle lane `128`건·`4 MiB`; application byte reservation은 payload+고정 retained cost `256 byte` 포함; 두 lane 모두 실행 중에도 reservation 유지, terminal completion에서만 반환; turn 경계에서 원자적 판단, 둘 다 ready면 lifecycle이 먼저 | 41 §2 | |
| R120 | 우선순위만으로 굶주림 못 막음 — owner 점유 상한(서로 다른 owner 사이, 시간, 기본 `10 ms`)과 lifecycle 연속 실행 상한(같은 owner 두 lane 사이, turn 수, 기본 `8 turn`); 양보 부채(lifecycle 연속 선택이 상한 도달 시 부채 표시, 부채 있는 동안 application이 ready인 한 application turn 1회 실행할 때까지 lifecycle 미선택, 실행하면 부채 소멸) | 41 §2 | |
| R121 | 이 규칙으로 handler 경계에서 application turn이 결국 선택됨(`10 ms`는 실행 중 handler를 안 끊는 제한, 최대 대기시간 보장 아님); 각 lane 안 순서는 수락 순서 그대로, 어느 lane에도 앞쪽 삽입 없음 | 41 §2 | |
| R122 | 작업 종료 시 미뤄 둔 뒤처리(실행중 표시 내림→대기열 다음 예약→잠금 해제→뒤처리 실행)에서 예약된 다음 작업과 뒤처리가 동시 실행될 수 있음; 뒤처리는 실행 권한을 놓기 전에 끝내거나 새 작업으로 대기열에 다시 넣음, 권한 밖 뒤처리는 owner 상태를 안 만지는 것만 허용 | 41 §2 함정4 | |
| R123 | 권한 안에서 그 자리 실행하는 재진입 우회는 자기 자신을 기다리는 교착을 피하지만 관찰 가능한 의미 차이(재진입 허용 시 중첩 호출이 대기열보다 먼저 끝남); 재진입을 허용하지 않는 것은 spec 규정 — 현재 turn 유지한 채 같은 gate·같은 Actor 자신을 기다리는 호출은 제출 전 `InvalidOperation`; 허용된 `SpotWide`·Instance에서 `Yield`는 gate를 먼저 반납하므로 재진입 아님 | 41 §2 함정5 | |
| R124 | 금지 대상은 public operation — 판정표 5행(자기 Actor에 request 보내고 대기=금지; 현재 turn 유지 terminal로 같은 gate 대기=금지; 허용된 문맥에서 `Yield`로 대기=허용; runtime의 내부 문맥 합성=허용, 새 제출 아님; 결과 안 기다리고 제출=허용); "제출 전"이 핵심(요청 나간 뒤 실패하면 원격 부작용만 남음) | 41 §2 | |
| R125 | 실행 자원은 Spot 수가 아닌 코어 수에 비례(Spot마다 전용 worker 두면 Spot 1만 개에 worker 2만 개 필요); 반납 대기가 자기 자신을 기다리는 문제는 자원을 늘려서가 아니라 반납한 작업을 같은 권한의 대기열에 다시 넣어 해결 | 41 §3 | |
| R126 | Actor 대기열에 독립 잠금을 두지 않음(SpotWide에서 꺼내는 쪽은 이미 권한을 쥐고 있어 넣는 쪽만 보호); 무경합 시 권한 획득이 원자 연산 하나로 끝남(경합 있을 때만 대기열); 안 하면 message마다 잠금 2번 | 41 §4 | |
| R127 | 작업을 넘겨가며 실행하면 상태가 이전 자원 캐시에만 남아 cache miss 비용이 누적(비교표: 넘겨가며 실행 vs 같은 자원 고정); 실행 자원 하나에 고정할지는 언어가 제약(이벤트 루프 하나인 언어는 선택 불가); 어느 쪽이든 계약 만족, 어느 쪽 골랐는지는 그 언어 문서에 기록 | 41 §5 | |
| R128 | 반납한 뒤 재개할 때는 새 작업으로 재개(하나의 작업이 대기 구간을 가로질러 유지되지 않음); 재개를 기다리는 중 Spot 종료·봉인되면 재개하지 않고 실패(이동 시작만으로는 안 멈춤 — 봉인 전까지 기존 message·timer 계속 처리) | 41 §6 | |
| R129 | "작업 하나가 끊기지 않는다"는 보장이 아니고 "한 실행 권한에서 두 작업이 동시에 실행되지 않는다"만 보장 — 반납 전 읽은 값이 재개 후 유효하다고 가정 불가(handler 작성자에게 영향, 언어별 가이드가 설명해야 함) | 41 §6 | |
| R130 | 반납은 `SpotWide` User Spot·Instance Spot에서만 가능; 그 밖에서 호출하면 원격 요청·queue 변경 전에 실패로 끝남 | 41 §6 | |
| R131 | 확인할 결과 19항(§7 목록) | 41 §7 | |

### 5.7 `50-internal-message-ownership` (R132–R155)

| # | 규칙 (수치·상태·오류) | 옛 위치 | 새 위치 |
|---|---|---|---|
| R132 | Binding이 강제하는 복사(없앨 수 없음, native 버퍼를 안전하게 못 유지하는 언어는 view 미제공 가능)와 framework가 만드는 복사(없앨 수 있음)를 구분; framework가 추가로 만드는 복사는 0을 목표로, binding 강제 복사는 언어별 사실로 인정 | 50 §1 | |
| R133 | 없앨 수 있는 복사 5유형(경계 왕복, 접근자마다 새 복사, 큐 넘기려는 복사, 이중 보관, 이동 기록 사전 생성) | 50 §2 | |
| R134 | 수락한 route message마다 relocation 대비 기록을 미리 만들면 relocation 여부와 무관하게 모든 message에 비용 추가(원본+중간 표현이 4벌까지 동시 상주 가능); 이동 기록은 봉인이 시작된 뒤에 만듦; 미리 만들어야 한다면 원인은 "seal 시점 전 원본 해제"이며 그건 원본 해제 시점을 고칠 문제 | 50 §3 | |
| R135 | 실행 대기열에 들어간 payload는 framework가 소유(대기 중 전송계층·application 미접근); 해제는 handler가 끝난 뒤(view 무효화 방지); 무엇을 해제하는지는 binding의 소유권 표현에 따르나 해제 시점은 항상 handler 완료 이후 | 50 §4 | |
| R136 | 소유권 전이는 한 방향(binding receive 뒤 보관 필요 시 경계에서 1회 복사·이전; queue 투입 뒤 encoded payload는 Framework 소유, decoded value는 native storage 소유·해제 책임 미포함); state diagram(bindingStorage→frameworkStorage→handlerValue, 각 단계 실패 시 released) | 50 §4 | |
| R137 | 모든 terminal 경로는 같은 release 지점으로 모임(exception·timeout·cancel·shutdown·relocation 정리에서도 release를 건너뛰거나 2번 실행 안 함); C++·.NET은 `close`·`Dispose`로 드러나고, GC 위임 언어도 같은 논리적 release 지점 유지 | 50 §4 | |
| R138 | Handler에는 역직렬화된 소유 객체를 넘기고 native 저장소·해제 책임은 안 넘김(typed handler 제공 이상 역직렬화 복사 1회는 불가피); "framework 복사 0"은 역직렬화를 제외한 값(목표는 `binding 강제 복사 + 역직렬화 1회`) | 50 §5 | |
| R139 | 복사 회계는 3종을 따로 셈(buffer 전체 복사=크기 비례 비용/view·slice=비용 거의 없음/객체 생성=buffer 복사가 아닌 codec·payload 모양에 좌우); "역직렬화 1회"는 buffer 복사 축의 값 | 50 §5 | |
| R140 | 불변 payload는 생성 시 1번·접근자 호출 시 또 1번 복사하기 쉬움(접근자마다 복사하면 handler가 2번 읽으면 복사도 2번); 공개 API 불변성은 유지하되 runtime 내부 이전에는 복사 없는 경로를 따로 둠; raw byte API는 transport 검사·codec extension 구현에만, 업무 handler 인자로 주면 계약 위반 | 50 §5 | |
| R141 | 헤더는 먼저 읽고 payload 역직렬화는 실행 권한을 얻은 뒤 handler 직전(헤더는 owner를 정하려 필요, payload는 handler 전엔 아무도 안 봄) | 50 §6 | |
| R142 | 실행 대기열에 수락되지 못한 message는 역직렬화 안 함(object·channel admission 거부는 handler 미도달, 미리 역직렬화하면 결국 거부할 message에도 비용); Application Job Queue 포화는 이 거부가 아님(host-wide permit을 취소 가능하게 기다림, reject·drop 아님) | 50 §6 | |
| R143 | Relocation seal 뒤 도착한 message는 거부 안 함(encoded payload·reply 정보 보류, abort 시 source 재개, 그 외 target handoff·Message Follow로 넘김); 실행 권한 얻기 전엔 역직렬화 안 함 | 50 §6 | |
| R144 | 실행 권한 얻기 전 역직렬화하면 거절된 message에도 비용 발생; 형식 판별을 위해 payload 전체를 두 번 해석 안 함(형식은 헤더가 알려줌) | 50 §6 | |
| R145 | 수락한 message의 typed payload는 최대 1회만 역직렬화(첫 typed 접근의 값·실패를 저장, 같은/다른 type 재접근에도 codec 재호출 안 함; 저장한 값을 못 쓰면 언어별 type mismatch, 첫 접근 실패는 같은 실패 재전달); 읽기전용 raw view·명시적 byte 복사본은 typed 결과를 안 만듦 | 50 §6 | |
| R146 | Codec은 여러 개가 동시 등록되어 선택이 필요; 선택 표현 API 모양은 언어마다 다름(internals는 단정 안 함); 송신(호출 지점 선언 message type, 못 찾으면 JSON)과 수신(정규화된 content-type, 못 찾으면 `ProtocolError`)은 서로 다른 경계 | 50 §7 | |
| R147 | 송신 selector에는 선언된 message type을 전달(실제 instance concrete type 아님), 둘 이상 일치 시 나중 등록 우선; 수신측은 정규화된 content-type을 registry key와 정확히 비교, 등록 안 됐거나 정규화 규칙 안 맞으면 `ProtocolError` | 50 §7 | |
| R148 | 내부 registry·codec 선택 cache·dispatch 구현은 계약이 아님(선택이 일어난다는 사실만 계약, 비용은 internals가 정함) | 50 §7 | |
| R149 | Codec 선택 조회 과정의 문자열·배열·호출 객체를 message마다 새로 만들면 안 됨(표 4행: 타입 키 조회+객체 생성/content-type 문자열화/후보 목록 배열/기본형식 문자열 비교) | 50 §7 | |
| R150 | Registry는 시작 뒤 불변이므로 선택 결과를 캐시(수신 캐시 키=정규화된 content-type, 시작 시점 전부 계산 가능; 송신 캐시 키=선언 message type, 미리 열거 불가, 처음 만난 타입에서 1회 계산); Registry 자체 불변이므로 캐시 결과는 1회 계산 뒤 안 바뀜 | 50 §7 | |
| R151 | 송신 캐시는 선언 type `1,024`개까지만 저장, 한도 도달해도 기존 entry 제거 안 함; 이후 처음 보는 type은 매번 재평가하고 캐시에 안 넣음 | 50 §7 | |
| R152 | 시작 뒤 불변이면 미리 계산해 두고 잠금 없이 읽음(실행 중 동시 접근 못 견디는 사전에 쓰면 race); content-type 비교용 문자열을 새로 안 만듦(다듬기는 등록 시점에); 후보 목록·임시 객체를 message마다 새로 안 만듦; 수신 content-type 못 찾으면 JSON으로 안 되돌리고 `ProtocolError` | 50 §7 | |
| R153 | 수신 content-type은 전달용 metadata가 아니라 codec을 고르는 필수 입력(선택에 안 쓰면 non-JSON payload 구분 못 하고 `ProtocolError`도 발생 안 함) | 50 §7 | |
| R154 | Core receive에서 retain한 record는 payload와 Core receive-credit lease의 shared owner 하나를 가짐; Application shared permit은 각 exact-target child callback의 실제 첫 instruction에서 반환, pre-start terminal child는 정확히 1회 반환; 첫 child enqueue 뒤 remaining child permit은 dispatch loop FIFO를 통해 lazy 확보, 미확보 child payload를 별도 무제한 queue에 복제 안 함 | 50 tail | |
| R155 | Shared retained owner는 모든 child terminal+필요한 record-level reply attempt가 terminal된 뒤 Core lease를 정확히 1회 반환; partial acquire/enqueue 중 cancel·decode failure·close·shutdown 시 미enqueue permit은 즉시 반환, enqueue한 child는 각자의 pre-start·handler-start 경계에서 정리; reply attempt 불필요한 one-way record는 모든 child terminal이 record terminal | 50 tail | |
| — | 확인할 결과 20항(§8 목록) — R132~R155에 이미 반영된 항목과 중복되므로 별도 R 번호 없이 검증 절 작성 시 §8 원문을 입력으로 사용 | 50 §8 | (검증 절에 흡수) |

### 5.8 이관 — session 파일럿의 shared permit 규칙 (R156–R158)

| # | 규칙 | 옛 위치 | 새 위치 |
|---|---|---|---|
| R156 | STREAM application packet과 cross-node Session application record는 같은 host shared permit을 씀, terminal response/error completion만 우회; handshake·bind·unbind·malformed ordinary record도 receive/claim 전 permit을 얻고 내부 처리 직후 반환; application packet은 실제 session callback 시작에서 반환, batch는 확보한 permit보다 많은 job을 만들지 않음 | 옛 `19-stream-session.ko.md` §10 "Session dispatch와 shared permit"(현재는 `04-session/01-stream-session.ko.md` §9 말미의 이관 pointer) | |
| R157 | Session application과 ordinary control은 같은 shared permit을 씀, terminal reply/error completion만 우회; 정확한 acquire/release는 dispatch loop 문서, retained payload는 payload ownership 문서를 따름 | 옛 `48-internal-session-binding.ko.md` 말미 "Session control permit"(현재는 `04-session/02-session-actor-binding.ko.md` §10 말미의 이관 pointer) | |
| R158 | Terminal reply/error completion만 shared permit 우회, 그 밖 ordinary ingress는 receive/claim 전 permit 획득; application permit은 exact-target handler의 실제 첫 instruction에서 반환, queue 게시·task 생성에서 반환 안 함; 이후 await엔 재획득 안 함; capacity wait는 cancellable하며 포화를 reject/drop으로 바꾸지 않음 | 05 §10(이 주제 자신의 사본 — R156·R157과 함께 통합) | |

R156·R157·R158은 서로 다른 표현으로 같은 명제를 말한다. `05-application-job-queue-and-backpressure`
§3 "Ordinary ingress permit 순서"가 이 세 행 모두를 흡수해 하나의 계약 문장으로 만들어야
새 위치가 채워진다 — 세 문장이 새 문서에 각각 살아남으면 §2.5의 "규칙을 덜어내지도 더하지도
않는다" 기준에서 대조 실패로 본다(중복 보존은 실패가 아니지만, 통합 없이 세 벌 그대로
옮기면 S2가 재발한 것이다).

## 6. 링크·코드·site 영향

| 대상 | 처리 |
|---|---|
| `scripts/verify-framework-submit-api.sh` | `05-async-execution-policy.ko.md`를 경로로 읽어 4개 문구를 needle로 검색한다. 이 문구들은 `01-submit-and-completion.ko.md`(R9·R10 근처)로 옮겨야 하며, 재작성 시 **원문 그대로 유지**한다(가이드 §2.5, README §3 요구사항 5). 이동 완료 시점에 스크립트의 경로도 갱신한다 |
| 스펙 내부 링크 | 다른 스펙 문서가 05·33·41·42·43·46·50을 인용하는 자리(94개 anchor 링크 + 경로만 있는 링크)를 새 경로·새 절 anchor로 치환. 절 제목이 바뀌므로 §3의 절 구성표에서 치환표를 만든다 |
| `04-session/01-stream-session.ko.md` §9, `04-session/02-session-actor-binding.ko.md` §10 | 이관 pointer 문구의 대상 경로를 `01-execution/04-application-job-queue-and-backpressure.ko.md#3-ordinary-ingress-permit-순서`로 갱신(§3.6.1) |
| 코드 주석(경로 미참조, 번호만 인용) | `mesh-dispatch-pump.ts`, `m6a-runtime.contract.ts`, `app.cpp`, `test_cpp_framework_application_job_queue.cpp`, `ApplicationJobQueueTests.cs`, `ServiceRuntimeFoundationTests.cs` — 이동 시점에 주석의 옛 번호를 새 슬러그로 갱신(needle 검색이 아니므로 실패 위험은 없으나 문서 추적성을 위해 갱신) |
| mkdocs nav | "Execution" 그룹 → `01-execution/README`, 6개 body 문서 |
| redirect | 캠페인 말미 site 작업에서 옛 경로 7개 → 새 경로로 매핑(1:N — 05는 4개 새 문서로, 46/42는 각 2개 새 문서로 흩어짐. redirect 표는 절 단위가 아니라 문서 단위이므로 가장 비중이 큰 새 문서로 연결하고 나머지는 새 README의 절 목차로 유도) |
| 검증 | `check_doc_links.py`, `mkdocs build --strict`, `git diff --check`, `scripts/verify-framework-submit-api.sh` |

## 7. 구현 대조 — 에이전트 입력

재작성이 끝나면 언어별 에이전트 4개(dotnet·jvm·cpp·node)에 다음을 준다.

- 입력: 새 7개 문서(README 포함)와 §5 대장(새 위치 열을 채운 것)
- 과제: 대장의 행마다 해당 언어 구현에서 **일치 / 불일치 / 스펙 미정 / 판단 불가** 중 하나와
  근거(파일:줄). R105(dispatcher 4,096 상한의 적용 범위)과 R120/R15(owner 점유 예산과 46 §4의
  관계, S15)는 특히 근거를 구체적으로 요구한다
- 금지: 스펙 수정, 코드 수정. 판정은 하지 않고 사실만 보고
- 출력 양식: `| R# | 판정 | 근거 | 비고 |`

판정(Claude): 옛 문서·4개 구현이 일치하는데 새 문서만 다르면 **문서 오류** → 수정. 옛
문서 때부터 달랐거나 언어끼리 다르면 **spec gap** → [spec-gap 대장](../../spec-gap.ko.md)
등록, 문서는 계약 의도대로 유지.

## 8. 작업 순서

1. `01-execution/README.ko.md` 초안(§2 질문표 기준) — 6개 body 문서 소속·중복 판정의 최종 확인
2. `02-handler-turn-and-execution-gate.ko.md` 먼저 재작성(가장 많은 병합·중복 통합이 몰려
   있고 나머지 문서가 이 문서를 링크하는 경우가 많음) → R23·R24·R28-32·R113-131·R88·R89·R90·
   R91·R92·R98-100 새 위치 채움
3. `01-submit-and-completion.ko.md`, `03-cancellation-and-shutdown.ko.md`,
   `04-spot-timer.ko.md`를 병렬로 재작성(서로 겹치지 않음) → 나머지 05 유래 R행 + R103-112
4. `05-application-job-queue-and-backpressure.ko.md` 재작성(S2·S10 통합이 핵심) → R46-71,
   R72-75·R78·R79·R83·R85-87, R93-97·R102, R156-158
5. `06-payload-ownership-and-codec.ko.md` 재작성 → R132-155
6. 등가성 대조 — 대장 빈 행 0, 추가 보장 0. R156-158은 3행이 아닌 통합 문장 1개로 귀결됐는지
   특히 확인
7. `04-session/01-stream-session.ko.md` §9, `04-session/02-session-actor-binding.ko.md` §10의 이관
   pointer를 새 경로로 갱신
8. en 짝 작성
9. 링크 치환·`scripts/verify-framework-submit-api.sh` 경로 갱신·nav → 검증 4종 그린
10. 구현 대조(§7) → 판정·기록

## spec-gap 후보

| # | 위치 | 내용 |
|---|---|---|
| G1 | `43-internal-completion.ko.md` §2("진행 중 operation과 dispatcher에서 대기·실행 중인 callback을 합친 수는 4,096개를 넘지 않는다") | 이 상한의 적용 범위(process 전체인지, host instance 단위인지, 언어별로 다를 수 있는지)를 밝히지 않는다. 재작성에서 새 보장을 만들지 않고 원문 그대로 옮기되(R105), 4언어 구현 대조에서 실제 적용 단위를 근거로 보고받아 결정 대기로 올린다 |
| G2 | `46-internal-dispatch-loop.ko.md` §4(owner 점유 시간 예산, 숫자 없음)와 `41-internal-serialization.ko.md` §2(owner 점유 상한 기본 `10 ms`) | 두 절이 같은 메커니즘을 가리키는지, 46의 일반 원칙이 `PerActor`(각 실행 단위가 이미 분리된 gate를 가짐)에도 같은 숫자로 적용되는지 문면상 명시돼 있지 않다(S15). 재작성 시 46 쪽 절이 41 §2를 링크해 같은 메커니즘임을 표시하되, 새 규칙을 만들지 않고 이 모호성을 판정 대기로 올린다 |
| G3 | `42-internal-progress-isolation.ko.md` §7과 `41-internal-serialization.ko.md` §2 | owner당 두 FIFO(application/lifecycle) 분리를 두 문서가 각각 서술하면서, 42는 우선순위·굶주림 방지 규칙의 소유를 41로 명시하지만(§7 "우선순위와 굶주림 방지 규칙은 41이 설명한다") 41 §2 자체는 이 참조를 받지 않는다 — 소유가 한쪽에서만 선언된 편방향 링크다. 스펙 결함이라기보다 구조 문제(S9)에 가까우나, 재작성 중 두 서술이 정말 같은 메커니즘인지 확인이 끝나기 전에는 spec-gap 후보로 남긴다 |

---

[04-session 매핑표](../04-session/mapping.ko.md) · [캠페인 지침](../../README.ko.md)
