# Java system structure 문서 위치

<!-- framework-adapter-nav:start -->
[Java 계약 목차](README.ko.md) | [언어별 interface 목차](../README.ko.md) | [다음: Java handler interface](02-handler-interfaces.ko.md)
<!-- framework-adapter-nav:end -->
Java server public signature는 [언어별 interface 목차](interfaces/README.ko.md)에서 기능별로 제공한다.

- [공통 runtime](interfaces/common-runtime.ko.md)
- [구성과 host](interfaces/configuration-host.ko.md)
- [Channel messaging](interfaces/channel-messaging.ko.md)
- [Monitoring](interfaces/monitoring.ko.md)

Object relocation은 `Relocate`, host 종료는 `Shutdown`으로 요청한다. 별도 host drain이나 MeshNode scoped drain public
interface는 제공하지 않는다. 정확한 결과와 monitoring 계약은 [공통 runtime](interfaces/common-runtime.ko.md)과
[Monitoring](interfaces/monitoring.ko.md)을 따른다.

공통 동작은 [Framework 공통 spec](../../README.ko.md)을 따른다.

---
<!-- framework-adapter-nav:bottom:start -->
[Java 계약 목차](README.ko.md) | [언어별 interface 목차](../README.ko.md) | [다음: Java handler interface](02-handler-interfaces.ko.md)
<!-- framework-adapter-nav:bottom:end -->
