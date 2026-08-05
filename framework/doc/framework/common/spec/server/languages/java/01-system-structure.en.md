# Java System Structure Document Location

Java server public signatures are provided per feature in the
[exact interface table of contents](interfaces/README.ko.md).

- [Common Runtime](interfaces/common-runtime.en.md)
- [Configuration And Host](interfaces/configuration-host.ko.md)
- [Channel Messaging](interfaces/channel-messaging.ko.md)
- [Monitoring](interfaces/monitoring.en.md)

Object relocation is requested with `Relocate`, and host shutdown with
`Shutdown`. A separate host drain or MeshNode-scoped drain public
interface isn't provided. The exact result and monitoring contract
follow [Common Runtime](interfaces/common-runtime.en.md) and
[Monitoring](interfaces/monitoring.en.md).

Common behavior follows the
[Framework Common Spec](../../../README.en.md).
