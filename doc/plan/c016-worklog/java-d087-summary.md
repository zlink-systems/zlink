# D-087 Java native resource cache

## 원인

- `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/LibraryLoader.java` 변경 전 60-66행에서 JVM마다 `zlink-native-*` 디렉터리를 만들고 native resource를 복사한 뒤 `deleteOnExit`에만 맡겼다. SIGKILL·crash에서는 shutdown hook이 실행되지 않아 디렉터리와 `libzlink.so`가 남았다.

## 변경

- `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/LibraryLoader.java:81-242`
  - resource를 64 KiB buffer로 읽으며 SHA-256과 크기를 계산한다.
  - `${ZLINK_JAVA_NATIVE_CACHE:-$HOME/.cache/zlink/native}/<sha256>/<libFile>`에 같은 크기의 파일이 있으면 그대로 쓴다.
  - 새 파일은 같은 디렉터리의 `<libFile>.<pid>.tmp`에 쓴 뒤 `ATOMIC_MOVE`로 배치한다. 경합에서 대상 파일이 먼저 생기면 기존 파일을 쓴다.
  - 대상 파일 크기가 resource와 다르면 지우고 다시 추출한다.
  - 캐시 경로를 만들거나 쓸 수 없으면 로그 없이 기존 `zlink-native-*` temp 추출로 돌아간다.
  - Windows OpenSSL dependency resource도 주 라이브러리의 hash 디렉터리에 추출하며, 이미 있으면 건너뛴다.
- `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/LibraryLoader.java:43-60`의 `ZLINK_LIBRARY_PATH` 우선 로딩과 `rememberLoaded`/`LOADED_LIBRARY_PATHS` 흐름은 유지했다. 기존 내부 문서도 `framework/doc/framework/java/internals/runtime-lifecycle.ko.md:88-92`에서 이 우선순위를 명시한다. 문서는 수정하지 않았다.

## 회귀 테스트

- `bindings/java/src/test/java/systems/zlink/runtime/nativeapi/LibraryLoaderTest.java:28-83`
  - 같은 resource를 두 번 추출해 SHA-256 디렉터리와 파일 1개를 재사용하고 새 `zlink-native-*` 디렉터리가 생기지 않는지 확인한다.
  - 캐시 파일 크기를 손상시킨 뒤 원본 내용으로 다시 추출되는지 확인한다.
  - `ZLINK_JAVA_NATIVE_CACHE`를 일반 파일로 지정한 별도 JVM에서 `ZLINK_LIBRARY_PATH` 없이 실제 `LibraryLoader.lookup()`과 `System.load`가 temp fallback으로 성공하는지 확인한다.
- test fixture: `bindings/java/src/test/resources/systems/zlink/runtime/nativeapi/native-test.bin`
- subprocess probe: `bindings/java/src/test/java/systems/zlink/runtime/nativeapi/LibraryLoaderFallbackProbe.java`

## 게이트 수치

- JDK: `/home/hep7hep7/.jdks/jdk-22.0.2+9`; Gradle worker 상한 2.
- 공식 `bindings/java/tests/run_tests.sh`: 통과.
  - `:test`: 93 tests, failures 0, errors 0, skipped 0.
  - `:integrationTest`: 17 tests, failures 0, errors 0, skipped 0.
  - `:zlink-ext-netty:test`: 3 tests, failures 0, errors 0, skipped 0.
  - `:kotlin-contract-test:test`: 4 tests, failures 0, errors 0, skipped 0.
  - 합계: 117 tests, failures 0, errors 0, skipped 0.
- `bindings/java/samples/run_samples.sh`: 7/7 sample tasks 통과.
- 변경 후 `LibraryLoaderTest` 강제 재실행: 3/3 통과.
- `git diff --check -- bindings/java`: 통과.

## BLOCKERS

- 없음.
- 시스템 기본 JDK는 21이지만 설치된 JDK 22를 명시해 class version 문제 없이 검증했다.
- 기존 untracked `core/build`, `core/build-dev` symlink는 건드리지 않았고 Core cmake/build/clean을 실행하지 않았다.
