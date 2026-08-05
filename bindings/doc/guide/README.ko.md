---
title: "언어별 바인딩 가이드"
---

<!-- bindings-nav:start -->
[다음: .NET](dotnet/index.ko.md)
<!-- bindings-nav:end -->

# 언어별 바인딩 가이드

> **이 장의 계약 소유 문서** — 각 언어의
> [바인딩 스펙](../spec/README.ko.md)이 공개 계약을 다룬다. 이 장은 언어를
> 고르고 자기 가이드로 넘어가는 진입점이다.

zlink는 C 코어 위에 여러 언어 바인딩을 제공합니다. 각 가이드는 **그 언어에서
zlink를 쓰는 방법**(설치, 관용 예제, 타입 매핑, 언어 고유 규칙)을 다룹니다.
메시징 **개념 자체**(소켓 패턴, 트랜스포트, 서비스, 라우팅 ID)는 언어에 매이지
않도록 [코어 가이드](https://kairos-code-dev.github.io/zlink/guide/01-overview/)가 한 번만 다루며, 각 언어 가이드는
개념이 필요한 자리마다 코어로 링크합니다.

## 읽는 순서

- **이미 메시징을 안다 / 빨리 쓰고 싶다** → 자기 언어 가이드로 바로 가세요. 막히는
  개념은 그 자리에서 코어 링크로 확인하면 됩니다.
- **메시징이 처음이다** → 코어 [개요](https://kairos-code-dev.github.io/zlink/guide/01-overview/)와
  [소켓 패턴](https://kairos-code-dev.github.io/zlink/guide/03-0-socket-patterns/)을 먼저 본 뒤 언어 가이드로 오세요.

## 언어 고르기

| 언어 | 사용 가이드 | 생성 API 레퍼런스 | 패키지 |
|------|------------|------------------|--------|
| .NET | [dotnet/](dotnet/index.ko.md) | docfx | `Systems.Zlink` |
| C++ | [cpp/](cpp/index.ko.md) | doxygen | `zlink::cpp` (CMake) |
| Java | [java/](java/index.ko.md) | javadoc | `systems.zlink:zlink` |
| Node | [node/](node/index.ko.md) | typedoc | `@zlink-systems/zlink` |
| Python | [python/](python/index.ko.md) | sphinx | `zlink` (PyPI) |
| Go | [go/](go/index.ko.md) | godoc | `zlink.systems/zlink/v11` |
| Rust | [rust/](rust/index.ko.md) | rustdoc | `zlink` (crates.io) |
| Kotlin | [java/ §Kotlin](java/index.ko.md#kotlin) | (java 공유) | `systems.zlink:zlink` |
| JavaScript | [node/ §JavaScript](node/index.ko.md#javascript) | (node 공유) | `@zlink-systems/zlink` |

> Kotlin과 JavaScript는 **런타임을 공유**합니다 — Kotlin은 Java 바인딩
> (`systems.zlink.*`), JavaScript는 Node 바인딩(`@zlink-systems/zlink`)을 그대로
> 씁니다. 별도 네이티브 바인딩이 없어 각각 Java·Node 가이드의 전용 절에서 다룹니다.
> 코어 가이드의 언어 탭에는 Kotlin·JavaScript 칸이 따로 있습니다.

> C는 코어 그 자체이므로 별도 바인딩 가이드 대신
> [코어 C API 가이드](https://kairos-code-dev.github.io/zlink/guide/02-core-api/)를 봅니다.

## 가이드 구성 (모든 언어 공통)

각 언어 가이드는 **단일 `index` 문서 한 장**으로, 그 언어 표면(설치·타입·소유권·
대응표·배포)만 다룹니다. 메시징·서비스·운영 개념은 코어 가이드가 한 번만 다루며,
언어 가이드의 `더 보기` 섹션이 해당 코어 문서로 링크합니다.

| index 섹션 | 내용 |
|------|------|
| 설치 | 패키지 설치 · 네임스페이스 |
| 5분 예제 | 첫 송수신 PING/ACK |
| 핵심 타입 | Context · Message · Received · RoutingId |
| 소유권과 수명 | 리소스 해제 규칙 |
| 에러 처리 | 작업별 예외/에러 타입 |
| C API 대응표 | C API ↔ 언어 표면 매핑 |
| 네이티브 라이브러리 / 배포 | 번들·로드 경로·게시 |
| 샘플 | 샘플 프로젝트 안내 |
| 더 보기 | 소켓 패턴·서비스·운영 코어 가이드 링크 |

> 소켓 패턴별 사용법, SPOT·Actor, 옵션·TLS·모니터링·폴러/타이머·
> 스레딩 등은 이제 코어 가이드에서 언어별 예제 탭과 함께 다룹니다.
