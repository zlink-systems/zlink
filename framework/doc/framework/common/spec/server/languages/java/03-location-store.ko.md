# Java Location Store 문서 위치

Java Location Store와 maintenance public signature는
[Location과 maintenance](interfaces/location-maintenance.ko.md)에서 제공한다. 공통 동작은
[Location runtime](../../../21-location-runtime.ko.md)과 [Redis Location Store](../../../22-location-store-redis.ko.md)를
따른다. Location provider는 Framework의 opaque record를 version 조건부 atomic batch로 저장하며,
Relocation Store는 Framework-issued reference의 immutable blob을 별도로 저장한다.
