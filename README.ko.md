[English](./README.md) | [한국어](./README.ko.md)

# zlink

> [libzmq](https://github.com/zeromq/libzmq) v4.3.5 기반의 현대적 메시징 라이브러리 — 핵심 패턴에 집중하고, Boost.Asio 기반 I/O와 개발 친화적 API를 제공합니다.

[![Build](https://github.com/ulala-x/zlink/actions/workflows/build.yml/badge.svg)](https://github.com/ulala-x/zlink/actions/workflows/build.yml)
[![License: MPL-2.0 / FSL-1.1 / Apache-2.0](https://img.shields.io/badge/License-multiple-blue.svg)](./doc/license/README.ko.md)

[공식 사이트](https://kairos-code-dev.github.io/zlink/) · [사용자 가이드](./doc/guide/01-overview.ko.md) · [스펙](./doc/spec/README.ko.md) · [바인딩](doc/bindings/overview.ko.md) · [내부 구조](./doc/internals/architecture.ko.md) · [빌드](./doc/building/build-guide.ko.md)

---

## 왜 zlink인가?

libzmq는 강력하지만 수십 년간 축적된 복잡성을 안고 있습니다 — 레거시 프로토콜, 거의 사용되지 않는 소켓 타입, 그리고 과거 시대에 설계된 I/O 엔진.

**zlink는 libzmq의 핵심만 남기고 현대적으로 재구축합니다:**

| | libzmq | zlink |
|---|--------|-------|
| **Socket Types** | 17종 (draft 포함) | **7종** — PAIR, PUB/SUB, XPUB/XSUB, DEALER/ROUTER, STREAM |
| **I/O Engine** | 자체 poll/epoll/kqueue | **Boost.Asio** (번들, 외부 의존성 없음) |
| **암호화** | CURVE (libsodium) | **TLS** (OpenSSL) — `tls://`, `wss://` |
| **Transport** | 10종+ (PGM, TIPC, VMCI 등) | **6종** — `tcp`, `ipc`, `inproc`, `ws`, `wss`, `tls` |
| **의존성** | libsodium, libbsd 등 | **OpenSSL만** |

---

## 주요 특징

### 간소화된 Core

REQ/REP, PUSH/PULL, 모든 draft socket을 제거했습니다. 남은 7종의 socket type — PAIR, PUB/SUB, XPUB/XSUB, DEALER/ROUTER, STREAM — 으로 실전 메시징 패턴의 대부분을 커버하면서, 복잡성에 의한 실수를 줄입니다. STREAM 소켓은 외부 클라이언트와의 RAW 통신을 위해 tcp, tls, ws, wss transport를 지원합니다.
현재 STREAM 기본값은 실전 처리량 기준으로 튜닝되어 있습니다: accept 동시성 `4`, 세션 스케줄링 `rr`, `SNDBUF/RCVBUF` 미지정 시 `256KB` 자동 적용. 기존 STREAM 런타임 튜닝 환경변수 대부분은 제거되고 내부 상수로 고정되었습니다.

### Boost.Asio 기반 I/O Engine

전체 I/O 계층을 **Boost.Asio**로 재작성했습니다 (header만 번들 — 외부 Boost 의존성 없음). 검증된 비동기 기반 위에 TLS와 WebSocket transport를 네이티브로 통합할 수 있습니다.

### 네이티브 TLS & WebSocket

외부 프록시 없이 암호화된 transport를 직접 지원합니다:

```c
// TLS 서버
zlink_setsockopt(socket, ZLINK_TLS_CERT, "/path/to/cert.pem", ...);
zlink_setsockopt(socket, ZLINK_TLS_KEY, "/path/to/key.pem", ...);
zlink_bind(socket, "tls://*:5555");

// TLS 클라이언트
zlink_setsockopt(socket, ZLINK_TLS_CA, "/path/to/ca.pem", ...);
zlink_connect(socket, "tls://server.example.com:5555");
```

---

## 아키텍처

zlink는 5개의 명확히 분리된 계층으로 구성됩니다:

```
┌──────────────────────────────────────────────────────┐
│  Application Layer                                    │
│  zlink_ctx_new() · zlink_socket() · zlink_send/recv()      │
├──────────────────────────────────────────────────────┤
│  Socket Logic Layer                                   │
│  PAIR · PUB/SUB · XPUB/XSUB · DEALER/ROUTER · STREAM  │
│  라우팅 전략: lb_t(Round-robin) · fq_t · dist_t       │
├──────────────────────────────────────────────────────┤
│  Engine Layer (Boost.Asio)                            │
│  asio_zmp_engine — ZMP v1.0 Protocol (8B 고정 헤더)   │
│  Proactor 패턴 · Speculative I/O · Backpressure       │
├──────────────────────────────────────────────────────┤
│  Transport Layer                                      │
│  tcp · ipc · inproc · ws — 평문                       │
│  tls · wss             — OpenSSL 암호화               │
├──────────────────────────────────────────────────────┤
│  Core Infrastructure                                  │
│  msg_t(64B 고정) · pipe_t(Lock-free YPipe)            │
│  ctx_t(I/O Thread Pool) · session_base_t(Bridge)      │
└──────────────────────────────────────────────────────┘
```

### 핵심 설계

| 설계 원칙 | 설명 |
|-----------|------|
| **Zero-Copy** | 메시지 복사 최소화 — VSM(33B 이하)은 inline 저장, 대용량은 참조 카운팅 |
| **Lock-Free** | Thread 간 통신에 YPipe(CAS 기반 FIFO) 사용, mutex 없음 |
| **True Async** | Proactor 패턴 기반 비동기 I/O + Speculative I/O 최적화 |
| **Protocol Agnostic** | Transport와 Protocol의 명확한 분리 — 자체 ZMP v1.0 프로토콜 사용 |

### Thread 모델

- **Application Thread**: `zlink_send()`/`zlink_recv()` 호출
- **I/O Thread**: Boost.Asio `io_context` 기반 비동기 네트워크 처리
- **Reaper Thread**: 종료된 소켓/세션의 자원 정리
- Thread 간 통신은 Lock-free YPipe + Mailbox 시스템으로 처리

> 상세한 내부 아키텍처는 [Architecture Document](./doc/internals/architecture.ko.md)를 참고하세요.

---

## 서비스

코어 소켓 계층 위에 구축된, 실전 분산 시스템을 위한 **고수준 서비스 계층**을 제공합니다:

```
┌───────────────────────────────────────────────────────┐
│                      Application                      │
│        Gateway (요청/응답) · SPOT (발행/구독)         │
├───────────────────────────────────────────────────────┤
│                Discovery (서비스 발견)                │
├───────────────────────────────────────────────────────┤
│               Registry (서비스 등록소)                │
├───────────────────────────────────────────────────────┤
│         zlink Core (7종 소켓 + 6종 Transport)         │
└───────────────────────────────────────────────────────┘
```

| 서비스 | 설명 | 가이드 |
|--------|------|:------:|
| **Discovery** | Registry 클러스터 HA, Heartbeat 기반 생존 확인, Client-side 서비스 캐시 | [Discovery](./doc/guide/07-1-discovery.ko.md) |
| **Gateway** | Discovery 기반 위치투명 요청/응답, 자동 로드밸런싱, Thread-safe 전송 | [Gateway](doc/guide/07-2-gateway.ko.md) |
| **SPOT** | 위치투명 토픽 PUB/SUB, Discovery 기반 자동 Mesh 구성 | [SPOT](./doc/guide/07-3-spot.ko.md) |

> 전체 기능 로드맵과 의존성 그래프는 [Feature Roadmap](doc/plan/feature-roadmap.ko.md)을 참고하세요.

---

## 부가 기능

| 기능 | 설명 | 가이드 |
|------|------|:------:|
| **Routing ID** | `zlink_routing_id_t` 표준 타입, own 16B UUID / peer 4B uint32 | [Routing ID](./doc/guide/08-routing-id.ko.md) |
| **모니터링** | Routing-ID 기반 이벤트 식별, Polling 방식 모니터 API | [Monitoring](./doc/guide/06-monitoring.ko.md) |

---

## 시작하기

### 요구 사항

- **CMake** 3.10+
- **C++17** 컴파일러 (GCC 7+, Clang 5+, MSVC 2017+)
- **OpenSSL** (TLS/WSS 지원)

### 빌드

```bash
# Linux
./core/builds/linux/build.sh x64 ON

# macOS
./core/builds/macos/build.sh arm64 ON

# Windows (PowerShell)
.\core\builds\windows\build.ps1 -Architecture x64 -RunTests "ON"
```

### CMake 직접 빌드

```bash
cmake -S core -B core/build/local -DWITH_TLS=ON -DBUILD_TESTS=ON
cmake --build core/build/local
ctest --test-dir core/build/local --output-on-failure
```

### C Perf 벤치마크 빌드 규칙

`bindings/c/perf/run_benchmarks_multi.sh` 는 `build_cpp_release` 를 쓰지 않는다.
standalone C 벤치마크 빌드는 zlink core runtime 을 `core/build` 에서 가져오기 때문에,
성능 수치는 그 runtime 을 다시 빌드한 뒤에만 신뢰할 수 있다.

```bash
cmake --build core/build
./bindings/c/perf/run_benchmarks_multi.sh --pattern SPOT_REQREP
```

이제 runner 는 실행 전에 실제 `libzlink.so` 경로를 출력하고, `core/src` 나
`core/include` 가 `core/build` runtime 보다 더 새로우면 즉시 실패한다.

### CMake 옵션

| 옵션 | 기본값 | 설명 |
|------|--------|------|
| `WITH_TLS` | `ON` | OpenSSL을 통한 TLS/WSS 활성화 |
| `BUILD_TESTS` | `ON` | 테스트 빌드 |
| `BUILD_BENCHMARKS` | `OFF` | 벤치마크 빌드 |
| `BUILD_SHARED` | `ON` | Shared Library 빌드 |
| `BUILD_STATIC` | `ON` | Static Library 빌드 |
| `ZLINK_CXX_STANDARD` | `17` | C++ 표준 (11, 14, 17, 20, 23) |

### OpenSSL 설치

```bash
# Ubuntu/Debian
sudo apt-get install libssl-dev

# macOS
brew install openssl@3

# Windows (vcpkg)
vcpkg install openssl:x64-windows
```

---

## 지원 플랫폼

| 플랫폼 | Architecture | 상태 |
|--------|:------------:|:----:|
| Linux | x64, ARM64 | Stable |
| macOS | x64, ARM64 | Stable |
| Windows | x64, ARM64 | Stable |

---

## 성능

libzmq 대비 64바이트 메시지 TCP 처리량 비교:

| 패턴 | libzmq | zlink | 차이 |
|------|-------:|------:|-----:|
| DEALER↔DEALER | 5,936 Kmsg/s | 6,168 Kmsg/s | **+3.9%** |
| PAIR | 6,195 Kmsg/s | 5,878 Kmsg/s | -5.1% |
| PUB/SUB | 5,654 Kmsg/s | 5,756 Kmsg/s | **+1.8%** |
| DEALER↔ROUTER | 5,609 Kmsg/s | 5,634 Kmsg/s | +0.4% |
| ROUTER↔ROUTER | 5,161 Kmsg/s | 5,250 Kmsg/s | **+1.7%** |
| ROUTER↔ROUTER (poll) | 4,405 Kmsg/s | 5,249 Kmsg/s | **+19.2%** |
| STREAM | 1,786 Kmsg/s | 5,216 Kmsg/s | **+192%** |

> 상세 분석은 [성능 리포트](doc/report/benchmark-2026-02-11.ko.md) 및 [성능 가이드](./doc/guide/10-performance.ko.md)를 참고하세요.

---

## 문서

| 문서 | 설명 |
|------|------|
| [문서 네비게이션](./doc/README.ko.md) | 전체 문서 목차 및 독자별 경로 |
| [라이브러리 스펙](./doc/spec/README.ko.md) | zlink 라이브러리 스펙 (코어 + 바인딩) |
| [사용자 가이드](./doc/guide/01-overview.ko.md) | zlink API 가이드 (12편) |
| [바인딩 가이드](doc/bindings/overview.ko.md) | C++/Java/.NET/Node.js/Python 바인딩 |
| [내부 아키텍처](./doc/internals/architecture.ko.md) | 시스템 아키텍처 및 내부 구현 |
| [빌드 가이드](./doc/building/build-guide.ko.md) | 빌드, 테스트, 패키징 |
| [Feature Roadmap](doc/plan/feature-roadmap.ko.md) | 기능 로드맵과 의존성 그래프 |

---

## 라이선스

이 저장소는 계층에 따라 세 라이선스를 씁니다.

| 계층 | 라이선스 |
|------|----------|
| `core/`, `bindings/` — 메시징 엔진과 언어별 네이티브 바인딩 | [Mozilla Public License 2.0](./LICENSE) |
| `framework/` — 상위 프레임워크(SPOT/actor, channel messaging, STREAM, drain) | [Functional Source License 1.1, ALv2 Future License](./framework/LICENSE) |
| `framework`의 언어별 `http-client` 패키지 — 각 플랫폼에서 흔히 쓰는 HTTP 클라이언트 라이브러리(.NET `System.Net.Http`, Java/Kotlin `java.net.http`, Node `undici`, C++ Boost.Beast)를 감싼 래퍼 | Apache License 2.0 |

`http-client` 패키지는 zlink가 자체 구현한 전송 계층이 아니라 허용적
라이선스의 HTTP 클라이언트 라이브러리를 감싼 것이라 FSL 대신 Apache-2.0을
씁니다. .NET·Node 패키지는 각자 매니페스트에 `Apache-2.0`을 직접 표기하고,
Java·Kotlin 패키지는 공유 Gradle 배포 설정에서 이 값을 받으며, C++ 패키지는
소스와 함께 라이선스 전문을 담아 배포합니다(CMake에는 표준 SPDX 매니페스트
필드가 없습니다).

`core`/`bindings`가 MPL-2.0인 이유는 `core`가 [libzmq](https://github.com/zeromq/libzmq)
v4.3.5(이미 MPL-2.0)에서 출발했기 때문입니다. `framework`는 FSL-1.1-ALv2로,
내부 사용·비상업 교육/연구·라이선스 사용자를 위한 전문 서비스에 쓸 수 있고,
그 외에도 Competing Use(경쟁 목적 사용)만 아니면 됩니다. Competing Use는
소프트웨어 자체를 대체하거나, 우리가 이 소프트웨어로 제공 중인 다른
제품·서비스를 대체하거나, 실질적으로 동일한 기능을 제공하는 상용 제품·서비스로
제공하는 것을 말합니다(전체 정의는 [framework/LICENSE](./framework/LICENSE) 참고).
자기 제품을 만들어 배포하는 것(임베드 포함)은 라이선스의 명시적 허용 목록이
아니라 이 "Competing Use가 아니다"라는 조항으로 커버됩니다.
`framework`의 각 릴리스는 발행일로부터 2년 뒤 자동으로 Apache License 2.0으로
전환됩니다.

서드파티 구성요소 및 바이너리 재배포 관련 고지는
[THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md)를, 정책 전반의 근거는
[doc/license/README.ko.md](./doc/license/README.ko.md)를 참고하세요.

[libzmq](https://github.com/zeromq/libzmq) 기반 — Copyright (c) 2007-2024 Contributors as noted in the [AUTHORS](./core/AUTHORS) file.
