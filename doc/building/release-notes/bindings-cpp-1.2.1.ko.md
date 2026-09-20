[English](./bindings-cpp-1.2.1.md) | [한국어](./bindings-cpp-1.2.1.ko.md)

# ZLink C++ binding 1.2.1 릴리스 노트

Core 1.2.0 위에서 동작하는 릴리스이며 Core는 변경되지 않았습니다.

## 변경

- Core 1.2.0의 `zlink_recv` typed no-data 결과를 ROUTER 수신 경로가 `-1`로 변환하지 않고 그대로 전달합니다. 이에 따라 호출자가 결과를 확인할 수 있으며, Framework C++의 receive timeout 처리가 Core의 결과를 유지합니다(#273).
## 검증

- Core 버전은 1.2.0으로 유지됩니다.

릴리스 태그는 `cpp/v1.2.1`입니다.
