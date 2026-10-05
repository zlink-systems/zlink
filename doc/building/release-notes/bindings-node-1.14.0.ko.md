[English](./bindings-node-1.14.0.md) | [한국어](./bindings-node-1.14.0.ko.md)

# ZLink Node.js binding 1.14.0 릴리스 노트

Core 1.14.0을 따릅니다.

## 변경

- Core 1.14.0의 연결 종료, terminal ERROR 쓰기 순서, handshake timeout, session 수명 수정을 포함합니다 (#1434).
- blocking REQUEST에서 `RCVTIMEO` 때문에 completion 수신이 `NO_DATA`를 반환해도 Core의 최종 completion을 계속 기다립니다. 이 수정은 1.13.1에 포함됐습니다.
