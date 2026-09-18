[English](./framework-cpp-0.17.0.md) | [한국어](./framework-cpp-0.17.0.ko.md)

# ZLink C++ Framework 0.17.0 릴리스 노트

Framework 0.17.0는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- 묶인 session이 없는 Actor에서 bound session으로 보낼 때의 결과를 다섯 언어가 같게 맞췄습니다. 유효한 binding이 없으면 `InvalidOperation`으로 끝나고, 그 실패는 다른 호출 실패와 같이 **호출의 terminal**에서 관측합니다. 호출 객체를 만드는 자리에서 던지지 않습니다.
- Binding이 없는 Actor의 push가 정상 완료하고 frame만 버려지던 문제를 고쳤습니다. 이제 다른 네 언어와 같이 실패합니다.

## 설치

[`framework-cpp/v0.17.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.17.0)에서 `zlink-framework-cpp-0.17.0.tar.gz`를 내려받고 `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. vcpkg overlay port와 Conan recipe로도 설치할 수 있습니다.
