# R3 bindings 심층 리뷰

## 요약 표

| 번호 | 제목 | 분류 | 행동 변경 | 규칙 수 | 성능 영향 | 확신 |
|---|---|---|---|---|---|---|
| F-R3-1 | WRITABLE의 RID를 일곱 바인딩이 다시 검증 | lower-layer-reverification | 없음 | 8 → 1 | 있음 | 높음 |
| F-R3-2 | Java의 3-part 이상 수신에 임시 Received 재채택 잔존 | scattered-control | 없음 | 2 → 1 | 있음 | 높음 |
| F-R3-3 | 수신 결과 수명과 HWM 회계의 분리 규칙 중복 | consolidation | 없음 | 10 → 1 | 없음 | 높음 |
| F-R3-4 | receive-flow 공통 투영 계약이 언어 문서에 분산 | consolidation | 없음 | 8 → 1 | 없음 | 높음 |
| F-R3-5 | provisional registry 등록 알고리즘을 공통 계약으로 강제 | consolidation | 없음 | 9 → 1 | 없음 | 높음 |
| F-R3-6 | codec 별도 배포 의무와 raw-only 정책 충돌 | consolidation | 없음 | 2 → 1 | 없음 | 높음 |
| F-R3-7 | Core payload 보관 모델과 0.17 wait-token 모델 혼재 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R3-8 | reply timeout의 시작점을 admission 이전으로 설명 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R3-9 | 제거된 Core callback을 완료 전달의 기준으로 설명 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R3-10 | monitor ABI v3·v4를 동시에 요구 | spec-impl-drift | 없음 | 2 → 1 | 없음 | 높음 |
| F-R3-11 | C++ SEND 대기 토큰이 기본 completion 진행을 중단 | parity-gap | 있음 | 2 → 1 | 있음 | 높음 |
| F-R3-12 | 네 언어가 NO_DATA 전에 WRITABLE을 재제출 | spec-impl-drift | 있음 | 2 → 1 | 있음 | 높음 |
| F-R3-13 | 정상적인 tokenless EAGAIN을 INTERNAL_ERROR로 변경 | spec-impl-drift | 있음 | 2 → 1 | 없음 | 높음 |
| F-R3-14 | Java가 target 제거 상태로 Core 오류를 재분류 | lower-layer-reverification | 있음 | 2 → 1 | 있음 | 높음 |
| F-R3-15 | .NET·Java 송신 잠금이 Core 제출 경합을 직렬화 | lower-layer-reverification | 있음 | 3 → 1 | 있음 | 높음 |
| F-R3-16 | Node 동기 request가 지정된 completion owner를 우회 | parity-gap | 있음 | 2 → 1 | 있음 | 높음 |
| F-R3-17 | Go HWM 공개 타입이 uint64 계약보다 좁음 | parity-gap | 있음 | 2 → 1 | 없음 | 높음 |

검토는 읽기 전용 정적 진단이다. 필수 스펙 13개, 11,902행을 모두 읽었다. 빌드·테스트·벤치마크는 실행하지 않았으며 성능 수치는 추정하지 않았다. 표의 성능 영향은 제거되거나 바뀌는 코드 경로가 있다는 뜻이다. 행동 변경은 문구 변경 자체가 아니라 제안을 적용했을 때 애플리케이션이 받는 결과·오류·순서·진행 조건·공개 타입이 달라지는지를 기준으로 표시했다. 현재 구현을 유지하는 문서 정합화는 ‘없음’으로 분류했다. 행동 변경이 있는 일곱 항목은 0.18.0 검토 대상이다.

규칙 수는 finding마다 명시한 판정 소유자, 독립적인 문서 규정 위치 또는 상충하는 의미를 센다. 코드 행 수나 전체 저장소의 규칙 수가 아니며 finding 간 합산하지 않는다. Core가 보장하는 wake 이후의 필수 재제출과 언어 waiter 연결은 제거 대상이 아니다. Core의 request 수명 관리와 binding의 언어 완료 객체 연결도 서로 다른 책임이다.

기준 커밋은 `2ab576f4f2b085e2178c6896d652c12e7e6f8643`이다. 검토 중 Core 문서와 CONTRIBUTING에 병행 편집이 나타났으므로 아래 **Core socket 문서 인용은 해당 커밋의 file:line**으로 고정했다. binding 스펙·구현은 읽은 작업 트리 기준이다. 시작부터 있던 Node provenance 변경과 untracked 파일은 검토 수정 대상에서 제외했다. D-B117의 .NET 수정과 D-B119/D-B120의 Core wake 수정은 반영된 기준으로 대조했다.

## Findings

### F-R3-1 WRITABLE의 RID를 일곱 바인딩이 다시 검증

- 분류: lower-layer-reverification
- 위치: `core/doc/spec/core/socket/README.ko.md:302`, `core/doc/spec/core/socket/README.ko.md:986–994`, `core/doc/spec/core/socket/README.ko.md:1072–1074`, `core/doc/spec/core/socket/README.ko.md:1147–1151`, `core/doc/spec/core/socket/README.ko.md:1406–1407`에서 submit RID 또는 empty의 echo를 반복 보장한다. Binding의 reply-token owner 검증은 별도 규칙인 `bindings/doc/spec/async-coroutine-policy.ko.md:86–94`다.
- 현재 규칙(인용): “`peer_rid`는 reservation 시점의 logical peer snapshot이다.” / “ROUTER·STREAM WRITABLE과 ROUTER REQUEST에서는 submit RID다.”
- 문제: socket-local context·token으로 대상 entry를 찾은 뒤에도 C++·Java·.NET·Node·Go·Rust·Python이 Core가 넣은 RID를 원래 target과 다시 비교하여 불일치를 INTERNAL_ERROR로 만든다. Java는 비교용 `RoutingId`를 만들고 .NET은 `ToBytes()`를 호출한다. Core의 동일 사실을 일곱 언어가 재판정한다. 여기서 제거할 것은 RID 동일성 검사이며 entry 조회, tagged union 분기, native 수명 보호나 다른 socket의 ReplyToken을 막는 검사가 아니다.
- 제안: 소유자는 Core socket의 completion record 계약이고 binding 공통 README의 완료 결과 투영 절에는 “**바인딩은 socket-local context·token으로 찾은 WRITABLE을 해당 waiter에 전달하며, Core가 보장한 submit RID echo를 다시 판정하지 않는다.**” 한 문장만 둔다.
- 규칙 수: before 8 → after 1 — Core RID echo 보장 1곳의 소유권 + 일곱 binding 판정 → Core 소유권 하나.
- 행동 변경: 없음 — Core 계약을 만족하는 completion에서 결과·재제출 대상·오류가 같다; 잘못된 Core record 주입을 정상 application 계약으로 간주하지 않는다.
- 영향: bindings(cpp, java, dotnet, node, go, rust, python) — 아래 WRITABLE 변환·검증 경로.
- 성능 영향: 있음 — WRITABLE마다 RID 길이·byte 비교가 사라지며 Java/.NET의 비교용 객체·배열 생성도 제거 대상이다; 양은 미측정.
- 근거 코드: C++ `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:321–347`, `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:385–401`; Java `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:709–721`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:1119–1124`; .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:610–620`, `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:984–993`; Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:529–544`; Go `bindings/go/internal/native/completion_owner.go:683–689`, `bindings/go/internal/native/completion_owner.go:781–804`와 `bindings/go/internal/native/dealer_router_request.go:126–134`; Rust `bindings/rust/src/internal/completion_owner.rs:715–724`; Python `bindings/python/src/zlink/_runtime/messaging/routed_async.py:815–820`, `bindings/python/src/zlink/_runtime/messaging/routed_async.py:845–869` 및 이를 호출하는 `bindings/python/src/zlink/_native/hotpath.h:1071`. C는 `bindings/c/include/zlink/socket/api.h:225–237`의 raw ABI이며 별도 언어 completion runtime이 없다.
- 확신: 높음 — 일곱 고수준 언어의 비교를 확인했다. Core 보장을 제거하거나 ReplyToken owner 검증까지 없애는 제안은 아니다.

### F-R3-2 Java의 3-part 이상 수신에 임시 Received 재채택 잔존

- 분류: scattered-control
- 위치: `bindings/doc/spec/README.ko.md:943–957`; `bindings/doc/spec/java/README.ko.md:674–680`, `bindings/doc/spec/java/README.ko.md:690–708`. 선행 정리의 범위는 `doc/plan/c016-worklog/decisions.ko.md:1263–1264`(D-B115), 공통 단순화 기준은 `AGENTS.md:66–70`의 단일 소유자·중복 금지 조항이다.
- 현재 규칙(인용): “`recv` 호출자는 long-lived 결과 저장소를 미리 만들어 매 호출마다 같은 인스턴스를 넘긴다.”
- 문제: Java ROUTER의 caller-provided receive는 1·2-part를 target에 직접 채우지만 3-part 이상에서는 `Received fresh`를 생성한 뒤 `receivedAdoptFrom(target, fresh)`로 다시 옮긴다. `adoptFrom`은 payload·routing·reply 상태를 target에 복사한 다음 source의 같은 필드를 비운다. D-B115의 수신 직접 이동이 메시지 모양에 따라 갈라져 있다. 임시 part 준비 자체와 이 임시 envelope의 소유권 왕복은 구분해야 한다.
- 제안: 소유자는 Java의 기존 수신 결과 저장소 초기화 경로이며, 공통 README 수신 저장소 절의 통합 문장은 “**완성된 part와 metadata는 part 수와 관계없이 호출자가 제공한 결과 저장소가 한 번만 채택하며, 중간 결과 객체를 거쳐 다시 채택하지 않는다.**”로 한다; 기존 성공 확정 시점과 실패 cleanup을 유지하면서 기존 채택 로직의 소유자를 한 곳으로 옮긴다.
- 규칙 수: before 2 → after 1 — 1·2-part 직접 채택 / 3-part 이상 임시 envelope 후 재채택 → 최종 저장소 채택 하나.
- 행동 변경: 없음 — 완성된 동일 part·metadata를 같은 성공 시점에 제공하고 기존 실패 시 정리·수명을 유지하는 범위의 제안이다.
- 영향: bindings(java) — `bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterReceiveSupport.java:165–232`.
- 성능 영향: 있음 — 3-part 이상 routed receive의 임시 Received 할당과 envelope 필드 이동·source 비우기가 사라진다; payload byte copy 감소라고 주장하지 않는다.
- 근거 코드: `bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterReceiveSupport.java:165–205`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterReceiveSupport.java:209–232`; `bindings/java/src/main/java/systems/zlink/contracts/messaging/Received.java:354–388`. 다른 언어의 모든 multipart envelope 구현은 이 finding에서 확인하지 않았다; Java 내부의 두 실제 경로를 대조했다.
- 확신: 높음 — 3-part 이상 잔존 경로를 확인했다. D-B115 전체가 미적용이라는 주장이 아니다.

### F-R3-3 수신 결과 수명과 HWM 회계의 분리 규칙 중복

- 분류: consolidation
- 위치: `bindings/doc/spec/README.ko.md:1314–1321`; `bindings/doc/spec/cpp/README.ko.md:532–537`, `bindings/doc/spec/cpp/README.ko.md:594–596`; `bindings/doc/spec/java/README.ko.md:703–708`; `bindings/doc/spec/node/README.ko.md:684–690`; `bindings/doc/spec/dotnet/README.ko.md:481–487`; `bindings/doc/spec/go/README.ko.md:129–130`, `bindings/doc/spec/go/README.ko.md:177–182`; `bindings/doc/spec/rust/README.ko.md:519–524`; `bindings/doc/spec/python/README.ko.md:108–113`. 하위 의미의 근거는 `core/doc/spec/core/systems/06-auto-hwm.ko.md:393–397`, `core/doc/spec/core/systems/06-auto-hwm.ko.md:465–469`이다.
- 현재 규칙(인용): “Receive가 complete message를 dequeue해 binding에 넘기면 그 charge는 끝난다.”
- 문제: 공통과 일곱 언어 문서가 동일한 HWM 회계 종료 시점·결과 수명 분리를 재서술하고 C++·Go는 자기 문서 안에서도 반복한다. 언어에 따라 다른 것은 결과 타입과 close/dispose/drop 표현이며 Core 회계가 아니다. 반복 중 C++의 “payload만 계산”은 공통의 physical frame metadata charge 설명보다 좁게 읽히기도 한다.
- 제안: 소유자는 공통 README 수신 ownership 절이며 “**수신 결과는 언어별 수명 규칙으로 payload를 소유하되, Core dequeue에서 끝난 HWM charge를 결과의 보유·재사용·정리와 다시 연결하지 않는다.**” 한 문장과 Core 회계 절 링크를 둔다; 언어 문서에는 타입·수명 API 차이만 남긴다.
- 규칙 수: before 10 → after 1 — 공통 1 + C++ 2 + Java·Node·.NET 각 1 + Go 2 + Rust·Python 각 1개의 독립 서술 위치.
- 행동 변경: 없음 — 문서 소유권 통합이며 receive 또는 cleanup을 변경하지 않는다.
- 영향: bindings(cpp, java, node, dotnet, go, rust, python) — 문서만; 대조 구현은 아래 두 언어.
- 성능 영향: 없음 — 실행 경로 변경을 제안하지 않는다.
- 근거 코드: Java `bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterReceiveSupport.java:142–174`; Go `bindings/go/internal/native/received.go:48–78`, `bindings/go/internal/native/received.go:145–163`. 이 두 경로에서 native part·metadata의 언어 수명 관리를 확인했다. 다른 다섯 언어의 모든 recv/subscribe cleanup을 코드로 검증하지 않았으며 HWM 수치 변화도 실행 검증하지 않았다.
- 확신: 높음 — 중복 문장은 전 범위에서 확인했다; HWM 구현 적합성 판정과는 별개다.

### F-R3-4 receive-flow 공통 투영 계약이 언어 문서에 분산

- 분류: consolidation
- 위치: `bindings/doc/spec/c/README.ko.md:165–201`; `bindings/doc/spec/cpp/README.ko.md:544–563`; `bindings/doc/spec/java/README.ko.md:819–838`; `bindings/doc/spec/node/README.ko.md:626–644`; `bindings/doc/spec/dotnet/README.ko.md:593–612`; `bindings/doc/spec/go/README.ko.md:209–228`; `bindings/doc/spec/rust/README.ko.md:465–485`; `bindings/doc/spec/python/README.ko.md:129–147`. 공통은 `bindings/doc/spec/README.ko.md:1323–1326`에서 setter만 언급한다. Core 의미는 `core/doc/spec/core/socket/README.ko.md:690–708`, `core/doc/spec/core/socket/README.ko.md:1413–1415`가 소유한다.
- 현재 규칙(인용): “Flow-state frame은 Core 안에 머문다.”
- 문제: RUNNING/PAUSED 의미, 재설정의 멱등성, 미지원 socket 결과, 공통 monitor event·flag·다섯 field, frame을 binding이 처리하지 않는다는 규칙이 언어별로 복제돼 있다. 이들은 언어별 재량이 아니다. 공통 계약만 읽는 사람은 이 동작을 알기 위해 특정 언어 문서를 찾아야 한다.
- 제안: 소유자는 공통 README의 receive-flow 투영 절이며 “**모든 바인딩은 Core receive-flow setter의 상태 전이·결과와 monitor 관측값을 손실 없이 투영하며 flow-state frame의 처리는 Core에 둔다.**” 한 문장과 Core 계약 링크를 둔다; 언어 문서는 enum·signature·예외 표현만 소유한다.
- 규칙 수: before 8 → after 1 — 여덟 언어 문서의 공통 행동 규정 → 공통 투영 규정 하나.
- 행동 변경: 없음 — 동일 계약의 위치를 통합하며 언어별 예외 타입을 통일하지 않는다.
- 영향: bindings(c, cpp, java, node, dotnet, go, rust, python) — 문서만.
- 성능 영향: 없음 — 실행 경로 변경 없음.
- 근거 코드: C++ `bindings/cpp/src/Runtime/Sockets/socket.cpp:157–162`; Java `bindings/java/src/main/java/systems/zlink/runtime/sockets/SocketOptionSupport.java:203–208`; Go `bindings/go/internal/native/socket_option_support.go:36–40`; Python `bindings/python/src/zlink/_runtime/sockets/socket_base.py:425–440`; C `bindings/c/include/zlink/socket/api.h:199–200`. .NET·Node·Rust의 monitor field 전체 변환은 이 finding에서 코드 검증하지 않았다.
- 확신: 높음 — 여덟 문서의 동일 행동을 확인했으며 구현 차이를 새로운 언어별 예외로 만들지 않았다.

### F-R3-5 provisional registry 등록 알고리즘을 공통 계약으로 강제

- 분류: consolidation
- 위치: `bindings/doc/spec/async-execution-model.ko.md:84–104`; `bindings/doc/spec/async-coroutine-policy.ko.md:54–66`; `bindings/doc/spec/cpp/README.ko.md:666–670`; `bindings/doc/spec/java/README.ko.md:648–655`, `bindings/doc/spec/java/README.ko.md:996–1001`; `bindings/doc/spec/node/README.ko.md:784–788`; `bindings/doc/spec/dotnet/README.ko.md:701–705`; `bindings/doc/spec/rust/README.ko.md:615–619`; `bindings/doc/spec/python/README.ko.md:172–176`. Core가 요구하는 실제 context 수명은 `core/doc/spec/core/socket/README.ko.md:948–952`, `core/doc/spec/core/socket/README.ko.md:1051–1056`이다.
- 현재 규칙(인용): “Completion-backed terminal은 native `FINAL` 호출 전에 language operation state를 stable `user_context`로 찾을 수 있도록 socket-local registry에 provisional 상태로 등록한다.”
- 문제: 완료 정확히 한 번과 context 수명이라는 계약을 ‘항상 native 호출 전에 map entry 등록’이라는 알고리즘으로 고정했고 이를 아홉 위치에 복제했다. Java는 drain을 막고 native 호출 후 등록하며 Go의 즉시 성공 SEND는 entry·channel·map 등록을 만들지 않는다. 이는 D-B121이 의도적으로 제거한 비용이다. C++은 조기 completion 보관으로 합류한다. 언어 내부 자료구조가 다르다는 사실을 공개 동작 차이로 취급하면 정상 최적화를 되돌리는 규칙이 된다.
- 제안: 소유자는 async-execution-model의 완료 합류 계약이며 “**바인딩은 Core에 전달한 context의 유효 수명을 보장하고 submit 결과와 completion이 경합해도 언어 terminal을 정확히 한 번 끝내며 남은 native payload를 정확히 한 번 정리한다.**”로 통합한다; 등록 시점·map·TCS·early-record 알고리즘은 각 구현이 소유하고 검증 절에는 관찰 가능한 결과만 둔다.
- 규칙 수: before 9 → after 1 — 공통 실행 모델 1 + coroutine 정책 1 + C++ 1 + Java 2 + Node·.NET·Rust·Python 각 1.
- 행동 변경: 없음 — 완료·수명·취소의 관찰 계약을 유지하는 문서 통합이다; PollCompletion의 미결정 progress 정의는 이 제안에 포함하지 않는다.
- 영향: bindings(cpp, java, dotnet, node, go, rust, python) — 문서만; 구현은 다양한 합류 방식의 근거다.
- 성능 영향: 없음 — 지금 구현을 바꾸지 않는다; 제거된 즉시 SEND 할당을 문서 때문에 되살릴 필요가 없어진다.
- 근거 코드: Go `bindings/go/internal/native/dealer_router_request.go:256–304`; Java `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:101–143`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:161–177`; .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:39–100`; C++ `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:63–80`와 `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:664–672`; Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:264–285`; Rust `bindings/rust/src/internal/completion_owner.rs:396–424`; Python `bindings/python/src/zlink/_native/hotpath.h:1455–1484`. C는 언어 registry를 제공하지 않는다.
- 확신: 높음 — 명세의 구체 등록 알고리즘과 실제 정상 빠른 경로가 다르다. 각 언어의 모든 취소 경합에 대한 실행 적합성까지 판정하지 않았다.

### F-R3-6 codec 별도 배포 의무와 raw-only 정책 충돌

- 분류: consolidation
- 위치: `bindings/doc/spec/README.ko.md:840–841`, `bindings/doc/spec/README.ko.md:3775–3802`, `bindings/doc/spec/README.ko.md:3821–3848`, `bindings/doc/spec/README.ko.md:3850–3867`; `bindings/doc/spec/cpp/README.ko.md:413–414`; `bindings/doc/spec/dotnet/README.ko.md:83–84`; `bindings/doc/spec/node/README.ko.md:94–95`; `bindings/doc/spec/go/README.ko.md:43–46`, `bindings/doc/spec/go/README.ko.md:277`; `bindings/doc/spec/rust/README.ko.md:75–76`. 관련 잔존 서술은 `bindings/doc/spec/java/README.ko.md:204`의 codec 디렉터리와 `bindings/doc/spec/dotnet/api-reference-comments.ko.md:20–21`의 codec package 전제다.
- 현재 규칙(인용): “`C`를 제외한 binding 은 codec extension layer 를 public contract 로 두며, `protobuf`, `json`, `messagepack` 세 codec 을 지원해야 한다.” / “Bindings는 더 이상 codec extension 배포 단위를 정의하지 않는다.”
- 문제: 같은 공통 문서가 binding-owned codec 세 개의 별도 배포를 의무화하면서 뒤에서는 여덟 언어 모두 ‘없음’으로 규정한다. Go·Python·Rust의 앞부분 예외와도 충돌한다. reader가 설치할 package와 binding의 책임 범위를 결정할 수 없다. 언어별 codec 선택표를 유지하면 이미 없어진 배포 의무를 다시 도입하는 근거가 된다.
- 제안: 소유자는 공통 README의 raw binding 범위이며 “**바인딩은 raw Message·byte payload 계약만 배포하고 codec extension 배포 단위를 소유하지 않는다.**”로 통합한다; 언어별 문서는 해당 언어의 실제 package 위치만 안내한다.
- 규칙 수: before 2 → after 1 — 비-C codec 별도 배포 의무 / 전 언어 raw-only → raw-only 하나.
- 행동 변경: 없음 — 현재 raw-only 정책과 확인한 package 구성을 명확히 하는 제안이며 기존 package 제거·dependency 변경을 제안하지 않는다.
- 영향: bindings(c, cpp, java, node, dotnet, go, rust, python) — 문서만.
- 성능 영향: 없음 — codec 도입이나 런타임 직렬화 변경 없음.
- 근거 코드: `bindings/rust/Cargo.toml:18–31`의 raw crate와 빈 workspace members; `bindings/node/package.json:19–36`의 단일 public root 및 배포 파일; `bindings/go/go.mod:1–5`의 module 구성. C·C++·Java·.NET·Python의 모든 배포 산출물이나 외부 registry에 남은 package는 확인하지 않았다.
- 확신: 높음 — 정식 문서 내부 모순은 확정이며 외부 package의 폐기를 결정한 finding이 아니다.

### F-R3-7 Core payload 보관 모델과 0.17 wait-token 모델 혼재

- 분류: spec-impl-drift
- 위치: `bindings/doc/spec/README.ko.md:1328–1334`; `bindings/doc/spec/async-execution-model.ko.md:92–100`; `bindings/doc/spec/async-coroutine-policy.ko.md:56–65`; `bindings/doc/spec/c/README.ko.md:298–309`, `bindings/doc/spec/c/README.ko.md:325–333`, `bindings/doc/spec/c/README.ko.md:448–451`. 버전 고정 문장은 `bindings/doc/spec/cpp/README.ko.md:664`, `bindings/doc/spec/java/README.ko.md:990`, `bindings/doc/spec/node/README.ko.md:782`, `bindings/doc/spec/dotnet/README.ko.md:699`, `bindings/doc/spec/go/README.ko.md:9–11`, `bindings/doc/spec/go/README.ko.md:21`, `bindings/doc/spec/go/README.ko.md:43`, `bindings/doc/spec/go/README.ko.md:185`, `bindings/doc/spec/go/README.ko.md:263–265`, `bindings/doc/spec/go/README.ko.md:285`, `bindings/doc/spec/rust/README.ko.md:613`, `bindings/doc/spec/python/README.ko.md:9–16`, `bindings/doc/spec/python/README.ko.md:170`에도 남아 있다. 대조 Core 계약은 `core/doc/spec/core/socket/README.ko.md:954–1022`, `core/doc/spec/core/socket/README.ko.md:1058–1088`, `core/doc/spec/core/socket/README.ko.md:1334–1366`.
- 현재 규칙(인용): “`DONTWAIT FINAL` send가 즉시 local admission되지 않으면 Core가 payload를 보관하고 nonzero completion ID를 반환한다.” / “Native submit이 실패하면 ID `0`을 확인하고 provisional state를 제거한 뒤 submit error로 terminal을 끝낸다.”
- 문제: C 문서는 payload-owning pending SEND와 SEND completion을 정의하지만 현행 Core는 실패 결과 BACKPRESSURED와 nonzero wait token을 반환하고 payload를 보관하지 않는다. 공통 실행 모델은 ‘실패이면 ID 0’, ‘successful nonzero SEND’만 설명하므로 가장 중요한 BACKPRESSURED+token 분기가 없다. Core 소유 재시도 FIFO와 unlimited pending이라는 설명도 현행 65,536 completion reservation 및 binding 보관 payload와 다르다. 이 문서대로 구현하면 토큰을 오류로 버리거나 payload를 잃는다.
- 제안: 소유자는 공통 README의 Core 결과 투영 절이며 “**SEND의 OK·ID 0은 admission 완료이고 REQUEST의 OK·nonzero ID는 reply 대기이며, BACKPRESSURED·nonzero ID는 Core가 payload 없이 보관하는 대기 토큰이므로 바인딩은 보관한 입력을 WRITABLE 수신과 NO_DATA drain 뒤 다시 제출한다.**”로 통합하고 C 문서는 Core ABI를 참조한다; package 버전은 실제 release metadata를 참조하게 한다.
- 규칙 수: before 2 → after 1 — Core payload-owning pending 모델 / payload-free wait-token 모델 → 현행 wait-token 모델.
- 행동 변경: 없음 — 제안은 이미 구현된 0.17 모델에 문서를 맞추는 범위다; F-R3-11~17의 구현 결함 수정은 포함하지 않는다.
- 영향: bindings(c, cpp, java, node, dotnet, go, rust, python) — 공통·언어 문서.
- 성능 영향: 없음 — 문서만 변경하며 새 payload queue나 timer를 제안하지 않는다.
- 근거 코드: C `bindings/c/include/zlink/socket/api.h:225–237`, `bindings/c/include/zlink/socket/api.h:253–261`; C++ `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:217–231`; Java `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:158–207`; .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:194–213`; Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:336–372`; Go `bindings/go/internal/native/dealer_router_request.go:368–404`; Rust `bindings/rust/src/runtime/messaging/operations/routed_async.rs:252–319`; Python `bindings/python/src/zlink/_native/hotpath.h:1477–1502`. Release 버전은 `bindings/node/package.json:3`, `bindings/rust/Cargo.toml:3`에서 0.17.0을 확인했다; 다른 언어의 설치된 binary·package 정확 버전은 확인하지 않았다.
- 확신: 높음 — 여덟 언어의 C ABI 또는 wrapper 경로가 토큰 모델을 사용한다. ‘모든 배포 binary가 갱신됐다’는 주장은 하지 않는다.

### F-R3-8 reply timeout의 시작점을 admission 이전으로 설명

- 분류: spec-impl-drift
- 위치: `bindings/doc/spec/README.ko.md:1349–1356`, `bindings/doc/spec/README.ko.md:3128–3135`; `bindings/doc/spec/dotnet/README.ko.md:589–591`; 하위 계약은 `core/doc/spec/core/socket/README.ko.md:1070`, `core/doc/spec/core/socket/README.ko.md:1083–1088`, `core/doc/spec/core/socket/README.ko.md:1359–1361`.
- 현재 규칙(인용): “timeout 은 send 대기 + reply 대기를 합산한 전체 경과 시간에 적용된다.” / “`Async(...)`는 선택한 exact target의 HWM credit을 기다리는 동안 request의 원래 deadline을 유지한다.”
- 문제: Core의 reply timeout은 OK admission 때 시작하고 wait token이 유지되는 동안 흐르지 않는다. 공통·.NET 문구는 최초 제출에서 deadline이 이미 시작된 것처럼 설명한다. binding의 재제출 경로는 같은 timeout 길이를 다시 Core에 넘길 뿐 자체 deadline을 차감하지 않는다. ‘timeout은 Core 소유’라는 동일 문단의 원칙과도 충돌한다.
- 제안: 소유자는 Core request timeout 계약이며 공통 binding 문장은 “**Request의 reply timeout은 Core admission부터 흐르며, admission 전 대기 토큰의 수명과 caller의 언어 wait cancellation은 reply timeout에 합산하지 않는다.**”로 통합한다.
- 규칙 수: before 2 → after 1 — 최초 제출부터 총 경과 / admission부터 reply 경과 → Core admission 기준 하나.
- 행동 변경: 없음 — 현행 Core와 wrapper의 timeout 인자 전달을 문서화하며 timeout 값이나 대기 예산을 변경하지 않는다.
- 영향: bindings(dotnet 및 공통 계약을 사용하는 c, cpp, java, node, go, rust, python) — 문서만.
- 성능 영향: 없음 — timer 추가·삭제나 budget 조정 없음.
- 근거 코드: C `bindings/c/include/zlink/socket/api.h:253–259`; .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:1404–1405`; Java `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:838–840`; Go `bindings/go/internal/native/dealer_router_request.go:51–60`, `bindings/go/internal/native/dealer_router_request.go:151–158`; Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:575–590`; Rust `bindings/rust/src/runtime/messaging/operations/routed_async.rs:252–267`; Python `bindings/python/src/zlink/_runtime/messaging/routed_async.py:1098–1107`. C++는 `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:176–188`의 native timeout 전달을 확인했다. 이들 파일 밖 모든 service timeout은 검토하지 않았다.
- 확신: 높음 — Core가 시작점을 명시하며 wrapper 경로에서 admission 전 경과를 reply budget에서 차감하는 로직은 확인되지 않았다.

### F-R3-9 제거된 Core callback을 완료 전달의 기준으로 설명

- 분류: spec-impl-drift
- 위치: `bindings/doc/spec/README.ko.md:1349–1354`, `bindings/doc/spec/README.ko.md:1364–1366`, `bindings/doc/spec/README.ko.md:1433–1439`, `bindings/doc/spec/README.ko.md:2977–2983`, `bindings/doc/spec/README.ko.md:3024–3065`, `bindings/doc/spec/README.ko.md:3093–3099`, `bindings/doc/spec/README.ko.md:3134`, `bindings/doc/spec/README.ko.md:3139–3141`, `bindings/doc/spec/README.ko.md:3179–3197`; `bindings/doc/spec/node/README.ko.md:716–717`. 현행 계약은 `bindings/doc/spec/c/README.ko.md:300–304`, `bindings/doc/spec/c/README.ko.md:429–432`; `bindings/doc/spec/async-execution-model.ko.md:63–82`; `bindings/doc/spec/async-coroutine-policy.ko.md:73–82`, `bindings/doc/spec/async-coroutine-policy.ko.md:104–112`; `core/doc/spec/core/socket/README.ko.md:1175–1180`, `core/doc/spec/core/socket/README.ko.md:1409–1411`.
- 현재 규칙(인용): “core 는 callback 기반 비동기 모델을 제공한다.” / “send completion과 request reply는 Core callback을 native TSFN으로 전달한다.”
- 문제: 현행 raw API는 completion pull이며 Core poller wait는 record를 소비하지 않는다. 그런데 공통 문서는 Core callback dispatch와 callback→Future mapping을 기준으로 삼고 C++ callback terminal 및 framework 소유 coroutine 연결까지 요구한다. Node 역시 현행 native completion recv·event-loop watcher와 다른 TSFN 경로를 설명한다. Core의 request wire matching과 binding의 language continuation 실행은 소유자가 다른 결정인데 옛 ‘dispatch’라는 한 단어로 합쳐져 있다.
- 제안: 소유자는 async-execution-model의 완료 전달 계약이며 “**Core는 REQUEST·WRITABLE record를 pull queue로 제공하고 바인딩의 단일 completion owner가 이를 언어 terminal로 전달한다.**”로 통합한다; 공통 README와 언어 README는 이 계약을 링크하고 각 언어 terminal signature만 소유한다.
- 규칙 수: before 2 → after 1 — Core callback dispatch / Core pull + binding owner → 후자 하나.
- 행동 변경: 없음 — 현재 pull 구현에 문서를 맞추며 public callback API를 새로 추가하거나 service callback을 제거하지 않는다.
- 영향: bindings(c, cpp, java, node, dotnet, go, rust, python) — 공통·Node 문서 중심.
- 성능 영향: 없음 — 실행 모델 변경을 제안하지 않는다.
- 근거 코드: C `bindings/c/include/zlink/socket/api.h:337–349`; C++ `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:644–683`; Java `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:636–677`; .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:349–400`; Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:453–462`, `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:659–672`; Go `bindings/go/internal/native/completion_owner.go:598–633`; Rust `bindings/rust/src/internal/completion_owner.rs:442–519`; Python `bindings/python/src/zlink/_native/hotpath.h:934–1006`, `bindings/python/src/zlink/_native/hotpath.h:1050–1133`.
- 확신: 높음 — 실제 소비 경로를 여덟 언어에서 확인했다. 이름에 callback이 있는 모든 service API를 obsolete로 분류한 것이 아니다.

### F-R3-10 monitor ABI v3·v4를 동시에 요구

- 분류: spec-impl-drift
- 위치: `bindings/doc/spec/README.ko.md:1285`, `bindings/doc/spec/README.ko.md:2281`; `bindings/doc/spec/java/README.ko.md:796–802`; 현재 v4 투영은 `bindings/doc/spec/c/README.ko.md:156`, `bindings/doc/spec/c/README.ko.md:197–198`, `bindings/doc/spec/cpp/README.ko.md:540`, `bindings/doc/spec/node/README.ko.md:609`, `bindings/doc/spec/dotnet/README.ko.md:571–576`. Core 기준은 `core/doc/spec/core/06-monitoring.ko.md:139–146`, `core/doc/spec/core/06-monitoring.ko.md:333`, `core/doc/spec/core/06-monitoring.ko.md:550`.
- 현재 규칙(인용): “`MonitorStatus`는 Core monitoring ABI v3의 byte HWM·pending 진단을 투영한다.” / “`abiVersion()`이 `3`이 아니거나 `structSize()`가 binding layout과 다르면 `UnsupportedOperationException`을 발생시킨다.”
- 문제: Java의 같은 절은 record layout을 v4라고 소개하고 여섯 행 뒤 v3만 받으라고 한다. 공통도 v3이지만 C public header와 Java/.NET 변환기는 v4를 요구한다. 문서 기준으로 v3 validation을 적용하면 정상적인 현행 Core snapshot을 거부한다.
- 제안: 소유자는 Core monitoring ABI 절이며 “**바인딩은 현재 Core monitor ABI의 version·size·field를 투영하고 호환되지 않는 layout만 해당 언어의 미지원 오류로 거부한다.**”로 통합한다; 실제 v4 값의 원천은 Core이며 언어 문서는 예외 타입만 정한다.
- 규칙 수: before 2 → after 1 — v3 요구 / v4 요구 → 현재 Core ABI 하나.
- 행동 변경: 없음 — 현행 v4 validator를 유지하는 문서 정합화다; ABI validation 자체를 제거하지 않는다.
- 영향: bindings(공통, java; 대조 c, dotnet) — 문서만.
- 성능 영향: 없음 — native layout 검사 비용 유지.
- 근거 코드: C `bindings/c/include/zlink/eventing/api.h:55–69`; Java `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/NativeMonitorStatuses.java:12–28`; .NET `bindings/dotnet/src/Zlink/Runtime/Native/NativeMonitorModels.cs:13–18`와 `bindings/dotnet/src/Zlink/Runtime/Eventing/MonitorConverters.cs:31–38`. C++·Node·Go·Rust·Python의 monitor 변환기 전체는 이 finding에서 코드 검증하지 않았다.
- 확신: 높음 — 모순과 실제 수용 버전을 직접 확인했다.

### F-R3-11 C++ SEND 대기 토큰이 기본 completion 진행을 중단

- 분류: parity-gap
- 위치: `bindings/doc/spec/async-execution-model.ko.md:68–82`; `bindings/doc/spec/async-coroutine-policy.ko.md:75–82`; `bindings/doc/spec/cpp/README.ko.md:666–674`. 하위 readiness·단일 소비자 경계는 `core/doc/spec/core/socket/README.ko.md:1175–1180`.
- 현재 규칙(인용): “Socket을 public poller에 `PollCompletion`으로 등록하지 않았으면 binding runtime이 owner다.”
- 문제: C++ `async()` SEND가 wait token을 받으면 `register_send_entry`는 기존 REQUEST fallback thread를 멈춘다. `_send_entry_count != 0`인 동안 runtime owner를 시작하지 않고 기존 drain도 멈춘다. 따라서 public PollCompletion poller 없이 pending SEND를 await하면 WRITABLE을 소비할 주체가 없다. 동일 owner에 있던 REQUEST 진행까지 함께 멈출 수 있다. `async_operation_state`의 `suspend(...)`는 continuation 등록만 하므로 이 공백을 메우지 않는다.
- 제안: 소유자는 async-execution-model의 owner 이전 계약이며 “**Public PollCompletion 등록 여부로 정한 하나의 completion owner가 SEND 대기 토큰과 REQUEST completion을 모두 진행한다.**”를 유지하고 C++의 SEND 전용 owner 예외를 삭제한다; 기존 owner를 재사용한다.
- 규칙 수: before 2 → after 1 — REQUEST는 runtime fallback / SEND는 public poller 필수 → 등록 여부로 결정하는 owner 하나.
- 행동 변경: 있음 — public poller 없는 awaitable SEND와 같은 socket의 REQUEST가 이전에는 멈추던 조건에서 진행한다.
- 영향: bindings(cpp) — `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:593–597`, `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:638–641`, `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:688–692`.
- 성능 영향: 있음 — 중단되던 completion 소비가 실행되며 SEND/REQUEST에 따른 owner 중단·재시작 분기가 사라진다; throughput 개선 폭은 미측정.
- 근거 코드: C++ `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:63–80`, `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:593–624`, `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:630–692`, `bindings/cpp/src/Runtime/Messaging/async_operation_state.hpp:82–102`. 대조: .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:99`, `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:623–677`; Java `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:138–140`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:774–784`; Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:369–371`, `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:659–672`; Go `bindings/go/internal/native/completion_owner.go:426–447`; Rust `bindings/rust/src/internal/completion_owner.rs:396–431`, `bindings/rust/src/internal/completion_owner.rs:593–618`; Python `bindings/python/src/zlink/_runtime/messaging/routed_async.py:569–604`. C는 caller가 drain한다.
- 확신: 높음 — 정적 호출 경로상 owner 공백이 명시적이다. 재현 테스트는 실행하지 않았다.

### F-R3-12 네 언어가 NO_DATA 전에 WRITABLE을 재제출

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/socket/README.ko.md:986–992`, `core/doc/spec/core/socket/README.ko.md:1072–1077`, `core/doc/spec/core/socket/README.ko.md:1175–1180`, `core/doc/spec/core/socket/README.ko.md:1335–1339`; `bindings/doc/spec/async-execution-model.ko.md:63–76`; `bindings/doc/spec/async-coroutine-policy.ko.md:75–82`; C의 drain 재서술은 `bindings/doc/spec/c/README.ko.md:300–304`, `bindings/doc/spec/c/README.ko.md:452–453`. 선행 판정은 `doc/plan/c016-worklog/decisions.ko.md:1269–1272`(D-B117), Core wake 원인 수정은 `doc/plan/c016-worklog/decisions.ko.md:1277–1279`, `doc/plan/c016-worklog/decisions.ko.md:1294–1297`이다.
- 현재 규칙(인용): “caller는 queue를 `NO_DATA`까지 비운 뒤 같은 request를 `DONTWAIT`로 다시 제출한다.”
- 문제: C++·Java·Node·Go는 completion recv loop 안의 capture에서 곧바로 native 재제출한다. 재제출이 만든 새 completion을 같은 drain에서 다시 소비할 수 있어 Core가 정한 drain 경계와 public wait의 작업 순서가 달라진다. .NET은 D-B117대로 NO_DATA에서 재제출하고 Python도 Python·native 구현 모두 drain 뒤 재제출한다. Rust의 일반 drain은 waker를 모았다가 drain 잠금을 놓은 뒤 깨운다. D-B120으로 Core의 spurious wake 원인이 수정됐더라도 binding의 순서 위반은 별개로 남는다.
- 제안: 소유자는 기존 socket completion owner이며 “**WRITABLE에 따른 재제출은 해당 owner가 현재 completion queue를 NO_DATA까지 비운 뒤에만 수행한다.**” 한 규칙으로 통합한다; 별도 poller·timer·retry 횟수 정책은 추가하지 않는다.
- 규칙 수: before 2 → after 1 — record capture 즉시 재제출 / NO_DATA 뒤 재제출 → NO_DATA 뒤 하나.
- 행동 변경: 있음 — admission·오류 전달과 public wait 반환 사이의 순서 및 한 drain이 처리하는 작업 범위가 달라진다.
- 영향: bindings(cpp, java, node, go) — 각 completion capture 안의 재제출 호출.
- 성능 영향: 있음 — 동일 drain 안에서 새 토큰을 다시 소비·제출하는 연쇄 경로가 사라진다; 공정성·drain 비용은 실행 검증하지 않았다.
- 근거 코드: C++ `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:360`, `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:418`, `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:630–683`; Java `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:636–677`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:1145–1153`; Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:453–462`, `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:525–590`; Go `bindings/go/internal/native/completion_owner.go:598–633`, `bindings/go/internal/native/completion_owner.go:732`, `bindings/go/internal/native/completion_owner.go:848`. 대조: .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:349–400`; Python `bindings/python/src/zlink/_runtime/messaging/routed_async.py:901–958` 및 실제 native 경로 `bindings/python/src/zlink/_native/hotpath.h:998`, `bindings/python/src/zlink/_native/hotpath.h:1071–1076`, `bindings/python/src/zlink/_native/hotpath.h:1127–1131`; Rust `bindings/rust/src/internal/completion_owner.rs:442–519`. C는 `bindings/c/include/zlink/socket/api.h:337–349`을 caller가 사용한다.
- 확신: 높음 — 네 언어의 loop→capture→native submit 경로를 확인했다. Rust의 모든 early-publish·parked-waker 경합까지 적합 판정한 것은 아니다.

### F-R3-13 정상적인 tokenless EAGAIN을 INTERNAL_ERROR로 변경

- 분류: spec-impl-drift
- 위치: `core/doc/spec/core/socket/README.ko.md:977–984`, `core/doc/spec/core/socket/README.ko.md:1058–1059`, `core/doc/spec/core/socket/README.ko.md:1351–1353`는 reservation 포화 REQUEST를 BACKPRESSURED·EAGAIN·ID 0으로 정한다. 동기 SEND timeout은 `core/doc/spec/core/socket/README.ko.md:964–966`, reply의 동기 admission은 `core/doc/spec/core/socket/README.ko.md:1094–1101`이다. Binding exact error 원칙은 `bindings/doc/spec/async-execution-model.ko.md:92–93`, `bindings/doc/spec/async-coroutine-policy.ko.md:59–60`, `bindings/doc/spec/README.ko.md:1685–1701`.
- 현재 규칙(인용): “Slot 포화는 flags와 무관하게 즉시 `ZLINK_SUBMIT_BACKPRESSURED`, `errno == EAGAIN`, ID `0`, completion 없음이다.”
- 문제: .NET·Java·Rust·Python의 async REQUEST 경로는 BACKPRESSURED에 nonzero token이 없으면 protocol/internal failure라고 가정한다. 그러나 completion reservation이 찬 경우는 Core가 명시적으로 허용한 tokenless EAGAIN이다. 따라서 애플리케이션은 정상 backpressure 대신 INTERNAL_ERROR를 받는다. Java의 동일 `submitFailure` helper는 blocking SEND·reply에도 사용돼 SNDTIMEO 만료의 EAGAIN·ID 0도 오분류한다. C++·Go·Node의 REQUEST 경로는 이 경우 원래 Backpressured 결과를 보존한다.
- 제안: 소유자는 Core submit result 계약이며 “**BACKPRESSURED와 nonzero token의 조합만 WRITABLE 대기로 연결하고, 토큰 없는 submit 실패는 Core가 반환한 result·errno를 그대로 언어 오류로 전달한다.**”로 통합한다.
- 규칙 수: before 2 → after 1 — Core의 합법적인 tokenless BACKPRESSURED / binding의 INTERNAL_ERROR 재분류 → Core result 분류 하나.
- 행동 변경: 있음 — reservation 포화 REQUEST 및 Java 동기 admission timeout에서 public 오류 종류가 INTERNAL_ERROR에서 BACKPRESSURED로 바뀐다.
- 영향: bindings(dotnet, java, rust, python) — request submit·재제출 결과 처리; Java는 동기 send/reply 공용 오류 helper도 포함.
- 성능 영향: 없음 — 오류 분류 경로의 정합화이며 reservation 상한이나 재시도 정책은 변경하지 않는다.
- 근거 코드: .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:194–220`, `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:607–608`, `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:1429–1437`; Java `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:147–181`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:248–254`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:856–861`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:904–924`; Rust `bindings/rust/src/runtime/messaging/operations/routed_async.rs:286–319`; Python `bindings/python/src/zlink/_native/hotpath.h:1487–1490`와 fallback `bindings/python/src/zlink/_runtime/messaging/routed_async.py:1116–1121`, `bindings/python/src/zlink/_runtime/messaging/routed_async.py:1207–1212`. 대조: C++ `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:225–231`; Go `bindings/go/internal/native/dealer_router_request.go:368–404`; Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:336–348`. C는 raw result를 노출하는 `bindings/c/include/zlink/socket/api.h:262–270`이다.
- 확신: 높음 — 정상 Core 반환 조합과 오류 override 분기가 일치한다. 65,536 slot 포화 실행은 하지 않았다.

### F-R3-14 Java가 target 제거 상태로 Core 오류를 재분류

- 분류: lower-layer-reverification
- 위치: `core/doc/spec/core/socket/README.ko.md:1006–1016`, `core/doc/spec/core/socket/README.ko.md:1045–1046`, `core/doc/spec/core/socket/README.ko.md:1078–1081`, `core/doc/spec/core/socket/README.ko.md:1153–1162`, `core/doc/spec/core/socket/README.ko.md:1348–1350`; `bindings/doc/spec/async-execution-model.ko.md:92–93`; `bindings/doc/spec/async-coroutine-policy.ko.md:59–60`, `bindings/doc/spec/async-coroutine-policy.ko.md:68–71`; `bindings/doc/spec/README.ko.md:2977–2978`, `bindings/doc/spec/README.ko.md:3139–3141`.
- 현재 규칙(인용): “target의 명시적 제거(`zlink_disconnect_rid`, 해당 RID의 endpoint termination)로 `send_result == ZLINK_SEND_TERMINAL`, `send_terminal_errno == ENOENT`인 WRITABLE record.”
- 문제: `SocketCore.disconnectRid`는 Core 호출 성공 뒤 Java pending 전체를 순회해 `targetRemoved`를 기록한다. 이후 SEND_TERMINAL뿐 아니라 재제출의 어떤 실패든 이 flag가 있으면 NOT_FOUND로 바꾼다. Core는 살아 있는 토큰의 제거 결과와 새 DONTWAIT 제출의 route 없음 결과를 이미 구분한다. 특히 WRITABLE이 먼저 queue에 들어간 뒤 disconnect가 발생한 경합을 Java가 별도 logical-operation 규칙으로 재해석한다. target 제거 사실과 terminal 분류의 소유자가 Core와 Java 두 곳이 된다.
- 제안: 소유자는 Core token·submit 결과 계약이며 “**바인딩은 대기 토큰의 종료는 Core completion으로, 재제출 실패는 그 Core submit 결과로 전달하며 target 제거 이력으로 오류를 다시 분류하지 않는다.**”로 통합한다.
- 규칙 수: before 2 → after 1 — Core 결과 판정 / Java targetRemoved 보상 판정 → Core 결과 판정 하나.
- 행동 변경: 있음 — queued WRITABLE 뒤 target 제거와 재제출이 경합할 때 Java가 강제하던 NOT_FOUND 대신 실제 Core NOT_CONNECTED 또는 다른 native 오류가 관찰될 수 있다.
- 영향: bindings(java) — `SocketCore.disconnectRid`와 completion owner의 targetRemoved 기록·오류 override.
- 성능 영향: 있음 — disconnect마다 pending snapshot 배열 생성·전체 순회 및 entry 상태 기록, terminal/retry의 flag 분기가 사라진다.
- 근거 코드: `bindings/java/src/main/java/systems/zlink/runtime/sockets/SocketCore.java:106–131`; `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:272–279`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:826–828`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:859–861`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:1046–1051`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:1125–1137`. 대조: C++ `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:403–418`; .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:1404–1437`; Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:636–640`; Go `bindings/go/internal/native/completion_owner.go:848–872`. Rust·Python의 모든 disconnect 경합은 이 finding에서 확인하지 않았다.
- 확신: 높음 — Java의 재분류는 명시적이다. ‘이미 발행한 WRITABLE도 나중에 제거 결과로 바꿔야 한다’는 별도 logical-operation 계약의 의도는 BLOCKERS에서 감독자에게 남긴다.

### F-R3-15 .NET·Java 송신 잠금이 Core 제출 경합을 직렬화

- 분류: lower-layer-reverification
- 위치: `bindings/doc/spec/README.ko.md:1336–1347`; `bindings/doc/spec/cpp/README.ko.md:464–468`; Core 동시 send·lifecycle 경계는 `core/doc/spec/core/socket/README.ko.md:49–56`, `core/doc/spec/core/socket/README.ko.md:617–629`. Staging copy의 별도 책임은 `bindings/doc/spec/async-coroutine-policy.ko.md:50–52`다.
- 현재 규칙(인용): “Part 단위 Core API를 사용하는 binding은 **송신 경로에 자체 lock이나 gate를 두지 않는다.**” / “binding은 이를 직렬화하거나 대기시키거나 재시도하지 않고 Core의 결과를 그대로 전달한다.”
- 문제: .NET의 blocking Send·Request·Reply는 native part sequence 전체를 `_submitSync` 안에서 호출한다. Java의 blocking Send·Reply는 `withSendSequenceLock`으로 동일 `drainLock`을 잡은 채 Core의 NONE admission을 기다린다. 이는 owner registry의 짧은 publication 경합 보호를 넘어 다른 sender와 Java completion drain까지 binding에서 대기시킨다. 문서는 경쟁 submit의 결과를 Core가 결정하도록 명시했지만 실제 호출은 native 진입 전에 직렬화된다.
- 제안: 소유자는 Core part-sequence admission이며 “**동시 송신 sequence의 수락·거절은 Core만 결정하고, 바인딩의 언어 상태 동기화는 그 상태를 보호하는 범위에만 머물며 native admission 대기를 직렬화하지 않는다.**”로 통합한다.
- 규칙 수: before 3 → after 1 — Core 제출 경합 판정 + .NET 전체 제출 잠금 + Java 전체 제출 잠금 → Core 제출 판정 하나.
- 행동 변경: 있음 — 같은 socket에서 동시에 제출한 호출의 대기·거절·성공 순서와 completion 진행이 바뀔 수 있다.
- 영향: bindings(dotnet, java) — 송신 전체를 감싸는 잠금 범위.
- 성능 영향: 있음 — 동시 sender의 binding 직렬 대기와 Java drain의 blocking admission 대기를 제거하는 경로다; 효과는 미측정.
- 근거 코드: .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:111–123`, `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:224–250`, `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:253–266`; Java `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:147–155`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:248–269`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:636–646`. 대조: C++ `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:145–206`; Rust `bindings/rust/src/internal/completion_owner.rs:357–366`의 공유 수명 guard는 전체 sender를 상호 배제하는 mutex와 구분했다. Go·Node·Python의 모든 native-call locking 경계는 이 finding에서 적합 판정하지 않았다.
- 확신: 높음 — 문서의 명시적 금지와 blocking native 호출을 감싼 잠금이 일치한다. SafeHandle·FFI pointer·registry 수명 보호까지 일괄 삭제하라는 제안은 아니다.

### F-R3-16 Node 동기 request가 지정된 completion owner를 우회

- 분류: parity-gap
- 위치: `bindings/doc/spec/async-execution-model.ko.md:68–82`; `bindings/doc/spec/async-coroutine-policy.ko.md:75–82`; `bindings/doc/spec/node/README.ko.md:784–792`; 하위 단일 drain owner·drain 순서는 `core/doc/spec/core/socket/README.ko.md:1175–1185`.
- 현재 규칙(인용): “Binding은 blocking terminal을 위해 owner를 가져오거나 별도 drain thread를 만들지 않는다.”
- 문제: Node `requestSync`는 `publicOwner`와 무관하게 native `socketRequestSync`를 호출한다. Addon은 자체 loop에서 `zlink_completion_recv(..., NONE)`를 호출하고 자기 completion ID를 찾으면 NO_DATA 전에 반환한다. 다른 operation completion은 임시 JS 배열에 쌓아 native 호출이 끝난 후 TypeScript owner에 전달한다. 일반 `drain(caller)`의 owner 검사와 큐 소비 규칙을 동기 request만 우회한다. Core queue의 정상적인 RCVTIMEO no-data도 이 별도 loop에서는 request-completion recv 실패가 된다.
- 제안: 소유자는 공통 async-execution-model의 지정된 completion owner이며 “**동기 request도 지정된 completion owner가 전달한 REQUEST 결과만 기다리고 자체 completion recv loop를 소유하지 않는다.**”로 통합한다.
- 규칙 수: before 2 → after 1 — 일반 completion owner / 동기 request 전용 native 소비자 → 지정된 owner 하나.
- 행동 변경: 있음 — public poller 등록 중 blocking request의 진행 조건, 다른 waiter의 완료 전달 시점, RCVTIMEO와 request timeout 사이의 오류 결과가 달라질 수 있다.
- 영향: bindings(node) — `requestSync`와 addon의 `socket_request_sync`.
- 성능 영향: 있음 — 다른 operation completion을 동기 request의 임시 배열에 보관했다가 다시 전달하는 경로가 사라진다; Node의 동기 실행 가능 조건은 감독자 판단이 필요하다.
- 근거 코드: `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:375–405`, `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:434–462`; `bindings/node/native/src/addon_core.cc:2477–2537`. 대조: Python `bindings/python/src/zlink/_runtime/messaging/routed_async.py:1257–1313`은 public owner를 확인하고 .NET `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:224–250`은 owner에 연결한 entry Task를 기다린다. C++·Java·Go·Rust의 동기 request 전체 경합은 이 finding에서 별도로 입증하지 않았다.
- 확신: 높음 — public owner와 별개인 native 소비 loop를 확인했다. Node thread·event-loop 제약을 해결할 새 API나 별도 poller는 정하지 않았다.

### F-R3-17 Go HWM 공개 타입이 uint64 계약보다 좁음

- 분류: parity-gap
- 위치: `bindings/doc/spec/README.ko.md:1368–1379`; `bindings/doc/spec/go/README.ko.md:88–91`, `bindings/doc/spec/go/README.ko.md:105–109`. Context budget 자체의 uint64 설명인 `bindings/doc/spec/go/README.ko.md:69–71`은 socket HWM과 구분했다.
- 현재 규칙(인용): “Getter도 Core의 전체 범위를 `uint64`로 반환한다.”
- 문제: Go public contracts는 native 구현 타입을 alias하고 `CommonSocketOptions`·connection socket HWM setter/getter는 `int`다. Native bridge는 8-byte uint64를 정확히 사용하지만 getter가 `math.MaxInt`를 넘으면 EOVERFLOW로 거부한다. 따라서 unsigned 전체 범위를 보장한 공통·Go 문서와 실제 공개 범위가 다르며 32-bit와 64-bit에서 범위도 달라진다. 문제가 되는 것은 wire 크기가 아니라 공개 타입과 range다.
- 제안: 소유자는 공통 HWM 언어 투영 표이며 “**Go socket HWM setter와 getter는 uint64를 사용하여 Core의 uint64_t 전체 범위를 손실 없이 전달한다.**” 한 규칙을 적용하고 Go 문서는 정확한 signature만 소유한다.
- 규칙 수: before 2 → after 1 — 공통 uint64 전체 범위 / 구현 int 범위 → uint64 전체 범위 하나.
- 행동 변경: 있음 — 공개 Go signature가 바뀌고 기존 MaxInt 초과 조회 오류가 값 반환으로 바뀐다; 단순 문서 수정이나 binary ABI 수정으로 분류하면 안 된다.
- 영향: bindings(go) — 공개 alias를 통해 노출되는 socket·option HWM API.
- 성능 영향: 없음 — 범위 투영을 맞추는 변경이며 성능 개선을 주장하지 않는다.
- 근거 코드: `bindings/go/contracts/sockets.go:16–35`; `bindings/go/internal/native/connection_socket.go:21–35`; `bindings/go/internal/native/socket_options.go:97–110`; `bindings/go/internal/native/socket_core.go:139–154`. 대조: Rust `bindings/rust/src/runtime/sockets/socket/socket_inner_runtime.rs:237–250`의 u64, .NET `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOptions.cs:53–56`의 ulong 전달. 다른 언어의 모든 HWM public overload는 이 finding에서 코드 검증하지 않았다.
- 확신: 높음 — public alias에서 native range 검사까지 직접 추적했다. Core 값의 silent truncation이라고 주장하지 않는다.

## 공통 규칙을 재서술하는 언어 문서

표의 행은 각각 하나의 공통 행동이며 언어별 signature·예외 타입 자체는 중복으로 세지 않았다. 정확한 전체 위치는 연결한 finding의 ‘위치’에 있다.

| 규칙 | 공통 위치 | 재서술하는 언어 문서 |
|---|---|---|
| dequeue 뒤 결과 수명은 Core HWM 회계 밖 | README:1314–1321 | C++:532·596, Java:703, Node:684, .NET:481, Go:129·177, Rust:519, Python:108 — F-R3-3 |
| Core receive-flow 상태·멱등성·monitor 투영, frame은 Core 내부 | README:1323–1326은 setter만 언급 | C:165, C++:544, Java:819, Node:626, .NET:593, Go:209, Rust:465, Python:129 — F-R3-4 |
| FINAL 이전 provisional registry 등록 | async-execution-model:86–100; async-coroutine-policy:56–66 | C++:668, Java:648·996, Node:786, .NET:703, Rust:617, Python:174 — F-R3-5; Go는 같은 구체 등록 문장을 재서술하지 않음 |
| PollCompletion 진행과 public owner 상태의 blocking 대기 | async-execution-model:68–82; async-coroutine-policy:75–82 | C++:671–674, Java:1001–1003, Node:789–792, .NET:706–709, Go:292–294, Rust:621–623, Python:178–180; C:300–304는 caller 소유 raw 계약 |

언어별 `**언어별 재량**` 표기는 공통 행위를 생략하거나 다르게 구현하는 허가가 아니다. 검토 기준 `doc/principal/documentation/spec-writing-guide.ko.md:331–418`의 §4.3·§4.4대로, signature·메모리 모델 차이의 이유와 동일한 관찰 결과를 확인할 기준만 언어 문서에 남겨야 한다.

## 0.17.0 wait-token 구현 대조

범위는 raw routed SEND·REQUEST와 completion owner다. ‘확인’은 읽은 소스의 정적 경로 확인이며 테스트 통과를 뜻하지 않는다. Token 보관용 entry와 Core request correlation map은 서로 다른 사실을 소유하므로 entry 존재 자체를 결함으로 세지 않았다.

| 언어 | submit → token → WRITABLE → 재제출 | 기본 owner / 공개 poller | 추가 차이와 코드 근거 |
|---|---|---|---|
| C | raw 결과를 caller에 노출 | caller가 NO_DATA까지 drain | `bindings/c/include/zlink/socket/api.h:225–261`, `bindings/c/include/zlink/socket/api.h:337–349`; C README의 SEND pending 서술이 오래됨(F7) |
| C++ | 토큰 뒤 capture 안에서 재제출 | SEND가 runtime owner를 중단 | `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:360`, `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:418`, `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:593–597`; F1·F11·F12; tokenless REQUEST 오류는 보존 |
| Java | 토큰 뒤 capture 안에서 재제출 | 기본 runtime owner 있음 | `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:1145–1153`, `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:774–784`; F1·F12·F13·F14·F15 |
| Node | 토큰 뒤 capture 안에서 재제출 | 일반 async는 watcher/공개 owner 이전 | `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:434–462`, `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:525–590`; F1·F12·F16; tokenless REQUEST 결과 코드는 보존 |
| .NET | NO_DATA 뒤 재제출 | 동일 owner로 SEND·REQUEST 진행 | `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:349–400`, `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:623–677`; D-B117 순서 수정 확인, F1·F13·F15 잔존 |
| Go | 토큰 뒤 capture 안에서 재제출 | socket completion owner 재사용 | `bindings/go/internal/native/completion_owner.go:426–447`, `bindings/go/internal/native/completion_owner.go:598–633`, `bindings/go/internal/native/completion_owner.go:732`, `bindings/go/internal/native/completion_owner.go:848`; F1·F12, 공개 Submit(ctx) admission/completion 분리는 D-B121·D-B123 미해결 |
| Rust | 일반 drain의 waker를 NO_DATA 뒤 깨움 | 기본 runtime owner 있음 | `bindings/rust/src/internal/completion_owner.rs:442–519`, `bindings/rust/src/internal/completion_owner.rs:593–618`; F1·F13; early-publish 경합 전부는 미검증 |
| Python | Python/native 경로 모두 drain 뒤 재제출 | runtime 또는 등록된 public owner | `bindings/python/src/zlink/_runtime/messaging/routed_async.py:901–958`, `bindings/python/src/zlink/_runtime/messaging/routed_async.py:1257–1313`; `bindings/python/src/zlink/_native/hotpath.h:1127–1131`; F1·F13 |

Core의 wake 원인은 `core/doc/spec/core/socket/README.ko.md:1075–1077`과 D-B119/D-B120이 확정한 ‘거절 자원의 회복’이다. 검토한 binding pump에서 correlation credit을 별도로 계산하는 새 readiness 판정은 확인하지 않았다. 이를 다시 검사할 준비 timer나 별도 pending-request timeout registry를 제안하지 않는다. 발견한 중복은 RID 검증, Java의 target 제거 추적, 제출 전체 잠금 및 완료 진행 예외다.

## 성능 정책의 계약 불일치

- 공통 binding 스펙 `bindings/doc/spec/README.ko.md:4832–4849`는 `doc/perf/PERF_SINGLE_TEST_POLICY.md`와 `doc/perf/PERF_MULTI_TEST_POLICY.md`를 기준으로 지정한다. Single의 `bindings/doc/spec/README.ko.md:50–65`는 synchronous callback terminal을 요구하고 Task/Future 실행을 배제하지만, `bindings/doc/spec/async-coroutine-policy.ko.md:104–112`의 현행 terminal 목록에는 그 callback terminal이 없다. 구현 예는 `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:63–80`의 async operation과 `bindings/go/internal/native/dealer_router_request.go:364–365`, `bindings/go/internal/native/dealer_router_request.go:398`의 완료 대기다. 새 public API 또는 runner 우회로 해소할 수 있는지 본 리뷰가 결정하지 않는다.
- Single `doc/perf/PERF_SINGLE_TEST_POLICY.md:128–136`은 admission backpressure까지 포화 제출을 요구한다. 고수준 request가 그 토큰 경계를 숨기는 현재 계약과의 충돌은 `doc/plan/c016-worklog/decisions.ko.md:1299–1301`(D-B121), `doc/plan/c016-worklog/decisions.ko.md:1316–1317`(D-B123), `doc/plan/c016-worklog/decisions.ko.md:1329–1331`(D-B127)에 이미 남아 있다. F-R3-7의 문서 정합화가 이 공개 admission 관측 수단을 새로 제공하는 것은 아니다.
- Single `doc/perf/PERF_SINGLE_TEST_POLICY.md:137–142`, `doc/perf/PERF_SINGLE_TEST_POLICY.md:293`의 같은 active 구간 집계와 D-B125 `doc/plan/c016-worklog/decisions.ko.md:1324`의 “latency 단계 분리”는 상충한다. 후행 D-B127 `doc/plan/c016-worklog/decisions.ko.md:1330–1331`이 결정을 보류했으므로 어느 쪽을 canonical 측정으로 삼을지 BLOCKERS에 남긴다. runner 소스를 추가 조사하거나 runner 결함을 새 finding으로 세지 않았다.
- Multi `doc/perf/PERF_MULTI_TEST_POLICY.md:55–60`는 C STREAM의 `zlink_send_async()`·nonzero SEND callback을 요구하지만 `bindings/doc/spec/c/README.ko.md:429–432`는 그 ABI를 제거했다. 현행 C 경계는 `bindings/c/include/zlink/socket/api.h:225–261`의 part submit·WRITABLE이다. 제안 방향은 **2 모델 → 1 현행 Core 계약**, 초안은 “**Multi의 C 송신은 공통 binding 계약이 참조하는 현재 Core part-submit·completion 계약을 따른다.**”이며 소유 위치는 Multi 정책의 API 사용 절이다. 이는 정책 서술의 정합화 제안이고 runner 변경·측정 결과 변경은 별도 행동 변경 판단 대상이다.

## 추가 후보(요약 1줄)

- Java ROUTER no-wait receive가 NO_DATA와 BUSY를 함께 false로 바꾼다: `bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterReceiveSupport.java:151–157` 대 `bindings/doc/spec/java/README.ko.md:700–701`, `bindings/doc/spec/README.ko.md:946–951`, `bindings/doc/spec/README.ko.md:1728–1730`; Core BUSY의 정확한 해당 경로와 다른 언어를 추가 대조해야 하며, 합칠 경우 **2 결과 분류 → 1 Core 결과 투영**, 초안 “NO_DATA만 정상 no-data로 반환하고 다른 recv 결과는 해당 언어 오류로 전달한다”(공통 오류 절), 행동 변경 있음.
- Go의 취소 가능한 REQUEST에는 `go stop()`가 남는다(`bindings/go/internal/native/completion_owner.go:272–275`): D-B121의 ‘요청당 poller/goroutine 제거’와 구분하여 cancellation callback 재진입을 확인해야 하며, 중복으로 확정되면 **2 cancellation 정리 경로 → 1 기존 완료 정리 경로**, 초안 “Caller wait cancellation의 정리는 기존 entry 완료 소유자가 담당한다”(async-execution-model); 지금은 제거·행동 불변을 확정하지 않는다.
- `bindings/doc/spec/draft/message-ownership.ko.md:71–75`의 invalid-object 오류 비보장과 `bindings/doc/spec/draft/message-ownership.ko.md:449`, `bindings/doc/spec/draft/message-ownership.ko.md:507–508`의 접근 실패 강제는 초안 내부 모순이다: **2 접근 계약 → 1 감독자가 선택한 계약**, 위치는 초안 상태 정의 절이며 통합 문장 선택은 BLOCKERS로 남긴다; 정식 구현 결함으로 세지 않는다.
- `bindings/doc/spec/README.ko.md:4464–4467`의 no-data “에러 경로”와 `bindings/doc/spec/README.ko.md:943–951`, `bindings/doc/spec/README.ko.md:1728–1730`, `bindings/doc/spec/README.ko.md:4515`의 정상 false 표현은 검증 문구 정합화 후보: **2 표현 → 1 정상 no-data**, 초안 “Nonblocking caller-provided receive의 데이터 없음은 오류 없이 미수신으로 관찰된다”(공통 검증 절), 행동 변경 없음; 언어별 추가 runtime 판정은 하지 않았다.

## 읽은 범위

### 필수 스펙: 13개 전체, 11,902행

아래 행 수는 파일의 전체 행 수이며 모두 처음부터 끝까지 읽었다. `dotnet/api-reference-comments`는 실제 파일명인 `dotnet/api-reference-comments.ko.md`로 확인했다. `bindings/doc/spec/**/*.ko.md` 검색 결과도 이 13개와 일치한다.

| 파일 | 읽은 행 수 |
|---|---:|
| `bindings/doc/spec/README.ko.md` | 5,334 / 전체 |
| `bindings/doc/spec/async-execution-model.ko.md` | 155 / 전체 |
| `bindings/doc/spec/async-coroutine-policy.ko.md` | 154 / 전체 |
| `bindings/doc/spec/draft/message-ownership.ko.md` | 511 / 전체 |
| `bindings/doc/spec/c/README.ko.md` | 469 / 전체 |
| `bindings/doc/spec/cpp/README.ko.md` | 818 / 전체 |
| `bindings/doc/spec/java/README.ko.md` | 1,130 / 전체 |
| `bindings/doc/spec/node/README.ko.md` | 912 / 전체 |
| `bindings/doc/spec/dotnet/README.ko.md` | 841 / 전체 |
| `bindings/doc/spec/dotnet/api-reference-comments.ko.md` | 47 / 전체 |
| `bindings/doc/spec/go/README.ko.md` | 436 / 전체 |
| `bindings/doc/spec/rust/README.ko.md` | 769 / 전체 |
| `bindings/doc/spec/python/README.ko.md` | 326 / 전체 |

### 기준 문서·Core 대조·구현

아래는 본문을 읽은 구간과 그 구간의 중복을 제외한 행 수다. 전체를 읽지 않은 구현 파일을 전수 검증으로 표시하지 않았다. 이 밖에 `rg`로 symbol·파일 위치를 찾은 단편은 전체 파일 독해에 합산하지 않았다.

| 파일 | 읽은 구간 | 읽은 행 수 |
|---|---|---:|
| `AGENTS.md` | 1–137 | 137 |
| `doc/AGENTS.md` | 1–51 | 51 |
| `doc/plan/c016-worklog/spec-review/README.ko.md` | 1–66 | 66 |
| `doc/principal/documentation/documentation-principles.ko.md` | 1–474 | 474 |
| `doc/principal/documentation/spec-writing-guide.ko.md` | 1–418, 701–755 | 473 |
| `doc/plan/c016-worklog/decisions.ko.md` | 1205–1378 | 174 |
| `core/doc/spec/core/socket/README.ko.md` | 49–57, 291–311, 610–633, 690–708, 940–1190, 1328–1415 | 412 |
| `core/doc/spec/core/systems/06-auto-hwm.ko.md` | 388–402, 465–477 | 28 |
| `core/doc/spec/core/06-monitoring.ko.md` | 134–151, 333, 550 | 20 |
| `doc/perf/PERF_SINGLE_TEST_POLICY.md` | 24–72, 123–145, 280–300 | 93 |
| `doc/perf/PERF_MULTI_TEST_POLICY.md` | 35–90 | 56 |
| `bindings/c/include/zlink/socket/api.h` | 40–80, 194–202, 218–274, 322–349 | 135 |
| `bindings/c/include/zlink/eventing/api.h` | 50–70 | 21 |
| `bindings/c/include/zlink.h` | 1–15 | 15 |
| `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp` | 1–837 | 837 |
| `bindings/cpp/src/Runtime/Messaging/completion_owner.hpp` | 1–127 | 127 |
| `bindings/cpp/src/Runtime/Messaging/async_operation_state.hpp` | 1–245 | 245 |
| `bindings/cpp/src/Runtime/Messaging/send_operations.cpp` | 1–140 | 140 |
| `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp` | 145–237 | 93 |
| `bindings/cpp/src/Runtime/Sockets/socket.cpp` | 151–166 | 16 |
| `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java` | 1–1265 | 1265 |
| `bindings/java/src/main/java/systems/zlink/runtime/sockets/SocketCore.java` | 102–138 | 37 |
| `bindings/java/src/main/java/systems/zlink/runtime/sockets/SocketOptionSupport.java` | 199–215 | 17 |
| `bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterReceiveSupport.java` | 1–261 | 261 |
| `bindings/java/src/main/java/systems/zlink/contracts/messaging/Received.java` | 350–401 | 52 |
| `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/NativeMonitorStatuses.java` | 1–86 | 86 |
| `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs` | 1–278, 324–403, 601–695, 805–832, 950–1055, 1187–1218, 1270–1441 | 791 |
| `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOptions.cs` | 1–110 | 110 |
| `bindings/dotnet/src/Zlink/Runtime/Native/NativeMonitorModels.cs` | 1–27 | 27 |
| `bindings/dotnet/src/Zlink/Runtime/Eventing/MonitorConverters.cs` | 25–41 | 17 |
| `bindings/dotnet/src/Zlink/Runtime/Eventing/MonitorStatus.State.cs` | 1–24 | 24 |
| `bindings/dotnet/src/Zlink/Zlink.csproj` | 1–31 | 31 |
| `bindings/go/internal/native/completion_owner.go` | 1–965 | 965 |
| `bindings/go/internal/native/dealer_router_request.go` | 1–410 | 410 |
| `bindings/go/internal/native/send_retry.go` | 1–90 | 90 |
| `bindings/go/internal/native/connection_socket.go` | 1–115 | 115 |
| `bindings/go/internal/native/socket_options.go` | 70–118 | 49 |
| `bindings/go/contracts/sockets.go` | 1–95 | 95 |
| `bindings/go/internal/native/operations.go` | 1–90 | 90 |
| `bindings/go/internal/native/socket_option_support.go` | 1–48 | 48 |
| `bindings/go/internal/native/socket_core.go` | 132–158, 180–229 | 77 |
| `bindings/go/internal/native/received.go` | 1–164 | 164 |
| `bindings/go/go.mod` | 1–5 | 5 |
| `bindings/rust/src/internal/completion_owner.rs` | 1–255, 305–525, 590–780 | 667 |
| `bindings/rust/src/runtime/messaging/operations/routed_async.rs` | 100–175, 242–321 | 156 |
| `bindings/rust/src/runtime/sockets/socket/socket_inner_runtime.rs` | 232–253, 455–464 | 32 |
| `bindings/rust/Cargo.toml` | 1–59 | 59 |
| `bindings/node/src/zlink/runtime/messaging/completion_owner.ts` | 1–722 | 722 |
| `bindings/node/native/src/addon_core.cc` | 2237–2275, 2440–2565 | 165 |
| `bindings/node/src/zlink/runtime/sockets/socket_base.ts` | 70–85 | 16 |
| `bindings/node/package.json` | 1–38 | 38 |
| `bindings/python/src/zlink/_runtime/messaging/routed_async.py` | 504–670, 800–1348 | 716 |
| `bindings/python/src/zlink/_native/hotpath.h` | 934–1006, 1050–1165, 1250–1284, 1455–1508, 1538–1559 | 300 |
| `bindings/python/src/zlink/_runtime/sockets/socket_base.py` | 425–443 | 19 |

추가 symbol 검색은 Java `ContractAccess.java`의 Received 접근 함수, Go `socket_types.go`의 HWM API, .NET의 monitor ABI 상수 및 각 언어 receive-flow setter·completion 관련 호출자를 대상으로 했다. 이 검색만으로 그 파일의 전체 구현 적합성을 주장하지 않았다.

생략한 범위와 이유:

- Core 구현·protocol 내부, Framework 구현·문서, 사이트 문서는 다른 job의 소유 범위이므로 수정하지 않았고 이 리뷰의 구현 적합성 판정에 사용하지 않았다. Core는 하위 공개 계약 문장만 대조했다.
- Binding 구현은 public 진입점에서 completion owner·wait token·오류 변환으로 이어지는 경로와 위 표의 수신·옵션 경로를 읽었다. 여덟 언어의 모든 service API, transport, monitor field, public XML 주석 및 cancellation 경합을 전수 검증하지 않았다.
- `draft/message-ownership.ko.md`는 전체를 읽었지만 정식 계약으로 승격해 구현을 결함 처리하지 않았다. 초안의 현행 구현 inventory도 현재 소스 증거와 혼동하지 않았다.
- Single/Multi runner 구현, generated/dist 출력, 설치 package·native binary의 freshness는 조사·실행하지 않았다. 성능 정책의 계약 충돌과 결정문만 대조했다.
- 기존 변경·untracked Node 테스트를 읽고 수정하거나 실행하지 않았다. 빌드·테스트·benchmark·gate·commit·상태 변경 git 명령은 실행하지 않았다. 작성 파일은 이 보고서 하나다.

## BLOCKERS

1. **WRITABLE만 처리한 PollCompletion의 의미:** `bindings/doc/spec/async-execution-model.ko.md:73–75`, `bindings/doc/spec/async-execution-model.ko.md:102–104`의 “live waiter를 끝내거나 detached state를 정리”에 언어 terminal이 아직 pending인 WRITABLE 토큰 소비·새 토큰 전환도 포함되는가? C++ `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:664–683`와 Node `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:453–462`처럼 처리 건수는 늘었지만 public terminal은 끝나지 않은 경우, 0.17의 progress 계약 문장을 어떤 관찰 결과로 고정해야 하는가?
2. **queued WRITABLE 뒤 명시적 target 제거:** Core가 이미 발행한 WRITABLE 뒤 새 DONTWAIT 제출의 missing-route 결과(`core/doc/spec/core/socket/README.ko.md:986–1008`, `core/doc/spec/core/socket/README.ko.md:1045–1046`)를 그대로 투영하는 것이 Java logical operation에도 최종 계약인가, 아니면 Java `targetRemoved`가 의도한 “이 logical operation은 항상 NOT_FOUND”라는 별도 결과를 0.18 Core 계약으로 검토해야 하는가?
3. **Node blocking request와 public owner:** `bindings/doc/spec/async-execution-model.ko.md:78–82`가 요구하는 다른 thread의 public-poller drain을 Node의 지원되는 socket·worker 사용 모델에서 어떻게 표현하는가, 그리고 F-R3-16의 native 직접 소비를 제거할 때 유지해야 할 공개 실행 조건은 무엇인가?
4. **Single 및 Go의 admission 관측:** D-B121·D-B123·D-B127(`doc/plan/c016-worklog/decisions.ko.md:1299–1301`, `doc/plan/c016-worklog/decisions.ko.md:1316–1317`, `doc/plan/c016-worklog/decisions.ko.md:1329–1331`)에 남은 admission/completion 분리를 어떤 공개 관찰 계약으로 닫을 것인가? D-B127의 POLLOUT 후보는 Core의 aggregate hint(`core/doc/spec/core/socket/README.ko.md:993–994`)와 거절 자원별 WRITABLE(`core/doc/spec/core/socket/README.ko.md:1075–1077`)이 같지 않다는 점까지 반영하여 승인된 것인가?
5. **Single의 latency 집계:** `doc/perf/PERF_SINGLE_TEST_POLICY.md:137–142`, `doc/perf/PERF_SINGLE_TEST_POLICY.md:293`의 같은 active 구간 집계를 유지하는가, D-B125 `doc/plan/c016-worklog/decisions.ko.md:1324`의 별도 latency 단계를 계약으로 삼는가? 후행 D-B127에서 보류한 결정을 어느 문서에 단일 규칙으로 확정할 것인가?
6. **Ownership 초안의 승격 경계:** `bindings/doc/spec/draft/message-ownership.ko.md:71–75`의 접근 오류 비보장과 `bindings/doc/spec/draft/message-ownership.ko.md:449`, `bindings/doc/spec/draft/message-ownership.ko.md:507–508`의 오류 강제 중 무엇이 최종 계약인가? 또한 `bindings/doc/spec/draft/message-ownership.ko.md:227–229`의 실패 시 기존 output 보존은 정식 `bindings/doc/spec/README.ko.md:1015`의 receive 진입 시 정리와 다른 0.18 행동 변경으로 의도한 것인가?

