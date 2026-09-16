[English](./bindings-cpp-1.2.0.md) | [한국어](./bindings-cpp-1.2.0.ko.md)

# ZLink C++ binding 1.2.0 릴리스 노트

Core 1.2.0 위에서 동작하는 릴리스입니다. 1.1.0 binding은 Core 1.1.0 ABI를 전제하므로 Core 1.2.0과 함께 쓸 수 없고, 이 릴리스가 그 간극을 닫습니다.

## 변경

- Core 1.2.0 native와 결합합니다. 패키지에 담긴 native와 provenance는 Core 1.2.0 릴리스 자산입니다.
- typed 결과 코드를 errno로 읽던 오류 투영을 고쳤습니다(#355).
- vcpkg 레이아웃으로 설치한 config도 찾습니다.
## 검증

- binding 테스트 스위트가 Core 1.2.0 패키지 위에서 통과했습니다.

릴리스 태그는 `cpp/v1.2.0`입니다.
