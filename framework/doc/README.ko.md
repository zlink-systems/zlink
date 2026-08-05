<!-- framework-adapter-nav:start -->
[문서 목록](README.ko.md) | [다음: ZLink Framework 공통 스펙](framework/common/README.ko.md)
<!-- framework-adapter-nav:end -->

# ZLink Framework 문서 맵

`ZLink Framework` 문서의 진입점이다. 문서는 **컴포넌트별**로 나뉘고, 각 컴포넌트는
**공통(언어 중립)** 과 **언어별** 문서로 나뉜다. 아래 맵에서 필요한 위치로 바로 이동한다.

## 컴포넌트 한눈에

| 컴포넌트 | 디렉토리 | 진입점 | 내용 |
|---------|----------|--------|------|
| **Framework adapter** | `framework/` | [공통 스펙](framework/common/README.ko.md) | 메시징·SPOT·actor·stream 프레임워크 본체 |
| **HTTP Client** | `framework/<lang>/guide/http-client/` | [언어별 문서](framework/dotnet/guide/http-client/README.ko.md) | fluent HTTP/JSON client |
| **Stream Connector** | `framework/<lang>/guide/stream-connector/` | [언어별 문서](framework/cpp/guide/stream-connector/README.ko.md) | client 측 STREAM 접속 라이브러리 |

정식 언어: `.NET` · `Java/Kotlin` · `Node.js` · `C++`.

---

## 1. Framework adapter (`framework/`)

### 1.1 공통 — 언어 중립 정식 계약 (`framework/common/`)

[공통 문서 색인](framework/common/README.ko.md) · 공통 의미를 바꿀 때는 여기를 먼저 고친다.

**스펙** (`framework/common/spec/` — 패키지별로 나뉜다: `server/` · `http-client/` · `stream-connector/`)

| 문서 | 다루는 범위 |
|------|-------------|
| [개요](framework/common/spec/02-overview.ko.md) | Framework의 목적과 우선 범위 |
| [상호작용 모델](framework/common/spec/03-interaction-model.ko.md) | request-response, command, publish-subscribe 사용자 모델 |
| [메시지 모델](framework/common/spec/04-message-model.ko.md) | header/payload 구조와 metadata 정책 |
| [Channel topology](framework/common/spec/07-channel-topology.ko.md) | channel grouping, discovery, 수동 연결, 내부 transport 매핑 |
| [Framework API](framework/common/spec/06-framework-api.ko.md) | 언어별 framework API의 공통 방향 |
| [비동기 실행 정책](framework/common/spec/05-async-execution-policy.ko.md) | async submit, blocking 금지, coroutine/adapter 공통 의미 |
| [Actor 모델](framework/common/spec/14-actor-model.ko.md) | actor 위치, session binding, Entry Spot, user Spot, dispatch 기준 |
| [Session Actor Dispatch](framework/common/spec/20-session-actor-dispatch.ko.md) | session과 actor를 연결하는 helper와 routing 정책 |
**공통 샘플 시나리오** ([색인](framework/common/sample/README.ko.md)) — 정본 6종을 모든 언어가
같은 역할 분리·메시지 이름·smoke 순서로 구현한다.

Sample과 E2E의 설정 파일, 환경 변수 금지와 언어별 typed binding 기준은
[Sample/E2E 설정 정책](framework/common/sample-e2e-configuration-policy.ko.md)을 따른다.

| 샘플 | 핵심 |
|------|------|
| TicTacToe | route mesh, actor game join (API+Play 직접 연결) |
| Bingo | Registry/Discovery, Entry Spot, room Spot, bound push (Session/API/Play 분리) |
| SupportChat | conversation Spot, idle/close timer, 재접속 |
| DeliveryDispatch | 배달 상태 전이, 재배정 timer, 고객 stream push |
| ShoppingMall | order workflow 상태 전이, event sourcing/보상 |
| GameQuest | stateless API scale-out, owner routing, fanout + event sourcing |

### 1.2 언어별 (`framework/<lang>/`, `framework/doc` 기준)

각 언어 문서는 이 디렉토리, 즉 `framework/doc/` 아래에서 관리한다.
`guide/server`, `guide/http-client`, `guide/stream-connector`(사용 안내)와
`internals`(구현·검증 기준)로 나뉘며, 각 언어 README가 그 언어 전체를 색인한다.
공개 계약은 `framework/common/spec/`에서 패키지와 언어별로 관리한다.

| 언어 | 진입점 | guide | spec | internals |
|------|--------|-------|------|-----------|
| `.NET` | [framework/dotnet](framework/dotnet/README.ko.md) | 기능 guide + samples | ASP.NET Core 계약 | backend·runtime·회귀 |
| `Java` | [framework/java](framework/java/README.ko.md) | Spring Boot 가이드 (blocking/`CompletionStage`) | Spring 계약 | 구현·검증 |
| `Kotlin` | [framework/kotlin](framework/kotlin/README.ko.md) | Kotlin 전용 가이드 (`suspend`/`Flow`) | Java 공유 | Java 공유 |
| `Node.js` | [framework/node](framework/node/README.ko.md) | NestJS 가이드 | NestJS 계약 | 구현·검증 |
| `C++` | [framework/cpp](framework/cpp/README.ko.md) | 가이드 + samples | C++ 계약 | runtime·backend·회귀 |

> 가이드 골격(언어별 세부 번호는 다를 수 있음): 개요 → 시작하기 → 핵심 개념 →
> 채널 메시징 → SPOT → actor/session → STREAM → Registry → 모니터링 →
> 인터페이스 카탈로그 → gRPC 대안 → 샘플 고르기 순으로 이어진다.

---

## 2. HTTP Client (`framework/<lang>/guide/http-client/`)

fluent HTTP/JSON client. 다섯 언어가 동일한 13장 골격을 공유한다. Kotlin은
coroutine `suspend` 표면으로 같은 기능을 제공한다.

| 언어 | 진입점 |
|------|--------|
| `.NET` | [framework/dotnet/guide/http-client](framework/dotnet/guide/http-client/README.ko.md) |
| `Java` | [framework/java/guide/http-client](framework/java/guide/http-client/README.ko.md) |
| `Kotlin` | [framework/kotlin/guide/http-client](framework/kotlin/guide/http-client/README.ko.md) |
| `Node.js` | [framework/node/guide/http-client](framework/node/guide/http-client/README.ko.md) |
| `C++` | [framework/cpp/guide/http-client](framework/cpp/guide/http-client/README.ko.md) |

장 구성: 01 개요 · 02 시작하기 · 03 client 설정 · 04 요청 보내기 · 05 request body ·
06 응답 처리 · 07 async/coroutine · 08 streaming · 09 인증/TLS · 10 redirect/retry/cookie ·
11 proxy · 12 압축 · 13 에러 처리. 각 언어 `spec/`에 공개 계약이 있다.

---

## 3. Stream Connector (`framework/<lang>/guide/stream-connector/`)

client 측에서 STREAM 서버에 접속하는 별도 라이브러리. 게임 엔진 adapter 포함.

**어떤 connector를 쓰는지는 언어가 아니라 "엔진 × 빌드 타깃"이 결정한다.**
웹(브라우저·WASM)으로 빌드하면 언어와 무관하게 TypeScript connector를 사용한다.

| 언어 | 대상 | 가이드 |
|------|------|--------|
| C++ | Unreal, Godot(GDExtension), Axmol, 일반 C++, 서버 e2e/perf | [INDEX](framework/cpp/guide/stream-connector/INDEX.ko.md) |
| `.NET` | Unity(네이티브), Godot C#, 데스크톱·서버 | [INDEX](framework/dotnet/guide/stream-connector/INDEX.ko.md) |
| Java | JVM application·도구·E2E client·봇 | [가이드](framework/java/guide/stream-connector/README.ko.md) |
| Kotlin | JVM application·도구·E2E client·봇 | [가이드](framework/kotlin/guide/stream-connector/README.ko.md) |
| Node.js/TypeScript | 브라우저 계열(웹·Unity WebGL·Cocos web·Godot Web) | [INDEX](framework/node/guide/stream-connector/INDEX.ko.md) |

| 영역 | 문서 |
|------|------|
| C++ 가이드 | [01 개요](framework/cpp/guide/stream-connector/01-overview.ko.md) · [02 시작하기](framework/cpp/guide/stream-connector/02-getting-started.ko.md) · [03 옵션](framework/cpp/guide/stream-connector/03-connector-options.ko.md) · [04 송신](framework/cpp/guide/stream-connector/04-sending.ko.md) · [05 수신](framework/cpp/guide/stream-connector/05-receiving.ko.md) · [06 lifecycle](framework/cpp/guide/stream-connector/06-lifecycle.ko.md) · [07 에러 처리](framework/cpp/guide/stream-connector/07-error-handling.ko.md) · [08 e2e client](framework/cpp/guide/stream-connector/08-e2e-client.ko.md) · [09 engine adapter](framework/cpp/guide/stream-connector/09-engine-adapters.ko.md) · [10 packaging](framework/cpp/guide/stream-connector/10-packaging.ko.md) · [11 성능](framework/cpp/guide/stream-connector/11-performance.ko.md) |
| C++ core | [async runtime](framework/cpp/guide/stream-connector/core/guide/async-runtime.ko.md) |
| C++ e2e-client | [coroutine client](framework/cpp/guide/stream-connector/e2e-client/guide/coroutine-client.ko.md) |
| `.NET` 가이드 | [01 개요](framework/dotnet/guide/stream-connector/01-overview.ko.md) · [02 Unity](framework/dotnet/guide/stream-connector/02-unity.ko.md) · [03 Godot C#](framework/dotnet/guide/stream-connector/03-godot-csharp.ko.md) |
| Node.js/TypeScript 가이드 | [01 개요](framework/node/guide/stream-connector/01-overview.ko.md) · [02 브라우저](framework/node/guide/stream-connector/02-browser.ko.md) |

계약의 정본은 [Stream Connector 공통 스펙](framework/common/spec/stream-connector/32-stream-connector.ko.md)이며,
언어별 public 표면은 `framework/common/spec/stream-connector/languages/<lang>/`가 소유한다.

Java와 Kotlin은 같은 JVM connector runtime을 사용한다. Java guide는 `CompletionStage` 사용법을,
Kotlin guide는 coroutine과 `Flow` 사용법을 설명한다.

> TypeScript package root는 browser-only WebSocket transport와 명시적 flow 전달을 제공한다.
> 실제 browser와 package 검증 상태는 TypeScript package의 test와 E2E 결과에서 확인한다.

---

## 읽는 순서

1. [공통 개요](framework/common/spec/02-overview.ko.md) → [상호작용 모델](framework/common/spec/03-interaction-model.ko.md) → [actor 모델](framework/common/spec/14-actor-model.ko.md)
2. 사용할 언어의 [framework/&lt;lang&gt;](framework/dotnet/README.ko.md) guide
3. HTTP가 필요하면 [framework/&lt;lang&gt;/guide/http-client](framework/dotnet/guide/http-client/README.ko.md), 외부 client 접속이면 [framework/&lt;lang&gt;/guide/stream-connector](framework/cpp/guide/stream-connector/INDEX.ko.md)

## 유지 규칙

- 공통 의미는 `framework/common/spec/`을 먼저 고치고, 언어 문서는 링크로 연결한다.
- 언어별 문서는 공통 의미를 해당 언어의 시그니처와 샘플로만 구체화한다.
- 언어별 문서는 모두 `framework/doc/` 아래에서 작성하고 수정한다. 새 언어별 문서를
  `framework/languages/<lang>/doc/` 아래에 추가하지 않는다.
- 기존 언어별 사용 안내를 수정해야 하면 `framework/doc/framework/<lang>/guide/` 아래의
  `server/`, `http-client/`, `stream-connector/` 중 대상에 맞는 위치에서 진행한다.
  구현 설명은 `framework/doc/framework/<lang>/internals/`에서 관리한다.
- 새 문서를 추가하면 이 맵과 해당 디렉토리 `README.ko.md`를 함께 갱신한다.
- 컴포넌트 경계를 지킨다: framework 본체는 `framework/`, HTTP client는 `http-client/`,
  STREAM connector는 `stream-connector/`.

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](README.ko.md) | [다음: ZLink Framework 공통 스펙](framework/common/README.ko.md)
<!-- framework-adapter-nav:bottom:end -->
