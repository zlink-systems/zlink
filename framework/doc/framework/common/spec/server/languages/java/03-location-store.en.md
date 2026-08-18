# Java Location Store Document Location

The Java Location Store and maintenance public signature is provided in
[Location And Maintenance](interfaces/location-maintenance.en.md).
Common behavior follows
[Location Runtime](../../21-location-runtime.en.md) and
[Redis Location Store](../../22-location-store-redis.en.md). The
Location provider stores the Framework's opaque record as a
version-conditional atomic batch, and the Relocation Store separately
stores an immutable blob at a Framework-issued reference. The state
handoff payload of an Actor/Spot relocation isn't stored in this Store —
it is transferred as chunks directly from source to target — and the
Relocation Store owns the Instance Spot cold activation record and the
terminal record of a pending request completed after relocation.
