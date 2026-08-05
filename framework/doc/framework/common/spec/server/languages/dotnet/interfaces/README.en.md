# .NET Exact Public Interface

[.NET contract table of contents](../README.en.md)

This directory owns only the exact C# public contract of ZLink Framework
server, organized per feature. The common Framework spec decides
behavior, and the following documents fix the type, member, generic
constraint, nullable annotation, and default. It includes the API the
application calls and the SPI an external provider implements, but not
types used only inside the framework, phase APIs, wire commands, or
implementation procedure. A Provider SPI is distinguished from the
application API in the document title and body. Framework-internal
coordinators, event publishers, native monitor values, state machines,
and recovery helpers aren't the public contract, so they aren't declared.
A public event only includes the handler the application implements and
the provider-neutral payload the handler receives.

| Document | Contract owned |
|---|---|
| [Common runtime](01-common-runtime.ko.md) | Defines the public types for metadata, call, async result, and common options. |
| [Configuration and host](02-configuration-host.ko.md) | Defines the package, ASP.NET Core host, DI, and startup interface. |
| [Topology configuration](03-configuration-topology.en.md) | Defines RouteMesh, ClientServer, and fanout builder and runtime options. |
| [Channel messaging](04-channel-messaging.ko.md) | Defines the call and handler for Node direct, ChannelName, and Logical Multicast. |
| [Spots](05-spots.en.md) | Defines Entry/User/Instance Spot lifecycle, relocation adapter, the [Spot](../../../../01-glossary.en.md#spot)-dedicated fluent call, [User Spot](../../../../01-glossary.en.md#entry-user-instance-spot) manager, and timer. |
| [Actors](06-actors.ko.md) | Defines Actor factory, context, client, manager, relocation adapter, and policy. |
| [Bound STREAM session](07-bound-stream-session.ko.md) | Defines the bound session call an Actor owns. |
| [STREAM session](07-stream-session.ko.md) | Defines the STREAM server session and handler interface. |
| [Location configuration and operations](08-location-maintenance.en.md) | Defines application-facing Location options, readiness, and operational queries. |
| [Location/Relocation provider](08-authority-relocation.ko.md) | Defines the generic atomic Location Store and immutable Relocation Store provider SPI. |
| [Official Redis Store](08-location-provider-redis.ko.md) | Defines the minimal constructor and options of the two Redis Store classes. |
| [Host and topology monitoring](10-topology-monitoring.en.md) | Defines host state, termination, topology snapshot, and metrics. |
| [Monitoring and errors](10-monitoring-errors.ko.md) | Defines monitoring sources and Framework errors. |
| [Codec extension](11-serialization.ko.md) | Defines the codec registration API and the external codec provider SPI. |

The application-facing API doesn't expose native handle, authority
version, relocation phase, or relocation reference. These internal
implementation contracts are explained in the internals document, not
this directory.

The valid range of public generation, revision, epoch, and sequence
ordinal is `1..9223372036854775807`. Even though the .NET type is
`ulong`, this range isn't widened. On reaching the maximum value, the
framework treats it as terminal exhaustion, without wrap or value reuse.
`0` is only used when the relevant contract explicitly specifies it to
represent a not-yet-determined value.
