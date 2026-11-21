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

etcd-enabled Linux release 시험에 사용한 gRPC 전이 그래프는
[resolved inventory](dependencies/vcpkg-resolved.json)에 기록한다. 이 파일은 각 package의
version·port-version·feature·ABI·의존 간선과 `CONTROL`·license notice의 SHA-256을 연결한다.
[licenses](dependencies/licenses/)에는 vcpkg가 배포한 notice 전문을 보존한다.
줄 끝 공백과 EOF 빈 줄만 정리하며 원본 hash는 `sourceNoticeSha256`에 별도로 기록한다.
이 inventory는 vcpkg lockfile이 아니며 다른 triplet이나 Conan의 resolved 그래프를 대신하지 않는다.
gRPC notice에는 Apache-2.0 외에 BSD-3-Clause·MPL-2.0 적용 파일 목록도 있으므로 전문을 확인한다.
compiler·libc는 이 gRPC package 그래프의 범위 밖이다.

[Client capability 결과](../../../../../scripts/etcd/README.ko.md#client-capability-결과)에서 시험 이름·명령·CI 원본과 미검증 범위를 확인한다.

## Client capability 시험

`test_cpp_framework_locations_etcd`의 다음 시험은 실제 gRPC transport를 사용한다.

- `MaximumSendLimitUsesStoreFailureBeforeServerDispatch`: AuthenticateRequest의
  `ByteSizeLong()`이 16 MiB−1/16 MiB/16 MiB+1인지 검사한다. 앞의 두 요청은
  Authenticate·Txn 각각 1회이며 초과 요청은 서버 수신 0회와 Store failure를 검사한다.
- `ExactSerializedReceiveLimitUsesStoreFailure`: token과 unknown field를 포함한 전체
  AuthenticateResponse가 8 MiB−1/8 MiB/8 MiB+1인지 검사한다. 초과 응답은
  Authenticate 1회·Txn 0회와 Store failure를 검사한다.
- `NoResponseDeadlineMovesNextOperation`: operation deadline 뒤 서버의 `IsCancelled()`를
  확인한다. `ConsumerWaitCancellationDoesNotCancelOrRetirePhysicalOperation`은 소비자 대기
  취소가 물리 RPC를 취소하지 않고 provider가 종료까지 보유하는지 확인한다.
- `MutualTlsCustomCaAndIdentityHostNameAndTrustValidation`과 인증 refresh 시험은
  fake gRPC server에서 정상·거부 경로를 검사한다.

실제 etcd 보안 시험은 `EtcdLive.KeysOnlyRevisionAndAuthenticatedMutualTls`다.
etcd v3.6.16을 별도 instance로 실행하며 `--client-cert-auth`, CA·server certificate·key를
설정하고 root 사용자 생성·root role 부여 후 `etcdctl auth enable`을 실행한다.
시험 certificate는 `tests/tls/`에 있으며 server certificate의 hostname은 `localhost`다.
시험은 `cpp-capability` namespace의 고정 key 두 개를 생성·삭제하므로 전용 instance를 사용한다.

```sh
export ZLINK_ETCD_SECURE_ENDPOINT=https://localhost:2379
export ZLINK_ETCD_ROOT_PASSWORD=your-test-root-password
<build-dir>/test_cpp_framework_locations_etcd \
  --gtest_filter=EtcdLive.KeysOnlyRevisionAndAuthenticatedMutualTls
```

이 시험은 nonempty payload를 쓴 revision을 고정하고 값을 바꾼 뒤 `keys_only=false/true`로
읽는다. key·create/mod revision·version은 같고 value는 각각 원본 bytes와 빈 bytes여야 한다.
같은 서버에서 public provider의 인증·mTLS read 성공과 잘못된 password·CA·hostname·누락된
client identity의 Store failure도 확인한다. 환경 변수가 없으면 skip하며 보안 검증 통과로 세지 않는다.
물리 취소 검증은 operation deadline에 한정하며 새 공개 cancellation option을 추가하지 않는다.

2026-11-20 Linux x86_64에서 gRPC 1.84.0과 위 resolved inventory로 실행했다.
`--gtest_filter=-EtcdLive.*`는 34개 시험을 통과했으며, 인증·mTLS를 활성화한 실제
etcd 3.6.16에서 `EtcdLive.KeysOnlyRevisionAndAuthenticatedMutualTls`도 통과했다.
후자의 성공에는 잘못된 password·CA·hostname·누락된 client identity의 거부가 포함된다.
