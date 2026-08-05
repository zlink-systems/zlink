# .NET exact public interface

[.NET 계약 목차](../README.ko.md)

이 디렉토리는 ZLink Framework server의 exact C# public contract만 기능별로 소유한다. 공통
Framework spec이 동작을 정하고 다음 문서가 type, member, generic constraint, nullable annotation과 기본값을
고정한다. Application이 호출하는 API와 외부 provider가 구현하는 SPI는 포함하지만 Framework 내부에서만
사용하는 type, phase API, wire command와 구현 절차는 포함하지 않는다. Provider SPI는 문서 제목과 본문에서
application API와 구분한다. Framework 내부 coordinator, event publisher, native monitor 값, state machine과
recovery helper는 public contract가 아니므로 선언하지 않는다. Public event는 application이 구현하는 handler와
handler가 받는 provider-neutral payload까지만 포함한다.

| 문서 | 소유하는 계약 |
|---|---|
| [Common runtime](01-common-runtime.ko.md) | Metadata, call, async result와 공통 option의 public type을 정의한다. |
| [Configuration과 host](02-configuration-host.ko.md) | Package, ASP.NET Core host, DI와 startup interface를 정의한다. |
| [Topology configuration](03-configuration-topology.ko.md) | RouteMesh, ClientServer와 fanout builder 및 runtime option을 정의한다. |
| [Channel messaging](04-channel-messaging.ko.md) | Node direct, ChannelName과 Logical Multicast의 call과 handler를 정의한다. |
| [Spots](05-spots.ko.md) | Entry·User·Instance Spot lifecycle, relocation adapter, [Spot](../../../../01-glossary.ko.md#spot) 전용 fluent call, [User Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) manager와 timer를 정의한다. |
| [Actors](06-actors.ko.md) | Actor factory, context, client, manager, relocation adapter와 policy를 정의한다. |
| [Bound STREAM session](07-bound-stream-session.ko.md) | Actor가 소유한 bound session call을 정의한다. |
| [STREAM session](07-stream-session.ko.md) | STREAM server session과 handler interface를 정의한다. |
| [Location 설정과 운영](08-location-maintenance.ko.md) | Application용 Location option, readiness와 운영 query를 정의한다. |
| [Location·Relocation provider](08-authority-relocation.ko.md) | Generic atomic Location Store와 immutable Relocation Store provider SPI를 정의한다. |
| [공식 Redis Store](08-location-provider-redis.ko.md) | 두 Redis Store class의 최소 constructor와 options를 정의한다. |
| [Host와 topology monitoring](10-topology-monitoring.ko.md) | Host state, termination, topology snapshot과 metric을 정의한다. |
| [Monitoring과 오류](10-monitoring-errors.ko.md) | Monitoring source와 Framework 오류를 정의한다. |
| [Codec extension](11-serialization.ko.md) | Codec 등록 API와 외부 codec provider SPI를 정의한다. |

Application-facing API는 native handle, authority version, relocation phase와 relocation reference를 노출하지
않는다. 이러한 내부 구현 계약은 이 디렉토리가 아니라 internals 문서에서 설명한다.

Public generation, revision, epoch와 sequence ordinal의 유효 범위는
`1..9223372036854775807`이다. .NET type이 `ulong`이어도 이 범위를 넓히지 않는다. 최대값에 도달하면
Framework는 wrap이나 값 재사용 없이 terminal exhaustion으로 처리한다. `0`은 값이 확정되지 않은 상태를
표현하도록 해당 계약이 명시한 경우에만 사용한다.
