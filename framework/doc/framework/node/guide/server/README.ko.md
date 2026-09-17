---
title: "가이드 홈 · Node/TypeScript"
---

# ZLink Framework Node.js — 사용 가이드

Node.js/NestJS 환경에서 ZLink Framework를 사용하는 순서다. 03~17장은 모든 언어가 같은 정본을
공유하며, 예제는 `.ts` 탭을 고르면 Node.js 코드로 바뀐다.

| 순서 | 문서 | 내용 |
|----|------|------|
| 1 | [개요](01-overview.ko.md) | 무엇을 푸는가, 기존 방식과 무엇이 달라지는가 |
| 2 | [퀵스타트](../../quickstart.ko.md) | 설치, 두 process가 서로 호출하는 최소 project, 첫 실행 점검 |
| 3 | [핵심 개념](03-concepts.ko.md) | channel·Spot·Actor·session이 각각 무엇인가 |
| 4 | [Channel 메시징](20-channel-messaging.ko.md) | 이름으로 부르는 경로 — 등록과 호출 |
| 5 | [Spot](21-spot.ko.md) | 여럿이 함께 사용하는 자리를 id로 만들고 부르기 |
| 6 | [Actor](22-actor.ko.md) | 개체 하나를 id로 만들고 부르기 |
| 7 | [STREAM](23-stream.ko.md) | mesh 밖의 client가 연결 하나로 붙기 |
| 8 | [Session과 Actor 연결](24-actor-session.ko.md) | 연결 하나를 Actor 하나에 묶기 |
| 9 | [Location](25-location.ko.md) | id로 지금 있는 node를 조회하기 |
| 10 | [모니터링](26-monitoring.ko.md) | 기능별 가이드 자리 표시 — 본문은 아직 없다 |
| 11 | [실행 모델](32-execution-model.ko.md) | 두 queue, 직렬화 범위, 실행권 |
| 12 | [Backpressure](33-backpressure.ko.md) | 처리보다 도착이 빠를 때와 영향을 주는 옵션 |
| 13 | [활성화와 수명](34-activation-lifetime.ko.md) | 종류별 생성 시점, lifecycle callback, 주입 수명 |
| 14 | [Actor membership](35-actor-membership.ko.md) | Spot 사이 이동, 예약과 상한 |
| 15 | [Timer와 worker](36-timer-worker.ko.md) | 주기 실행, 줄 밖 실행, 실행권 반납 |
| 16 | [Relocation](37-relocation.ko.md) | 옮겨도 남는 것, adapter, 이동 단위 |
| 17 | [Channel 동작 원리](30-channel-patterns.ko.md) | 패턴 차이, 대상 선택, pub/sub, 연결과 discovery |
| 18 | [Handler와 메시지 처리](31-handler-dispatch.ko.md) | 등록 변형, filter, codec, handler 종류 |
| 19 | [STREAM의 동작 원리](38-stream-boundary.ko.md) | 등록 검증, 오류 귀속, 응답 token, 실행 방식 |
| 20 | [Session 묶음의 동작 원리](39-session-binding.ko.md) | 묶는 개수, 경로 갱신, 끊김 통지, 실패 |
| 21 | [ZLink를 어디에 쓰나](17-alternative.ko.md) | 사용처, 문제 신호, 기술 선택 경계, 라이선스 |
| 22 | [운영과 lifecycle](12-operations.ko.md) | 런타임 메트릭, relocate, drain, readiness 연결 |
| 23 | [Options](16-options.ko.md) | 옵션 목록, 기본값과 바꾸는 시점 |
| 24 | [샘플 고르기](14-samples.ko.md) | 어떤 샘플을 먼저 볼지 고르고 실행하는 방법 |
| 25 | [E2E 테스트](15-e2e-testing.ko.md) | client library로 시스템 전체를 검증하기 |
| 26 | [주요 타입 사용 색인](13-interface-catalog.ko.md) | 계약 인터페이스를 검증 코드로 색인 |
| 27 | [모니터링](26-monitoring.ko.md) | 재작성 대기 — 상태 snapshot과 진단 |

파일 번호는 언어에 상관없이 같은 장을 가리키는 식별자다. 읽는 순서는 이 표가 정한다.

01 · 11 · 13 · 16장은 설치 방법과 표면 이름이 언어마다 달라 Node.js 전용으로
따로 작성되어 있다. 위 표의 링크에서 각 장을 바로 연다.

## 관련 문서

- 공개 계약: [Node.js 공개 계약](../../../common/spec/server/languages/node/README.ko.md)
- 언어 중립 의미: [공통 스펙](../../../common/README.ko.md)
- client library: [HTTP client](../http-client/README.ko.md) · [Stream connector](../stream-connector/README.ko.md)
