# Node.js Public Interface Table Of Contents

[Node.js contract table of contents](../README.en.md) · [Common Spec](../../../../README.en.md)

This directory fixes, per category, the exact public TypeScript
declarations the ZLink Framework's `@zlink-systems/framework`,
`@zlink-systems/nestjs`, and `@zlink-systems/framework-locations-redis`
package roots export. The same declaration isn't repeated across
multiple documents. Feature meaning and state transitions are owned by
the common spec, and this directory owns package export and signature
parity. The public contract of the HTTP client and Stream Connector is
owned by each package's separate spec.

| No. | Document | Scope |
|---:|---|---|
| 01 | [Foundation Types And Configuration](01-foundation-configuration.en.md) | Global ID/ref, object role/capacity, Actor/Spot relocation adapter and explicit policy |
| 02 | [Channel, Request, And Routing](02-channel-messaging.en.md) | Entry Spot actor messaging, Channel/Fanout/RouteMesh calls and handlers |
| 03 | [Location, Host Lifecycle, And Observability](03-location-observability.en.md) | Operational queries, relocation mode/target version, runtime events, metrics, and tracing |
| 04 | [Spot And Instance Spot](04-spots.en.md) | [Spot](../../../../01-glossary.en.md#spot) lifecycle, [User Spot](../../../../01-glossary.en.md#entry-user-instance-spot) manager, and the Instance cold-activation fluent call |
| 05 | [Actor And Session Binding](05-actors.en.md) | Actor lifecycle, Actor call, and bound session |
| 06 | [STREAM, Timer, And Worker](06-stream-worker.en.md) | STREAM session, timer, and worker scheduling |
| 07 | [NestJS Host Adapter](07-nestjs-host.en.md) | Module, DI token, decorator, and host builder |
| 08 | [Location/Relocation Provider](08-location-maintenance.en.md) | Opaque atomic Location Store, immutable Relocation Store, and the official Redis provider |

The export name set of the deployment package and every file this table
of contents points to must be the same in both directions. The binding
package's service type, backend adapter, and runtime internal subpath
aren't included in this export set. The Framework runtime internally
uses the binding's public raw socket API. Verification scenarios are
owned by the Node.js regression verification matrix. This directory only
defines the exact public declaration and doesn't keep a progress table.

The valid range of public generation, revision, epoch, and sequence
ordinal is `1n..9223372036854775807n`. Even though the TypeScript type
is `bigint`, this range isn't widened. On reaching the maximum value,
the framework treats it as terminal exhaustion, without wrap or value
reuse. `0n` is only used when the relevant contract explicitly specifies
it to represent a not-yet-determined value.
