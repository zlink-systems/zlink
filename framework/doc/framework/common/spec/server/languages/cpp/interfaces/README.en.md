# C++ Exact Public Interface

[C++ contract table of contents](../README.en.md)

This directory owns the ZLink Framework server's exact C++ public
interface per capability. The common Framework spec fixes behavior,
and the following documents fix namespace, type, member, template
constraint, and default value.

| Document | Owned contract and installed public header |
|---|---|
| [Common runtime](01-common-runtime.en.md) | Defines the common public type of `dispatch`, `errors`, `messaging`, `codecs`, and `workers`. |
| [Configuration and host](02-configuration-host.en.md) | Defines `configuration`, `http`, host, DI, module, and the relocation and shutdown lifecycle public interface that distinguishes planned maintenance/rolling update. |
| [Channel messaging](03-channel-messaging.en.md) | Defines `channels` and `handlers`, topology builder, Object role/capacity/weight, and the automatic RID contract. |
| [Spots](04-spots.en.md) | Defines global SpotId/SpotRef, the relocation adapter and callback, Instance Spot cold activation, and the [User Spot](../../../../01-glossary.en.md#entry-spot-user-spot-and-instance-spot) manager. |
| [Actors](05-actors.en.md) | Defines global ActorId/ActorRef, the relocation adapter, ID-only messaging, manager create, and exact mutation/bind. |
| [STREAM session](06-stream-session.en.md) | Defines the interworking interface between `streams`'s packet session and the bound session an Actor owns. |
| [Location · Relocation Store · Redis](07-location-store.en.md) | Defines the opaque atomic Location Store, the immutable Relocation Store, operational query, and the official Redis provider. |
| [Monitoring](08-monitoring.en.md) | Defines the runtime status/snapshot/health and structured logging boundary an application uses. |

`zlink/framework.hpp` is a facade that gathers the installed headers
above. The application-facing API doesn't expose Core service handle,
claim, receive batch, reply token, service liveness command, and
[authority](../../../../01-glossary.en.md#authority)/relocation
internal transaction. Framework runtime only uses the installed C++
binding's public raw socket API.

The valid range of public generation, revision, epoch, and sequence
ordinal is `1..9223372036854775807`. Even though the C++ type is
`std::uint64_t`, this range isn't widened. On reaching the maximum
value, Framework treats it as terminal exhaustion without wraparound
or value reuse. `0` is used only when the corresponding contract
specifies it to express an undetermined value.

## Public Surface

The Channel, [Spot](../../../../01-glossary.en.md#spot), Actor, STREAM,
handler, builder, host, DI, maintenance, and state relocation members
declared in this document set are the C++ 11.0 public contract. Core
service handle, dispatch record, and service liveness interval/deadline
aren't included in this contract.
