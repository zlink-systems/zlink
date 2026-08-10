# Framework internals 4개 runtime 통합 계획

**상태**: canonical 설계 결정 확정 — 보호 문서 반영과 구현 정렬 전

**작성**: Codex

**대상 runtime 계열**: C++, .NET, JVM(Java/Kotlin), Node

**대상 공개 언어 표면**: C++, .NET, Java, Kotlin, Node

**보호 문서 반영 대상**: `framework/doc/framework/common/spec/`,
`framework/doc/framework/common/internals/`

**후속 구현 대상**: `framework/languages/`, `framework/runtime/protocol/`

**독자와 질문**: common internals를 개정하고 네 runtime 구현을 정렬할 개발자가
“어떤 동작을 어느 internals 문서에 명시하고, 각 runtime에서 무엇을 바꾸며, 무엇으로
완료를 판정하는가”에 답하는 문서다.

**문서 성격**: 1.2와 5장의 canonical 결정은 구현이 도달해야 할 설계상 모델이다. 현재
구현 상태는 5장의 `MATCH`/`LANGUAGE-MAPPING`/`DIVERGED`/`UNVERIFIED` 열과 WP1 ledger가
구분해 기록한다.

> 이 문서의 IC-01~IC-14는 이후 구현자가 다시 선택하지 않는 canonical 설계 결정이다.
> 아직 공통 spec/internals의 효력을 바꾸거나 구현 완료를 주장하지는 않는다. 보호 문서에는
> 이 결정을 의미 변경 없이 옮기고, 현재 구현과 다른 부분은 gap으로 추적한다. `CONFIRMED`는
> 구현자의 언어별 재량을 닫았다는 뜻이지 증거 gate를 생략한다는 뜻이 아니다. 착수 전
> 검증에서 반증이 나오면 구현자가 대안을 고르지 않고 보호 문서 변경 심사로 돌아간다.

## 1. 문제와 목표

같은 Framework 기능이 언어마다 다른 상태 기계, 순서, 경계값, 실패 처리로 구현되면
공개 API 모양이 같아도 실행 결과는 같지 않다. 한 runtime에서 고친 경쟁 조건이나
재시도 결함을 다른 runtime에서 다시 발견하게 되고, cross-language 조합은 특정 순서나
부하에서만 실패할 수 있다.

현재 internals에는 표현 수단을 언어별 재량으로 둘 수 있는 항목과, 문서가 결정하지 않아
구현이 자연스럽게 갈라진 항목이 함께 존재한다. 실제 조사에서는 다음과 같은 차이가
확인되었다.

- serial scheduler의 메시지 수 한도, byte 한도, lifecycle burst, owner time budget이
  runtime마다 다르다.
- Node의 같은 owner 중첩 실행은 inline reentrancy를 허용하지만, C++과 JVM의 대응 경로는
  다시 대기열에 넣어 현재 handler 안으로 재진입하지 않는다.
- runtime observation은 .NET, JVM, C++, Node가 서로 다른 보존·합치기·thread 자원 모델을
  사용한다.
- session relocation은 seal, accepted high-water, ingress 차단, restart fallback의 의미가
  runtime마다 완전히 같다고 보기 어렵다.
- Message Follow의 정확한 suppression key와 terminal 보존 책임이 일부 구현에서는 하나의
  큰 모듈에 함께 들어 있다.

기대하는 결과는 모든 언어가 같은 문법과 class 배치를 사용하는 것이 아니다. 다음
항목이 같아야 한다.

1. 같은 입력과 상태에서 같은 성공·거부·실패 결과를 낸다.
2. 같은 상태 전이, 선형화 지점, ordering, boundedness, retry, terminal 규칙을 사용한다.
3. 같은 wire command와 fence identity를 해석한다.
4. 같은 부하에서 같은 논리 한도와 loss accounting을 적용한다.
5. restart, timeout, duplicate, stale message에서 같은 안전 보장을 유지한다.
6. 언어 binding 때문에 달라지는 복사·thread·callback 표면은 허용하되, 그 차이가 공통
   의미를 바꾸지 않는다는 검증 기준을 가진다.

이 기준이 없으면 사용자가 보게 되는 실패는 “어느 언어를 선택했는가” 또는 “어떤 두
언어를 연결했는가”에 따라 달라진다. 이 계획의 목표는 이러한 차이를 전수 조사하고,
POSD/DDD 관점에서 하나의 canonical logical design을 선택한 뒤, 문서와 구현을 순서대로
정렬하는 것이다.

### 1.1 실제 통합 범위

이 계획은 네 runtime의 다음 책임을 통합한다.

- **실행 queue와 admission**: 용량, byte accounting, fairness, 재진입, wakeup을 같게 한다.
- **관측 queue**: 중간 상태 합치기, terminal 보존, loss counter와 실행 자원 상한을 같게 한다.
- **relocation과 Message Follow**: seal, fence, ACK high-water, retry와 중복 억제를 같게 한다.
- **operation completion**: reply/timeout/cancel/close 중 하나만 terminal winner가 되게 한다.
- **객체 종류와 runtime 상태**: Entry/User/Instance의 불변식과 public readiness authority를
   같게 한다.
- **payload·codec·계층 책임**: 소유권, copy/deserialize 상한, codec 선택과 adapter 경계를
   같게 한다.
- **비대칭 분기 관리**: 한 언어에만 있는 거부·retry·fallback을 재량으로 남기지 않는다.

### 1.2 internals 명시와 구현 통일 항목

아래 표가 **현재 확정된 문서·구현 작업 목록**이다. 각 행은 언어별로 결과가 달라지는
상황, internals에 적을 기대 동작, 구현이 맡을 책임과 차이가 남았을 때 보이는 실패를 함께
보여 준다. `internals 반영 위치`의 한국어·영어 문서를 같은 patch에서 바꾸고,
`네 runtime에서 통일할 구현`을 production과 test에 반영한다. 5장은 각 결정의 근거와
금지 변형을 설명하며 canonical 값의 원본이다. 이 표는 독자가 변경 범위를 한 곳에서
판단하도록 같은 값을 요약한다. IC를 바꿀 때는 이 표와 5장의 결정 표를 함께 고치고,
WP3 checker가 ID와 canonical 값의 불일치를 거부한다.

```text
IC 결정
  ├─ common internals 한국어·영어 결정문
  ├─ C++/.NET/JVM/Node production owner
  ├─ 공통 scenario fixture와 언어별 white-box test
  └─ package·전체 회귀·sample·필요한 cross-language/E2E 증거
```

| ID | 현재 상황과 통합 대상 | internals 반영 위치 | 네 runtime에서 통일할 기대 동작 | 차이가 남으면 보이는 실패 |
|---|---|---|---|---|
| IC-01 | C++/JVM은 application 1,024건을, .NET/Node는 4,096건을 사용하고 lifecycle 한도와 turn budget도 다르다. | `02-serialization`, `03-progress-isolation`, `07-dispatch-loop`, `08-object-lifecycle` | application 1,024건/64 MiB, lifecycle 128건/4 MiB, owner 10 ms, burst 8, fixed cost 256 byte를 공통 fixture로 사용한다. .NET/Node 값을 정렬하고 C++/JVM의 실제 소비 지점을 검증한다. | 같은 부하에서 local Request/control의 `CapacityExceeded`, remote Request/control의 `Unavailable`, local Send의 `DeadlineExceeded`, 이미 완료된 remote Send의 metric·log·trace 기록 시점과 peak memory·overload latency가 달라진다. |
| IC-02 | Node는 같은 owner의 중첩 호출을 inline으로 실행할 수 있고, C++/JVM 대응 경로는 새 turn을 사용한다. | `02-serialization`, `07-dispatch-loop` | implicit inline 실행을 허용하지 않는다. 중첩 wait는 explicit yield/release 뒤 새 turn에서 resume하며, yield 없는 자기 queue wait는 같은 오류로 거부한다. | 같은 handler가 언어에 따라 재진입하거나 대기해 ordering, deadlock과 예외 결과가 달라진다. |
| IC-03 | runtime마다 application/lifecycle queue의 물리 분리, 실행 자원 공급과 wakeup 방식이 다르다. | `03-progress-isolation`, `07-dispatch-loop` | owner마다 application/lifecycle 물리 FIFO와 독립 admission state를 두고 process-wide 실행 자원을 주입한다. empty→non-empty는 즉시 signal/callback으로 깨운다. | application 포화가 lifecycle을 막거나 owner 수에 따라 thread/executor가 늘고, polling 주기만큼 처리가 늦어진다. |
| IC-04 | admission의 state 확인, capacity 예약, sequence 발급과 enqueue가 같은 commit인지 네 call path의 검증이 끝나지 않았다. | `03-progress-isolation`, `07-dispatch-loop` | state 확인, count·byte 예약, sequence 발급, enqueue commit을 한 선형화 지점에서 처리한다. 거부·enqueue 실패는 capacity와 authority를 전진시키지 않는다. | 거부된 작업이 capacity를 소비하거나 sequence·authority를 전진시켜 이후 정상 작업이 실패한다. |
| IC-05 | .NET/JVM/C++/Node가 중간 상태 합치기, terminal 보존과 subscriber 실행 자원을 서로 다르게 관리한다. | `10-liveness-and-state` | subscriber별 source-latest intermediate와 terminal FIFO 64, shared dispatcher, intermediate/terminal loss counter를 사용한다. | 느린 observer에서 terminal event가 사라지거나 순서와 loss metric이 달라지고 subscriber 수만큼 thread가 증가한다. |
| IC-06 | relocation 구현의 seal, accepted high-water, ingress 차단, duplicate와 restart 처리가 하나의 공통 상태 기계로 입증되지 않았다. | `05-relocation-continuity`, `09-session-binding`, `12-service-wire-protocol` | command 42/43이 exact fence와 accepted high-water로 capture/replay 장벽을 만든다. Target은 restore·queue merge·dispatch switch 뒤 application admission과 Ready를 먼저 연다. command 44/45 route switch·ACK는 `Completed` 단계의 비동기 수렴이며 Ready를 막지 않는다. | 이동 중 수락한 message가 유실·중복되거나 stale session ingress가 새 owner에 전달되고, route ACK 지연 때문에 정상 target admission까지 멈춘다. |
| IC-07 | Message Follow의 suppression key와 reply/completion 책임이 runtime마다 다른 모듈에 섞여 있다. | `05-relocation-continuity`, `06-routing-and-cache`, `12-service-wire-protocol` | exact route fence 전체를 key로 쓰는 전용 registry에서 `idle/inFlight/sentUntilExpiry`를 관리한다. stale fence, expiry와 target별 multicast identity를 같은 방식으로 처리한다. | 같은 알림이 반복 전송되거나 이전 fence의 억제 기록이 새 target 알림까지 막는다. |
| IC-08 | reply, timeout, cancellation과 close가 만날 때 terminal winner와 callback dispatch 경계의 call path 검증이 끝나지 않았다. | `04-completion`, `12-service-wire-protocol` | wire `OperationId`의 `{high:u64, low:u64}` identity와 별도 `ReplyRouteId`를 보존한다. 완료 표에서 entry를 원자적으로 가져간 경로만 terminal winner가 되고 callback/promise/coroutine 완료는 lock 밖에서 dispatch한다. | 축소한 ID가 다른 operation과 충돌하거나 한 operation이 두 번 완료되며, lock 안 callback 재진입으로 교착 상태가 생긴다. |
| IC-09 | Entry/User/Instance의 차이를 일부 runtime이 tag와 조건 분기로 표현해 invalid kind/state 조합을 만들 수 있다. | `08-object-lifecycle` | 세 종류를 invalid 조합을 만들 수 없는 domain variant로 표현한다. activation, relocation, idle close와 destroy 규칙은 종류별 aggregate가 소유한다. | 이동하지 않는 종류가 relocation되거나 유휴 정리 대상이 아닌 객체가 닫히는 등 lifecycle 결과가 달라진다. |
| IC-10 | binding ingress부터 handler/accessor까지 payload copy, deserialize와 release 횟수의 검증이 끝나지 않았다. | `02-serialization`, `11-message-ownership` | admission 뒤 Framework가 payload를 한 번 소유한다. binding 경계 copy 최대 1회, accessor copy 0, deserialize 최대 1회와 모든 terminal 경로의 release exactly-once를 계측한다. | handler 반환 뒤 buffer를 참조하거나 double release가 발생하고, 언어에 따라 copy 비용과 peak memory가 달라진다. |
| IC-11 | JNI binding adapter와 raw backend port는 책임이 다르지만 언어별 directory와 module 경계가 달라 비교가 어렵다. | `01-layering` | public adapter → use case → domain owner → backend port dependency를 고정한다. 이름을 기계적으로 맞추지 않고 의미 없는 pass-through wrapper만 제거한다. | domain이 concrete binding에 의존하거나 같은 정책이 여러 wrapper에 복제되어 수정 결과가 언어별로 달라진다. |
| IC-12 | runtime마다 송신 codec 선택 기준, 수신 fallback과 선택 결과 cache가 다르다. | `11-message-ownership`, `12-service-wire-protocol` | 송신은 호출 지점에 선언된 message type으로 selector를 평가하고, 여러 조건이 맞으면 나중에 등록한 조건을 우선한다. 수신은 normalized content-type으로 고르며 unknown content-type은 `ProtocolError`다. immutable registry와 bounded send-type cache 1,024를 사용한다. | 같은 호출이 전달한 subtype instance에 따라 다른 codec을 쓰거나 selector 우선순위가 바뀌며, unknown payload의 잘못된 JSON 해석과 type cache 증가가 발생한다. |
| IC-13 | public 7-state 값은 같지만 별도 ready cell과 maintenance/discovery 상태를 public authority처럼 읽는 경로가 남아 있다. | `10-liveness-and-state` | public host 7-state를 단일 authority로 두고 host `IsReady = state == Serving`으로 계산한다. topology `IsReady`는 host readiness와 ready target을 결합한 별도 projection이며, maintenance/discovery/admission 상태는 단방향 context projection으로만 유지한다. | 같은 host를 한 API는 ready로, 다른 API는 not-ready로 보고 traffic을 너무 일찍 받거나 계속 제외한다. |
| IC-14 | 한 runtime에만 있는 거부, retry, fallback과 side effect가 공통 결정인지 구현 gap인지 분류되지 않은 항목이 있다. | `internals/README`, `01`~`12` 관련 장 | 한쪽 전용 branch를 공통 결정, 입증된 `LANGUAGE-MAPPING`, 또는 명시적 gap 중 하나로 분류한다. | 문서가 같은 기능이라고 표시해도 특정 언어에서만 retry하거나 authority·metric을 바꾸는 숨은 차이가 남는다. |

이 목록 밖의 문법, private 이름, mutex/event-loop primitive와 파일 배치는 통일 대상이 아니다.
반대로 WP1에서 사용자 관찰 결과, wire, ordering, bound 또는 failure가 달라지는 새 항목을 찾으면
기존 행에 숨기지 않고 IC-15 이후의 새 결정 행, internals 반영 위치와 검증 기준을 먼저 추가한다.
각 행은 다음 증거가 모두 있을 때만 닫힌다.

- 한국어·영어 internals에 state/algorithm/bound/금지 변형이 명시되어 있다.
- C++/.NET/JVM/Node production call path가 같은 결정을 소비한다.
- 공통 scenario fixture와 언어별 white-box test가 성공·실패·race를 검증한다.
- 영향받는 package, 전체 회귀, 35개 sample과 필요한 cross-language/E2E lane이 통과한다.

## 2. 범위와 제외 범위

### 2.1 포함 범위

- common internals 12개 장의 결정, 재량, 미규정 구간
- common spec 및 exact interface와 internals 사이의 의미 일치
- C++, .NET, JVM, Node의 production source, white-box test, package 경로
- Java와 Kotlin의 공개 API 표면 및 JVM runtime 공유 경계
- service wire schema, generated asset, golden fixture, cross-language decoder
- concurrency, lifecycle, ownership, retry, restart, resource bound
- package/clean-consumer, aggregate test, 7종 sample 실제 process, 영향 기반 E2E,
  성능·자원 증거

### 2.2 제외 범위

- 문법, naming convention, 파일 개수의 기계적 통일
- C++의 mutex, .NET의 `lock`, JVM의 `synchronized`, Node event loop처럼 언어 실행
  모델이 정하는 primitive의 통일
- exact interface 승인 없이 공개 API 모양을 변경하는 작업
- 공통 의미를 바꾸지 않는 binding 내부 최적화를 동일한 코드로 만드는 작업
- 좁은 unit test 통과만으로 cross-language 완료를 선언하는 작업

### 2.3 통합 경계

이 계획은 모든 언어가 같은 class, directory와 실행 primitive를 사용하게 만들지 않는다.
trace thunk, mutex/event loop, callback/Flow처럼 관찰 결과가 같은 표현은
`LANGUAGE-MAPPING`으로 유지한다. 각 mapping에는 결과가 같은 이유와 검증 기준을 기록한다.

상태 전이, ordering, failure, wire, latency와 resource bound를 바꾸는 정책은 언어별
mapping으로 인정하지 않는다. Java의 JNI binding adapter와 raw backend port, Node의
`infrastructure` execution area와 serial `lifecycle` lane처럼 책임이 다른 개념도 이름을
맞추기 위해 합치지 않는다.

## 3. 통합 판정 기준

각 조사 행에는 다음 **현재 구현 상태** 중 하나만 부여한다. 이 값은 canonical 결정의
확정 여부가 아니다. 이 문서의 canonical 결정은 모두 확정되어 있으며, `UNVERIFIED`는
구현 call path 증거가 부족하다는 뜻으로만 사용한다. 다만 부족한 증거가 결정의 전제를
반박하면 gap을 억지로 만들지 않고 G2의 결정 무결성 심사로 돌아가 before/after 의미와
migration을 다시 승인받는다.

| 상태 | 의미 | 후속 처리 |
|---|---|---|
| `MATCH` | 논리 동작과 실패 의미가 같다 | 공통 contract test로 고정 |
| `LANGUAGE-MAPPING` | primitive/API 표면만 다르고 관찰 결과는 같다 | 허용 사유와 검증 기준을 internals에 기록 |
| `DIVERGED` | 상태 전이, 순서, 한도, 실패 또는 자원 특성이 다르다 | 이 문서의 확정 결정으로 구현 정렬 |
| `UNVERIFIED` | 문서·식별자만 확인했고 실제 경로 증거가 부족하다 | source call path와 runtime test 추가 |

“같다”는 symbol 존재나 테스트 이름으로 판정하지 않는다. 각 항목은 최소한 다음 증거를
연결해야 한다.

- 공통 spec 또는 exact interface의 요구사항
- 실제 production call path와 상태 소유자
- 성공, 거부, timeout, duplicate, restart의 test
- package를 소비하는 clean consumer 증거
- wire 항목이면 schema와 양방향 golden fixture
- process 경계 항목이면 실제 client/server process 로그와 종료 상태

## 4. POSD/DDD 선택 원칙

통합안은 다수결로 선택하지 않는다. 한 언어 구현 전체를 기준으로 삼을 수도 있고, 각
구현의 잘된 부분을 조합할 수도 있다. 다만 조합 결과가 새로운 책임 혼합을 만들지 않도록
항목마다 최소 두 설계를 비교하고 다음 gate를 통과해야 한다.

### 4.1 POSD gate

- **Deep module**: 호출자는 정책의 세부 순서나 보상 작업을 알지 않아야 한다.
- **Information hiding**: 상태와 불변식은 한 owner가 관리하고 외부에는 작은 계약만
  노출한다.
- **Complexity downward**: retry, coalescing, fence 검증, loss accounting은 호출자에게
  전파하지 않고 담당 모듈 안으로 내린다.
- **Error by construction**: stale/invalid 조합을 표현하거나 commit할 수 없게 만든다.
- **Pass-through 제거**: 의미를 추가하지 않는 wrapper와 동일한 정책의 중복 구현을
  만들지 않는다.
- **시간 순서가 아닌 책임 분해**: prepare/commit/cleanup이라는 호출 순서만으로 class를
  나누지 않고, 상태 소유권과 불변식으로 경계를 정한다.

### 4.2 DDD gate

- bounded context의 용어를 하나로 정한다. 예: `application`/`lifecycle`, seal,
  accepted high-water, terminal observation.
- aggregate root가 lifecycle과 authority 전이를 단독으로 소유한다.
- entity identity에는 node RID, object generation, authority fence처럼 stale 여부를
  결정하는 모든 값이 포함된다.
- value object는 불완전한 fence나 token 조합을 만들 수 없게 한다.
- terminal state는 한 번만 결정되고, duplicate/retry는 idempotent 결과를 돌려준다.
- 다른 bounded context로 넘기는 wire payload는 내부 journal object가 아니라 canonical
  contract object다.

### 4.3 선택 기록 형식

각 결정에는 다음 내용을 남긴다.

1. 현재 네 runtime의 방식
2. 설계 A와 B, 필요한 경우 조합안 C
3. POSD/DDD gate별 장단점
4. 선택한 canonical logical design
5. 금지할 변형과 허용할 language mapping
6. migration 비용과 호환성 영향
7. 결정을 뒤집을 수 있는 새 증거

### 4.4 구현 모델에 넘길 확정 수준

고추론 모델을 사용하더라도 모든 파일, class, 함수 호출 순서까지 이 계획에서 미리 고정하지
않는다. 그렇게 하면 조사 뒤 더 깊은 module 경계가 발견돼도 얕은 구조를 복제하게 된다.
반대로 “알아서 통합”만 남기면 언어별 정책 선택이 다시 생긴다. 다음 경계를 실행 계약으로
사용한다.

**사전에 확정할 것**

- 사용자 관찰 결과와 error category
- aggregate/state owner, state transition과 선형화 지점
- queue/resource bound, ordering, retry/idempotency와 terminal winner
- wire field, identity/fence, backward compatibility
- forbidden variant와 허용 가능한 language mapping
- package, 전체 회귀, 35개 sample, 선택 E2E의 판정 기준

**구현 모델이 선택할 수 있는 것**

- mutex/CAS/event loop/structured concurrency 같은 언어 primitive
- 위 책임 경계를 보존하는 내부 class·file 분할과 private naming
- 공개 결과와 자원 상한을 바꾸지 않는 allocation/cache 최적화
- 공통 fixture를 각 test framework에 표현하는 문법

**다시 승인받아야 하는 것**

- public API, wire, canonical bound, failure/ordering 의미의 변경
- implicit fallback, skip, test-only path 또는 package source fallback 추가
- 하나의 IC를 만족하기 위해 다른 IC의 owner나 불변식을 바꾸는 조합
- 5장의 착수 전 증거 gate가 canonical 결정의 전제를 반박한 경우

따라서 실행 지시는 “구현 세부를 그대로 복사하라”가 아니라 “확정된 logical design과
acceptance criteria를 만족하는 가장 깊고 작은 언어별 module을 선택하라”로 작성한다.

## 5. 확정한 canonical 결정

아래 결정은 모두 `CONFIRMED`다. source audit에서 새 차이가 나오면 구현 gap을 추가할 뿐,
각 언어 구현자가 다른 정책을 다시 고르지 않는다. 결정을 바꾸려면 common contract의
before/after 의미와 파생 보장, migration을 포함한 별도 변경 승인이 필요하다.

| ID | 영역 | 확정한 canonical 결정 | 현재 구현 상태 |
|---|---|---|---|
| IC-01 | serial limit | application 1,024건/64 MiB, lifecycle 128건/4 MiB, owner 10 ms, burst 8, fixed cost 256 byte로 고정 | `DIVERGED` |
| IC-02 | serial reentrancy | 같은 owner의 implicit inline 실행을 금지하고 explicit yield 뒤 새 turn으로만 resume | `DIVERGED` |
| IC-03 | progress lane | owner마다 `application`/`lifecycle` 두 FIFO와 독립 admission state를 두고 process-wide 실행 자원을 주입하며 즉시 signal/callback으로 깨움 | `DIVERGED` |
| IC-04 | admission | state check, count·byte reserve, sequence 할당, enqueue를 하나의 선형화 지점에서 처리 | `UNVERIFIED` |
| IC-05 | observation | subscriber별 source-latest map + terminal FIFO, 기본 64, shared dispatcher, 두 loss counter로 고정하고 push/pull은 소비 표면으로만 허용 | `DIVERGED` |
| IC-06 | relocation seal | command 42/43 exact seal·high-water 장벽과 Ready 후 command 44/45 route 비동기 수렴, sealed ingress, duplicate idempotency를 단일 상태 기계로 고정 | `DIVERGED` |
| IC-07 | Message Follow | exact route-fence key의 전용 suppression registry와 `idle/inFlight/sentUntilExpiry` 상태를 사용 | `DIVERGED` |
| IC-08 | completion | wire 128-bit `OperationId`와 별도 `ReplyRouteId`, 완료 표 entry의 단일 atomic take와 lock 밖 completion dispatch를 사용 | `UNVERIFIED` |
| IC-09 | object kind | Entry/User/Instance를 서로 다른 domain variant로 만들고 wire tag는 domain 분기에 사용하지 않음 | `DIVERGED` |
| IC-10 | payload ownership | admission 뒤 Framework 단일 owner, binding 경계 최대 1회 copy, accessor copy 0, deserialize 최대 1회 | `UNVERIFIED` |
| IC-11 | layering | 공통 responsibility/dependency graph를 고정하고 JNI binding adapter와 raw backend port는 별도 context로 유지 | `LANGUAGE-MAPPING` |
| IC-12 | codec | 송신은 호출 지점에 선언된 message type과 나중 등록 우선 selector, 수신은 normalized content-type registry로 선택하고 결과를 bounded cache | `DIVERGED` |
| IC-13 | runtime state | public host 7-state를 단일 authority로 두고 host `IsReady = state == Serving`; topology `IsReady`는 host readiness와 ready target의 별도 projection이며 그 밖의 축약 projection은 단방향·context 전용 | `DIVERGED` |
| IC-14 | asymmetric branch | 관찰 가능한 한쪽 전용 분기는 공통 결정·입증된 mapping·명시적 gap 중 하나로 반드시 분류 | `UNVERIFIED` |

`CONFIRMED`와 구현 증거의 충분성은 별도 축이다. 다음 항목은 WP3 보호 문서 변경 전에 아래
증거 gate를 반드시 닫는다. 실패하면 해당 결정을 언어 구현에 강행하지 않고 G2로 되돌린다.

| 결정 | 착수 전 증거 gate | 실패 시 처리 |
|---|---|---|
| IC-01 | .NET/Node의 4,096 기준과 canonical 1,024 기준을 같은 workload로 비교해 throughput, p95/p99 latency, peak retained bytes, rejection 지점을 기록 | 기존 공개 SLA 또는 합의한 자원 bound를 어기면 수치 결정 재심사 |
| IC-02 | production, test, sample, clean consumer에서 같은 owner의 nested awaited call을 전수 조사하고 explicit yield/resume migration test 작성 | 합법적 호출을 호환 가능한 표면으로 옮길 수 없으면 no-inline 결정 재심사 |
| IC-04 | state check부터 enqueue commit까지 네 runtime call path와 거부 보상 test 확보 | 하나의 선형화 지점으로 만들 수 없는 binding 제약을 기록하고 재심사 |
| IC-08 | reply/timeout/cancel/close race에서 terminal winner와 lock 밖 dispatch를 재현 | 단일 winner가 공개 completion 의미를 깨면 terminal contract 재심사 |
| IC-10 | binding ingress부터 handler/accessor까지 copy/deserialize counter 계측 | lifetime 안전성과 copy 상한을 동시에 만족하지 못하면 ownership contract 재심사 |
| IC-14 | 12개 장의 asymmetric branch inventory와 분류 근거 완성 | 미분류 분기가 있으면 WP3 진입 금지 |

### 5.1 현재 source 근거

다음은 2026-08-10 checkout에서 직접 확인한 결정 근거다. 행 번호는 조사 기준선을
식별하기 위한 것이며, WP0에서 대상 SHA와 함께 다시 고정한다.

| 근거 | production source | 확인 내용 |
|---|---|---|
| EV-01 | `framework/languages/cpp/framework/src/runtime/dispatch/dispatch_limits.hpp:10` | C++ application/control count·byte, 10 ms, burst 8, fixed cost 256 |
| EV-02 | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/execution/ZLinkAsyncSerialQueue.java:19` | JVM이 C++과 같은 기본 count·byte·time·burst를 사용 |
| EV-03 | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs:5` | .NET은 4,096/256건, burst 32와 단일 `_admissionGate`를 사용 |
| EV-04 | `framework/languages/node/packages/framework/src/runtime/execution/serial-scheduler.ts:20` | Node는 4,096/1,024건, application 16 MiB, 50 ms를 사용 |
| EV-05 | `framework/languages/node/packages/framework/src/runtime/spots/spot-serial-executor.ts:81` | 같은 Node executor turn의 `execute`가 operation을 다시 enqueue하지 않음 |
| EV-06 | `framework/languages/cpp/framework/src/runtime/diagnostics/runtime_observation.hpp:29` | C++ observer state가 observer별 worker thread를 생성 |
| EV-07 | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Diagnostics/ZLinkObservationQueue.cs:7` | .NET latest intermediate, terminal FIFO, ordering, loss accounting |
| EV-08 | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/monitoring/ZLinkStatusPublisher.java:18` | JVM subscriber별 bounded queue와 shared dispatcher |
| EV-09 | `framework/languages/node/packages/framework/src/runtime/diagnostics/topology-runtime-projections.ts:286` | Node queue는 capacity를 검증하지만 pending/terminal slot이 각각 하나임 |
| EV-10 | `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:1137` | C++ seal/ACK exact fence와 ACK high-water encoding·strict decode |
| EV-11 | `framework/languages/node/packages/framework/src/runtime/streams/actor-session-binding-registry.ts:155` | Node accepted high-water, sealed ingress, exact fence, idempotent seal |
| EV-12 | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkActorMessageFollower.cs:132` | .NET Message Follow key가 object/node/authority/lease generation을 포함 |
| EV-13 | `framework/languages/cpp/framework/include/zlink/framework/contracts/configuration/lifecycle.hpp:11` | C++ public runtime state가 `preparing`부터 `error`까지 0..6을 사용 |
| EV-14 | `framework/languages/dotnet/src/Zlink.Framework/Contracts/Configuration/ZLinkDrainContracts.cs:3` | .NET public runtime state가 같은 일곱 값과 번호를 사용 |
| EV-15 | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntimeState.java:3` | JVM public runtime state가 같은 일곱 값과 번호를 사용 |
| EV-16 | `framework/languages/node/packages/framework/src/contracts/Locations/Rows.ts:11` | Node public runtime state가 같은 일곱 값과 번호를 사용 |
| EV-17 | `framework/languages/cpp/framework/src/runtime/stateful/maintenance_runtime.hpp:542` | C++ maintenance 내부에는 `retiring`을 포함한 별도 6-state가 있음 |
| EV-18 | `framework/languages/node/packages/framework/src/runtime/foundation/service-discovery-registry.ts:1` | Node discovery descriptor에는 `retiring`·`disconnected`를 포함한 전용 상태가 있음 |
| EV-19 | `framework/languages/node/packages/framework/src/runtime/execution/index.ts:27` | Node `application`/`infrastructure` execution area는 serial scheduler lane과 별도 context임 |
| EV-20 | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/host/ZLinkFrameworkRuntime.java:130` | JVM은 7-state와 별도 `ready` gate를 함께 보유하며 status와 public method의 소비 방식이 다름 |

EV-05는 Node가 잘못되었다는 근거만으로 사용하지 않는다. nested wait의 호환성을 test하고,
현재 inline 호출은 확정한 no-inline + explicit yield/resume으로 이행한다. 기존 API가
명시적 yield를 표현하지 못하면 exact interface에 그 표면을 추가한다. EV-09도 public observation
종류마다 source retention owner가 다를 수 있으므로 전체 호출 경로를 확인하기 전까지
Node observation 전체의 판정으로 확대하지 않는다.

## 6. 핵심 영역별 POSD/DDD 통합 방향

### 6.1 Serial execution과 progress isolation

#### 현재 차이

- C++의 `dispatch_limits.hpp`와 JVM의 `ZLinkAsyncSerialQueue`는 application 1,024건,
  lifecycle 128건, 각각 64 MiB/4 MiB, lifecycle burst 8, owner budget 10 ms,
  고정 work cost 256을 사용한다.
- .NET `ZLinkSerialExecutionQueue`는 application 4,096건, lifecycle 256건,
  lifecycle burst 32와 C++/JVM과 같은 owner budget 10 ms를 사용한다.
- Node `serial-scheduler.ts`는 application 4,096건/16 MiB, lifecycle 1,024건/4 MiB,
  owner budget 50 ms, lifecycle burst 8을 사용한다.
- C++은 active handler 뒤의 continuation을 별도 phase에 재배치하고 inline으로 실행하지
  않는다. JVM도 같은 owner 상태를 확인하더라도 명시적 yield 경로를 사용한다.
- Node `spot-serial-executor.ts`는 같은 executor turn에서 중첩 호출을 Promise microtask로
  inline 실행할 수 있다.

#### 비교안

| 안 | 설명 | 판단 |
|---|---|---|
| A | 각 runtime의 현재 수치와 reentrancy를 유지하고 결과 test만 추가 | 같은 부하에서 거부 시점과 stack/ordering이 달라져 제외 |
| B | C++ 구현 전체를 각 언어에 그대로 모사 | 중앙 한도와 phase 모델은 좋지만 thread primitive까지 모사하면 language mapping을 침해 |
| C | C++·JVM의 논리 값과 no-inline 규칙을 선택하고 실행 자원은 process-wide로 주입 | 공통 의미와 자원 scaling을 함께 고정하므로 채택 |

#### 수치 선택 근거와 호환성 gate

1,024를 선택한 이유는 C++ 구현을 다수결 기준으로 삼아서가 아니다. queue count는 처리량을
만드는 worker 수가 아니라 이미 수락했지만 아직 처리하지 못한 backlog의 상한이다. 같은 drain
rate에서 4,096은 정상 처리량을 높이지 않고 최악의 retained work와 overload tail만 네 배까지
늘릴 수 있다. 현재 두 runtime이 이미 사용하는 1,024/64 MiB 조합은 네 runtime이 공통으로
만족할 수 있는 더 작은 bounded admission 계약이고, lifecycle 128/burst 8은 application
starvation과 shutdown work의 점유 상한을 더 엄격하게 제한한다.

이 논리만으로 실제 workload 영향을 가정하지 않는다. WP1에서 .NET/Node의 현재 값과 canonical
값을 같은 process 수, payload 분포, producer rate, worker 수로 비교한다. 기존 공개 SLA가 있으면
그 SLA가 우선한다. 명시된 SLA가 없으면 반복 측정의 분산을 함께 기록하고, steady-state
throughput 저하가 기준선 대비 5%를 넘거나 p99 latency가 악화되는데 peak retained bytes·overload
recovery에서 상응하는 개선이 없으면 G2로 돌아가 수치를 재심사한다. 의도한 조기 내부
admission 거부는 그 자체만으로 실패로 판정하지 않는다. public `CapacityExceeded`·
`Unavailable`·`DeadlineExceeded` 매핑과 send completion 시점을 함께 기록하여 새 admission
contract의 migration 영향을 판정한다.

#### 확정 결정

- logical lane은 `application`과 `lifecycle` 두 개다. 각 owner는 두 lane의 FIFO와
  admission state를 물리적으로 분리해 소유한다. 하나의 priority queue에 tag만 붙이는
  구현은 사용하지 않는다.
- 기본 한도는 application 1,024건/64 MiB, lifecycle 128건/4 MiB, owner turn 10 ms,
  lifecycle 연속 burst 8, payload가 없는 work의 고정 비용 256 byte다. 한도 변경은 네
  runtime과 공통 fixture를 동시에 바꾸는 contract 변경이다.
- lifecycle starvation을 막는 debt와 burst 규칙을 공통 상태 기계로 정의한다.
- 실행 자원은 process-wide bounded pool 또는 언어 runtime의 단일 event loop에서 공급한다.
  owner를 전용 thread/executor에 고정하지 않으며, 연속 turn이 같은 worker에서 실행되는 것은
  비계약적 locality 최적화일 뿐 ordering·fairness·한도에 영향을 줄 수 없다. scheduler는
  필요할 때 다른 shared worker로 handoff할 수 있어야 한다. C++의 shared offload executor,
  JVM의 process execution lane, .NET task runner, Node event loop는 이 원칙의 language
  mapping이다.
- empty에서 non-empty가 되는 ready-set 전이는 실행 자원을 즉시 signal하거나 event-loop
  callback을 예약한다. 주기적 polling을 scheduler 정확성이나 정상 지연 경로로 사용하지
  않는다. signal coalescing은 허용하지만 깨어난 쪽은 ready-set을 다시 읽어 missed wakeup을
  없애야 한다.
- active handler가 만든 후속 작업은 queue 뒤 또는 명시적 after-active phase로 이동한다.
  after-active phase는 application lane의 같은 count·byte reservation과 sequence domain을
  사용한다. 별도 무제한 queue가 아니다. 앞 삽입과 암묵적 inline reentrancy는 금지한다.
- 중첩 wait가 필요한 API는 owner turn을 명시적으로 yield/release하고, 완료 뒤 새 turn으로
  resume한다. yield 없이 현재 owner의 queued 결과를 기다리는 호출은 즉시
  `InvalidOperation`으로 실패한다.
- C++ `dispatch_limits.hpp`처럼 논리 한도를 한 모듈이 소유한다. 동일 값은 공통 contract
  fixture에서도 읽거나 대조한다.

IC-02는 즉시 breaking behavior로 투입하지 않는다. WP1에서 handler가 같은 owner의 queued
결과를 `await`하는 모든 public call path를 찾고, exact interface에 explicit yield/resume이
없으면 먼저 추가한다. sample과 clean consumer를 새 표면으로 옮기고 migration 경로와
nested-wait 회귀 test를 확보한 뒤 implicit inline을 제거한다. 합법적 호출을 이 경로로
표현할 수 없으면 14장의 중단 조건에 따라 구현을 멈추고 결정을 재심사한다.

#### 파생 보장

- 같은 입력 순서와 work cost에서 네 runtime의 처리 순서와 거부 지점이 같다.
- handler가 자기 queue로 다시 들어와 lifecycle 불변식을 중간 상태에서 관찰하지 않는다.
- owner 수가 늘어도 OS thread 또는 executor 수가 owner 수에 선형 비례해 늘지 않는다.

### 6.2 Atomic admission과 bounded accounting

#### 비교안

- 안 A: count와 byte 검사를 호출자와 queue가 나누어 수행한다.
- 안 B: 하나의 admission gate 안에서 상태 확인, count/byte 예약, enqueue를 한 선형화
  지점으로 처리한다.

#### 확정 결정

.NET의 단일 admission gate 형태와 C++·Node의 count+byte 이중 accounting을 조합한다.
lock, mutex, CAS, 단일 event-loop turn은 language mapping이다. 다음 결과는 고정한다.

1. closed/sealed/busy 확인과 용량 예약 사이에 다른 작업이 끼어들 수 없다.
2. 거부된 작업은 authority, sequence, capacity를 전진시키지 않는다.
3. enqueue 실패 시 예약은 같은 gate 안에서 원자적으로 복원된다.
4. byte cost 계산 함수는 payload ownership 모듈이 한 번만 정의한다.
5. accepted sequence는 queue append와 같은 commit에서만 발급한다. 예약 또는 validation
   단계에서는 발급하지 않는다.
6. running item도 해당 lane의 count·byte 사용량에 포함하고 terminal completion 뒤 한 번만
   반환한다.

### 6.3 Runtime observation과 liveness

#### 현재 차이

- .NET `ZLinkObservationQueue`는 latest intermediate 하나, bounded terminal FIFO, sequence
  ordering, saturating loss counter를 한 모듈에서 다룬다.
- JVM `ZLinkStatusPublisher`는 subscriber별 bounded queue와 shared dispatcher,
  source snapshot/fingerprint, preserved milestone을 다룬다.
- C++ `runtime_observation.hpp`는 observer마다 callback thread를 만들 수 있어 subscriber
  수가 자원 수로 전파된다.
- Node의 runtime event queue는 capacity를 받지만 실제 terminal/source 보존 규칙이 공통
  spec의 bounded multi-source 의미와 같은지 추가 검증이 필요하다.

#### 비교안

| 안 | 장점 | 문제 |
|---|---|---|
| A: C++ callback queue | 소비 표면이 단순함 | observer별 thread와 multi-source 보존 비용이 위로 노출됨 |
| B: .NET bounded queue | terminal/order/loss 불변식이 한 deep module에 있음 | dispatcher와 source snapshot 정책을 별도로 보강해야 함 |
| C: .NET queue + JVM dispatcher | bounded 의미와 자원 scaling을 함께 숨김 | 공통 source key와 sequence 정의가 먼저 필요 |

#### 확정 결정

- subscriber마다 `source key → latest intermediate` map 하나와 terminal FIFO 하나를 둔다.
  terminal retention 기본값은 64이며 public API가 양의 capacity를 받으면 그 값을 사용한다.
  capacity를 공개하지 않는 host observation도 64를 사용한다.
- terminal FIFO가 가득 차면 가장 오래된 terminal을 버린다. intermediate는 같은 source의
  값만 교체하며 다른 source의 slot을 덮지 않는다.
- `coalescedCount`와 `discardedTerminalCount`를 구독별로 분리하고 둘 다 signed 64-bit
  최댓값에서 포화시킨다. 새 구독은 0에서 시작한다.
- JVM의 shared dispatcher, source snapshot/fingerprint, preserved milestone 처리 방식을
  결합한다.
- producer는 subscriber callback을 호출하지 않고 non-blocking publish만 수행한다.
- subscriber별 public 소비 표면은 C++ callback, .NET async enumeration, Java Flow,
  Node async iterable로 달라도 된다. producer는 항상 retained subscriber state에 push하고,
  callback·iterator·Flow의 push/pull 차이는 그 상태를 소비하는 binding 표면에만 존재한다.
  소비 표면이 retention, ordering, loss 또는 알림 스케줄링 정책을 다시 구현해서는 안 된다.
- C++ observer별 thread는 shared dispatcher로 통합하고, Node는 capacity를 실제 terminal
  retention에 적용하며 source별 latest map을 구현하도록 정렬한다.
- terminal이 전달되거나 capacity 초과로 폐기된 뒤에만 해당 source slot을 제거한다.
  overflow만으로 stream을 닫지 않으며 취소는 그 subscriber만 닫는다.

#### 파생 보장

- 느린 observer가 producer와 다른 observer를 막지 않는다.
- intermediate는 source별 최신 상태로 합쳐지고 terminal은 순서대로 제한된 수만큼
  보존된다.
- overflow는 stream 종료가 아니라 `coalesced`와 `discarded` loss로 관찰된다.

### 6.4 Session relocation seal과 high-water

#### 현재 차이

- C++ codec에는 service wire command 42 `sessionRelocationSeal`과 command 43
  `sessionRelocationSealed`가 있고 ACK가 `last_accepted_session_sequence`를 운반한다.
- Node session binding registry는 binding token, authority fence, accepted high-water,
  seal identity를 한 route aggregate가 소유하며 sealed 동안 ingress를 거부한다.
- JVM은 42/43 경로와 ACK high-water token을 사용하지만 no-seal/restart fallback의 의미가
  C++의 exact ACK high-water 의미로 정렬해야 한다.
- .NET은 generic relocation seal과 internal relay가 있으나 session route의 high-water와
  ingress barrier가 같은 end-to-end 상태 기계인지 source/test 검증이 필요하다.

#### 비교안

- 안 A: 현재 transport별 seal 구현을 유지하고 monotonic 값만 비교한다.
- 안 B: C++ wire를 그대로 채택하지만 owner aggregate와 restart 의미는 각 언어에 맡긴다.
- 안 C: wire, aggregate, token, idempotency를 하나의 상태 기계로 고정하고 구현 primitive만
  언어에 맡긴다.

안 C로 확정한다. 보호 스펙에는 아래 상태 기계를 그대로 반영한다.

#### 확정한 canonical 상태 기계

1. 현재 binding의 정확한 identity와 authority fence가 맞을 때만 owner가 seal을 생성한다.
2. seal 생성과 ingress 차단은 같은 선형화 지점에서 일어난다.
3. command 43은 owner가 실제로 수락한 마지막 session sequence를 seal token과 함께
   반환한다.
4. source는 command 43의 high-water까지 session record를 capture해 relocation state에
   포함한다. 사전에 추정한 local counter를 대신 사용하지 않는다.
5. target은 그 high-water까지 replay하고 owner membership CAS, lifecycle callback,
   pending work·timer 복원, 기존 queue와 temporary queue의 ordered merge, temporary queue
   등록 제거와 atomic dispatch switch를 끝낸 뒤 application admission을 열고 Ready를
   공개한다. 이 단계에서 session route는 stage 상태이며 switch·unseal하지 않는다.
6. durable source cleanup과 authority의 `Completed` CAS는 Ready를 막지 않는 비동기 작업이다.
   `Completed` 뒤 target이 command 44 `sessionRelocationRoute`를 보내면 session owner는
   binding fence, authority fence, seal identity와 replayed high-water가 모두 일치할 때
   route를 atomic하게 바꾸고 command 45 `sessionRelocationRouted`로 ACK한다.
7. 동일 bytes의 duplicate 42/43/44/45는 idempotent하고, 같은 identity에 다른 값이 오면
   `ProtocolError`다. 상태를 두 번 전진시키지 않는다.
8. 42/43은 capture/replay 장벽의 phase와 deadline을 따르고, 44/45는 `Completed` phase의
   route 수렴 규칙을 따른다. 44/45의 손실·재전송은 target application admission이나 Ready를
   되돌리거나 지연시키지 않는다.
9. abort는 같은 seal identity만 해제한다. 더 새 binding이나 다른 relocation seal을
   해제하지 않는다.
10. no-seal 또는 restart 뒤 seal/high-water를 복원할 durable evidence가 없으면 relocation을
   `RelocationFailed`로 abort한다. 추정한 monotonic 값으로 command 44를 보내는 fallback은
   금지한다.
11. preflight에서 상대의 schema/profile/version이 42/43 seal·high-water 장벽, Ready 조건과
    Ready 후 44/45 route 수렴, exact fence 의미를 지원한다고 확인할 수 없으면 bound-session
    relocation을 시작하지 않고 `Blocked/StateIncompatible`로 거부한다. 구형 peer에 맞춘
    no-seal·추정 high-water downgrade는 제공하지 않는다.

이 방향은 C++의 canonical wire/codec 검증, Node의 정확한 in-memory aggregate, JVM의
ACK-returned token을 조합한다. .NET의 idempotent completion/retry primitive는 상태 기계의
terminal 처리에 활용할 수 있다.

### 6.5 Message Follow suppression

#### 현재 차이와 문제

.NET 구현의 follow key는 source/target node RID, object generation, node generation,
authority owner generation, lease generation을 폭넓게 포함한다. stale route를 구별하는
입력으로는 강점이 있다. 그러나 bounded admission, follow queue, direct reply terminal
보존까지 하나의 큰 class가 소유하면 suppression 규칙을 재사용하거나 검증하기 어렵다.

#### 비교안과 확정 결정

- 안 A: .NET의 큰 모듈 전체를 기준 구현으로 선택한다. 정확한 key는 얻지만 책임이
  결합되어 POSD deep module 경계가 흐려진다.
- 안 B: 각 runtime의 map 위치만 맞춘다. 상태 전이와 expiry 차이가 남는다.
- 안 C: 정확한 fence key는 채택하고 suppression만 담당하는 작은 deep module로 분리한다.

안 C를 선택한다.

canonical `MessageFollowSuppressionRegistry`의 책임은 다음으로 제한한다.

- key: object kind와 logical ID, source/target node RID, object generation, node generation,
  authority owner generation, owner lease generation을 모두 포함한 exact source/target fence
- state: `idle`, `inFlight`, `sentUntilExpiry`
- operation: atomic begin, mark sent, abort/release, expiry
- invariant: 같은 key의 follow는 suppression window 안에서 한 번만 wire로 나간다.
- `idle → inFlight`: wire send admission을 얻은 한 호출만 성공한다.
- `inFlight → sentUntilExpiry`: transport가 notification을 수락했을 때만 전이한다.
- send 거부·예외: `inFlight → idle`로 복원해 다음 시도를 허용한다.
- expiry: source route cache가 만료되거나 exact fence가 교체될 때 marker도 함께 제거한다.
  시간만 따로 재는 suppression timer는 두지 않는다.
- bound: marker는 retained route 하나당 최대 하나이며 route cache capacity를 넘어 별도
  무제한 map으로 증가하지 않는다.

payload ownership, reply route, direct reply terminal retention은 별도 completion/reply-route
aggregate가 소유한다. C++의 정확한 `actor_route_fence_t`도 공통 key 설계의 필수 입력으로
검증한다.

### 6.6 Completion과 terminal winner

특정 언어의 completion table 전체를 복제하지 않는다. canonical completion 모듈은 다음
형태로 확정한다.

- operation identity와 terminal state를 소유하는 작은 pending-operation aggregate
- `tryTake(operationId)` 또는 동등한 완료 표 entry의 atomic take 한 곳만 terminal winner를 결정
- timeout, reply, close, cancellation이 모두 같은 operation을 경쟁
- winner가 정해진 뒤 callback/promise/task dispatch는 별도 책임
- 동일 terminal의 duplicate reply와 늦은 timeout은 상태를 바꾸거나 두 번째 결과를 만들지
  않고 duplicate/late diagnostic만 기록
- 같은 operation에 서로 다른 terminal이 오면 두 번째 값은 application에 전달하지 않고
  protocol/diagnostic error로 기록
- callback, promise, task completion과 observer 호출은 registry gate 밖의 새 execution turn
  에서 실행
- wire `OperationId`는 `{high: u64, low: u64}`이며 terminal completion operation에서는 두
  값이 동시에 0일 수 없다. `ReplyRouteId`는 별개의 u64 상관값이며 request에서는 nonzero다.
- `OperationId`와 `ReplyRouteId`는 source owner lifecycle 안에서 각각 unique하다. wrap과
  reuse는 새 operation 거부로 복구하는 자원 오류가 아니라 terminal runtime error다.
- 구현 내부에 process-local u64 상관값이 필요하면 local correlation 또는 `ReplyRouteId`로
  이름 붙인다. 이를 wire `OperationId` 대신 직렬화하거나 128-bit identity를 축소하지 않는다.
- pending operation table은 기본 4,096개 admission bound를 사용한다. source runtime이
  소유한 local bounded resource이므로 등록 전에 한도를 넘으면 `CapacityExceeded`로 거부한다.
- terminal winner는 완료 표에서 entry를 원자적으로 제거한다. 완료 entry나 별도 terminal
  tombstone을 deadline까지 보존하지 않는다. 늦은 reply가 제거된 entry를 만나면 unknown/late
  diagnostic만 기록하며, lifecycle 동안 ID를 재사용하지 않으므로 새 operation을 완료할 수 없다.

C++ pending operation state와 JVM/Node operation registry를 비교하고, .NET table의
timeout·close 처리를 함께 평가한다. 전체 completion table을 한 언어에서 그대로 복제하지
않는다.

### 6.7 Object lifecycle 표현

domain에서는 `EntrySpot`, `UserSpot`, `InstanceSpot` 세 variant를 반드시 분리한다. 상속,
sealed hierarchy, `std::variant`, discriminated union 중 어느 문법으로 표현할지만 언어
mapping이다. 다음을 공통 결정으로 고정한다.

- kind별 허용 lifecycle transition과 command 집합
- entry/instance/service 등 서로 양립할 수 없는 상태를 동시에 만들 수 없음
- wire/store의 `SpotKind` tag는 decode 시 variant를 한 번 만드는 데만 사용하고, 생성 뒤
  domain 동작 분기용 mutable flag로 재사용하지 않음
- owner aggregate 밖에서 kind를 바꿀 수 없음
- 생성, handoff, returning, destroyed terminal 전이 test를 언어별 동일 fixture로 검증

모든 언어가 construction boundary에서 invalid 조합을 거부한다. C ABI나 binding 경계에서는
검증된 value object/variant를 만든 뒤 domain으로 진입한다. .NET과 C++의 tag 중심 내부
경로는 세 variant로 분해하고, 공통 동작은 의미 있는 base/composed module에만 둔다.

### 6.8 Payload ownership과 codec

#### Payload ownership 확정 결정

- receive payload는 `binding-borrowed → framework-owned → application-borrowed → released`
  상태로만 이동한다. 역방향 전이는 없다.
- binding이 ownership handoff를 지원하면 copy 0, 지원하지 않으면 binding boundary에서
  deep copy 최대 1회만 허용한다. Framework owner가 된 뒤 queue·handler 경계에서 추가
  deep copy는 0회다.
- readonly accessor는 view를 반환하며 호출할 때마다 array/list를 복사하지 않는다.
- admission 거부는 deserialize 전에 끝낸다. 수락한 payload는 owner turn에서 최대 한 번만
  deserialize한다.
- zero-copy raw view의 유효 기간은 handler turn까지다. 독립 객체로 deserialize된 typed
  payload는 각 언어의 일반 객체 수명을 따르며 backing transport buffer를 참조하지 않는다.
  raw view를 보존하려면 application/extension이 자기 소유 사본을 명시적으로 만든다.
- release는 terminal, rejection, cancellation, exception의 모든 경로에서 정확히 한 번
  실행하며 copy count와 retained byte를 test metric으로 검증한다.

#### Codec 선택 확정 결정

- registry entry는 normalized unique content-type, serializer와 송신 type selector를 가진다.
  C++ compile-time descriptor, .NET/Java predicate, Node predicate/thunk는 같은 의미의
  language mapping이다.
- 송신은 호출 지점에 선언된 message type으로 selector를 평가한다. 실제 instance의 subtype을
  선택 입력으로 사용하지 않는다. matching extension이 없으면 기본 JSON serializer를
  사용하고, 둘 이상의 조건이 match하면 나중에 등록한 조건을 우선한다.
- 수신은 wire content-type으로만 고른다. 등록되지 않은 content-type은 JSON fallback 없이
  `ProtocolError`다.
- registry는 startup 뒤 immutable이다. 수신 table은 startup 때 완성하고, 송신 type 결과는
  처음 한 번 계산해 bounded cache에 저장한다.
- 송신 cache 기본 상한은 1,024 type이다. 상한 뒤의 새 type은 결과를 캐시하지 않고 같은
  selector를 실행하며, 기존 entry를 임의 eviction해 선택 결과를 바꾸지 않는다.
- hot path에서 content-type normalize, codec array/list, wrapper allocation을 만들지 않는다.
- 공개 method 이름과 generic 문법은 exact interface별로 달라도 되지만 위 등록 정보와
  선택·실패 의미는 C++/.NET/Java/Kotlin/Node 모두 제공한다.

### 6.9 Runtime 상태와 bounded-context projection

현재 source 근거 EV-13~EV-20에 따르면, C++/.NET/JVM/Node의 public runtime state는 모두
`preparing`, `serving`, `relocating`, `relocated`, `draining`, `stopped`, `error`에 대응하는
0..6 값을 가진다. status의 `isReady`처럼 enum에서 계산되는 bool은 “참·거짓 하나로
상태를 관리”하는 위반이 아니다. 따라서 public 상태 **집합과 wire 값**은 `MATCH`로
확정한다.

readiness authority는 public 7-state 하나로 확정한다.

- host `IsReady`와 동등한 public method는 항상 `state == Serving`으로 계산한다. 별도 mutable
  ready bool을 public readiness authority로 사용하지 않는다.
- `AcceptingWork`는 `state == Serving && admissionOpen`으로 계산한다. `admissionOpen`은
  in-flight drain을 위한 gate일 수 있지만 `IsReady`를 바꾸지 않는다.
- startup completion은 endpoint bind와 실제 address publication, peer admission,
  handler/object runtime 준비가 모두 끝난 뒤 `Preparing → Serving` 전이와 함께 완료한다.
- topology readiness는 host readiness와 ready target 수를 조합한 별도 topology projection이다.
  host state를 대신 쓰거나 host를 다시 전이시키지 않는다.
- JVM의 별도 `AtomicBoolean ready`처럼 public state와 독립적으로 읽히는 cell은 제거하거나
  `admissionOpen`으로 역할을 좁힌다. public `isReady()`는 state에서 직접 계산한다.

반면 다음 축약 상태는 별도 bounded context에 존재한다.

- C++ `host_runtime_state_t`: maintenance aggregate 내부의 6-state이며 `retiring`을 가진다.
- Node `ClientServerDescriptor.state`: discovery 전용 상태이며 `retiring`과
  `disconnected`를 가진다.
- Node `ZLinkExecutionArea`: `application`/`infrastructure` 실행 문맥이며 serial scheduler의
  `application`/`lifecycle` lane과 같은 타입이 아니다.

DDD 관점에서 public host, maintenance admission, service discovery, execution isolation은
별도 bounded context로 유지한다. 축약 상태는 다음 이름과 mapping으로 고정한다.

| public state | `MaintenanceAdmissionState` | `DiscoveryAvailability` |
|---|---|---|
| `Preparing` | `Preparing` | `Preparing` |
| `Serving` | `Serving` | `Serving` |
| `Relocating` | `Retiring` | `Retiring` |
| `Relocated` | `Retiring` | `Retiring` |
| `Draining` | `Draining` | `Retiring` |
| `Stopped` | `Stopped` | `Stopped` |
| `Error` | `Error` | `Error` |

`DiscoveryAvailability.Disconnected`는 transport connection이 만드는 discovery 전용 값이며
public host state에서 만들지 않는다. 두 projection은 단방향이고 public state로 역변환하지
않는다. C++ `host_runtime_state_t`는 `maintenance_admission_state_t`로, Node의 `preflight`
동의어는 `Preparing`으로 정렬한다. Java/.NET에도 이 context가 필요하면 같은 six-state를
사용하며 별도 boolean 조합으로 복제하지 않는다.

`10-liveness-and-state.ko.md`에는 특정 구현의 이름이나 과거 사례 대신 위
단일-authority 결정과 이를 어겼을 때 생기는 failure를 적는다. 현재 위반 runtime은 gap
ledger에서 파일·행과 test로 추적한다.

### 6.10 명칭 통합의 경계

용어 통일은 같은 bounded context와 같은 책임일 때만 수행한다.

공통 dependency 방향은 다음으로 고정한다.

```text
public API / handler adapter
  → use-case coordinator
    → domain aggregate owner
      → backend port

binding adapter ──implements──> backend port
binding adapter ──────────────> public binding API
```

binding이 없는 C++ direct backend는 마지막 두 node를 한 deep module로 합칠 수 있다.
반대 방향 dependency, domain에서 binding concrete type 검사, raw-frame 우회와 의미 없는
pass-through wrapper는 금지한다.

- serial work lane은 네 runtime에서 `application`/`lifecycle`로 통일한다.
- Node execution area의 `application`/`infrastructure`는 Framework-owned 실행 문맥을
  뜻하므로 이름을 유지한다. serial lane 이름으로 재사용하지 않는다.
- Java `runtime/binding`은 JNI/native API 변환 adapter context로 유지한다.
- C++/.NET/Node의 `runtime/backend`는 raw backend port와 binding 구현 context로 유지한다.
- internals responsibility graph에는 `binding adapter`와 `backend port`를 별도 node로 두고,
  한 언어에서 둘이 합쳐져 있으면 한 module이 두 책임을 구현한다고 표시한다.

이 원칙은 이름을 맞추기 위해 의미 있는 adapter를 합치거나, 반대로 이름만 다른
pass-through wrapper를 새로 만드는 것을 막는다.

## 7. common internals 12개 장 전수 조사표

| 장 | 조사할 책임과 불변식 | 우선 비교 대상 | 필수 실패 시나리오 |
|---|---|---|---|
| 01 layering | dependency 방향, adapter 책임, identifier, shutdown owner | backend/binding adapter와 process owner | 부분 초기화, shutdown 중 late callback |
| 02 serialization | frame ownership, FIFO, pin/handoff, copy budget | encoder/decoder와 queue insertion | malformed frame, enqueue 실패, release 누락 |
| 03 progress isolation | application/lifecycle lane, capacity, fairness | 네 serial scheduler | app 포화, lifecycle debt, reentrancy |
| 04 completion | terminal winner, timeout, cancellation, close | pending operation table | reply-timeout race, duplicate reply, close race |
| 05 relocation | prepare/seal/commit/abort, retry, restart | actor/session relocation state | A→B→A, stale commit, lost reply, restart |
| 06 route/cache/selector | exact fence, invalidation, selection authority | route cache와 selector | stale cache, generation reuse, multicast |
| 07 dispatch loop | receive ownership, wakeup, timer, callback 경계 | poll/dispatch/timer loop | wake loss, timer starvation, handler exception |
| 08 object lifecycle | kind와 lifecycle, returning, destroy | object aggregate | invalid kind/state, double return, late message |
| 09 session binding | binding token, route owner, unbind, ingress | session registry | stale token, bind/unbind race, sealed ingress |
| 10 liveness/state | snapshot authority, observation, loss | runtime monitor/publisher | slow observer, terminal overflow, source churn |
| 11 message ownership | copy, codec lifetime, application handoff | binding adapter와 payload | handler return 후 use, double release, oversize |
| 12 service wire | command, fence, strict decode, compatibility | schema/generated codec | unknown field/command, truncation, version skew |

각 셀은 네 runtime의 production file, test file, package provenance, 판정 상태를 가진 ledger
행으로 확장한다. 문서에 “한 구현은”이라고 적힌 설명은 source call path와 test로 현재
사실인지 확인한다.

### 7.1 기존 재량 표기의 확정 inventory

현재 internals에 명시된 재량과 source에서 확인한 추가 차이를 다음 목록으로 관리한다.
WP1은 이 목록을 최소 inventory로 사용한다.

| 문서 | 현재 표현 | 1차 분류 | 통합 방향 |
|---|---|---|---|
| README tracing gate | template lambda, `Func<>`, `Supplier<>`, thunk | `LANGUAGE-MAPPING` | disabled hot path의 allocation/string 생성 0만 고정 |
| 01 layering | interface/class/protocol/function/direct binding | `LANGUAGE-MAPPING` | 책임·dependency·ownership·비용을 고정하고 파일 형태는 허용 |
| 02 serialization | 실행 자원 고정 또는 handoff | `LANGUAGE-MAPPING` | shared bounded 실행 자원에서 handoff를 허용하고 owner 전용 자원·계약적 affinity는 금지 |
| 03 progress isolation | 두 queue 또는 한 priority queue, context marker | `DIVERGED` | 두 physical FIFO·독립 admission·fairness·잘못된 문맥 실패로 고정 |
| 07 dispatch admission | lock 또는 다른 수단 | `LANGUAGE-MAPPING` | check+reserve+enqueue의 단일 선형화 지점만 고정 |
| 07 wakeup | signal/callback 또는 polling | `DIVERGED` | empty→non-empty에서 즉시 signal/callback; 정상 경로의 주기적 polling은 금지 |
| 08 object lifecycle | inheritance/composition/tagged union | `LANGUAGE-MAPPING` | invalid kind/state 생성 불가와 lifecycle transition을 고정 |
| 10 observation | push 또는 pull | `LANGUAGE-MAPPING` | producer는 공통 retained state에 push하고 push/pull은 binding 소비 표면으로만 허용 |
| 11 immutable payload | binding별 내부 copy 경로 | `LANGUAGE-MAPPING` | public immutability와 ownership을 유지하며 framework copy budget 고정 |
| 11 codec API | exact interface별 API shape | `DIVERGED` | 언어 문법은 달라도 type-selector/content-type 선택 의미를 동일하게 제공 |
| 12 Message Follow | merge 방식과 suppression marker 위치 | `DIVERGED` | exact key와 suppression state machine을 deep module로 고정 |

이 inventory에는 언어별 정책 선택을 남기지 않는다. 성능 차이가 관찰되는 현재 선택은 위
canonical 방향으로 확정해 `DIVERGED`로 추적하고, `LANGUAGE-MAPPING`은 같은
ordering·failure·resource bound를 입증할 때만 사용한다. 이후 새 선택 지점이 발견되어도
언어 구현에서 고르지 않고 common contract 변경 절차에서 먼저 하나의 의미로 확정한다.

### 7.2 구현별 사례 문구의 현재 사실 검증

현재 한국어 internals에는 “한 구현”이라는 서술이 여러 장에 남아 있다. 구현별 사례가
현재 source와 다르면 독자는 이미 존재하지 않는 결함을 현재 제약으로 오해한다. common
internals에는 canonical 결정과 visible failure만 남기고, runtime별 위반은 gap ledger가
소유한다.

각 문구에 다음 중 하나를 부여한다.

- `CURRENT-VIOLATION`: 현재 source와 test가 위반을 재현하므로 gap ledger에 기록하고
  internals에는 일반화한 visible failure를 적는다.
- `STALE`: 현재 어느 runtime에도 해당하지 않으므로 internals에서 제거한다.
- `UNVERIFIED`: 식별자 검색만 했거나 call path 재현이 없으므로 contract 근거로 사용하지
  않는다.

검증 순서는 `10-liveness-and-state`의 bool-only readiness와 early-serving 문구,
completion의 terminal 방식, serialization의 inline/front insertion, message ownership의
copy/codec 사례다.

## 8. internals 결정문의 표준 형식

재량을 지우고 특정 구현을 적는 것만으로는 부족하다. 모든 공통 결정은 다음 순서로
작성한다.

1. **상황**: 어떤 concurrent/lifecycle 조건에서 결정이 필요한가
2. **기대 동작**: 사용자가 관찰할 결과와 상태 전이
3. **보이는 실패**: 결정을 어기면 timeout, duplicate, stale route, resource growth 중
   무엇이 나타나는가
4. **authority와 owner**: 상태를 쓰고 terminal을 결정하는 단 하나의 모듈
5. **identity와 data**: fence, sequence, token, capacity 계산 입력
6. **algorithm**: 정상 경로와 최소 pseudocode
7. **선형화 지점**: check/reserve/enqueue 또는 seal/ingress 차단이 원자적인 위치
8. **ordering과 bound**: FIFO, priority/debt, message/byte/time 한도
9. **실패·retry·restart**: duplicate, timeout, abort, crash 뒤 의미
10. **concurrency와 ownership**: lock 밖 callback, payload lifetime, release 책임
11. **금지 변형**: front insertion, implicit reentrancy처럼 같은 결과를 보장하지 못하는 방식
12. **허용 language mapping**: mutex/event loop, callback/Flow처럼 결과가 같은 표현
13. **검증**: 공통 fixture와 언어별 test/명령
14. **파생 보장**: 이 결정 때문에 추가로 성립하는 안전·자원 보장

한국어와 영어 internals가 모두 존재하는 항목은 같은 patch에서 의미를 맞춘다. 표 안에
설명을 끼워 넣어 Markdown 구조가 깨지지 않는지 link checker와 rendering 관점에서
검토한다.

## 9. 변경 gate

### G0. 기준선과 provenance gate

- branch, commit SHA, dirty path를 기록한다.
- local source, installed package, generated asset의 commit/version을 구분한다.
- 기존 사용자 변경을 덮지 않으며 path-limited diff로 작업한다.

### G1. source evidence gate

- 모든 ledger 행에 production call path와 실제 owner를 기록한다.
- symbol 존재만 확인한 행은 `UNVERIFIED`로 남긴다.
- Java와 Kotlin은 JVM 구현을 공유하더라도 두 exact interface와 consumer test를 따로
  확인한다.

### G2. POSD/DDD decision integrity gate

- 확정 결정마다 비교한 최소 두 안과 제외 근거가 남아 있는지 확인한다.
- 특정 언어 구현을 사용한 결정은 그 언어가 아니라 불변식을 선택한 이유를 기록한다.
- 조합안은 책임을 다시 분리하고 새 pass-through wrapper를 만들지 않는지 확인한다.

### G3. contract owner gate

- 사용자 관찰 결과, wire, 공개 API가 바뀌면 common spec/exact interface가 먼저다.
- 내부 책임 배치만 바뀌면 internals 결정으로 제한한다.
- service wire schema와 generated asset은 한 integration owner가 순서대로 갱신한다.

### G4. 보호 문서 승인 gate

- 대상 파일과 절, before/after 의미, 호환성, 구현 영향, 검증 명령을 제시한다.
- 명시적 승인을 받은 뒤 `common/spec`과 `common/internals`를 수정한다.
- 승인 전에는 production implementation을 확정한 새 의미에 맞춰 먼저 바꾸지 않는다.

### G5. closure evidence gate

static/contract, unit, schema/golden, package, aggregate CI, 7종 sample process,
cross-language, real process E2E, performance/resource를 별도 결과로 기록한다. 한 lane의
PASS를 다른 lane의 PASS로 대체하지 않는다. 현재 E2E inventory가 완결되지 않았으므로
이번 구현 수렴의 완료 조건에 E2E 전체 PASS를 넣지 않는다. E2E는 영향받는 시나리오만 별도
workstream에서 구현 상태부터 검증하고, 실행하지 않은 항목은 `PASS`나 `N/A`가 아니라
근거와 재개 명령이 있는 `E2E-DEFERRED`로 남긴다.

## 10. 문서 변경 후 구현 작업 계획

의존 순서는 다음과 같다.

```text
WP0 기준선
  → WP1 전수 source/test matrix
    → WP2 확정 결정의 contract patch 설계
      → WP3 보호 spec/internals/exact-interface 승인 및 변경
        → WP4 schema·generated asset·공통 fixture
          → WP5 언어별 production + white-box test
            → WP6 package·clean consumer
              → WP7 언어별 전체 회귀 test
                → WP8 7종 sample × 5개 공개 표면
                  → WP9 영향 기반 cross-language·resource
                    → WP10 독립 검토와 중복 제거

별도 후속 E2E workstream
  E2E0 영향 시나리오 선택
    → E2E1 E2E 구현 완전성 검증
      → E2E2 fixture/runner gap 정리
        → E2E3 선택 시나리오 실제 process 검증
```

### WP0. 기준선 고정

- 대상 commit, 변경 중인 파일, package provenance를 ledger 머리에 기록한다.
- 기존 regression matrix와 gap ledger의 PASS를 새 결과로 간주하지 않고 실행 날짜와 SHA를
  확인한다.
- 공통 문서의 현재 결정과 exact interface를 요구사항 ID로 고정한다.

**산출물**: baseline 표, command inventory, 미검증 목록

### WP1. 네 runtime 전수 조사

- 1.2의 IC-01~IC-14를 초기 ledger 행으로 사용한다. 새 정책 차이는 IC-15 이후의 행으로
  추가하며 internals 반영 위치, 기대 동작, visible failure와 검증을 함께 적는다.
- 12개 internals 장 × 4개 runtime의 production/test 행을 채운다.
- JVM은 구현 행 하나와 Java/Kotlin 공개 표면 행 둘로 나눈다.
- success만 아니라 full queue, concurrent close, retry, duplicate, restart, malformed wire를
  추적한다.
- 논리 값은 상수 정의와 실제 사용 지점을 함께 확인한다.
- thread/executor, queue, timer, callback이 owner 수 또는 subscriber 수에 따라 어떻게
  증가하는지 측정한다.
- 같은 책임 모듈의 branch, 거부, retry, journal/metric 기록, authority side effect를
  나란히 놓은 **asymmetric branch inventory**를 만든다.
- 한 runtime에만 있는 `if`는 갭 확정이 아니라 조사 대상으로 표시한다. binding 제약이나
  더 깊은 module이 같은 결과를 보장하는지 확인한 뒤 `DIVERGED`를 부여한다.

WP1은 다음 경로가 현재 존재하는지 source와 test에서 확인한다.

- relocation envelope에 canonical object가 아닌 journal record가 실린 경로
- `busy` 거부가 capacity reservation을 되돌리며 authority를 전진시킨 경로
- deferred Join 실패 원인을 기록하지 않은 경로

경로가 존재하면 `DIVERGED`와 재현 test를 기록한다. 존재하지 않으면 현재 gap으로 남기지
않으며, 같은 결함을 막는 contract test가 있는지만 확인한다.

**종료 조건**: `UNVERIFIED` 행마다 확인 명령 또는 명시적 차단 사유가 있음

### WP2. 확정 결정의 contract patch 설계

- 1.2의 통합 목록에서 IC 결정, internals 한국어·영어 절, 네 runtime production owner와
  test를 한 행으로 연결한 patch matrix를 만든다.
- 공통 값, state diagram, pseudocode, forbidden variant, language mapping을 보호 문서용
  문장으로 변환한다.
- relocation과 service wire는 backward compatibility와 mixed-version migration을 함께
  작성한다. canonical 의미 자체를 다시 선택하지 않는다.

**종료 조건**: 구현자가 언어별로 다시 정책을 선택할 여지가 없음

### WP3. 보호 문서 변경

변경 순서는 다음과 같다.

1. stale한 “한 구현” 서술과 Markdown 결정 표를 현재 source에 맞게 정정
2. 입증된 language mapping과 구현 gap을 7.1 inventory 기준으로 재분류
3. 사용자 관찰 결과와 wire 의미를 common spec에 반영
4. 공개 API 영향이 있으면 C++/.NET/Java/Kotlin/Node exact interface 동시 반영
5. state owner, algorithm, bound, retry를 common internals 한국어·영어에 반영
6. implementation gap 문서에서 완료되지 않은 runtime을 `DIVERGED`로 추적
7. 문서 contract checker에 새 결정의 핵심 상수·금지 변형 검사를 추가
8. 공통 sample 문서의 ZoneWorld 지원 언어와 실제 C++/.NET/Java/Kotlin/Node runner 목록을
   일치시킨다. 최종 목표는 일곱 sample의 다섯 공개 표면이며, 빠진 구현을 문구로 숨기지
   않고 `DIVERGED`로 남긴다.

`90-implementation-gap` 같은 진행 문서는 현재 상태를 추적하고, internals 본문은 최종
결정과 근거만 담는다. public 문서에서 이 임시 plan을 링크하지 않는다.

**중요**: WP3 patch가 승인·merge되기 전에는 WP4 이후를 시작하지 않는다.

### WP4. 공유 protocol과 conformance fixture

- `service-wire-v1.schema.json`을 먼저 변경하고 validation을 통과시킨다.
- 한 integration owner가 C++/.NET/JVM/Node generated asset을 재생성한다.
- command 42/43/44/45, Message Follow fence, malformed frame의 golden fixture를 추가한다.
- 각 runtime encoder 결과를 다른 세 runtime decoder가 읽는 matrix를 만든다.
- scheduler/observation처럼 wire가 아닌 항목은 language-neutral scenario fixture와 예상
  event trace를 만든다.

### WP5. 언어별 production과 white-box test

#### C++

- `dispatch_limits`를 canonical limit의 단일 owner로 유지하고 모든 queue가 같은 값을
  사용하도록 정리한다.
- per-owner gate가 shared offload executor를 사용하고 implicit inline 경로가 없는지
  고정한다.
- observer별 thread를 shared dispatcher 또는 bounded shared execution lane으로 바꾼다.
- relocation codec/state가 exact seal token과 fence를 검증하게 한다.
- route fence에는 generation 대용 값이 아니라 정확한 `actor_route_fence_t`를 사용한다.
- tag 중심 Spot 내부 상태를 Entry/User/Instance domain variant로 분리한다.
- `host_runtime_state_t`를 `maintenance_admission_state_t`로 바꾸고 public state로의 역변환을
  제거한다.
- codec registry의 receive table과 bounded send-type cache, payload copy-count test를 추가한다.
- concurrency test, A→B→A relocation, logical multicast, ASan을 별도 실행한다.

#### .NET

- serial count/burst 값을 canonical limit에 맞추고 단일 admission gate는 보존한다.
- Message Follow class에서 suppression registry와 direct reply/completion 책임을 분리한다.
- observation queue의 terminal/order/loss 의미를 공통 fixture의 기준 구현으로 유지하되,
  기본 terminal capacity 64, shared dispatcher와 source retention을 보강한다.
- session relocation seal의 ingress barrier와 ACK high-water를 canonical state machine에
  맞춘다.
- SpotKind 조건 분기를 Entry/User/Instance domain variant로 이동한다.
- declared message type과 나중 등록 selector 우선순위, bounded type cache와 accessor-copy 0
  test를 추가한다.
- public `IsReady`가 host state에서만 계산되는지 고정한다.
- 안정성이 확인된 serial test 실행을 기본으로 하고 병렬 실행은 격리 증거가 있을 때만
  추가한다.

#### JVM(Java/Kotlin)

- 모든 per-owner serial queue가 process execution lanes를 주입받게 하고, owner마다 새
  executor를 소유하는 기본 경로를 제거한다.
- scheduler limit와 no-inline/yield 규칙을 공통 fixture로 고정한다.
- status publisher의 shared dispatcher/source snapshot 강점을 보존하면서 terminal
  capacity 기본값을 64로 맞추고 loss ordering을 공통 queue 의미에 맞춘다.
- relocation no-seal/restart fallback을 승인된 스펙과 일치시키고, ACK token 외의 추정
  high-water 사용을 금지한다.
- 별도 `AtomicBoolean ready`는 admission gate로 역할을 좁히고 public `isReady()`는
  `runtimeState == SERVING`으로 계산한다.
- declared message type과 나중 등록 selector 우선순위, bounded type cache, payload
  copy-count를 Java/Kotlin 공통 fixture로 검증한다.
- Java와 Kotlin bridge/API snapshot, package consumer와 7종 sample을 각각 확인한다. E2E는
  영향 시나리오를 별도 workstream에서 구현 상태부터 확인한다.
- Gradle 검증은 framework jar를 먼저 만들고 `--no-daemon --no-parallel --max-workers=1`로
  실행해 build ordering과 runtime failure를 구분한다.

#### Node

- scheduler count/byte/time/burst를 canonical limit에 맞춘다.
- 같은 owner 중첩 호출의 implicit inline 실행을 제거하고 explicit yield/resume으로
  대체한다.
- observation queue가 source별 latest intermediate와 bounded terminal FIFO를 실제
  capacity 기본값 64로 관리하게 한다.
- in-process session binding registry의 강점을 service wire seal/ACK state machine과
  연결한다.
- `addSerializer`에 declared message type selector 의미와 나중 등록 조건 우선순위를
  제공하고 bounded type cache를 추가한다.
- public 7-state, maintenance admission state, discovery availability의 단방향 mapping을
  분리하고 `infrastructure` execution area는 그대로 유지한다.
- Promise rejection, AbortSignal, async iterator 종료가 공통 terminal winner를 우회하지
  않게 한다.
- 지원 Node 버전별 runtime matrix와 cross-language fixture를 실행한다.

각 언어 작업은 production refactor와 해당 불변식을 이름으로 드러내는 test refactor를
같은 checkpoint에 둔다. 예: `reject_does_not_advance_authority`,
`nested_submit_resumes_on_new_turn`, `terminal_observation_survives_intermediate_overflow`.

### WP6. package와 clean consumer

source tree test와 package 증거를 구분한다.

- C++: `framework/languages/cpp/scripts/verify_packaged_contract.sh`
- .NET: `framework/languages/dotnet/scripts/verify_packaged_contract.sh`
- JVM: `framework/languages/java/scripts/verify_api_snapshot.sh`,
  `framework/languages/java/scripts/verify_packaged_contract.sh`
- Node: `framework/languages/node/scripts/verify_packaged_contract.sh`와 release/runtime matrix

각 package는 임시 clean consumer가 설치하고, local source fallback 없이 exact API와
최소 runtime scenario를 실행해야 한다.

### WP7. 언어별 전체 회귀 test

WP5와 WP6가 끝난 같은 target SHA에서, focused test가 아니라 각 언어가 유지하는 전체
regression/aggregate gate를 다시 실행한다. 과거 PASS, 변경 도중의 PASS, 일부 test filter의
PASS는 이 단계의 증거로 승격하지 않는다.

- C++는 전체 test가 켜진 configure option과 제외된 optional dependency를 기록한 뒤 전체
  `ctest`를 실행한다. ASan/TSan처럼 일반 build와 양립하지 않는 lane은 별도 build에서 실행한다.
- .NET은 solution의 전체 test project를 안정성이 확인된 직렬 정책으로 실행한다. 지원 TFM/RID
  matrix는 CI gate로 별도 확인하며 한 TFM의 성공을 전체 성공으로 확장하지 않는다.
- JVM은 framework jar를 먼저 준비한 뒤 `clean check`가 unit, contract, fake-backend,
  integration, sample contract task를 모두 포함하는지 확인한다. Java와 Kotlin 공개 표면의 API
  snapshot은 별도 행으로 남긴다.
- Node는 build, typecheck, lint, 모든 `*.test.js`와 지원 Node version matrix를 실행한다.
  skip 환경 변수나 파일 allowlist를 사용한 실행은 PASS가 아니다.
- 모든 runner는 target SHA, package/runtime provenance, 명령, 시작·종료 시각, test 수,
  pass/fail/skip 수, exit code와 원본 log 경로를 ledger에 남긴다.
- WP7 시작 뒤 production, generated asset, package 또는 test code가 바뀌면 해당 runtime의
  WP6 package gate부터 WP7 전체 회귀까지 무효화하고 다시 실행한다.

**종료 조건**: C++/.NET/JVM/Node 전체 regression이 filter·skip 없이 같은 target SHA에서
PASS하고, 누락된 test target이 없음. E2E aggregate는 이 종료 조건에 포함하지 않는다.

### WP8. 7종 sample × 5개 공개 표면 실제 동작 검증

전체 test 뒤 다음 일곱 sample을 C++/.NET/Java/Kotlin/Node에서 실제 client/server process로
검증한다.

1. TicTacToe
2. Bingo
3. DeliveryDispatch
4. SupportChat
5. GameQuest
6. ShoppingMall
7. ZoneWorld

JVM은 production runtime은 하나지만 Java와 Kotlin이 서로 다른 공개 API·sample source를
가지므로 두 표면을 별도 실행한다. 따라서 최종 gate는 네 runtime의 네 행이 아니라
**7종 × 5개 공개 표면 = 35개 실행 셀**이다. 구현 또는 runner가 없는 셀은 `SKIP`이나
`N/A`가 아니라 `DIVERGED`다.

| sample | C++ | .NET | Java | Kotlin | Node |
|---|---|---|---|---|---|
| TicTacToe | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` |
| Bingo | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` |
| DeliveryDispatch | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` |
| SupportChat | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` |
| GameQuest | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` |
| ShoppingMall | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` |
| ZoneWorld | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` | `<상태/log>` |

실행과 판정 원칙은 다음과 같다.

- 공개 표면별 통합 runner는 sample을 순차 실행한다. Java 전체가 끝난 뒤 Kotlin을 실행하며,
  다른 언어 runner도 port, Redis, browser profile과 run directory가 격리되지 않았다면 겹쳐
  실행하지 않는다.
- 각 셀은 runner exit 0뿐 아니라 client self-check, 요청/응답 또는 browser-visible 결과,
  기대한 server 역할의 처리 log, 정상 framework 종료, 자식 process·Redis·browser cleanup을
  모두 가져야 PASS다. ready marker만 있거나 server가 떴다는 사실만으로는 PASS가 아니다.
- 통합 runner가 일곱 sample을 한 번씩 호출했는지 시작/완료 marker와 개별 log로 대조한다.
  중간 sample 실패를 retry로 숨기거나 뒤 sample을 성공으로 집계하지 않는다.
- 실패 시 원본 run directory와 역할별 stdout/stderr/framework log를 보존하고, timeout,
  crash, unhandled exception, bind 충돌, 남은 process/container를 별도 실패 사유로 기록한다.
- 35개 셀의 clean PASS 뒤 relocation/concurrency 영향이 큰 Bingo, ShoppingMall, ZoneWorld는
  각 공개 표면에서 focused runner를 **연속 3회** 실행한다. 이는 실패를 덮는 retry가 아니라
  flaky 0을 확인하는 안정성 gate이며, 한 번이라도 실패하면 원인을 수정한 뒤 연속 횟수를
  처음부터 다시 센다.
- sample 실행 뒤 source나 runner를 고쳤다면 영향받은 공개 표면의 일곱 sample 전체와 WP7
  전체 회귀를 다시 실행한다.

**종료 조건**: 35개 셀이 모두 PASS하고, 고위험 세 sample의 연속 실행도 실패·누수 없이 PASS함

### WP9. 영향 기반 cross-language와 resource 검증

- wire/schema/codec이 바뀌었으면 영향받는 encoder→decoder 조합을 선택하고, 축소하지 않은
  전체 matrix와 비교해 제외한 조합의 대칭성 근거를 기록한다.
- wire가 바뀌지 않았으면 cross-language 실행을 억지로 추가하지 않고 `NOT-TRIGGERED`와
  근거가 된 path diff를 남긴다.
- scheduler saturation과 slow observer에서 latency, memory, thread/executor 수, loss counter를
  비교한다.
- WP8의 ZoneWorld를 relocation, dispatch, session binding의 공통 회귀 시나리오로 참조하되,
  sample PASS는 공통 fixture, package, 필요한 cross-language matrix를 대신하지 않는 별도
  evidence lane으로 기록한다.

### 별도 후속 E2E workstream

기존 E2E가 아직 전체 구현·검증 완료 상태가 아니므로 WP0~WP10과 섞어 전부 재실행하지 않는다.
WP10에서 `IMPLEMENTATION-CLOSED / E2E-DEFERRED` 상태와 target SHA, 영향 후보, 재개 명령을
고정한 뒤 다음 단계만 나중에 독립적으로 수행할 수 있다.

#### E2E0. 필요한 시나리오 선택

- 변경한 IC, public API, wire command, lifecycle/error 의미를 common E2E requirement와
  언어별 feature-map에 연결한다.
- 직접 영향, shared runner 영향, cross-language 위험이 있는 시나리오만 선택한다.
- 선택하지 않은 E2E는 이유와 현재 gap을 기록한다. 이를 PASS로 세지 않는다.

#### E2E1. E2E 구현 완전성부터 검증

선택한 시나리오는 실행 전에 다음을 source와 static/contract test로 확인한다.

- common E2E의 역할 수, topology, config, 요청 순서와 기대 실패가 feature-map에 정확히
  연결되어 있음
- 각 역할 executable/project, runner entry, 설정 생성, dynamic endpoint/Redis 격리가 실제로
  존재하고 placeholder, unconditional skip, exit 2가 없음
- client가 업무 결과를 self-check하고 server 역할별 처리 증거를 남기며, ready marker만으로
  성공하지 않음
- 정상·실패·timeout 모두에서 자식 process, Redis, browser와 임시 자원을 정리하고 실패 log
  경로를 보존함
- aggregate inventory가 미구현 시나리오를 성공으로 집계하지 않음

각 시나리오는 `READY-TO-RUN`, `FIXTURE-GAP`, `PRODUCTION-GAP` 중 하나로 판정한다.
`FIXTURE-GAP`이나 `PRODUCTION-GAP`을 반복 실행으로 확인하려 하지 않는다.

#### E2E2. fixture/runner gap 정리

- `FIXTURE-GAP`은 E2E 역할/runner/config/self-check/cleanup 구현을 먼저 완성하고 E2E1을
  다시 통과시킨다.
- `PRODUCTION-GAP`은 해당 runtime 구현 checkpoint로 돌려보낸다. production을 수정했다면
  영향받은 runtime의 WP6 package, WP7 전체 회귀와 WP8 일곱 sample을 다시 실행한다.
- E2E fixture만 수정했고 production/package/sample source가 바뀌지 않았으면 WP6~WP8을
  재실행하지 않고 선택한 E2E의 구현 검증과 실행만 다시 한다.

#### E2E3. 선택 시나리오 실제 process 검증

- `READY-TO-RUN`인 시나리오만 실행한다.
- request/response 또는 browser self-check, server 역할 증거, cleanup, 최종 exit status와
  원본 log를 함께 수집한다.
- focused scenario를 먼저 통과시키고, shared aggregate runner를 수정했거나 집계 회귀 위험이
  있을 때만 영향받는 aggregate lane을 추가한다. 미완료 E2E 전체를 무조건 실행하지 않는다.
- 실행 중 code 변경이 없고 pinned implementation SHA/artifact provenance가 같으면 나중에
  E2E3만 재실행할 수 있다.

**E2E 종료 조건**: 선택한 시나리오가 E2E1 구현 검증과 E2E3 실제 process 검증을 모두
통과하고, 선택하지 않은 시나리오는 재개 가능한 `E2E-DEFERRED` ledger로 남음

### WP10. 독립 검토와 정리

- 공통 스펙의 요구사항별로 네 runtime 증거를 다시 대조한다.
- 새 pass-through wrapper, 중복 helper, temporal decomposition, test-only 우회가 생겼는지
  POSD/DDD 관점에서 검토한다.
- package, 전체 회귀, 35개 sample 증거가 target SHA와 같은지 확인한다. 이미 실행한 선택 E2E가
  있으면 그 SHA를 별도 확인하고, 남은 항목은 `E2E-DEFERRED` ledger에 고정한다.
- 남은 차이는 `LANGUAGE-MAPPING` 근거 또는 gap 항목 중 하나로만 남긴다.
- 모든 공개 문서가 plan 경로를 참조하지 않는지 확인한 뒤, 완료 시 이 임시 plan을
  삭제하거나 별도 기록 정책에 따라 보관한다.

## 11. 공통 검증 시나리오

### 11.1 Scheduler와 admission

- count limit 직전/직후, byte limit 직전/직후
- application 포화 중 lifecycle 진행
- lifecycle 연속 유입 중 application starvation 방지
- 같은 owner에서 nested submit과 explicit yield
- close/seal과 enqueue 동시 실행
- enqueue 거부 뒤 capacity·authority·sequence 불변
- handler exception 뒤 다음 turn 진행

### 11.2 Completion과 retry

- reply와 timeout 동시 도착
- cancellation과 close 동시 도착
- duplicate reply, 늦은 reply, unknown operation
- callback/promise가 다시 같은 owner 작업을 제출
- terminal callback 예외가 state table을 손상하지 않음

### 11.3 Relocation과 Message Follow

- A→B→A에서 첫 A의 stale cleanup과 새 A의 fence 구분
- seal 직전/직후 ingress와 accepted high-water
- duplicate 42/43/44, 순서가 바뀐 ACK/commit
- seal ACK 유실, commit reply 유실, deadline abort
- restart 뒤 seal 복원 또는 명시적 실패
- 같은 follow key의 동시 요청은 한 번만 전송
- expiry 또는 fence 변경 뒤 새 follow 허용
- logical multicast에서 target별 identity 분리

### 11.4 Observation과 ownership

- 한 source와 여러 source의 intermediate 폭주
- terminal queue overflow와 oldest-terminal discard
- 느린 observer와 callback 예외
- subscriber 추가/제거 반복 중 thread/executor 수 상한
- handler return 후 payload 접근 방지
- binding copy와 framework copy 횟수 측정
- malformed/oversize payload의 동일 error category

### 11.5 7종 sample의 공통 핵심 판정

| sample | 다섯 공개 표면에서 반드시 보일 핵심 동작 |
|---|---|
| TicTacToe | 수동 endpoint scale-out, room routing, request/response와 실시간 game flow; planned relocation 없음 |
| Bingo | matchmaking, session gateway, Actor binding, room `SpotWide` relocation과 bound push |
| DeliveryDispatch | courier 선택, timeout 재배정, tracking/customer push; planned relocation 없음 |
| SupportChat | conversation messaging, reconnect, idle timer, close와 bound push; planned relocation 없음 |
| GameQuest | missing Instance Spot cold activation, event/projection 갱신, explicit close 뒤 새 generation |
| ShoppingMall | 주문 workflow planned relocation 뒤 event replay와 다음 단계 재개 |
| ZoneWorld | zone 경계 Actor relocation, bound session 유지, border sync와 운영 fanout |

이 표는 runner 이름의 존재가 아니라 client가 확인할 업무 결과다. 언어별 API 문법은 달라도
역할 분리, relocation policy, 결과와 실패 의미는 공통 sample 문서와 같아야 한다.

## 12. 검증 명령 목록

계획 문서 자체의 최소 검증은 다음과 같다.

```bash
python3 doc/site/scripts/check_doc_links.py framework-plan
git diff --check -- framework/doc/plan/codex-framework-internals-four-language-convergence-plan.ko.md
```

보호 문서 patch 단계에서는 다음 공통 검증을 추가한다.

```bash
bash scripts/verify-framework-doc-contracts.sh
node framework/runtime/protocol/validate-service-wire-schema.mjs
node framework/runtime/protocol/generate-service-wire-assets.mjs --check
node framework/runtime/protocol/verify-service-wire-decoder-fixtures.mjs
```

WP7의 전체 회귀 command inventory는 WP1에서 build option과 지원 version을 고정하되, 현재
유지되는 진입점은 다음과 같다.

```bash
cmake --build framework/languages/cpp/build --parallel 1
ctest --test-dir framework/languages/cpp/build --output-on-failure

dotnet test framework/languages/dotnet/Zlink.Framework.sln -c Release -m:1

(cd framework/languages/java && \
  ./gradlew --no-daemon --no-parallel --max-workers=1 clean check)

(cd framework/languages/node && npm run verify:runtime-matrix)
```

C++ configure option 때문에 등록되지 않은 target, .NET의 지원 TFM/RID, JVM composite build
prerequisite와 Node version별 결과를 command inventory에 명시한다. 한 명령이 일부만
실행했다면 전체 PASS로 세지 않는다.

WP8의 35개 sample cell은 다음 통합 runner로 만든다. JVM은 Java를 먼저 완료하고 Kotlin을
시작한다.

```bash
framework/languages/cpp/samples/run_samples.sh
framework/languages/dotnet/samples/run_samples.sh
ZLINK_SAMPLE_LANGUAGES=java framework/languages/java/samples/run_samples.sh
ZLINK_SAMPLE_LANGUAGES=kotlin framework/languages/java/samples/run_samples.sh
framework/languages/node/samples/run_samples.sh
```

각 명령에는 실행 환경에 맞는 명시적 전체 timeout을 두되, timeout 증가로 실패를 숨기지
않는다. 통합 출력과 일곱 개별 run log를 대조해 35개 matrix를 채운다.

E2E 명령은 E2E0에서 선택한 scenario별로만 inventory에 넣는다. `run_e2e_all.sh`를 먼저
실행하지 않으며, E2E1의 source/fixture/runner/cleanup 구현 검증이 `READY-TO-RUN`으로 끝난
뒤 focused `run_e2e.sh`를 실행한다. aggregate 명령은 shared runner 변경 영향이 있을 때만
추가한다.

`verify-framework-doc-contracts.sh`의 현재 범위는 internals 전체 수렴을 증명하지 않는다.
WP3에서 다음을 검사하는 별도 checker 또는 확장 규칙을 추가한다.

- canonical scheduler limit의 네 runtime 일치
- 금지된 implicit inline path 또는 front insertion의 재등장
- wire command/fence field와 generated asset 일치
- 1.2 통합 목록과 5장 canonical 결정 표의 ID·값 일치
- internals 결정 ID와 regression test matrix의 연결
- 한국어/영어 결정 표의 누락

언어별 상세 명령은 각 regression test matrix의 실제 runner를 기준으로 WP1에서 고정한다.
문서에 명령 이름만 적고 실행하지 않은 결과를 PASS로 기록하지 않는다.

## 13. checkpoint와 변경 단위

변경은 다음 checkpoint로 나눈다.

1. `docs(contract)`: 승인된 spec/exact-interface/internals 결정
2. `protocol`: schema, generated asset, golden fixture
3. `runtime(cpp)`: production + white-box test
4. `runtime(dotnet)`: production + white-box test
5. `runtime(jvm)`: production + Java/Kotlin test
6. `runtime(node)`: production + test
7. `package-regression`: clean consumer + 언어별 전체 회귀
8. `samples`: 7종 × C++/.NET/Java/Kotlin/Node 실제 process
9. `conformance`: 영향받는 cross-language fixture + 성능·자원
10. `cleanup`: 중복 정책 제거와 독립 검토 반영
11. `e2e-follow-up`: 선택 E2E 구현 검증 + fixture 보완 + focused process 실행

공유 protocol/generated asset은 한 checkpoint와 한 integration owner가 관리한다. 언어별
작업은 다른 언어의 미완성 파일을 대신 수정하지 않는다. 각 checkpoint는 behavior 이름의
production 책임과 같은 이름의 test를 함께 정리한다.

보호 문서와 구현을 반드시 한 commit에 섞지는 않는다. contract commit이 먼저 들어가면
gap ledger에 영향받는 runtime, owner, 다음 checkpoint와 임시 `DIVERGED` 상태를 기록한다.
구현 checkpoint는 해당 결정을 production과 test에 함께 반영한다. 이 방식으로 spec-first
원칙을 지키면서도 문서와 구현이 어긋난 기간을 명시적으로 제한한다.

### 13.1 POSDDD 리팩터링 시점

한 모듈이 상태와 불변식을 소유하고, 호출자는 그 안의 보상·retry·ordering 절차를 알지
않아도 되는 구조로 정리한다. 이 계획에서는 이러한 POSD/DDD 구조 검토와 production·test
정리를 `POSDDD 리팩터링`이라 부른다. 동작을 확인하기 전에 넓게 구조를 바꾸면 회귀 원인과
책임 이동을 구분할 수 없으므로 다음 시점에 수행한다.

| 시점 | 상황 | 수행할 리팩터링 | 다음 단계 진입 조건 |
|---|---|---|---|
| WP1 조사 완료 뒤 | 실제 owner와 call path를 확인했지만 production 동작은 바꾸기 전이다. | 책임 graph, aggregate owner, 제거할 중복 정책과 허용할 language mapping을 설계한다. 이 단계에서는 source를 넓게 이동하지 않는다. | IC 행마다 바꿀 owner와 보존할 공개 동작이 정해져 있다. |
| 각 WP5 focused test PASS 뒤 | canonical 동작이 test로 고정되어 구조 변경의 전후를 비교할 수 있다. | production과 test를 함께 정리한다. 중복 상수·정책, pass-through wrapper, 시간 순서만 나눈 helper와 test-only 우회를 제거하고 deep module 안으로 불변식을 내린다. | 리팩터링 뒤 같은 focused test와 race/failure test가 다시 PASS한다. |
| runtime checkpoint 통합 뒤 | 언어별 구현은 동작하지만 공통 책임 이름과 fixture 연결이 어긋날 수 있다. | 네 runtime의 owner, error와 test 이름을 1.2의 IC 행에 맞춰 대조한다. 언어 primitive와 private layout은 같게 만들지 않는다. | 새 `DIVERGED`나 미분류 branch가 없고 package gate를 시작할 수 있다. |
| WP7·WP8 뒤 독립 검토 | 전체 회귀와 sample 증거가 있어 최종 구조 finding의 영향을 판단할 수 있다. | 필요한 finding만 좁게 수정한다. production을 바꾸면 영향받은 WP6~WP8 증거를 무효화하고 다시 실행한다. | 검증을 다시 통과했거나 finding이 근거와 함께 gap으로 남아 있다. |

리팩터링 commit은 behavior 변경과 같은 IC를 다루는 production·test만 포함한다. 다른 IC의
대규모 이름 변경, formatting과 unrelated cleanup을 함께 넣지 않는다. test는 private 호출
순서를 그대로 복제하지 않고 `reject_does_not_advance_authority`처럼 보존할 불변식을 이름과
assertion으로 드러낸다.

### 13.2 commit과 push gate

각 checkpoint는 다음 순서로 닫는다. 실행 중 commit·push를 요청받은 경우 이 gate를 통과한
checkpoint부터 현재 작업 branch에 순차적으로 push한다.

1. owned path와 사용자 변경을 `git status`와 path-limited diff로 구분한다.
2. checkpoint의 production, test, contract·ledger를 함께 검토하고 POSDDD finding을 반영한다.
3. focused test, 해당 문서/schema checker와 `git diff --check`를 다시 실행한다.
4. source fallback, skip, binary, package archive, 임시 log와 run directory가 stage되지 않았는지
   확인한다.
5. 해당 checkpoint의 owned path만 stage하고 한 IC 또는 결합할 수밖에 없는 IC 묶음으로
   commit한다.
6. commit SHA, 실행한 명령, 결과와 남은 gap을 ledger에 기록한다.
7. 현재 작업 branch를 push하고 local HEAD와 upstream SHA의 일치를 확인한다.

권장 commit 경계는 다음과 같다.

| commit | 포함 범위 | push 전 필수 증거 |
|---|---|---|
| `docs(contract)` | 승인된 common spec, exact interface, internals 한국어·영어와 checker | 문서 link/contract/schema 검사, 승인 범위와 path diff |
| `protocol` | schema, generated asset, golden fixture | generator `--check`, decoder fixture와 generated diff 검토 |
| `runtime(cpp)` | C++ production + 같은 IC의 white-box test | focused CTest, sanitizer가 필요한 IC는 sanitizer lane |
| `runtime(dotnet)` | .NET production + 같은 IC의 test | 안정된 직렬 focused/unit/contract test |
| `runtime(jvm)` | JVM production + Java/Kotlin API·test | framework-first 직렬 Gradle gate와 두 공개 표면 snapshot |
| `runtime(node)` | Node production + 같은 IC의 test | build, typecheck, lint와 focused runtime/contract test |
| `conformance` | package ledger, 공통 fixture, 필요한 cross-language와 resource 결과 | clean consumer와 영향 matrix |
| `samples` | sample/runner 수정과 35개 matrix ledger | 영향받은 전체 회귀와 7종 × 5개 process 증거 |
| `cleanup` | 독립 POSDDD·문서 원칙 리뷰에서 검증된 좁은 정리 | 영향받은 gate 재실행과 미해결 finding 기록 |
| `e2e-follow-up` | 선택한 E2E fixture/runner와 E2E ledger | E2E1 구현 검증, focused E2E process 증거 |

공통 schema와 generated asset은 integration owner만 commit한다. 언어별 agent는 다른 언어의
미완성 파일이나 공유 asset을 함께 stage하지 않는다. 보호 문서 commit은 G4 승인을 받은 뒤에만
만든다. push된 checkpoint를 후속 finding 때문에 바꿔야 하면 검증된 후속 commit으로 수정하고,
실패를 숨기기 위한 amend·force-push는 사용하지 않는다. main 반영 방식은 repository 정책과
사용자 승인 범위를 따르며, 중간 checkpoint는 작업 branch에 push한다.

## 14. 중단·rollback 조건

다음 상황에서는 구현을 계속 밀지 않고 확정 contract의 변경 심사로 돌아간다. 아래 항목은
미결정 사항이 아니라, 현재 확정안을 바꾸려면 새 before/after 의미와 migration 증거가
필요하다는 중단 조건이다.

- mixed-version peer가 exact relocation barrier를 지원하지 않음. 이 경우 downgrade하지 않고
  확정한 `Blocked/StateIncompatible`를 반환한다.
- 한 언어 binding 제약 때문에 공통 ownership/lifetime을 만족할 수 없음
- 수치 통일이 기존 공개 SLA 또는 memory bound를 악화시킴
- no-inline 전환이 공개 API의 합법적인 nested wait를 불가능하게 만듦
- relocation restart에서 안전한 high-water를 복원할 authority가 없음
- generated asset과 schema의 integration owner가 정해지지 않음
- package 또는 실제 process 증거가 source test와 반대 결과를 보임

rollback은 보호 문서와 구현의 의미가 다시 어긋나지 않게 같은 결정 단위로 수행한다.
실패한 실험을 이유로 language discretion을 포괄적으로 되살리지 않고, 불가능한 제약과
검증 기준을 구체적으로 기록한다.

## 15. 완료 정의

완료는 **결정별 구현 완료**와 **전체 프로그램 완료**, **후속 E2E 완료**를 구분한다. 한 IC는
영향받는 문서, 네 runtime 또는 입증된 mapping, package, 전체 회귀, 해당 sample/resource
검증이 같은 target SHA에서 닫히면 `DECISION-CLOSED`로 표시할 수 있다. 다른 IC가 열려 있다는
이유로 이 증거를 무효화하지 않지만, 열린 IC를 PASS로 세지도 않는다.

다음 조건을 모두 만족하면 WP0~WP10을 `IMPLEMENTATION-CLOSED`로 닫는다.

- 1.2의 모든 통합 행에 internals 결정문, 네 runtime 구현 판정과 검증 증거가 연결되어 있다.
- internals 12개 장의 모든 조사 행이 `MATCH` 또는 근거 있는 `LANGUAGE-MAPPING`이다.
- `DIVERGED`와 `UNVERIFIED`가 남아 있지 않다.
- common spec, exact interface, 한국어/영어 internals, schema가 같은 의미를 갖는다.
- 네 runtime의 state transition, bound, ordering, retry, terminal, ownership test가 같은
  scenario fixture를 통과한다.
- schema와 모든 generated asset, cross-language decoder fixture가 일치한다.
- C++, .NET, JVM(Java/Kotlin), Node package를 clean consumer가 실제로 사용한다.
- 언어별 전체 회귀가 filter·skip 없이 PASS하고, 일곱 sample × 다섯 공개 표면의 35개 실제
  process cell이 역할 증거, self-check, cleanup, exit status를 남긴다.
- slow observer와 owner 증가 시 queue, memory, thread/executor가 결정된 상한 안에 있다.
- ASan 등 언어별 memory/concurrency 검증과 aggregate runner가 별도 PASS다.
- 독립 검토에서 pass-through wrapper, 중복 정책, 숨은 fallback, test-only 우회가 없다.
- 공개 문서가 이 임시 plan을 링크하지 않는다.

기존 미완료 E2E는 `IMPLEMENTATION-CLOSED`를 막지 않는다. 대신 각 E2E 항목에 영향 결정,
현재 구현 상태, 선택/보류 이유, pinned SHA/artifact, 재개 명령을 기록해
`E2E-DEFERRED`로 넘긴다. 나중에 선택한 항목이 E2E1과 E2E3을 통과하면 그 항목만
`E2E-CLOSED`로 바꾼다. E2E 과정에서 production이 바뀌지 않았다면 WP6~WP8을 다시 실행하지
않는다.

## 16. 이번 문서 이후의 첫 승인 요청 범위

첫 승인은 크기가 다른 두 묶음으로 요청한다.

### 16.1 사실성 정정 묶음

코드 동작을 바꾸지 않는 보호 문서 정정부터 분리한다.

1. `10-liveness-and-state`의 bool-only readiness와 early-serving “한 구현” 문구를 현재
   source/test에서 재확인한다.
2. public 7-state enum의 네 runtime 일치와 status `isReady` derivation, 별도 readiness
   gate의 authority 관계를 기록한다.
3. C++ maintenance 6-state와 Node discovery state는 public state 위반으로 단정하지 않고
   projection 조사 항목으로 연결한다.
4. `07-dispatch-loop`의 wakeup을 empty→non-empty 즉시 signal/callback으로 확정하고 정상
   처리에서 주기적 polling을 금지한다.
5. internals README 결정 표가 설명 문단 때문에 끊기지 않게 Markdown 구조를 검증한다.
6. 한국어·영어 대응 절과 문서 checker 영향 범위를 함께 제시하고 승인을 받는다.

이 묶음은 internals가 현재 canonical 동작만 기술하게 만든다. source 재확인 결과 실제
위반이 있으면 일반화한 visible failure와 별도 implementation gap으로 분리한다.

### 16.2 첫 동작 통합 묶음

사실성 정정 뒤 serial execution을 첫 구현 묶음으로 진행한다.

1. IC-01/IC-02/IC-03의 source/test matrix와 5장의 착수 전 benchmark/nested-wait 증거
   gate를 완성한다.
2. canonical limit, logical lane, no-inline, explicit yield/resume을 결정문 형식으로 만든다.
3. 영향을 받는 common spec, `02-serialization`, `03-progress-isolation`,
   `07-dispatch-loop`, 관련 exact interface 절을 파일과 절 단위로 제시한다.
4. .NET과 Node의 migration diff 예상, nested wait compatibility, benchmark/test 명령을
   첨부한다.
5. 보호 문서 변경 승인을 받은 뒤 contract patch를 먼저 merge하고 gap ledger에 runtime별
   구현 checkpoint를 기록한다.
6. shared fixture와 C++/.NET/JVM/Node 구현을 각 checkpoint로 정렬한다.
7. package/clean consumer, 언어별 전체 회귀, 7종 × 5개 공개 표면 sample을 순서대로
   실행한다. E2E는 영향 항목을 `E2E-DEFERRED`로 넘기고, 별도 workstream에서 E2E 구현
   검증부터 시작한다.

그 다음 observation/state projection, relocation/Message Follow, object-kind lifecycle
순으로 확장한다. object-kind의 .NET/C++ domain variant 도입은 가장 큰 구조 변경이므로
마지막 구현 묶음으로 둔다. relocation은 wire와 restart 의미가 함께 바뀌므로 독립
승인 묶음으로 유지한다.
