# Kotlin Location Store Document Location

<!-- framework-adapter-nav:start -->
[Kotlin contract table of contents](README.en.md) | [Language interface table of contents](../README.en.md) | [Previous: Kotlin handler interface](02-handler-interfaces.en.md)
<!-- framework-adapter-nav:end -->
Kotlin Location Store and maintenance extensions and generated JVM
signatures are provided in
[Location And Maintenance](interfaces/location-maintenance.en.md). The
Java public types follow
[Java Location And Maintenance](../java/interfaces/location-maintenance.en.md).
Kotlin doesn't add a per-domain Store — it implements the Java
`ZLinkLocationStore`'s opaque key/value atomic batch contract and the
`ZLinkRelocationStore`'s immutable blob contract based on a
Framework-issued reference, unchanged. The state handoff payload of an
Actor/Spot relocation isn't stored in this Store — it is transferred as
chunks directly from source to target — and the Relocation Store owns
the Instance Spot cold activation record and the terminal record of a
pending request completed after relocation.

---
<!-- framework-adapter-nav:bottom:start -->
[Kotlin contract table of contents](README.en.md) | [Language interface table of contents](../README.en.md) | [Previous: Kotlin handler interface](02-handler-interfaces.en.md)
<!-- framework-adapter-nav:bottom:end -->
