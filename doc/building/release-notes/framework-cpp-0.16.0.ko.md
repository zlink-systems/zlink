[English](./framework-cpp-0.16.0.md) | [한국어](./framework-cpp-0.16.0.ko.md)

# ZLink C++ Framework 0.16.0 릴리스 노트

Framework 0.16.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- Select-one channel에서 eligibility와 drain 조건을 적용한 뒤 남은 member가 하나도 없으면 `Unavailable`로 끝납니다. Request와 one-way send가 같은 kind입니다. Weight가 `0`이거나 draining이어서 후보에서 빠진 경우가 여기에 해당하며, 송신 경로와 connection은 그대로 있으므로 `NotFound`가 아닙니다. 이전에는 언어마다 답이 달랐습니다.
- Channel send가 이 경우에 `not_found`로 끝나던 문제를 고쳤습니다. Request와 답이 달랐습니다.

## 고친 문제

- 프로세스 종료 중 대기 중이던 지연 타이머가 이미 파괴된 레지스트리를 조회해 죽던 문제를 고쳤습니다. 요청 만료 대기가 띄운 타이머가 정적 소멸과 겹칠 때 드러났습니다.

## 설치

[`framework-cpp/v0.16.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.16.0)에서 `zlink-framework-cpp-0.16.0.tar.gz`를 내려받고 `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. vcpkg overlay port와 Conan recipe로도 설치할 수 있습니다.
