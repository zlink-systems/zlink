[English](./bindings-java-1.20.0.md) | [한국어](./bindings-java-1.20.0.ko.md)

# ZLink Java binding 1.20.0 릴리스 노트

Core 1.20.0을 따릅니다.

## 변경

- Core 1.20.0: handshake 중 completion poller 등록이 임시 transport owner의 executor를 멈춰 pair가 30초 안에 READY에 도달하지 못하던 결함을 수정했습니다(#2504, PR #2505). 이 Core 수정은 C++·.NET·Java·Node.js binding 모두에 영향을 줍니다.
