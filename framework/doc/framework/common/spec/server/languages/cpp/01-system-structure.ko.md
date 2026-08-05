<!-- framework-adapter-nav:start -->
[스펙 목차](README.ko.md) | [다음: C++ exact interface](interfaces/README.ko.md)
<!-- framework-adapter-nav:end -->

# C++ 시스템 구조 — 패키지, 등록과 부트스트랩

[스펙 목차](README.ko.md)

> 이 문서는 **C++에서 ZLink framework를 어떻게 구성하는가**를 소유한다. 패키지·빌드 타깃,
> application host, **DI 컨테이너**, **configuration**, **logging**, lifecycle, 그리고 각 기능의
> **등록 표면**이다.
>
> **기능의 의미와 동작 규칙은 공통 스펙이 소유한다** — [channel-messaging](../../../08-channel-messaging.ko.md),
> [spot-messaging](../../../12-spot-messaging.ko.md), [MeshNode](../../../13-mesh-node.ko.md),
> [stream-session](../../../19-stream-session.ko.md), [actor-model](../../../14-actor-model.ko.md),
> [session-actor-dispatch](../../../20-session-actor-dispatch.ko.md),
> [runtime-monitoring](../../../24-runtime-monitoring.ko.md),
> [location-runtime](../../../21-location-runtime.ko.md),
> [channel-topology](../../../07-channel-topology.ko.md).
>
> **public 타입과 시그니처는 [기능별 exact interface](interfaces/README.ko.md)가 소유한다.**
> HTTP는 [60](60-http-hosting.ko.md)·[61](61-embedded-http-server.ko.md)이 소유한다.
> **내부 runtime 구조는 [internals/runtime-architecture](../../../../internals/README.ko.md)가 소유한다** —
> 공개 계약이 아니다.

## 1. 제품 포지션

**C++ framework는 binding helper가 아니라 application framework다.** host, DI, configuration,
logging, lifecycle을 함께 제공한다.

**다른 언어와 결정적으로 다르다.** `.NET`은 ASP.NET Core를, Node는 NestJS를, Java는 Spring Boot를
**빌려 쓴다.** C++에는 그런 host가 없으므로 **framework가 직접 제공한다.** 그래서 C++ 문서만
기능별 스펙을 유지한다.

framework가 제공해야 하는 것:

| 축 | 내용 |
|---|---|
| **application host** | Application bootstrap을 시작하고 hosted service, module과 lifecycle을 관리한다. |
| **DI 컨테이너** | Service lifetime과 scope를 관리하고 생성자 주입으로 의존성을 제공한다. |
| **configuration** | Application과 Framework option을 계층적 설정 model로 제공한다. |
| **logging** | Log level과 backend를 선택하고 bounded 비동기 queue와 회전 file을 관리한다. |
| **HTTP hosting** | Application lifecycle 안에서 내장 HTTP server, route와 middleware를 제공한다. |
| **zlink messaging** | Channel, Spot, STREAM과 Actor message API를 제공한다. |
| **handler model** | Handler를 등록하고 선택된 실행 문맥에서 dispatch하며 filter를 적용한다. |
| **observability** | Metric, message flow와 health 상태를 application에 제공한다. |

**public API는 C++ 관례로 표현한다** — RAII, value type, template, coroutine.

## 2. 패키지와 빌드 타깃

| 타깃 | CMake | 내용 |
|---|---|---|
| `zlink_framework` | **`zlink::framework`** (STATIC) | Framework core를 제공하며 C++20(`cxx_std_20`)이 필요하다. |

**client connector는 별도 제품군이다** — [C++ Stream Connector 가이드](../../../../../cpp/guide/stream-connector/INDEX.ko.md)가
소유한다. 서버 framework와 상호 의존하지 않는다.

## 3. Application Host

```cpp
class app_t;            // host 인스턴스
class app_advanced_t;   // 고급 설정 접근
class hosted_service_t; // 시작·종료 훅
class module_t;         // 기능 묶음 등록
```

- **`module_t`는 관련 등록을 한 덩어리로 묶는다.** 큰 app을 기능 단위로 나눌 때 쓴다.
- **runtime은 host startup에서 만들고 shutdown에서 정리한다.** lazy 생성으로 숨기지 않는다
  ([channel-messaging §2](../../../08-channel-messaging.ko.md)).

### 3.1 hosted service의 실행 순서

| 단계 | 규칙 |
|---|---|
| **시작** | 등록 순서대로 시작한다 |
| **종료** | **시작 역순으로 정리한다** |

### 3.2 시작 실패 — fail-fast

**시작 도중 한 service라도 실패하면, 그때까지 시작한 service를 역순으로 정리한 뒤 예외를 다시
던진다.** 반쯤 시작된 host를 남기지 않는다.

**정리는 실패하지 않는다**(`noexcept`). 정리 중의 오류가 원래 실패를 가리면 안 되기 때문이다.

## 4. DI 컨테이너

### 4.1 Lifetime

```cpp
enum class service_lifetime_t { singleton, scoped, transient };
```

| lifetime | 의미 |
|---|---|
| `singleton` | host 전체에서 하나 |
| `scoped` | **scope 하나당 하나**(§4.2) |
| `transient` | resolve할 때마다 새로 만든다 |

### 4.2 Scope 경계

`scoped` service의 생성과 정리는 Framework가 handler, STREAM session과 object lifecycle 경계에서
수행한다. Application은 scope 종류를 선택하거나 scope를 직접 만들지 않는다.

### 4.3 등록

```cpp
class service_collection_t
{
public:
    template <typename T> service_collection_t &add_singleton ();
    template <typename T, typename... TDependencies>
    requires (sizeof...(TDependencies) > 0) service_collection_t &add_singleton ();
    template <typename T> service_collection_t &add_singleton (std::unique_ptr<T> instance);

    template <typename T> service_collection_t &add_scoped ();
    template <typename T, typename... TDependencies>
    requires (sizeof...(TDependencies) > 0) service_collection_t &add_scoped ();

    template <typename T> service_collection_t &add_transient ();
    template <typename T, typename... TDependencies>
    requires (sizeof...(TDependencies) > 0) service_collection_t &add_transient ();

    template <typename T, typename TFactory>
    service_collection_t &add_factory (
      TFactory factory,
      service_lifetime_t lifetime = service_lifetime_t::transient);
    template <typename T> service_collection_t &add_framework_dependency ();
};
```

- **의존성은 template 인자로 선언한다.** 인자가 없으면 **기본 생성자가 필요하다**(정적으로
  검증한다).
- **`dependency_list_t<...>`로 handler의 의존성을 선언하면** framework가 그 타입들을 주입해
  handler를 만든다.
- **`logger_t<TCategory>`는 framework 의존성이다** — `add_framework_dependency`로 자동 배선된다.

### 4.4 Resolve

```cpp
class service_provider_t
{
public:
    template <typename T> T &get_required ();                              // 없으면 실패
    template <typename T> std::optional<std::reference_wrapper<T>> get (); // 없으면 빈 값
};

```

**handler는 service locator를 받지 않는다.** 생성자 주입만 쓴다.

### 4.5 오류 계약

| 상황 | 결과 |
|---|---|
| **같은 타입을 두 번 등록** | **등록 시점에 실패한다** — 조용히 덮어쓰지 않는다 |
| **등록되지 않은 타입을 `get_required`** | **실패한다** |
| 등록되지 않은 타입을 `get` | **빈 값을 돌려준다.** 실패하지 않는다 |
| **scope 없이 `scoped` 서비스를 resolve** | **실패한다** — scoped는 scope를 요구한다 |
| **닫힌 provider에서 resolve한다** | **[shutdown](../../../01-glossary.ko.md#shutdown) 경계 오류로 실패한다** |

### 4.6 수명과 정리

- **`singleton`은 처음 resolve할 때 만들고 host 수명 동안 재사용한다.**
- **`scoped`는 그 scope에서 처음 resolve할 때 만들고 scope 안에서 재사용한다.**
- **Framework가 scope를 닫으면 그 scope의 `scoped`·`transient` 인스턴스를 함께 정리한다.**

**닫힌 provider는 다시 사용할 수 없다.** 이후의 resolve는 전부 실패한다.

## 5. Configuration

```cpp
enum class optional_t;      // 필수/선택 구분
class configuration_model_t;
```

**설정 소스는 계층으로 합친다.** 뒤에 추가한 소스가 앞을 덮어쓴다. 필수 값이 없으면 **host 시작
전에 실패한다.**

## 6. Logging

**framework가 logging을 제공한다.** 외부 로깅 라이브러리를 강제하지 않는다.

```cpp
enum class log_level_t { trace, debug, info, warn, error, critical, off };
enum class logging_backend_t { builtin, structured };
enum class logging_overflow_policy_t { drop_debug, drop_oldest, block };

struct log_field_t;    // 구조화 로그의 key-value
struct log_record_t;   // 한 건의 로그

struct logging_async_options_t
{
    std::size_t queue_capacity = 8192;
    logging_overflow_policy_t overflow_policy = logging_overflow_policy_t::drop_debug;
};

struct rotating_file_options_t
{
    std::size_t max_file_size = 10 * 1024 * 1024;   // 10 MiB
    std::size_t max_files = 5;
};

template <typename TCategory = void> class logger_t;  // DI로 주입받는다
class logger_factory_t;
```

**비동기 로깅의 overflow 정책이 계약이다.**

| 정책 | 동작 |
|---|---|
| `drop_debug` **(기본)** | 큐가 차면 **debug 이하를 먼저 버린다.** 중요한 로그를 살린다 |
| `drop_oldest` | 가장 오래된 항목을 버린다 |
| `block` | **호출자를 막는다.** 로그를 잃지 않지만 지연이 전파된다 |

**`logger_t<TCategory>`는 DI로 주입받는다.** category는 타입으로 구분한다.

## 7. HTTP hosting

**framework가 내장 HTTP 서버를 제공한다.** 계약은 [60](60-http-hosting.ko.md)·[61](61-embedded-http-server.ko.md)이
소유하고, public 타입은 [configuration과 host](interfaces/02-configuration-host.ko.md)가 소유한다. 여기서는
**시스템 구조에 걸리는 규칙**만 정리한다.

### 7.1 요청당 DI scope

**요청 하나가 scope 하나다.** route handler와 middleware는 **같은 요청 scope의 provider**를
받는다. 요청이 끝나면 그 scope의 `scoped`·`transient` 인스턴스를 정리한다(§4.6).

### 7.2 Middleware 실행 순서

**middleware는 `before`/`after` 쌍이다.** handler filter의 `next` delegate 방식과 다르다
([framework API §8.1](../../../06-framework-api.ko.md#81-handler-filter)).

| 단계 | 순서 |
|---|---|
| `before` | **등록 순서대로** |
| route handler | — |
| `after` | **역순으로** |

**`after`는 `before`를 실행한 middleware에 대해서만 실행된다.**

## 8. Handler 등록과 filter

handler 등록 표면과 filter 계약은 [channel messaging §3](interfaces/03-channel-messaging.ko.md#3-handler-registry)이
소유한다. filter의 언어 중립 의미는
[framework API §8.1](../../../06-framework-api.ko.md#81-handler-filter)이 소유한다.

## 9. 기능 등록

각 기능의 등록 표면은 [기능별 exact interface](interfaces/README.ko.md)가 소유한다.

| 기능 | 절 |
|---|---|
| channel | §7 Channel Builder |
| SPOT · actor | §11 Spot Framework API와 Instance Spot 등록·호출 |
| STREAM | §12 Hosted Service와 Module |
| HTTP | [60](60-http-hosting.ko.md) · [61](61-embedded-http-server.ko.md) |
| monitoring · location | §13 Configuration과 Logging |

**startup validation의 항목은 공통 스펙이 소유한다** — [channel-messaging §4](../../../08-channel-messaging.ko.md),
[spot-messaging §8](../../../12-spot-messaging.ko.md), [stream-session §7.2](../../../19-stream-session.ko.md),
[runtime-monitoring §6](../../../24-runtime-monitoring.ko.md).

**C++은 모든 위반을 host 시작 전에 실패로 만든다.** 오류는 예외가 아니라
`result_t`/`framework_exception_t` 경계 규약을 따른다([common runtime](interfaces/01-common-runtime.ko.md)).

## 10. 회귀 테스트

등록과 startup validation의 회귀 항목은
[regression-test-matrix](../../../../../cpp/internals/regression-test-matrix.ko.md)가 소유한다.
