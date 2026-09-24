[English](./bindings-cpp-1.6.0.md) | [한국어](./bindings-cpp-1.6.0.ko.md)

# ZLink C++ binding 1.6.0 릴리스 노트

이 릴리스는 C++ binding 패키지 버전을 Core 1.6.0에 맞춥니다.

## 변경

- C++ binding 패키지 버전을 Core 1.6.0 릴리스에 맞춰 1.6.0으로 올립니다.
- Core 1.6.0에서는 완전한 REQUEST를 수신한 뒤 source pipe의 연결이 종료되어도 ROUTER가 reply token을 발행합니다. 공개 C API와 ABI는 변경되지 않았습니다([#1051](https://github.com/zlink-systems/zlink/issues/1051)).
- C++ binding의 동작은 변경되지 않았습니다. binding 버전을 Core 1.6.0에 맞춥니다.

## 검증

- 릴리스 사전 검사는 C++ binding 버전, 릴리스 노트, Conan·vcpkg 패키지 메타데이터를 확인합니다.

릴리스 태그는 `cpp/v1.6.0`입니다.
