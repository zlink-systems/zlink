[English](./bindings-node-1.10.0.md) | [한국어](./bindings-node-1.10.0.ko.md)

# ZLink Node.js binding 1.10.0 릴리스 노트

이 릴리스는 npm 패키지 버전을 1.10.0으로 올리고 Core 1.10.0을 요구합니다.

## 변경

- 명시적으로 poller를 닫을 때 Core가 `ZLINK_CLOSE_BUSY`/`EBUSY`를 반환하면 공개 `Busy` 오류를 전달하고 poller를 유효하게 유지합니다. 진행 중인 `wait()`가 끝난 뒤 다시 닫을 수 있습니다.
- context를 닫을 때 `zlink_ctx_shutdown`을 호출한 뒤 `zlink_ctx_term`을 호출합니다.
- poller가 반환한 Core 결과를 그대로 전달합니다. monitor에 `POLLOUT`을 등록하면 `NotSupported`, `POLLCOMPLETION`을 등록하면 `InvalidArgument`입니다.
- ROUTER의 선택 route를 `routesSnapshot()`, `PollRoute`, `routeGeneration`로 관찰할 수 있습니다. 수신 메시지의 route generation은 snapshot의 값과 동등성만 비교하는 token입니다.

## 검증

- 릴리스 사전 검사는 Node.js binding 버전, 릴리스 노트, npm 메타데이터를 확인합니다.

릴리스 태그는 `node/v1.10.0`입니다.
