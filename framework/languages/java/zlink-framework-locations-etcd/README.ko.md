# zlink-framework-locations-etcd

이 package는 **Redis with etcd** plugin의 etcd Location Store를 제공한다. Relocation Store에는 Redis package를 함께 설치하고 별도로 등록한다. 설치와 두 등록 호출은 [Store plugin 설정](../../../doc/framework/java/guide/server/25-location.ko.md#store-setup)을 따른다. [Kotlin](../../../doc/framework/kotlin/guide/server/25-location.ko.md#store-setup)도 같은 Java package를 사용한다.

## etcd client

2026-11-15 기준 저장소 선언과 잠금 상태다.

| 항목 | 현재 상태 | 근거 |
|---|---|---|
| Client | 공식 etcd v3.6.16 proto에서 생성한 Java gRPC stub을 사용한다. | [build.gradle.kts:58](build.gradle.kts) |
| Version | `grpc-netty-shaded`·`grpc-protobuf`·`grpc-stub`과 `protoc-gen-grpc-java`는 1.70.0, `protobuf-java`와 `protoc`는 4.30.2다. Compile 전용 `javax.annotation-api`는 1.3.2, protobuf Gradle plugin은 0.9.4다. | [build.gradle.kts:47](build.gradle.kts) |
| Lockfile | 추적 중인 Gradle dependency lockfile은 없다. 직접 dependency 버전 선언은 resolved 전이 의존성 lock이 아니다. | [build.gradle.kts:47](build.gradle.kts) |

Proto와 import의 원본 license는 [THIRD_PARTY_NOTICES.md:40](../../../../THIRD_PARTY_NOTICES.md)에 기록되어 있다.

Client와 resolved 전이 의존성의 버전별 license 목록은 저장소에 없다. Proto license 목록은 이 목록을 대체하지 않는다. Lockfile과 해석 그래프에 연결한 license 목록은 남은 작업이다.

[Client capability 결과](../../../../scripts/etcd/README.ko.md#client-capability-결과)에서 시험 이름·명령·CI 원본과 미검증 범위를 확인한다.
