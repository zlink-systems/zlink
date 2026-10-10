[English](./bindings-rust-1.18.0.md) | [한국어](./bindings-rust-1.18.0.ko.md)

# ZLink Rust binding 1.18.0 릴리스 노트

Core 1.18.0을 따릅니다.

## 변경

- Core 1.18.0은 backpressure로 대기하는 REQUEST의 writable waiter를 등록할 때 기존 waiter 전체를 다시 검사하던 누적 O(n²) 비용을 제거했습니다. 등록 시 새 waiter만 검사하고, correlation waiter 전체 재검사는 실제 correlation 해제 시에만 수행합니다. #1590의 공개 API 측정에서 1,000건 등록 시간이 300.106ms에서 0.918ms로 줄었습니다 (#1466, #1590).
