# Store plugin 선택

Store plugin은 다음 두 구성 중 하나로 선택한다. 두 Store는 각각 등록한다.

| Plugin | Location Store | Relocation Store | 사용 조건 |
| --- | --- | --- | --- |
| Redis | Redis | Redis (같은 server) | [Redis 내구성 조건](../../../common/spec/server/05-location-relocation/02-location-store-redis.ko.md#1-범위와-독자)을 충족하는 배포. |
| Redis with etcd | etcd (3 voters) | Redis | 자동 장애 전환이 필요한 배포. |

Redis 운영 조건은 [Location Store Redis](../../../common/spec/server/05-location-relocation/02-location-store-redis.ko.md)가 정한다. etcd 구성과 복구 조건은 [Location Store etcd](../../../common/spec/server/05-location-relocation/07-location-store-etcd.ko.md)가 정한다.

언어별 설치와 등록:

- [C#/.NET](../../../dotnet/guide/server/25-location.ko.md#store-setup)
- [C++](../../../cpp/guide/server/25-location.ko.md#store-setup)
- [Java](../../../java/guide/server/25-location.ko.md#store-setup)
- [Kotlin](../../../kotlin/guide/server/25-location.ko.md#store-setup)
- [Node/TypeScript](../../../node/guide/server/25-location.ko.md#store-setup)
