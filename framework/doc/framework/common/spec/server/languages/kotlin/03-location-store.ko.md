# Kotlin Location Store 문서 위치

Kotlin Location Store와 maintenance extension, generated JVM signature는
[Location과 maintenance](interfaces/location-maintenance.ko.md)에서 제공한다. Java public type은
[Java Location과 maintenance](../java/interfaces/location-maintenance.ko.md)를 따른다. Kotlin은
domain별 Store를 추가하지 않고 Java `ZLinkLocationStore`의 opaque key·value atomic batch와
`ZLinkRelocationStore`의 Framework-issued reference 기반 immutable blob 계약을 그대로 구현한다.
