# C++ F-R8-14 Redis encoded blob 상한

공식 C++ Redis relocation-store provider는 encoded blob을 `64 MiB + 23 bytes`
(67,108,887 bytes)까지 수락한다. `64 MiB + 24 bytes`(67,108,888 bytes)는
`std::invalid_argument`로 거부하며 Redis에 제출하지 않는다. 기존 `put`의 크기 검사와
오류 문구를 수정했다. Application data chunk 상한은 64 MiB다.

- 원인 재확인: `framework/languages/cpp/extensions/framework-locations-redis/include/zlink/locations/redis.hpp:965`의 `put`은 수정 전 `:970`에서 encoded payload에 `64u * 1024u * 1024u` 상한을 적용했다. F-R8-14의 인용 위치와 원인이 일치한다. 수정한 검사는 현재 `:971`이다.
- 소유 계층: Framework의 공식 Redis relocation-store provider. Provider 입력의 크기 검증은 이 모듈이 소유한다.
- Spec 조항: `framework/doc/framework/common/spec/server/05-location-relocation/03-relocation-store-redis.ko.md` §3, `:121–122`, `:135–138`. Application data chunk와 23-byte immutable envelope를 포함한 encoded blob의 상한을 구분한다.
- 교차언어 대조: `.NET`의 `framework/languages/dotnet/src/Zlink.Framework.Locations.Redis/ZLinkRedisRelocationStore.cs:18–19`, `:339–342`는 이미 `64 * 1024 * 1024 + 23`을 적용한다. C++ 수정은 언어 구조 차이가 아니라 잘못 적용한 입력 상한의 교정이다. 다른 언어는 수정하지 않았다.
- 변경 분류: **B — 기존 결함**. D-114의 F-R8-14 구현 parity 수정 결정과 작업 지시에 따른다.
- 수정 전/후 규칙 수: encoded blob의 상충하는 입력 상한 **2 → 1**. C++의 64 MiB 판정을 공통 계약·.NET의 `64 MiB + 23 bytes`에 맞췄다. 검사 분기는 기존 1개를 유지하며 상태·옵션·재시도를 추가하지 않았다.

변경 파일은 다음과 같다.

| 파일 | 변경 내용 |
|---|---|
| `framework/languages/cpp/extensions/framework-locations-redis/include/zlink/locations/redis.hpp:970` | Encoded blob 상한과 초과 시 오류 문구 수정 |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_redis_blob_bound.cpp:20` | 상한 수락과 상한 + 1 byte 거부를 검증하는 회귀 테스트 2개 |
| `framework/languages/cpp/tests/support/fake_redis/sw/redis++/redis++.h:1` | 해당 테스트 target에서만 사용하는 Redis client fake |
| `framework/languages/cpp/CMakeLists.txt:1719` | Redis 서버·redis++ 설치 없이 실행하는 `framework-unit` 회귀 target 등록 |
| `doc/plan/c016-worklog/fix-cpp-r8-parity-summary.md` | 수정 근거와 검증 결과 |

회귀 테스트는 실제 `redis_relocation_store_t`를 `relocation_store_t` 공개 인터페이스로
호출한다. Fake에는 크기 제한을 구현하지 않았다. 수락 테스트는 67,108,887 bytes 전체가
원본과 동일하게 Redis EVAL에 전달되고 `blob_stored_t`가 반환되는지 검증한다. 거부
테스트는 67,108,888 bytes가 예외를 발생시키고 EVAL 호출 수가 0인지 검증한다.

수정 전 실행에서 수락 테스트는 `relocation blob exceeds 64 MiB` 예외로 실패했고 거부
테스트는 통과했다. 수정 후 두 테스트를 포함한 target은 **5회 모두 통과**했다.

Build는 `framework/languages/cpp`에서 실행했다.

```bash
cmake --build --preset linux-ninja-debug -j 4
```

Gate와 반복 검증은 repository root에서 실행했다.

```bash
flock -w7200 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build/linux-ninja-debug -L framework-unit --output-on-failure
flock -w7200 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build/linux-ninja-debug -R '^test_cpp_framework_redis_blob_bound$' --repeat until-fail:5 --output-on-failure
```

| 검증 | 결과 | 로그 |
|---|---|---|
| 수정 전 회귀 재현 | 수락 테스트 실패, 거부 테스트 통과 | `/tmp/zlink-cpp-r8-before-fix.log` |
| `linux-ninja-debug` 전체 build | exit 0 | `/tmp/zlink-cpp-r8-build.log` |
| `framework-unit` gate | **51/51 통과, 실패 0건**, 97.69초 | `/tmp/zlink-cpp-r8-framework-unit.log` |
| 새 회귀 target 5회 | **5/5 통과**, 매회 경계 테스트 2개 실행 | `/tmp/zlink-cpp-r8-regression-5x.log` |

남은 테스트 실패는 없다. `framework/doc/framework/common/spec/server/languages/cpp/interfaces/07-location-store.ko.md:260`의
64 MiB 문구는 공통 §3과 다르며, encoded blob 기준 `64 MiB + 23 bytes`로 정정할 대상이다.
이 작업에서는 보호된 문서 경로를 수정하지 않았다. Core·binding·다른 언어와 기존 사용자
변경을 수정하지 않았으며 commit·push를 수행하지 않았다.
