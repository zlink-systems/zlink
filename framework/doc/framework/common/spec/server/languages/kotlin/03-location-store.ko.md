# Kotlin Location Store 문서 위치

<!-- framework-adapter-nav:start -->
[Kotlin 계약 목차](README.ko.md) | [언어별 interface 목차](../README.ko.md) | [이전: Kotlin handler interface](02-handler-interfaces.ko.md)
<!-- framework-adapter-nav:end -->
Kotlin Location Store와 maintenance extension, generated JVM signature는
[Location과 maintenance](interfaces/location-maintenance.ko.md)에서 제공한다. Java public type은
[Java Location과 maintenance](../java/interfaces/location-maintenance.ko.md)를 따른다. Kotlin은
domain별 Store를 추가하지 않고 Java `ZLinkLocationStore`의 opaque key·value atomic batch와
`ZLinkRelocationStore`의 Framework-issued reference 기반 immutable blob 계약을 그대로 구현한다.
Actor·Spot relocation의 state handoff payload는 이 Store에 저장하지 않고 source에서 target으로 직접
chunk 전송하며, Relocation Store는 Instance Spot cold activation 기록과 relocation 뒤 완료되는
pending request의 terminal 기록을 소유한다.

---
<!-- framework-adapter-nav:bottom:start -->
[Kotlin 계약 목차](README.ko.md) | [언어별 interface 목차](../README.ko.md) | [이전: Kotlin handler interface](02-handler-interfaces.ko.md)
<!-- framework-adapter-nav:bottom:end -->
