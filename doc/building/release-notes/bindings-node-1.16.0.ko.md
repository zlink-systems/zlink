[English](./bindings-node-1.16.0.md) | [한국어](./bindings-node-1.16.0.ko.md)

# ZLink Node.js binding 1.16.0 릴리스 노트

Core 1.16.0을 따릅니다.

## 추가

- `sharedContext()`는 프로세스 전체에서 공유하는 Core context를 제공합니다. main thread와 `worker_threads` Worker가 같은 context를 사용하므로 Worker끼리 `inproc://`로 통신할 수 있습니다. 바인딩은 thread 참조로 context 수명을 관리하며 `SharedContext`에는 `close()`와 `shutdown()`이 없습니다 (#1478).

## 변경

- Core 1.16.0에 따라 `ZLINK_OPT_BINDTODEVICE`를 `tcp`, `tls`, `ws`, `wss` socket의 bind·connect 전에 적용합니다. 지원하지 않는 플랫폼에서 비어 있지 않은 값을 설정하면 `ZLINK_CONFIG_NOT_SUPPORTED`를 반환합니다 (#1469).
- Core 1.16.0에 따라 socket mailbox signaler 생성에 실패하면(프로세스 fd 한도 도달 포함) `zlink_socket()`이 보존된 시스템 errno와 함께 `NULL`을 반환합니다 (#1481).
