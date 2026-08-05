---
title: "가이드 홈 · C#/.NET"
---

# ZLink Framework .NET — 사용 가이드

`.NET`/`ASP.NET Core` 환경에서 ZLink Framework를 쓰는 순서다. 03~17장은 모든 언어가 같은
정본을 공유하며, 예제는 `C#/.NET` 탭을 고르면 `.NET` 코드로 바뀐다.

| 순서 | 문서 | 내용 |
|----|------|------|
| 1 | [1. 개요](01-overview.ko.md) | 무엇/왜/누구를 위한 것, 기존 방식 대비 체감 난이도, 4축 |
| 2 | [2. 시작하기](02-getting-started.ko.md) | NuGet 설치, 두 process 최소 예제, TicTacToe 방 생성 흐름 |
| 3 | [3. 핵심 개념](03-concepts.ko.md) | 핵심 개념과 공통 스펙 매핑 |
| 4 | [4. Backpressure](04-backpressure.ko.md) | 처리보다 도착이 빠를 때의 동작 원리와 영향을 주는 옵션 |
| 5 | [5. Channel Messaging](05-channel-messaging.ko.md) | request / send / pub-sub 등록과 호출 사용법 |
| 6 | [6. Spot](06-spot.ko.md) | room / stage / zone 같은 동적 SPOT 등록과 호출 사용법 |
| 7 | [7. Actor와 Spot](07-actor-spot.ko.md) | actor 모델과 Spot 의 actor 호스팅(lifecycle 콜백·트리거 함수, location 축) |
| 8 | [8. Session과 Actor binding](08-actor-session.ko.md) | session ↔ actor relay·binding·bound session push (binding 축) |
| 9 | [9. STREAM](09-stream.ko.md) | 외부 client STREAM 서버와 Stream Connector 사용법 |
| 10 | [10. Location](10-location.ko.md) | location store 등록, 자동 연결, 운영 조회 사용법 |
| 11 | [11. Monitoring](11-monitoring.ko.md) | 상태 snapshot·status stream과 진단으로 runtime을 관측하는 방법 |
| 12 | [12. 운영](12-operations.ko.md) | 운영 — 런타임 메트릭, graceful drain, readiness 통합 |
| 13 | [13. 주요 타입 사용 색인](13-interface-catalog.ko.md) | 모든 계약 인터페이스를 ContractTests 검증 코드로 색인 |
| 14 | [14. 샘플 고르기](14-samples.ko.md) | 어떤 샘플을 먼저 볼지 고르고 실행하는 방법 |
| 15 | [15. E2E 테스트](15-e2e-testing.ko.md) | client 라이브러리로 시스템 전체를 검증하는 E2E 테스트 만드는 법 |
| 16 | [16. Options](16-options.ko.md) | 설정 — 옵션 목록, 기본값과 변경 시점 |
| 17 | [17. ZLink를 어디에 쓰나](17-alternative.ko.md) | 사용처, 문제 신호, 기술 선택 경계 |

파일 번호는 언어에 상관없이 같은 장을 가리키는 식별자다. 읽는 순서는 이 표가 정한다.

01 · 02 · 11 · 13 · 16장은 설치 방법과 표면 이름이 언어마다 달라 `.NET` 전용으로 따로 쓴다.

## 관련 문서

- `.NET` 문서 진입점: [ZLink Framework for .NET](../../README.ko.md)
- 공개 계약: [.NET 공개 계약](../../../common/spec/server/languages/dotnet/README.ko.md)
- 언어 중립 의미: [공통 스펙](../../../common/README.ko.md)
- client library: [HTTP client](../http-client/README.ko.md) · [Stream connector](../stream-connector/INDEX.ko.md)
