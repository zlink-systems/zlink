[English](./framework-cpp-0.13.0.md) | [한국어](./framework-cpp-0.13.0.ko.md)

# ZLink C++ Framework 0.13.0 릴리스 노트

Framework 0.13.0는 binding 1.1.0과 Core 1.1.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 주요 변경

- 이 릴리스는 C++ 패키징 수정을 게시본에 담기 위해 냅니다. 사용자에게 보이는 API 변경은 없습니다.
- C++ 설치본이 vcpkg와 Conan 레이아웃에서도 성립합니다. 설치한 파일과 CMake config가 약속하는 것이 어긋나 `find_package(zlink_framework CONFIG REQUIRED)`가 실패하던 문제를 고쳤습니다.

## 설치

[`framework-cpp/v0.13.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.13.0)에서 `zlink-framework-cpp-0.13.0.tar.gz`를 내려받고 `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. vcpkg overlay port와 Conan recipe로도 설치할 수 있습니다.
