# ZLink Core 문서

ZLink Core는 context, raw socket, message, poller, timer, monitor와 transport를 제공하는 C runtime이다.
Application topology와 stateful object runtime은 Framework 문서에서 다룬다.

| 영역 | 위치 | 내용 |
|---|---|---|
| 사용자 가이드 | [guide/](guide/README.ko.md) | raw socket pattern, transport, TLS, monitoring과 성능 |
| 공개 계약 | [spec/](spec/README.ko.md) | Core C API의 정확한 계약 |
| 내부 구현 | [internals/](internals/architecture.ko.md) | context, socket, engine, protocol과 transport 구조 |

언어별 사용법은 [`bindings/doc/`](../../bindings/doc/README.ko.md), application runtime은
[`framework/doc/`](../../framework/doc/README.ko.md)에서 확인한다.
