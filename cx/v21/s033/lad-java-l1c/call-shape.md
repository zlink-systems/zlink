# Java L0·L1 호출 형태

| 단계 | 경로 | Binding bench | L0/L1 bench | 비교 |
|---|---|---|---|---|
| L0·L1 | source send | `RawStack.java:345-377` (raw request/send submit) | `FrameworkStack.java:133-164` → 같은 `RawStack.send()/request()` | 동일 raw send/request, 응답 completion 포함. L1 permit은 target ingress에만 추가 |
| L0·L1 | target receive | `ZLinkRawBenchServer.java:94-109` (`recv` 후 body decode) | `ZLinkFrameworkBenchServer.java:144-170` (`recv` 후 fixed handler) | 둘 다 blocking `recv` 1회/message; L1은 receive 전 permit acquire, handler 직전 transfer 추가 |
| L0·L1 | target reply | `ZLinkRawBenchServer.java:115-127` | `ZLinkFrameworkBenchServer.java:241-248` | 양쪽 새 protobuf body와 response header를 만들고 2 parts submit; 같은 metrics record/counters |
| L0·L1 | source reply completion | `RawStack.java:345-377` | `FrameworkStack.java:133-164` → 같은 `RawStack.request()` | 같은 binding raw completion/validation; L1은 수신 permit 처리만 추가 |

비용 대조: receive wait는 양쪽 메시지당 blocking 1회다. L0는 추가 permit 할당·atomic·copy·wait가 없다. L1은 수신 전 `ZLinkApplicationJobQueue` permit acquire와 handler 직전 `handlerStarted()` transfer/permit 관리가 더해진다. 두 단계 모두 새 protobuf reply 생성, 동일 metrics 경로를 사용한다. 차이는 `ladder-steps.md`의 L1 application job queue permit에 한정된다.
