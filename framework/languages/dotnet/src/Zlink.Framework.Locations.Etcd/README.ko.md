# Zlink.Framework.Locations.Etcd

이 package는 **Redis with etcd** plugin의 etcd Location Store를 제공한다. Relocation Store에는 Redis package를 함께 설치하고 별도로 등록한다. 설치와 두 등록 호출은 [Store plugin 설정](../../../../doc/framework/dotnet/guide/server/25-location.ko.md#store-setup)을 따른다.

## etcd client

2026-11-15 기준 저장소 선언과 잠금 상태다.

| 항목 | 현재 상태 | 근거 |
|---|---|---|
| Client | 공식 etcd v3.6.16 proto에서 생성한 stub과 `Grpc.Net.Client`를 사용한다. | [Zlink.Framework.Locations.Etcd.csproj:7](Zlink.Framework.Locations.Etcd.csproj) |
| Version | 중앙 선언은 `Grpc.Net.Client`·`Grpc.Core.Api`·`Grpc.Tools` 2.62.0과 `Google.Protobuf` 3.29.1이다. Provider의 직접 package 참조는 `Grpc.Net.Client`, `Google.Protobuf`, build 전용 `Grpc.Tools`다. | [Directory.Packages.props:25](../../Directory.Packages.props) |
| Lockfile | 추적 중인 `packages.lock.json`은 없다. 중앙 버전 선언과 package contract snapshot은 resolved 전이 의존성·content hash를 고정하는 lockfile이 아니다. Provider는 중앙 전이 의존성 pinning도 끈다. | [Zlink.Framework.Locations.Etcd.csproj:6](Zlink.Framework.Locations.Etcd.csproj) |

Proto와 import의 원본 license는 [THIRD_PARTY_NOTICES.md:40](../../../../../THIRD_PARTY_NOTICES.md)에 기록되어 있다.

Client와 resolved 전이 의존성의 버전별 license 목록은 저장소에 없다. Proto license 목록은 이 목록을 대체하지 않는다. Lockfile과 해석 그래프에 연결한 license 목록은 남은 작업이다.

[Client capability 결과](../../../../../scripts/etcd/README.ko.md#client-capability-결과)에서 시험 이름·명령·CI 원본과 미검증 범위를 확인한다.
