---
title: "가이드 홈 · Java"
---

# ZLink Framework Java — 사용 가이드

Java/Spring Boot 환경에서 ZLink Framework를 쓰는 순서다. 03~17장은 모든 언어가 같은 정본을
공유하며, 예제는 `.java` 탭을 고르면 Java 코드로 바뀐다.

| 순서 | 문서 | 내용 |
|----|------|------|
| 1 | [1. 개요](01-overview.ko.md) | 무엇을 만드나, 무엇을 대체하나, 통합 4축과 전체 topology |
| 2 | [2. 시작하기](02-getting-started.ko.md) | 의존성, Spring Boot 얹기, handler와 client, 실행 |
| 3 | [3. 핵심 개념](03-concepts.ko.md) | channel · Spot · Actor · session · relocation |
| 4 | [4. Backpressure](04-backpressure.ko.md) | 처리보다 도착이 빠를 때의 동작과 영향을 주는 옵션 |
| 5 | [5. Channel Messaging](05-channel-messaging.ko.md) | request / send / pub-sub 등록과 호출 |
| 6 | [6. Spot](06-spot.ko.md) | room · stage · zone 같은 동적 상태 단위 |
| 7 | [7. Actor와 Spot](07-actor-spot.ko.md) | actor 호스팅, membership, relocation |
| 8 | [8. Session과 Actor binding](08-actor-session.ko.md) | session ↔ actor relay · binding · push |
| 9 | [9. STREAM](09-stream.ko.md) | 외부 client 실시간 연결과 Stream Connector |
| 10 | [10. Location](10-location.ko.md) | location store 등록, 자동 연결, 운영 조회 |
| 11 | [11. Monitoring](11-monitoring.ko.md) | 상태 snapshot · status stream · 진단 |
| 12 | [12. 운영](12-operations.ko.md) | 런타임 메트릭, graceful drain, readiness |
| 13 | [13. 주요 타입 사용 색인](13-interface-catalog.ko.md) | 기능별 interface 색인과 얻는 방법 |
| 14 | [14. 샘플 고르기](14-samples.ko.md) | 어떤 샘플을 먼저 볼지 고르고 실행하는 방법 |
| 15 | [15. E2E 테스트](15-e2e-testing.ko.md) | client로 시스템 전체를 검증하는 방법 |
| 16 | [16. Options](16-options.ko.md) | 옵션 목록, 기본값과 변경 시점 |
| 17 | [17. ZLink를 어디에 쓰나](17-alternative.ko.md) | 사용처, 문제 신호, 기술 선택 경계 |

파일 번호는 언어에 상관없이 같은 장을 가리키는 식별자다. 읽는 순서는 이 표가 정한다.

01 · 02 · 11 · 13 · 16장은 설치 방법과 표면 이름이 언어마다 달라 Java 전용으로
따로 작성되어 있다. 위 표의 링크에서 각 장을 바로 연다.

## 관련 문서

- 공개 계약: [Java 공개 계약](../../../common/spec/server/languages/java/README.ko.md)
- 언어 중립 의미: [공통 스펙](../../../common/README.ko.md)
- client library: [HTTP client](../http-client/README.ko.md) · [Stream connector](../stream-connector/README.ko.md)
