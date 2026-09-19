[English](./framework-cpp-0.18.3.md) | [한국어](./framework-cpp-0.18.3.ko.md)

# ZLink C++ Framework 0.18.3 릴리스 노트

Framework 0.18.3은 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

공개 API는 바뀌지 않습니다. framework 소스는 0.18.2와 같습니다.

## 수정

- 배포 zip(`zlink-tutorial-cpp.zip`, `zlink-samples-cpp.zip`)의 `bootstrap.cmake`가 framework 소스 아카이브 버전을 `0.18.0`으로 손으로 고정하고 있어, 0.18.1·0.18.2 zip이 그 릴리스의 framework가 아니라 0.18.0을 받아 빌드했습니다. 이 값을 `sync-version`이 소유하도록 해 zip이 자기 릴리스 태그의 소스 아카이브를 받습니다. 0.18.1·0.18.2 zip을 받은 경우 이 릴리스의 zip으로 바꾸면 #692·#665 수정이 포함됩니다. (#718)

0.18.1·0.18.2의 변경은 [0.18.1](./framework-cpp-0.18.1.ko.md)·[0.18.2](./framework-cpp-0.18.2.ko.md) 릴리스 노트를 참고합니다.

## 설치

[`framework-cpp/v0.18.3` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.18.3)에서 `zlink-framework-cpp-0.18.3.tar.gz`를 내려받고 `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. vcpkg overlay port와 Conan recipe로도 설치할 수 있습니다.
