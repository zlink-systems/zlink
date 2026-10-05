[English](./bindings-rust-1.14.0.md) | [한국어](./bindings-rust-1.14.0.ko.md)

# ZLink Rust binding 1.14.0 릴리스 노트

Core 1.14.0을 따릅니다.

## 변경

- Core 1.14.0의 연결 종료, terminal ERROR 쓰기 순서, handshake timeout, session 수명 수정을 포함합니다 (#1434).
- `linger`, send timeout, receive timeout의 getter와 setter는 signed `i32` millisecond를 사용하며 무제한 대기를 뜻하는 Core의 `-1`을 지원합니다 (#1164). 1.13.0의 `Duration` signature를 대체하므로 호출자는 millisecond를 전달해야 합니다. 이 변경은 1.13.1에 포함됐습니다.
