# 01 — 개요

[← 목차](INDEX.ko.md) | [다음: 시작하기 →](02-getting-started.ko.md)

---

C++ Stream Connector는 ZLink STREAM 서버에 연결하는 client-side library다. 게임 엔진, 일반 C++ 애플리케이션, 서버 e2e 테스트 등 다양한 환경에서 같은 STREAM 프로토콜을 사용할 수 있도록 제품군을 분리해 제공한다.

## 제품군 구성

```
connector/
├── core/          — 일반 C++ client용 connector runtime (no-exception, no-coroutine)
├── e2e-client/    — 서버 e2e/perf scenario용 coroutine helper
├── engines/       — Unreal, Godot, Axmol 엔진 어댑터
└── perf/          — 성능 테스트 client와 runner
```

각 산출물은 독립적으로 배포한다. core를 설치하지 않고 engine adapter만 사용할 수 있고, e2e client를 사용하지 않아도 core는 동작한다.

## 배포 단위

| 산출물 | CMake target | 배포 형식 | 주요 사용자 |
|--------|-------------|-----------|-------------|
| `zlink-stream-connector` | `zlink::stream_connector` | CMake, vcpkg, Conan | 일반 C++ client, 게임 엔진 내부 |
| `zlink-stream-e2e-client` | `zlink::stream_e2e_client` | CMake, vcpkg, Conan | 서버 e2e/smoke/perf test |
| `zlink-unreal-stream-connector` | Unreal plugin module | source plugin | Unreal Engine game |
| `zlink-godot-stream-connector` | GDExtension | source GDExtension | Godot 4 game |
| `zlink-axmol-connector` | CMake target | source package | Axmol engine game |

## core — 기본 connector

core는 C++ exception과 coroutine에 의존하지 않는 독립 라이브러리다. 예외가 꺼진 게임 엔진에서도 빌드된다. public header는 `<coroutine>`이나 Boost.Asio executor type을 노출하지 않는다.

```cpp
#include <zlink/stream_connector.hpp>

zlink::stream_connector::connector_options_t options;
options.endpoint = "tcp://game.example.com:7000";
auto connector = zlink::stream_connector::connector_factory_t::create(options);
```

실패는 `result_t<T>`로 반환한다.

## e2e-client — coroutine helper

e2e client는 core 위에 올라가는 선택 표면이다. C++20 coroutine을 안정적으로 켤 수 있는 환경에서 사용한다. 일반 게임 client의 기본 API가 아니다.

```cpp
#include <zlink/stream_e2e_client.hpp>

auto client = zlink::stream_e2e_client::use(connector);
auto reply = co_await client.request(ping_t{"player-1", 1}).async<pong_t>();
```

`async()`는 blocking `submit()`을 호출하지 않는다. coroutine이 대기하는 동안 worker thread는 다른 작업을 처리한다.

## 지원 엔진

| 엔진 | 지원 | 어댑터 형식 |
|------|------|-------------|
| Unreal Engine | 지원 | `.uplugin` + `UObject` API + Game Thread delegate |
| Godot 4 | 지원 | GDExtension + Godot signal |
| Axmol Engine | 지원 | C++ source package + `Scheduler::runOnAxmolThread` |
| Cocos Creator 3.x | C++ 미지원 | TypeScript connector 사용 |
| Cocos2d-x | 미지원 | 업데이트 중단 |

## transport 지원

| scheme | transport | build feature |
|--------|-----------|---------------|
| `tcp://host:port` | TCP | 항상 포함 |
| `tls://host:port` | TLS over TCP | `WITH_TLS` (OpenSSL) |
| `ws://host:port/path` | WebSocket | `WITH_WEBSOCKET` |
| `wss://host:port/path` | WebSocket over TLS | `WITH_WEBSOCKET` + `WITH_TLS` |

## 서버 framework와의 관계

connector는 STREAM 서버에 연결하는 client library다. 서버 framework package와 상호 의존하지 않는다. 양쪽은 STREAM header/payload wire 계약만 공유한다.
