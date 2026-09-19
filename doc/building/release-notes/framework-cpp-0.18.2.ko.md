[English](./framework-cpp-0.18.2.md) | [한국어](./framework-cpp-0.18.2.ko.md)

# ZLink C++ Framework 0.18.2 릴리스 노트

Framework 0.18.2는 binding 1.2.0과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

공개 API는 바뀌지 않습니다.

## 공통 변경

- 이 릴리스의 GitHub Release에 붙는 tutorial·samples zip 여덟 개 가운데 .NET zip이 framework .NET 0.18.2(binding 1.2.2, Windows에서 배포 패키지만으로 동작)를 고정합니다. C++ zip의 내용은 0.18.1과 같습니다. 0.18.1의 변경은 [0.18.1 릴리스 노트](./framework-cpp-0.18.1.ko.md)를 참고합니다. (#702)

## 수정

- User Spot을 materialize할 때 reservation의 `AuthorityOwnerGeneration`을 Spot 컨텍스트에 넘기지 않아 기본값 1이 되고, 올바른 generation으로 fence된 메시지가 turn에 들어가지 못하던 것을 고쳤습니다. relocation target에서 같은 zone으로 보내는 ZoneWorld `UpdatePosition`이 이 때문에 ZW-B2에서 timeout이 났습니다. .NET은 값을 그대로 넘깁니다. (#665)
- ZoneWorld 샘플을 계약 정본 §8에 맞췄습니다. join completion을 OperationId로 중복 제거하고, same-zone 이동에서 Zone Spot에 `UpdatePosition`을 보내 사본을 갱신합니다. 이로써 일곱 샘플의 계약 정합이 네 언어에서 끝났습니다. (#665)

## 설치

[`framework-cpp/v0.18.2` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.18.2)에서 `zlink-framework-cpp-0.18.2.tar.gz`를 내려받고 `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. vcpkg overlay port와 Conan recipe로도 설치할 수 있습니다.
