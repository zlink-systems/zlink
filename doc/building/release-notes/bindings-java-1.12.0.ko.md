[English](./bindings-java-1.12.0.md) | [한국어](./bindings-java-1.12.0.ko.md)

# ZLink Java binding 1.12.0 릴리스 노트

Core 1.12.0을 사용합니다.

## 변경

- STREAM socket은 connect·disconnect를 제공하지 않으며 `disconnectRid`를 제공합니다. 존재하지 않는 RID는 connect 오류 NotFound (605)입니다.

## 동작 변경

- 동시 수신 중 Core가 `BUSY`를 반환하면 빈 수신으로 처리하지 않고 `ZlinkRecvException`으로 보고합니다 (#1200).
- PUB·XPUB publish가 전달한 send flags를 반영합니다. `NONE`은 send timeout까지 대기할 수 있고, `DONT_WAIT`은 즉시 반환합니다 (#1200).
- 긴 구독 topic 수신 시 버퍼를 Core가 보고한 길이까지 늘립니다 (#1218).
- REQUEST 결과별 대표 errno를 Core 표에 맞춥니다. `CONFLICT`는 `EEXIST`, `INTERNAL_ERROR`는 `EIO`를 반환합니다 (#1218, #1248).
