[English](./bindings-cpp-1.13.0.md) | [한국어](./bindings-cpp-1.13.0.ko.md)

# ZLink C++ binding 1.13.0 릴리스 노트

Core 1.13.0을 사용합니다.

## 변경

- Core 1.13.0의 수신 진입 판정, 긴 구독 trie와 물리 연결 종료 monitor 수정이 포함됩니다 (#1292, #1334).
- REQUEST completion의 대표 errno를 Core REQUEST 표의 첫 errno에 맞춥니다 (#1248).
- REQUEST record를 소비한 뒤 `EPROTO`로 유실하던 단일 part 수신 overload를 삭제했습니다 (#1231).

## 유지하는 계약

다음 항목은 1.12.0에 포함된 동작을 유지하며, 1.13.0에서 새로 추가한 변경이 아닙니다.

- `stream_socket_t::disconnect_rid`를 제공합니다 (#1194).
- `request_result_t::backpressured` (113)를 제공합니다 (#1198).
- WRITABLE completion은 Core 결과를 보고하며, timeout으로 끝난 대기는 `BACKPRESSURED`와 `EAGAIN`입니다 (#1168).
