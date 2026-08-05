# Kotlin Location Store Document Location

Kotlin Location Store and maintenance extensions and generated JVM
signatures are provided in
[Location And Maintenance](interfaces/location-maintenance.en.md). The
Java public types follow
[Java Location And Maintenance](../java/interfaces/location-maintenance.en.md).
Kotlin doesn't add a per-domain Store — it implements the Java
`ZLinkLocationStore`'s opaque key/value atomic batch contract and the
`ZLinkRelocationStore`'s immutable blob contract based on a
Framework-issued reference, unchanged.
