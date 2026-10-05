[English](./bindings-go-1.15.0.md) | [한국어](./bindings-go-1.15.0.ko.md)

# ZLink Go binding 1.15.0 릴리스 노트

Core 1.15.0을 따릅니다.

## 변경

- Core 1.15.0의 대기 토큰 계약을 따릅니다. backpressure로 기다리는 `DONTWAIT` send·request는 더 이상 시간 초과로 끝나지 않고, 자원 회복·대상 제거·socket close로 끝납니다 (#1452).
- Core가 더 이상 발행하지 않는 ABI 보존 값 `ZLINK_SEND_TIMED_OUT`(803)을 담은 WRITABLE completion은 `BACKPRESSURED`·`EAGAIN` 대신 다른 알 수 없는 값과 같이 `INTERNAL_ERROR`·`EPROTO`로 투영합니다 (#1452).
