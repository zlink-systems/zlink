[English](../../../README.md) | [한국어](README.ko.md)

# zlink Core 가이드

이 가이드는 raw Core C API의 용도와 사용 순서를 설명한다. 정확한 함수 계약은
[`core/doc/spec/core/`](../spec/core/README.ko.md)을 기준으로 한다.

## 시작하기

- [01 개요](01-overview.ko.md): runtime 범위와 socket pattern
- [02 Core C API](02-core-api.ko.md): context, socket과 eventing API
- [03-0 Socket Pattern](03-0-socket-patterns.ko.md): 통신 방식 선택

## Socket과 transport

- [PAIR](03-1-pair.ko.md), [PUB/SUB](03-2-pubsub.ko.md),
  [DEALER](03-3-dealer.ko.md), [ROUTER](03-4-router.ko.md)
- [STREAM](03-5-stream.ko.md), [Proxy](03-6-proxy.ko.md)
- [Transport](04-transports.ko.md), [TLS](05-tls-security.ko.md)

## Message와 운영

- [Routing ID](08-routing-id.ko.md)
- [Message API](09-message-api.ko.md)
- [Monitoring](06-monitoring.ko.md)
- [성능](10-performance.ko.md), [Thread safety](11-thread-safety.ko.md),
  [Socket option](12-socket-options.ko.md)
- [신뢰성](reliability.ko.md), [공통 시나리오](scenarios.ko.md), [용어](glossary.ko.md)

Application topology와 stateful object 사용법은 `framework/doc/` 아래의 언어별 Framework
가이드에서 설명한다.
