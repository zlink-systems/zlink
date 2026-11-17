# framework-locations-etcd

이 package는 **Redis with etcd** plugin의 etcd Location Store를 제공한다. Relocation Store에는 Redis package를 함께 설치하고 별도로 등록한다. 설치와 두 등록 호출은 [Store plugin 설정](../../../../doc/framework/cpp/guide/server/25-location.ko.md#store-setup)을 따른다.

## etcd client

2026-11-15 기준 저장소 선언과 잠금 상태다.

| 항목 | 현재 상태 | 근거 |
|---|---|---|
| Client | 공식 etcd v3.6.16 proto에서 생성한 C++ gRPC stub을 사용한다. | [CMakeLists.txt:3](CMakeLists.txt) |
| Version | Conan의 `with_etcd`는 `grpc/1.84.0`을 선언하고, 공통 선언은 `protobuf/6.33.5`·`abseil/20260107.1`이다. | [conanfile.py:73](../../packaging/conan/conanfile.py), [conanfile.py:27](../../packaging/conan/conanfile.py) |
| Lockfile | 네 Conan lock은 prebuilt 배포물의 그래프(`with_etcd=False`)라 protobuf 6.33.5·abseil 20260107.1이 있고 gRPC entry는 없다. etcd의 gRPC 버전은 conanfile.py의 정확한 버전이 정한다. | [conan.linux-x64.lock:3](../../packaging/conan/conan.linux-x64.lock) |
| vcpkg | `builtin-baseline`을 CI의 vcpkg commit `2ec0f1747d`로 고정하고, overrides로 grpc 1.84.0·abseil 20260107.1·protobuf 6.33.4(port-version 2)를 정한다. vcpkg 레지스트리에 protobuf 6.33.5가 없어 Conan과 patch 하나가 다르다. | [vcpkg.json:4](../../vcpkg.json), [vcpkg.json:35](../../vcpkg.json) |
| CMake | `find_package(gRPC CONFIG REQUIRED)`는 버전을 지정하지 않는다. 버전은 위 Conan 선언과 vcpkg overrides가 정한다. | [CMakeLists.txt:671](../../CMakeLists.txt) |

[linux-x64:3](../../packaging/conan/conan.linux-x64.lock) · [linux-arm64:3](../../packaging/conan/conan.linux-arm64.lock) · [macos-arm64:3](../../packaging/conan/conan.macos-arm64.lock) · [windows-x64:3](../../packaging/conan/conan.windows-x64.lock)

Proto와 import의 원본 license는 [THIRD_PARTY_NOTICES.md:40](../../../../../THIRD_PARTY_NOTICES.md)에 기록되어 있다.

Client와 resolved 전이 의존성의 버전별 license 목록은 저장소에 없다. Proto license 목록은 이 목록을 대체하지 않는다. Lockfile과 해석 그래프에 연결한 license 목록은 남은 작업이다.

[Client capability 결과](../../../../../scripts/etcd/README.ko.md#client-capability-결과)에서 시험 이름·명령·CI 원본과 미검증 범위를 확인한다.
