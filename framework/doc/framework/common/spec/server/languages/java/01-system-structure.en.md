# Java System Structure Document Location

<!-- framework-adapter-nav:start -->
[Java contract table of contents](README.en.md) | [Language interface table of contents](../README.en.md) | [Next: Java handler interface](02-handler-interfaces.en.md)
<!-- framework-adapter-nav:end -->
Java server public signatures are provided per feature in the
[per-language interface table of contents](interfaces/README.en.md).

- [Common Runtime](interfaces/common-runtime.en.md)
- [Configuration And Host](interfaces/configuration-host.en.md)
- [Channel Messaging](interfaces/channel-messaging.en.md)
- [Monitoring](interfaces/monitoring.en.md)

Object relocation is requested with `Relocate`, and host shutdown with
`Shutdown`. A separate host drain or MeshNode-scoped drain public
interface isn't provided. The result and monitoring contract
follow [Common Runtime](interfaces/common-runtime.en.md) and
[Monitoring](interfaces/monitoring.en.md).

Common behavior follows the
[Framework Common Spec](../../README.en.md).

---
<!-- framework-adapter-nav:bottom:start -->
[Java contract table of contents](README.en.md) | [Language interface table of contents](../README.en.md) | [Next: Java handler interface](02-handler-interfaces.en.md)
<!-- framework-adapter-nav:bottom:end -->
