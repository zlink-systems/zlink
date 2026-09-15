[English](./framework-cpp-0.14.0.md) | [한국어](./framework-cpp-0.14.0.ko.md)

# ZLink C++ Framework 0.14.0 릴리스 노트

Framework 0.14.0는 binding 1.1.0과 Core 1.1.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 주요 변경

- 이 릴리스는 C++ 패키징 수정을 게시본에 담기 위해 냅니다. 사용자에게 보이는 API 변경은 없습니다.
- vcpkg로 설치한 프로그램이 실행 중 SIGSEGV로 죽던 문제를 고쳤습니다. Core와 framework가 서로 다른 Boost 트리를 한 바이너리에 담고 있었습니다.
- vcpkg의 공유 설치 트리에서 의존 패키지의 헤더까지 다시 담아 패키징이 거부되던 문제를 고쳤습니다.

## 설치

[`framework-cpp/v0.14.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.14.0)에서 `zlink-framework-cpp-0.14.0.tar.gz`를 내려받고 `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. vcpkg overlay port와 Conan recipe로도 설치할 수 있습니다.
