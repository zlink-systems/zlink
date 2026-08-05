# Node.js 공개 인터페이스 목차

[Node.js 계약 목차](../README.ko.md) · [공통 스펙](../../../../README.ko.md)

이 디렉터리는 ZLink Framework의 `@zlink-systems/framework`,
`@zlink-systems/nestjs`, `@zlink-systems/framework-locations-redis` package root가 내보내는 정확한
public TypeScript declaration을 범주별로 고정한다. 같은 declaration을 여러 문서에 반복하지 않는다.
기능 의미와 상태 전이는 공통 스펙이 소유하고, 이 디렉터리는 package export와 signature parity를
소유한다. HTTP client와 Stream Connector의 공개 계약은 각 package의 별도 spec이 소유한다.

| 번호 | 문서 | 범위 |
|---:|---|---|
| 01 | [기초 타입과 구성](01-foundation-configuration.ko.md) | global ID·ref, object role·capacity, Actor·Spot relocation adapter와 explicit policy |
| 02 | [Channel, request와 routing](02-channel-messaging.ko.md) | Entry Spot actor messaging, Channel·Fanout·RouteMesh 호출과 handler |
| 03 | [Location, host lifecycle과 observability](03-location-observability.ko.md) | 운영 조회, relocation mode·target version, runtime event, metric과 tracing |
| 04 | [Spot과 Instance Spot](04-spots.ko.md) | [Spot](../../../../01-glossary.ko.md#spot) lifecycle, [User Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) manager와 Instance cold activation fluent call |
| 05 | [Actor와 session binding](05-actors.ko.md) | Actor lifecycle, Actor call과 bound session |
| 06 | [STREAM, timer와 worker](06-stream-worker.ko.md) | STREAM session, timer와 worker scheduling |
| 07 | [NestJS host adapter](07-nestjs-host.ko.md) | module, DI token, decorator와 host builder |
| 08 | [Location·Relocation provider](08-location-maintenance.ko.md) | opaque atomic Location Store, immutable Relocation Store와 공식 Redis provider |

배포 package와 이 목차가 가리키는 모든 파일의 export 이름 집합은 양방향으로 같아야 한다.
Binding package의 service type, backend adapter와 runtime internal subpath는 이 export 집합에 포함하지
않는다. Framework runtime은 binding의 public raw socket API를 내부에서 사용한다.
검증 scenario는 Node.js 회귀 검증 matrix가 소유한다. 이 디렉터리는 정확한 public declaration만
정의하며 진행표를 두지 않는다.

Public generation, revision, epoch와 sequence ordinal의 유효 범위는
`1n..9223372036854775807n`이다. TypeScript type이 `bigint`여도 이 범위를 넓히지 않는다. 최대값에
도달하면 Framework는 wrap이나 값 재사용 없이 terminal exhaustion으로 처리한다. `0n`은 값이 확정되지
않은 상태를 표현하도록 해당 계약이 명시한 경우에만 사용한다.
