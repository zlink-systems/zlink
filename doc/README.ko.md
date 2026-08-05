[English](README.md) | [한국어](README.ko.md)

# zlink 문서

> zlink 프로젝트 문서 네비게이션

---

## 독자별 경로

| 독자 | 시작 문서 | 설명 |
|------|-----------|------|
| **라이브러리 사용자** | [guide/01-overview.ko.md](../core/doc/guide/01-overview.ko.md) | zlink API로 메시징 애플리케이션 개발 |
| **바인딩 사용자** | [guide/bindings/README.ko.md](../bindings/doc/guide/README.ko.md) | C++/Java/.NET/Node.js/Python 바인딩 |
| **라이브러리 개발자** | [internals/architecture.ko.md](../core/doc/internals/architecture.ko.md) | 내부 아키텍처 및 구현 상세 |
| **빌드/배포 담당자** | [building/build-guide.ko.md](building/build-guide.ko.md) | 빌드, 테스트, 패키징 |
| **채택 검토자 / 법무 담당자** | [license/README.ko.md](license/README.ko.md) | `core`/`bindings`/`framework`/`http-client` 전반의 라이선스 정책 |

---

## 사용자 가이드 (guide/)

### Core
| 문서 | 설명 |
|------|------|
| [01-overview.ko.md](../core/doc/guide/01-overview.ko.md) | zlink 개요 및 시작하기 |
| [02-core-api.ko.md](../core/doc/guide/02-core-api.ko.md) | Core C API 상세 가이드 |
| [03-0-socket-patterns.ko.md](../core/doc/guide/03-0-socket-patterns.ko.md) | 소켓 패턴 개요 및 선택 가이드 |
| [03-1-pair.ko.md](../core/doc/guide/03-1-pair.ko.md) | PAIR 소켓 (1:1 양방향) |
| [03-2-pubsub.ko.md](../core/doc/guide/03-2-pubsub.ko.md) | PUB/SUB/XPUB/XSUB 발행-구독 |
| [03-3-dealer.ko.md](../core/doc/guide/03-3-dealer.ko.md) | DEALER 소켓 (비동기 요청) |
| [03-4-router.ko.md](../core/doc/guide/03-4-router.ko.md) | ROUTER 소켓 (ID 기반 라우팅) |
| [03-5-stream.ko.md](../core/doc/guide/03-5-stream.ko.md) | STREAM 소켓 (RAW 통신) |
| [04-transports.ko.md](../core/doc/guide/04-transports.ko.md) | Transport 가이드 (tcp/ipc/inproc/ws/wss/tls) |
| [05-tls-security.ko.md](../core/doc/guide/05-tls-security.ko.md) | TLS/SSL 설정 및 보안 가이드 |
| [06-monitoring.ko.md](../core/doc/guide/06-monitoring.ko.md) | 모니터링 API 사용법 |

### Services
| 문서 | 설명 |
|------|------|
| [Framework 서비스 개요](../framework/doc/framework/common/guide/server/03-concepts.ko.md) | Framework 서비스와 객체 모델 개요 |
| [Spot](../framework/doc/framework/common/guide/server/06-spot.ko.md) | Spot 생성·메시징·수명 |
| [Actor](../framework/doc/framework/common/guide/server/07-actor-spot.ko.md) | Actor 모델과 Spot membership |

### Reference
| 문서 | 설명 |
|------|------|
| [08-routing-id.ko.md](../core/doc/guide/08-routing-id.ko.md) | Routing ID 개념 및 사용법 |
| [09-message-api.ko.md](../core/doc/guide/09-message-api.ko.md) | 메시지 API 상세 |
| [10-performance.ko.md](../core/doc/guide/10-performance.ko.md) | 성능 특성 및 튜닝 가이드 |

## 라이브러리 스펙 (spec/)

| 문서 | 설명 |
|------|------|
| [spec/README.ko.md](../core/doc/spec/README.ko.md) | 스펙 마스터 인덱스 |
| [spec/core/README.ko.md](../core/doc/spec/core/README.ko.md) | 코어 C 라이브러리 스펙 |
| [spec/core/socket/](../core/doc/spec/core/socket/README.ko.md) | 소켓 스펙 (공통 + 타입별) |
| [spec/bindings/README.md](../bindings/doc/spec/README.en.md) | Cross-language 바인딩 정책 및 언어별 스펙 |

## 바인딩 가이드 (guide/bindings/)

각 언어 사용 가이드는 [`guide/bindings/`](../bindings/doc/guide/README.ko.md)에 모여 있다.
메시징 개념은 코어 가이드가 소유하고, 언어 가이드는 그 언어의 사용법·타입
매핑·고유 규칙을 다룬다.

| 언어 | 가이드 | 비고 |
|------|------|------|
| .NET | [dotnet/](../bindings/doc/guide/dotnet/index.ko.md) | LibraryImport, .NET 8+ |
| C++ / Java / Node / Python / Go / Rust | [바인딩 가이드 목록](../bindings/doc/guide/README.ko.md) | 언어별 사용법·타입 매핑 |

## 내부 구조 (internals/)

| 문서 | 설명 |
|------|------|
| [architecture.ko.md](../core/doc/internals/architecture.ko.md) | 시스템 아키텍처 전체 (5계층 상세) |
| [protocol-zmp.ko.md](../core/doc/internals/protocol-zmp.ko.md) | ZMP v1.0 프로토콜 상세 |
| [protocol-raw.ko.md](../core/doc/internals/protocol-raw.ko.md) | RAW (STREAM) 프로토콜 상세 |
| [stream-socket.ko.md](../core/doc/internals/stream-socket.ko.md) | STREAM 소켓 내부 구조, WS/WSS 최적화, 런타임 기본값 |
| [socket-option-defaults.ko.md](../core/doc/internals/socket-option-defaults.ko.md) | 코드 기준 소켓 옵션 실효 기본값 |
| [threading-model.ko.md](../core/doc/internals/threading-model.ko.md) | 스레딩 및 동시성 모델 |
| [Framework 내부 구조](../framework/doc/framework/common/internals/README.ko.md) | Framework runtime 내부 구조와 책임 경계 |
| [design-decisions.ko.md](../core/doc/internals/design-decisions.ko.md) | 설계 결정 기록 |

## 빌드 및 개발 (building/)

| 문서 | 설명 |
|------|------|
| [build-guide.ko.md](building/build-guide.ko.md) | 빌드 방법 (CMake, 플랫폼별) |
| [cmake-options.ko.md](building/cmake-options.ko.md) | CMake 옵션 상세 |
| [packaging.ko.md](building/packaging.ko.md) | 릴리즈 및 패키징 |
| [release-accounts.ko.md](building/release-accounts.ko.md) | 공식 배포 계정/시크릿 |
| [platforms.ko.md](building/platforms.ko.md) | 지원 플랫폼 및 컴파일러 |

## 라이선스 정책 (license/)

| 문서 | 설명 |
|------|------|
| [README.ko.md](license/README.ko.md) | 이 저장소가 세 라이선스(MPL-2.0 / FSL-1.1-ALv2 / Apache-2.0)를 쓰는 이유, 각 계층이 허용하는 것, 정본 텍스트 위치 |
