[English](./bindings-cpp-1.7.0.md) | [한국어](./bindings-cpp-1.7.0.ko.md)

# ZLink C++ binding 1.7.0 릴리스 노트

이 릴리스는 C++ binding 패키지 버전을 Core 1.7.0에 맞춥니다.

## 변경

- C++ binding 패키지 버전을 1.7.0으로 올리고 Core 1.7.0을 요구합니다.
- Core 1.7.0은 Linux의 Godot C++ GDExtension 첫 editor 가져오기에서
  static TLS 부족으로 `libzlink`를 로드하지 못하던 문제를 해결합니다
  ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- C++ binding의 공개 API와 동작은 변경되지 않았습니다.

## 검증

- 릴리스 사전 검사는 C++ binding 버전, 릴리스 노트, Conan·vcpkg 패키지 메타데이터를 확인합니다.

릴리스 태그는 `cpp/v1.7.0`입니다.
