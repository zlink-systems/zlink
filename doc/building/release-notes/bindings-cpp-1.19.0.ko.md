[English](./bindings-cpp-1.19.0.md) | [한국어](./bindings-cpp-1.19.0.ko.md)

# ZLink C++ binding 1.19.0 릴리스 노트

Core 1.19.0을 따릅니다.

## 변경

- Core 1.19.0에서 `zlink_socket_set_receive_flow_state`가 socket turn을 반납한 뒤 pipe 송신 끝에 control을 쓰던 결함을 수정했습니다. 다른 thread의 send와 control writer가 겹치면 `pipe.cpp:795/798` assertion이나 SIGSEGV로 종료될 수 있었습니다. 이제 마지막 control 쓰기와 deferred control flush가 끝날 때까지 socket turn을 유지합니다 (#1622, PR #1623).

- C++ 1.18.0 source archive의 checksum과 태그를 Conan/vcpkg recipe에 반영한 변경이 포함됩니다. C++ binding runtime 코드 변경은 없습니다.
