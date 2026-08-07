# Framework 공통 스펙

이 디렉터리의 문서는 Framework의 공통 공개 계약을 설명한다. 각 문서는 구현과
contract test에 필요한 입력, 상태, 정상 흐름, 실패와 완료 조건을 자체적으로
정의한다.

## 작성 기준과 공통 용어

- [스펙 문서 작성 가이드](../../../../../doc/principal/documentation/spec-writing-guide.ko.md)
- [00 공개 계약 관리](00-public-contract-governance.ko.md)
- [01 Framework 메시징 용어집](01-glossary.ko.md)

## 기반 계약

- [02 Framework 개요](02-overview.ko.md)
- [03 상호작용 모델](03-interaction-model.ko.md)
- [04 메시지 모델](04-message-model.ko.md)
- [05 비동기 실행 정책](05-async-execution-policy.ko.md)
- [06 Framework API](06-framework-api.ko.md)

## Channel과 network

- [07 RouteMesh topology](07-channel-topology.ko.md)
- [08 Channel 메시징](08-channel-messaging.ko.md)
- [09 ClientServer Channel](09-client-server-channel.ko.md)
- [10 Network listener identity](10-network-listener-identity.ko.md)

## Object 메시징

- [11 Spot 모델](11-spot-model.ko.md)
- [12 Spot 메시징](12-spot-messaging.ko.md)
- [13 MeshNode](13-mesh-node.ko.md)
- [14 Actor 모델](14-actor-model.ko.md)
- [15 Spot과 Actor membership](15-spot-actor.ko.md)
- [16 Spot 주소 메시징](16-spot-address-messaging.ko.md)
- [17 Stage wrapper on Spot](17-stage-wrapper-on-spot.ko.md)
- [18 Spot·Actor routing](18-object-routing.ko.md)

## STREAM과 session

- [19 STREAM 서버 session](19-stream-session.ko.md)
- [20 Session Actor dispatch](20-session-actor-dispatch.ko.md)

## Location Store와 relocation

- [21 Location runtime](21-location-runtime.ko.md) — Framework가 object 위치, authority와 두 Store를 사용하는 순서를 정의한다.
- [22 Location Store provider SPI와 공식 Redis 구현](22-location-store-redis.ko.md) — Provider가 구현할 atomic key/value와 scan 계약을 정의한다.
- [23 Relocation Store provider SPI와 공식 Redis 구현](23-relocation-store-redis.ko.md) — Provider가 구현할 immutable payload 저장 계약을 정의한다.

## 관측과 종료

- [24 Runtime 상태와 운영 진단](24-runtime-monitoring.ko.md) — Application이 읽는 health, topology status와 structured log를 정의한다.
- [25 Runtime metric 이름과 label](25-runtime-metrics.ko.md) — Metric 이름, 단위와 bounded label만 정의한다.
- [26 Message flow tracing](26-message-flow-tracing.ko.md) — Message 한 건의 phase, outcome과 trace attribute를 정의한다.
- [27 Request correlation과 causal flow](27-flow-correlation.ko.md) — Correlation ID와 flow ID의 생성·전파를 정의한다.
- [28 Host Relocate와 Shutdown](28-graceful-drain-handoff.ko.md) — 두 relocation mode와 종료 lifecycle을 정의한다.
- [29 Transport liveness](29-transport-liveness.ko.md)
- [31 장애 대응과 failover 범위](31-failure-failover-policy.ko.md) — target 재선택, reconnect, 생성 recovery와 stateful relocation의 자동 복구 경계를 정의한다.
- [32 Framework 오류 모델](32-framework-error-model.ko.md) — 공통 `ErrorKind`, Send·Request 완료 조건과 Application의 재시도 판단 경계를 정의한다.

## Server 언어별 exact interface

공통 server 계약이 각 언어에서 사용하는 정확한 public type, signature와 비동기
표현은 다음 문서가 소유한다.

- [C++](server/languages/cpp/README.ko.md)
- [.NET](server/languages/dotnet/README.ko.md)
- [Java](server/languages/java/README.ko.md)
- [Kotlin](server/languages/kotlin/README.ko.md)
- [Node.js](server/languages/node/README.ko.md)

## HTTP client

- [HTTP client 스펙 목차](http-client/README.ko.md)
- [12 HTTP client 통합 계약](http-client/12-http-client.ko.md)
- [언어별 HTTP client 계약](http-client/language-interfaces.ko.md)

`10-revision-candidates.ko.md`는 공개 계약이 아니라 다음 revision의 설계 후보를
관리하는 문서다.

## Stream connector

- [32 Stream connector](stream-connector/32-stream-connector.ko.md)
- [언어별 Stream connector 계약](stream-connector/README.ko.md#언어별-public-api)
