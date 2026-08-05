# Java Location Store Document Location

The Java Location Store and maintenance public signature is provided in
[Location And Maintenance](interfaces/location-maintenance.ko.md).
Common behavior follows
[Location Runtime](../../../21-location-runtime.en.md) and
[Redis Location Store](../../../22-location-store-redis.en.md). The
Location provider stores the Framework's opaque record as a
version-conditional atomic batch, and the Relocation Store separately
stores an immutable blob at a Framework-issued reference.
