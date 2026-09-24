[English](./framework-cpp-0.24.1.md) | [한국어](./framework-cpp-0.24.1.ko.md)

# ZLink C++ Framework 0.24.1 릴리스 노트

Framework 0.24.1은 C++ binding 1.7.0과 Core 1.7.0을 사용합니다.

## 수정

- Linux에서 Godot C++ GDExtension을 처음 editor에 가져올 때 static TLS
  부족으로 `libzlink` 로드에 실패하던 문제를 해결합니다. Core 1.7.0은
  `initial-exec` 모델의 libstdc++ TLS를 더 이상 참조하지 않습니다
  ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- source archive의 `.sha256` 파일은 checksum 뒤에 디렉터리 경로 없이 archive 파일명만 기록합니다
  ([#1041](https://github.com/zlink-systems/zlink/issues/1041)).

## 설치

[`framework-cpp/v0.24.1` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.24.1)에 첨부된 `linux-x64`, `linux-arm64`, `macos-arm64`, `windows-x64` framework archive 중 환경에 맞는 것을 선택하거나 `bootstrap.cmake`를 실행해 해당 archive를 내려받습니다. `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. C++ binding 1.7.0과 Core 1.7.0이 필요합니다.

릴리스 태그는 [`framework-cpp/v0.24.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.24.1)입니다.
