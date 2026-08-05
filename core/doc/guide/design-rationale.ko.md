---
title: "설계 근거 — 왜 이렇게 만들었나"
---

<!-- zlink-nav:start -->
[가이드 목록](README.ko.md) | [이전: Raw messaging 신뢰성](reliability.ko.md) | [다음: Message API와 ownership](09-message-api.ko.md)
<!-- zlink-nav:end -->

# 설계 근거 — 왜 이렇게 만들었나

> **이 장이 답하는 것** — zlink 핵심 설계 결정의 이유를 사용자 관점에서 설명한다.
> 구현 상세는 [internals](../internals/architecture.ko.md)가 소유한다.

이 문서는 zlink가 채택한 핵심 설계 결정의 **이유**를 사용자 관점에서 설명한다.
구현 상세는 [internals](../internals/architecture.ko.md)가 소유하며, 여기서는
"이 선택이 사용자에게 어떤 의미인가"에 집중한다. zlink를 도입할지 평가하거나
성능 특성을 이해하려는 독자를 위한 문서다.

## 출발점 — libzmq에서 무엇을 바꿨나

zlink는 [libzmq](https://github.com/zeromq/libzmq) v4.3.5에서 출발해 **핵심 패턴에
집중하고 표면을 좁힌** 라이브러리다. 자세한 대비표는 [개요](01-overview.ko.md)에
있다. 요지는 다음과 같다.

- 소켓 타입 17종 → **8종** (PAIR, PUB/SUB, XPUB/XSUB, DEALER/ROUTER, STREAM).
- I/O 엔진을 자체 poll/epoll/kqueue → **Boost.Asio**(번들, 외부 의존성 없음).
- 암호화를 CURVE(libsodium) → **TLS**(OpenSSL).
- 의존성을 **OpenSSL 하나로** 축소.

좁힌 이유는 단순하다. 적게 노출할수록 **각 패턴을 깊게 다듬고 일관되게 유지**하기
좋고, 사용자가 고를 것이 줄어 잘못 고를 여지도 준다.

## 핵심 설계 원칙

### Zero-Copy — 작은 메시지는 inline, 큰 메시지는 참조 카운팅

작은 메시지(64-bit에서 41바이트 이하)는 별도 힙 할당 없이 메시지 객체 안에 직접
저장한다(VSM, Very Small Message). 그보다 큰 메시지는 참조 카운팅으로 복사 없이 공유한다.

**사용자에게 의미**: 작은 제어 메시지(틱, 하트비트, 짧은 명령)가 많은
워크로드에서 할당·복사 비용이 사라진다. 송신이 메시지를 "소비"하는 이동 시맨틱도
이 모델에서 나온다 — 보관이 필요하면 명시적으로 복사한다([09 메시지
API](09-message-api.ko.md)).

> VSM은 **메모리 최적화일 뿐 wire 형식에는 영향이 없다.** 수신 측은 송신 측이
> inline 저장을 썼는지 알 필요가 없다([ZMP 레퍼런스](zmp-protocol.ko.md)).

### Lock-Free — 스레드 간 통신에 YPipe

스레드 사이 메시지 전달에 락 대신 CAS(Compare-And-Swap) 연산 기반 FIFO 큐(YPipe)를
쓴다.

**사용자에게 의미**: 핫 패스에서 락 경합이 없어 멀티코어 확장이 잘 된다. 대신
소켓은 스레드 안전하지 않다 — 같은 소켓을 여러 스레드에서 동시에 다루지 않는 것이
전제다([11 스레드 안전성](11-thread-safety.ko.md)).

### True Async — Proactor 패턴

Boost.Asio 기반으로 I/O **완료** 이벤트를 핸들러로 전달한다(Proactor). I/O를 직접
폴링하지 않고 완료를 통지받는 구조다.

**사용자에게 의미**: 콜백은 Context가 소유한 I/O 스레드에서 실행된다 — 콜백은 짧게
유지하고 lock을 잡지 않으며, 그 안에서 핸들을 닫지 않는다. 다중 소켓을 한 루프에서
poller로 묶는다(개념은 [02 Core API](02-core-api.ko.md), 언어 표면은 각
[바인딩 가이드](../../../bindings/doc/guide/README.ko.md)).

### Protocol Agnostic — Transport와 Protocol의 분리

wire protocol(ZMP)과 transport(tcp/ipc/inproc/ws/tls)를 명확히 분리한다.

**사용자에게 의미**: 같은 메시징 코드가 transport만 바꿔 inproc(같은 프로세스)에서
tcp(네트워크)로 옮겨 간다 — 주소 스킴만 바뀐다. 단 tls/wss는 주소 스킴 외에
`zlink_set_tls_server()`(서버 cert/key) / 필요 시 `zlink_set_tls_client()` 설정이 더
필요하다([04 Transport](04-transports.ko.md), [05 TLS/보안](05-tls-security.ko.md)).

## 더 깊이

- 계층 아키텍처 전체: [internals/architecture](../internals/architecture.ko.md)
- 설계 결정의 트레이드오프: [internals/design-decisions](../internals/design-decisions.ko.md)
- wire protocol: [ZMP 프로토콜 레퍼런스](zmp-protocol.ko.md)
- 전달 보장이 어디까지인지: [신뢰성·전달 보장](reliability.ko.md)
