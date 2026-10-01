[English](./bindings-cpp-1.12.0.md) | [한국어](./bindings-cpp-1.12.0.ko.md)

# ZLink C++ binding 1.12.0 릴리스 노트

Core 1.12.0을 사용합니다.

## 변경

- STREAM socket은 connect·disconnect를 제공하지 않으며 `disconnectRid`를 제공합니다. 존재하지 않는 RID는 connect 오류 NotFound (605)입니다.
- `stream_socket_t::disconnect_rid`를 공개합니다 (PR #1194).

## 동작 변경

- REQUEST 결과 `backpressured`(113)를 공개합니다. 해당 오류의 errno는 Core 기준에 따라 `EAGAIN`입니다 (#1198).
- REQUEST 결과별 대표 errno를 Core 표에 맞춥니다. `CONFLICT`는 `EEXIST`, `INTERNAL_ERROR`는 `EIO`를 반환합니다 (#1248).
- DEALER와 ROUTER의 단일 part 수신 overload를 제거합니다. 다중 part 메시지를 받는 코드는 part 목록을 받는 API를 사용해야 합니다 (#1231).
