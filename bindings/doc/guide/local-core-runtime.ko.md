---
title: "바인딩 로컬 Core 빌드"
---

# 바인딩 로컬 Core 빌드

바인딩의 기본 Core 입력은 검증된 release 패키지다. 작업 중인 Core를 명시적으로
사용할 때만 다음을 실행한다. 먼저 `core/build`에 `libzlink`를 빌드해야 한다.

```bash
export ZLINK_CORE_SOURCE=local
source bindings/tools/local_core_runtime.sh
```

이 명령은 `ZLINK_CORE_INCLUDE_DIR=core/include`와
`ZLINK_CORE_LIB_DIR=core/build/lib`를 export한다. 이후 같은 셸에서 빌드해야 한다.
`ZLINK_CORE_SOURCE`를 지정하지 않으면 release가 기본값이며, helper는 기존처럼
release 패키지를 가져오고 검증한다.

## C와 C++

C++는 `./bindings/cpp/build.sh OFF OFF`로 빌드한다. C는
`./bindings/c/tests/run_tests.sh` 또는 CMake 진입점으로 빌드한다. 두 CMake
프로젝트는 local일 때 `core/build`와 `core/include`를 사용한다.

## Rust

`./bindings/rust/build.sh`를 사용한다. 이 진입점은 Cargo에 local runtime을
전달하며, `cargo build`를 직접 실행할 때도 위 helper를 source한 환경을 유지해야
한다.

## Node와 JavaScript

Node 네이티브 addon은 `cd bindings/node && npm run rebuild-native`로 다시
빌드한다. `binding.gyp`는 local일 때 export된 include/lib 디렉터리를 사용한다.
JavaScript 샘플은 별도 native addon이 없으므로 빌드된 Node addon을 그대로 쓴다.

## Python

`./bindings/python/build.sh` 또는 helper를 source한 뒤
`cd bindings/python && python3 setup.py build_ext`를 실행한다. Python 확장은
local include/lib 디렉터리로 컴파일·링크한다. 배포 wheel을 만들 때는 release
입력을 유지해야 한다.

## Go

`./bindings/go/build.sh ./...`를 사용한다. 이 진입점은 cgo의 include/link/runtime
경로를 local Core에 맞춘다. 테스트와 샘플 runner도 같은 환경을 설정한다.

## Java와 Kotlin

`cd bindings/java && ./gradlew build`를 실행한다. Gradle은 local일 때 JNI bridge를
`ZLINK_CORE_INCLUDE_DIR`와 `ZLINK_CORE_LIB_DIR`로 빌드하고, Kotlin은 이 Java
runtime을 공유한다.

## .NET

helper를 source한 뒤 `dotnet build bindings/dotnet/Zlink.sln`을 실행한다.
테스트와 샘플 runner는 local runtime을 process loader에 전달한다. `dotnet pack`도
local일 때 `ZLINK_CORE_LIB_DIR`의 Linux native payload를 명시적으로 사용한다.

