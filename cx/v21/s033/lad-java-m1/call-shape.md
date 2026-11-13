# Java L1–L3 호출 형태 비교

| 단계 | 경로 | binding (`zlink-java`) | Ladder (`zlink-framework-java`) | 일치 여부와 복원 기능 |
|---|---|---|---|---|
| L1 | source send | `BenchClient.java:91-105` → `RawStack.java:178-180,217-250` | `BenchClient.java:107-112` → `FrameworkStack.java:132-150` → `RawStack.java:178-180,217-250` | 동일한 ROUTER send와 2-part submit. source ladder adapter 진입은 고정 직접 호출 |
| L1 | target receive | `ZLinkRawBenchServer.java:94-109`, 메시지마다 blocking `recv(NONE)` 1회 | `ZLinkFrameworkBenchServer.java:126-177,140-151`, blocking `recv(NONE)` 1회, 매 수신 전에 permit 획득 및 handler 진입 직전 전달 | L1 복원: application ingress permit. drain 없음. 1개 `Received` 재사용 |
| L1 | target reply | `ZLinkRawBenchServer.java:115-127` | `ZLinkFrameworkBenchServer.java:215-219` | 동일: 새 응답 헤더와 protobuf body, 2 part submit |
| L1 | source completion / reply 수신 | `RawStack.java:84-92,314-345` | `RawStack.java:84-92,314-345` | 동일: admission future와 completion poller. 메시지별 추가 대기·복사·원자 연산 없음 |
| L2 | source send | `BenchClient.java:91-105` → `RawStack.java:178-180,217-250` | `BenchClient.java:107-112` → `FrameworkStack.java:132-157` → `RawStack.java:98-132,259-281` | L2 복원: 첫 part를 Framework envelope header로 encode, 동일한 2 part submit |
| L2 | target receive | `ZLinkRawBenchServer.java:94-109` | `ZLinkFrameworkBenchServer.java:126-177,186-220,196-209` | L2 복원: ingress envelope header decode/검증. recv 횟수·drain·metrics는 동일 |
| L2 | target reply | `ZLinkRawBenchServer.java:115-127` | `ZLinkFrameworkBenchServer.java:215-219` | 동일한 raw 2-part 응답 |
| L2 | source completion / reply 수신 | `RawStack.java:84-92,314-345` | `RawStack.java:122-126,314-345` | 동일한 admission/completion 흐름. reply validation은 body part를 확인 |
| L3 | source send | `BenchClient.java:91-105` → `RawStack.java:178-180,217-250` | `BenchClient.java:107-112` → `FrameworkStack.java:132-157` → `RawStack.java:134-175,283-312` | L3 복원: service header + service multipart frame의 outer 2-part submit |
| L3 | target receive | `ZLinkRawBenchServer.java:94-109` | `ZLinkFrameworkBenchServer.java:126-177,223-282` | L3 복원: service header와 multipart frame decode 후 L2 envelope 처리. recv 1회, drain 없음 |
| L3 | target reply | `ZLinkRawBenchServer.java:115-127` | `ZLinkFrameworkBenchServer.java:271-280` | L3 복원: response 2 part를 service frame으로 감싸 outer 2 part submit |
| L3 | source completion / reply 수신 | `RawStack.java:84-92,314-345` | `RawStack.java:157-170,314-345` | L3 복원: reply service header/correlation와 multipart frame decode 후 payload 확인 |

## 비용 대조

- binding: 메시지마다 target blocking receive 1회, `Received` 재사용, body decode 1회, metrics 기록 1회, 요청 reply는 새 header/body 생성과 2-part submit.
- L1: 여기에 Framework permit acquire/transfer만 추가한다. payload copy와 metrics atomic 수는 동일하다.
- L2: L1에 envelope header 생성/encode와 target header decode/검증을 추가한다. outer part 수와 body copy는 동일하다.
- L3: L2에 service header와 multipart frame encode/decode를 추가하고, reply frame도 동일하게 감싼다. receive loop, metrics atomic, handler당 receive 수는 바뀌지 않는다.
