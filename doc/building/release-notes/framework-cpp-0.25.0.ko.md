[English](./framework-cpp-0.25.0.md) | [한국어](./framework-cpp-0.25.0.ko.md)

# ZLink C++ Framework 0.25.0 릴리스 노트

Framework 0.25.0은 C++ binding 1.7.0과 Core 1.7.0을 사용합니다.

## 변경

- C++ Stream Connector가 Core와 C++ binding에 의존하지 않습니다. raw payload, typed codec, compression codec 인터페이스의 바이트 타입이 `zlink::message_t`에서 `std::vector<std::uint8_t>`로 바뀝니다. connector 패키지(CMake config, Conan, vcpkg)는 Core를 요구하지 않고, Godot·Unreal·Axmol 확장은 `libzlink`를 적재하지 않습니다. connector 공개 헤더는 예외를 끈 빌드(`-fno-exceptions`, `/EHs-c-`)에서 컴파일됩니다 ([#1077](https://github.com/zlink-systems/zlink/issues/1077)).

## 수정

- Linux에서 Godot C++ GDExtension을 처음 editor에 가져올 때 static TLS
  부족으로 `libzlink` 로드에 실패하던 문제를 해결합니다. Core 1.7.0은
  `initial-exec` 모델의 libstdc++ TLS를 더 이상 참조하지 않습니다
  ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- source archive의 `.sha256` 파일은 checksum 뒤에 디렉터리 경로 없이 archive 파일명만 기록합니다
  ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- 샘플 README의 실행 명령을 샘플 디렉터리 기준으로 고치고, tutorial의 PowerShell POST 예시가 JSON을 그대로 보내도록 고쳤습니다 ([#1035](https://github.com/zlink-systems/zlink/issues/1035)).

## 설치

[`framework-cpp/v0.25.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.25.0)에 첨부된 `linux-x64`, `linux-arm64`, `macos-arm64`, `windows-x64` framework archive 중 환경에 맞는 것을 선택하거나 `bootstrap.cmake`를 실행해 해당 archive를 내려받습니다. `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. 서버 framework는 C++ binding 1.7.0과 Core 1.7.0이 필요하며, archive에 함께 들어 있습니다. Stream Connector만 쓰는 client는 Core와 binding이 필요하지 않습니다.

릴리스 태그는 [`framework-cpp/v0.25.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.25.0)입니다.
