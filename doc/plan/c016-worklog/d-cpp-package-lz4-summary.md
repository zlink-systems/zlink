# C++ package system LZ4 수정 결과

## 선택한 계약

설치된 Framework와 Stream Connector 패키지는 LZ4를 패키지 안에 포함하고, export target은
그 package-relative 파일을 참조한다. `find_dependency(lz4)`로 소비자의 system LZ4를 다시 찾게
하지 않는다.

근거는 다음과 같다.

- `stream_connector_consumer.cmake:60-68`은 StreamConnector 컴포넌트 단독 설치에
  `lib/liblz4.a`가 포함되어야 한다고 검사한다.
- 같은 테스트의 `:49-53`은 export에 producer의 `ZLINK_LZ4_LIBRARY` 또는 vcpkg 절대 경로가
  남는 것을 금지한다.
- C++ Stream Connector 패키징 가이드는 설치 export가 LZ4를 package prefix 기준으로
  참조한다고 설명한다.
- 기존 vcpkg 정적 배치에서는 이미 선택된 `liblz4.a`를 그대로 설치하므로 기존 동작을 바꾸지
  않는다.

## 변경

- `framework/languages/cpp/CMakeLists.txt:430-442`: 빌드용으로 찾은 LZ4 library와 설치용
  library를 구분했다. Unix에서 같은 library 디렉터리에 `liblz4.a`가 있으면 설치용으로 정적
  archive를 선택하고, 없으면 기존에 찾은 library를 그대로 사용한다. 설치 export의 상대 파일명도
  설치용 library에서 계산한다.
- `framework/languages/cpp/CMakeLists.txt:724-745`: Framework 및 StreamConnector 컴포넌트가
  선택된 설치용 LZ4 library를 복사하도록 변경했다.

Ubuntu `liblz4-dev`에서는 빌드 link 입력 `/usr/lib/x86_64-linux-gnu/liblz4.so`를 유지하면서
설치 산출물은 완전한 `/usr/lib/x86_64-linux-gnu/liblz4.a`가 된다. 따라서 target이 없는
`liblz4.so` symlink만 패키지에 복사되는 문제가 없다.

## 검증 결과

- `cmake --preset linux-ninja-debug -B build/linux-ninja-c-e2e`: PASS
- `cmake --build build/linux-ninja-c-e2e --target zlink_framework zlink_http_client zlink_stream_connector zlink_unreal_stream_connector -j2`: PASS
- `test_cpp_framework_install_consumer`: 3회 PASS
- `test_cpp_stream_connector_install_consumer`: 3회 모두 `lib/liblz4.a` 설치 및 존재 검사를 PASS한
  뒤 아래 별도 blocker에서 FAIL
- `git diff --check -- framework/languages/cpp/CMakeLists.txt`: PASS

## BLOCKERS

`test_cpp_stream_connector_install_consumer`는 지정된 Debug 단일 구성 빌드에서도
`stream_connector_consumer.cmake:30`의 `--config Release`를 고정한다. 이 때문에 CMake install은
`zlink_stream_connector_cppTargets.cmake`는 복사하지만 Debug target 위치를 가진
`zlink_stream_connector_cppTargets-debug.cmake`는 복사하지 않는다. 이어지는 consumer configure가
`stream_connector_consumer.cmake:97`에서 다음 오류로 실패한다.

```text
IMPORTED_LOCATION not set for imported target "zlink::stream_connector".
```

이는 LZ4 누락 뒤에 가려져 있던 별도 package-test 구성 문제다. 이번 작업의 한 원인 범위를 지키기
위해 테스트의 구성 선택은 수정하지 않았다. 해결 시 테스트가 현재 build configuration을 install에
전달하도록 별도 수정해야 한다.
