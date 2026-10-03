[English](./bindings-rust-1.13.1.md) | [한국어](./bindings-rust-1.13.1.ko.md)

# ZLink Rust binding 1.13.1 릴리스 노트

Core 1.13.0을 사용합니다.

## 변경

- `set_linger`, `set_send_timeout`, `set_receive_timeout`과 대응하는 조회 함수가 `Duration` 대신 Core와 같은 `i32` millisecond를 받고 돌려줍니다. `-1`은 무제한 대기입니다. `Duration`으로는 Core의 `-1`을 표현할 수 없었습니다 (#1164).
  - 호출하는 코드는 `Duration::from_millis(n)` 대신 `n`을 넘깁니다.
