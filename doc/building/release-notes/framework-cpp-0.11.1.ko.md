[English](./framework-cpp-0.11.1.md) | [한국어](./framework-cpp-0.11.1.ko.md)

# ZLink C++ Framework 0.11.1 릴리스 노트

Framework 0.11.1은 binding 0.17.7과 Core 0.17.5를 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 주요 변경

- 게시된 C++ framework standalone package가 필요한 runtime 입력을 포함해 설치·사용되도록 수정했습니다.
- Windows 전체 빌드와 대표 PowerShell 단독 실행이 성공했습니다: Release 28 targets/install, TicTacToe, DeliveryDispatch.

## 설치

[`framework-cpp/v0.11.1` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.11.1)에서 `zlink-framework-cpp-0.11.1.tar.gz`를 내려받고 `find_package(zlink_framework_cpp CONFIG REQUIRED)`를 사용합니다.
