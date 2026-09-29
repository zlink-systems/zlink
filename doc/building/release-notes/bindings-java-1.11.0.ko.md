[English](./bindings-java-1.11.0.md) | [한국어](./bindings-java-1.11.0.ko.md)

# ZLink Java binding 1.11.0 릴리스 노트

Core 1.11.0을 사용합니다. WRITABLE TIMED_OUT (803)을 BACKPRESSURED·EAGAIN으로 투영하고 ADMITTED일 때만 재제출합니다. 바인딩별로 Core와 달랐던 동작을 Core와 같게 맞추고 binding 자체 재시도·보관·시간 규칙을 제거했습니다 (#1154).

## 변경

- Java binding은 Core 제출을 lock 밖에서 수행하고 자체 worker pool과 기본 timeout을 제거했습니다 (#1164).

