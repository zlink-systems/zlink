---
title: "가이드 홈 · Kotlin"
---

# ZLink Framework Kotlin — 사용 가이드

Kotlin/Spring Boot 환경에서 ZLink Framework를 쓰는 순서다. 03~17장은 모든 언어가 같은 정본을
공유하며, 예제는 `.kt` 탭을 고르면 Kotlin 코드로 바뀐다.

| 순서 | 문서 | 내용 |
|----|------|------|
| 1 | [1. 개요](01-overview.ko.md) | Kotlin 레이어가 얹는 것, 통합 4축과 전체 topology |
| 2 | [2. 시작하기](02-getting-started.ko.md) | 의존성, 등록, suspend handler, 두 가지 호출 방법 |
| 3 | [3. 핵심 개념](03-concepts.ko.md) | channel · Spot · Actor · session · relocation |
| 4 | [4. Backpressure](04-backpressure.ko.md) | 처리보다 도착이 빠를 때의 동작과 영향을 주는 옵션 |
| 5 | [5. Channel Messaging](05-channel-messaging.ko.md) | request / send / pub-sub 등록과 호출 |
| 6 | [6. Spot](06-spot.ko.md) | room · stage · zone 같은 동적 상태 단위 |
| 7 | [7. Actor와 Spot](07-actor-spot.ko.md) | actor 호스팅, membership, relocation |
| 8 | [8. Session과 Actor binding](08-actor-session.ko.md) | session ↔ actor relay · binding · push |
| 9 | [9. STREAM](09-stream.ko.md) | 외부 client 실시간 연결과 Stream Connector |
| 10 | [10. Location](10-location.ko.md) | location store 등록, 자동 연결, 운영 조회 |
| 11 | [11. Monitoring](11-monitoring.ko.md) | Flow로 받기, 람다 observer |
| 12 | [12. 운영](12-operations.ko.md) | 런타임 메트릭, graceful drain, readiness |
| 13 | [13. 주요 타입 사용 색인](13-interface-catalog.ko.md) | suspend 계약, .kotlin() wrapper, 확장 함수 |
| 14 | [14. 샘플 고르기](14-samples.ko.md) | 어떤 샘플을 먼저 볼지 고르고 실행하는 방법 |
| 15 | [15. E2E 테스트](15-e2e-testing.ko.md) | client로 시스템 전체를 검증하는 방법 |
| 16 | [16. Options](../../../java/guide/server/16-options.ko.md) | 옵션 표면이 Java와 같다 |
| 17 | [17. ZLink를 어디에 쓰나](17-alternative.ko.md) | 사용처, 문제 신호, 기술 선택 경계 |

파일 번호는 언어에 상관없이 같은 장을 가리키는 식별자다. 읽는 순서는 이 표가 정한다.

**Kotlin은 Java 런타임을 그대로 쓴다.** `zlink-framework-kotlin`은 별도 구현이 아니라
coroutine idiom을 얹는 얇은 레이어다. 그래서 이 가이드는 **Java와 다른 지점만** 쓰고
나머지는 [Java 가이드](../../../java/guide/server/README.ko.md)를 가리킨다.

- **01 · 02** — 의존성과 등록 코드 모양이 달라 Kotlin 전용으로 쓴다.
- **11 · 13 · 16** — 표면은 Java와 같고 idiom만 다르다. 차이만 쓰고 나머지는 Java 장을
  가리킨다.

같은 내용을 두 벌로 두지 않는 것이 목적이다. Java 문서가 바뀌면 Kotlin 독자도 같은
문서를 본다.

## 관련 문서

- 공개 계약: [Kotlin 공개 계약](../../../common/spec/server/languages/kotlin/README.ko.md)
- 언어 중립 의미: [공통 스펙](../../../common/README.ko.md)
- client library: [HTTP client](../http-client/README.ko.md) · [Stream connector](../stream-connector/README.ko.md)
