# C++ stream connector package configuration 복구 결과

## 변경 내용

`framework/languages/cpp/CMakeLists.txt:1702-1714`는 package consumer test에
`ZLINK_FRAMEWORK_CPP_CONFIGURATION`과
`ZLINK_FRAMEWORK_CPP_IS_MULTI_CONFIG`를 전달한다. 단일 구성 generator에서는
`CMAKE_BUILD_TYPE`를 전달하고, multi-config generator에서는 `$<CONFIG>`를 전달하므로
CTest를 `-C <configuration>`으로 실행한 구성을 사용한다.

`framework/languages/cpp/tests/Zlink.Framework.PackageTests/stream_connector_consumer.cmake:7-13`은
두 입력을 필수로 확인한다. `:33-38`의 component install과 `:112-114`의 consumer build는
같은 configuration을 `--config`에 전달한다. 단일 구성 consumer configure에는
`:101-104`에서 같은 값을 `CMAKE_BUILD_TYPE`로 전달한다. 따라서 Debug producer는 Debug
export file을 install하고 Debug consumer를 build한다. Release 및 vcpkg Release 경로도
Release configuration을 그대로 사용한다.

## 검증 결과

`framework/languages/cpp/build/linux-ninja-c-e2e`는 Ninja 단일 구성 tree이며
`CMAKE_BUILD_TYPE=Debug`였다. 재구성 뒤 CTest 등록 명령이
`ZLINK_FRAMEWORK_CPP_CONFIGURATION=Debug`를 전달하는 것을 확인했다.

| 검증 | 결과 |
|---|---|
| `ctest --test-dir build/linux-ninja-c-e2e -R '^test_cpp_stream_connector_install_consumer$' --output-on-failure` (1회) | 통과, 2.09초 |
| 같은 명령 (2회) | 통과, 2.62초 |
| 같은 명령 (3회) | 통과, 2.13초 |
| `ctest --test-dir build/linux-ninja-c-e2e -R '^test_cpp_framework_install_consumer$' --output-on-failure` | 통과, 12.89초 |

## BLOCKERS

없음.
