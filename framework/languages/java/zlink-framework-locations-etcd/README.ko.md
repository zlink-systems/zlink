# zlink-framework-locations-etcd

이 package는 **Redis with etcd** plugin의 etcd Location Store를 제공한다. Relocation Store에는 Redis package를 함께 설치하고 별도로 등록한다. 설치와 두 등록 호출은 [Store plugin 설정](../../../doc/framework/java/guide/server/25-location.ko.md#store-setup)을 따른다. [Kotlin](../../../doc/framework/kotlin/guide/server/25-location.ko.md#store-setup)도 같은 Java package를 사용한다.

## etcd client

2026-11-20 기준 저장소 선언과 잠금 상태다.

| 항목 | 현재 상태 | 근거 |
|---|---|---|
| Client | 공식 etcd v3.6.16 proto에서 생성한 Java gRPC stub을 사용한다. | [build.gradle.kts:58](build.gradle.kts) |
| Version | `grpc-netty-shaded`·`grpc-protobuf`·`grpc-stub`과 `protoc-gen-grpc-java`는 1.70.0, `protobuf-java`와 `protoc`는 4.30.2다. Compile 전용 `javax.annotation-api`는 1.3.2, protobuf Gradle plugin은 0.9.4다. | [build.gradle.kts:47](build.gradle.kts) |
| Lockfile | 이 module의 해석 가능한 configuration은 Gradle dependency locking을 사용한다. | [gradle.lockfile](gradle.lockfile) |

Proto와 import의 원본 license는 [THIRD_PARTY_NOTICES.md:40](../../../../THIRD_PARTY_NOTICES.md)에 기록되어 있다.

Client의 resolved compile/runtime 의존성, 코드 생성 도구와 shaded native 구성 요소의 license는 [의존성 목록](CLIENT_DEPENDENCIES.md)에 기록한다.

[Client capability 결과](../../../../scripts/etcd/README.ko.md#client-capability-결과)에서 시험 이름·명령·CI 원본과 미검증 범위를 확인한다.

## Client capability 시험

`ZLinkEtcdTransportLimitTest`는 직렬화된 인증 요청의 16 MiB−1·16 MiB·16 MiB+1과 응답의 8 MiB−1·8 MiB·8 MiB+1을 검사한다. 한도 초과는 `IOException` 기반 Store failure이며 초과 요청은 서버에 전달되지 않는다. 응답 padding은 unknown field에 넣어 인증 token의 크기와 구분한다.

Gradle 명령은 `framework/languages/java`에서 실행한다.

```sh
flock -o ~/heavy.lock ./gradlew --no-daemon --max-workers=4 :zlink-framework-locations-etcd:test :zlink-framework-locations-etcd:contractTest
```

`ZLinkEtcdClientLiveTest`는 별도로 실행한 mTLS·비밀번호 인증 etcd에 연결한다. 고유 key를 쓰고 갱신한 뒤 이전 revision의 Range에서 `keys_only=false`는 원래 payload를 반환하고 `true`는 value만 생략하는지 검사한다. 정상 보안 연결과 잘못된 비밀번호·CA·hostname·누락된 client 인증서의 Store failure를 함께 검사한다. Provider 재작성이나 Store clock process는 필요하지 않다. Host clock VM 검증은 이 시험의 대상이 아니다.

다음 환경 값을 설정한다. 시험 전용 etcd에 root 권한의 인증 사용자를 준비하고 client 인증서를 요구하도록 설정한다. 서버 인증서에는 DNS `localhost`를 넣고 IP SAN은 넣지 않는다.

| 환경 변수 | 값 |
|---|---|
| `ZLINK_ETCD_CLIENT_ENDPOINT` | `https://localhost:PORT` |
| `ZLINK_ETCD_CLIENT_USER`, `ZLINK_ETCD_CLIENT_PASSWORD` | 시험 사용자와 비밀번호 |
| `ZLINK_ETCD_CLIENT_CA` | CA PEM 파일 경로 |
| `ZLINK_ETCD_CLIENT_CERT`, `ZLINK_ETCD_CLIENT_KEY` | client 인증서와 개인 key PEM 파일 경로 |

```sh
flock -o ~/heavy.lock ./gradlew --no-daemon --max-workers=4 :zlink-framework-locations-etcd:integrationTest --tests '*ZLinkEtcdClientLiveTest'
```

환경 값이 없으면 live 시험은 skip된다. Fake server의 `ZLinkEtcdTlsTest` 결과와 실제 etcd 실행 결과는 구분해야 한다.
