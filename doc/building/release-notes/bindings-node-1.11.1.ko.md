[English](./bindings-node-1.11.1.md) | [한국어](./bindings-node-1.11.1.ko.md)

# ZLink Node.js binding 1.11.1 릴리스 노트

Core 1.11.0을 사용합니다.

## 변경

- `StreamSocket.disconnectRid()`가 Core의 connect 결과를 connect 오류 계열로 투영합니다. 없는 routing ID는 이제 `ConfigError`가 아니라 `ConnectError`(`ConnectResult.NotFound`, 605)로 끝납니다. 다른 socket 타입과 같은 결과입니다 (#1082).
