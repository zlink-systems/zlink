# ZLink Framework C++ 공개 계약

이 디렉토리는 C++ framework가 제공해야 하는 **정식 public contract**를 소유한다. public header와
contract test는 이 계약을 따라야 한다.

C++ Framework는 별도 application framework에 host 기능을 위임하지 않는다.
ASP.NET Core를 사용하는 .NET, NestJS를 사용하는 Node.js, Spring Boot를 사용하는
Java와 달리 host, DI, configuration, logging과 HTTP 기능을 직접 제공한다. 따라서
C++의 HTTP 공개 계약은 이 디렉토리에서 별도 문서로 정의한다.

| 번호 | 문서 | 범위 |
|---|------|------|
| `01` | [시스템 구조](01-system-structure.ko.md) | 패키지·빌드 타깃, application host, **DI 컨테이너**, **configuration**, **logging**, lifecycle, 등록 표면 |
| `02` | [기능별 exact interface](interfaces/README.ko.md) | Server package의 기능별 C++ public type과 member |
| `03` | [Location·Relocation Store·Redis 이동 안내](03-location-store.ko.md) | 기능별 exact interface의 Store·Redis 문서로 연결 |
| `60` | [HTTP hosting](60-http-hosting.ko.md) | HTTP 호스팅 계약 |
| `61` | [내장 HTTP 서버](61-embedded-http-server.ko.md) | 내장 서버 |

**기능의 의미와 동작 규칙은 [공통 스펙](../../../README.ko.md)이 소유한다.** 이 디렉토리는 그 의미가
C++에서 갖는 **정확한 public 표면**을 고정한다.

**내부 runtime 구조는 공개 계약이 아니다** —
[internals/runtime-architecture](../../../../internals/README.ko.md)가 소유한다.

client connector는 [C++ Stream Connector 가이드](../../../../../cpp/guide/stream-connector/INDEX.ko.md)와
[Stream Connector 공통 스펙](../../../stream-connector/32-stream-connector.ko.md)이 소유한다.

## 취소 인자

C++ public interface에는 **`.NET` 모양을 옮긴 custom cancellation token을 기본 callback 인자로
두지 않는다.** 중단 가능한 장기 작업에 명시적 중단 전달이 필요하면 **C++ 표준 수명과 중단
관례**를 사용한다. timeout, host shutdown, RAII cleanup과 coroutine 수명은 각 기능 계약을 따른다.
