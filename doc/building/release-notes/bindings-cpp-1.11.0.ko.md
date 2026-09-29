[English](./bindings-cpp-1.11.0.md) | [한국어](./bindings-cpp-1.11.0.ko.md)

# ZLink C++ binding 1.11.0 릴리스 노트

Core 1.11.0을 사용합니다. WRITABLE TIMED_OUT (803)을 BACKPRESSURED·EAGAIN으로 투영하고 ADMITTED일 때만 재제출합니다. 바인딩별로 Core와 달랐던 동작을 Core와 같게 맞추고 binding 자체 재시도·보관·시간 규칙을 제거했습니다 (#1154).

## 변경

- C++ binding은 버려진 비동기 send를 재제출하지 않으며, request 등록 lock은 map 접근에만 적용합니다 (#1164).

