# Zlink.Framework.Locations.Etcd

이 package는 **Redis with etcd** plugin의 etcd Location Store를 제공한다. Relocation Store에는 Redis package를 함께 설치하고 별도로 등록한다. 설치와 두 등록 호출은 [Store plugin 설정](../../../../doc/framework/dotnet/guide/server/25-location.ko.md#store-setup)을 따른다.

## Client capability 결과

| 항목 | 결과 |
|---|---|
| `keys_only=true`, revision 고정, mTLS·인증 | 2026-11-27 ws2의 Docker etcd 3.6.16에서 `EtcdIntegrationTests.Authenticated_mutual_tls_preserves_revision_and_keys_only_omits_values`가 Release net8.0·net10.0에서 각각 1건 통과했다. |
| `keys_only` 요청 필드 | fake gRPC 시험: Debug net8.0·Release net10.0 통과(2026-11-27 로컬). |

## etcd client

2026-11-27 기준 저장소 선언과 잠금 상태다.

| 항목 | 현재 상태 | 근거 |
|---|---|---|
| Client | 공식 etcd v3.6.16 proto에서 생성한 stub과 `Grpc.Net.Client`를 사용한다. | [Zlink.Framework.Locations.Etcd.csproj:7](Zlink.Framework.Locations.Etcd.csproj) |
| Version | 중앙 선언은 `Grpc.Net.Client`·`Grpc.Core.Api`·`Grpc.Tools` 2.62.0과 `Google.Protobuf` 3.30.0이다. Provider의 직접 package 참조는 `Grpc.Net.Client`, `Google.Protobuf`, build 전용 `Grpc.Tools`다. | [Directory.Packages.props:25](../../Directory.Packages.props) |
| Lockfile | Provider의 단일 net8.0과 net8.0·net10.0 대상 조합별로 직접 및 전이 의존성과 content hash를 잠근다. Provider는 중앙 전이 의존성 pinning을 끈다. | [packages.lock.json](packages.lock.json), [packages.net8.lock.json](packages.net8.lock.json) |

Proto와 import의 원본 license는 [THIRD_PARTY_NOTICES.md:40](../../../../../THIRD_PARTY_NOTICES.md)에 기록되어 있다.

[Client 의존성 license 목록](CLIENT_DEPENDENCIES.md)은 복원된 NuGet package metadata를 lockfile의 전체 resolved 그래프와 대조한 결과다.

[Client capability 결과](../../../../../scripts/etcd/README.ko.md#client-capability-결과)에서 시험 이름·명령·CI 원본과 미검증 범위를 확인한다.
