# Java Location Store 문서 위치

<!-- framework-adapter-nav:start -->
[Java 계약 목차](README.ko.md) | [언어별 interface 목차](../README.ko.md) | [이전: Java handler interface](02-handler-interfaces.ko.md)
<!-- framework-adapter-nav:end -->
Java Location Store와 maintenance public signature는
[Location과 maintenance](interfaces/location-maintenance.ko.md)에서 제공한다. 공통 동작은
[Location runtime](../../05-location-relocation/01-location-runtime.ko.md)과 [Redis Location Store](../../05-location-relocation/02-location-store-redis.ko.md)를
따른다. Location provider는 Framework의 opaque record를 version 조건부 atomic batch로 저장하며,
Relocation Store는 Framework-issued reference의 immutable blob을 별도로 저장한다.
Actor·Spot relocation의 state handoff payload는 이 Store에 저장하지 않고 source에서 target으로 직접
chunk 전송하며, Relocation Store는 Instance Spot cold activation 기록과 relocation 뒤 완료되는
pending request의 terminal 기록을 소유한다.

---
<!-- framework-adapter-nav:bottom:start -->
[Java 계약 목차](README.ko.md) | [언어별 interface 목차](../README.ko.md) | [이전: Java handler interface](02-handler-interfaces.ko.md)
<!-- framework-adapter-nav:bottom:end -->
