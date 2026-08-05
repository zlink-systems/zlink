---
title: "18. DI 컨테이너 · C++"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: 3. 핵심 개념](03-concepts.ko.md) | [다음: 19. Configuration](19-configuration.ko.md)
<!-- framework-adapter-nav:end -->

# 18. DI 컨테이너

> **이 장의 계약 소유 문서** — [C++ configuration과 host 공개 계약](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md)이
> 다룬다. 이 챕터는 C++에만 있는 내장 DI 컨테이너의 등록·해석 방법을 설명한다.

프레임워크는 ASP.NET Core 스타일의 DI(의존성 주입) 컨테이너를 내장한다.
`service_collection_t`에 서비스를 등록하고, `service_provider_t`로 꺼낸다.
핸들러는 `dependency_types`만 선언하면 프레임워크가 생성자 인자를 자동으로 주입한다.

## 1. 수명(lifetime) 세 가지

| 수명 | 등록 | 인스턴스 수 |
|------|------|------------|
| **singleton** | `add_singleton<T>()` | 앱 전체에서 1개 |
| **scoped** | `add_scoped<T>()` | 실행 컨텍스트(scope)마다 1개 |
| **transient** | `add_transient<T>()` | resolve할 때마다 새 인스턴스 |

`service_scope_kind_t`는 Framework가 만드는 scope의 용도를 구분하는 enum이다.
Application은 scope를 직접 만들지 않는다. Framework가 handler dispatch, STREAM session과
Spot activation 경계에서 알맞은 scope를 만들고 정리한다. Actor payload는 containing Spot
member function으로 처리하며 별도 Actor handler 등록 표면이나 public handler class를 만들지 않는다.

| scope 종류 | 수명 범위 |
|-----------|----------|
| `handler_invocation` | HTTP 요청 하나를 처리하는 handler 호출 |
| `stream_session` | stream 연결 하나의 수명 |
| `spot_activation` | spot 활성화 용도 |
| `entry_spot` | entry spot 용도 |
| `actor_creation` | actor 생성 용도 |

## 2. 등록 방법

### 기본 생성자로 등록

```cpp
// T()로 생성 가능한 타입
options.services ().add_singleton<season_store_t> ();
options.services ().add_transient<request_counter_t> ();
```

### 의존성 주입으로 등록 — 생성자 인자를 컨테이너가 resolve

```cpp
// add_singleton<T, Dep1, Dep2, ...>() — 생성자 T(Dep1&, Dep2&, ...) 가 있어야 한다
options.services ()
    .add_singleton<bingo_room_allocator_t> ()
    .add_singleton<agent_availability_directory_t> ()
    .add_singleton<agent_assignment_service_t,
                   bingo_room_allocator_t,
                   agent_availability_directory_t> ();
//                 ↑ 생성자에서 두 deps를 레퍼런스로 받는다
```

생성자 시그니처 규칙: **의존성은 `T&` 레퍼런스로** 받아야 한다. 값이나 포인터는 안 된다.

```cpp
class agent_assignment_service_t
{
  public:
    // 컨테이너가 이 생성자를 호출한다
    agent_assignment_service_t (bingo_room_allocator_t &allocator,
                                agent_availability_directory_t &availability)
        : _allocator (allocator), _availability (availability) {}
};
```

### 미리 만든 인스턴스 등록

```cpp
auto topology = std::make_unique<sample_topology_t> (config);
options.services ().add_singleton<sample_topology_t> (std::move (topology));
```

외부에서 초기화가 필요한 객체 — 설정을 파싱해서 넣어야 하는 topology, 연결 문자열이 필요한 외부 클라이언트 — 에 쓴다.

### 팩토리 람다로 등록

```cpp
options.services ().add_factory<http_client_t> (
    [] (zlink::framework::service_provider_t &provider) {
        auto &config = provider.get_required<connection_config_t> ();
        return std::make_unique<http_client_t> (config.base_url, config.timeout);
    },
    zlink::framework::service_lifetime_t::singleton);
```

생성자 주입으로 표현하기 어려운 복잡한 초기화 로직에 쓴다.

## 3. 핸들러 자동 주입

채널·HTTP 핸들러는 `dependency_types`를 선언하면 dispatch scope에서 생성자 주입을
받는다. 별도로 `add_transient<T>()`를 호출할 필요가 없다. Spot packet과 Actor payload
handler는 Spot member function이므로 DI handler class로 등록하지 않는다. 별도 class인
timer handler는 Spot activation마다 한 번 만들고, 같은 activation의 timer tick에서
재사용한다. Timer handler의 `dependency_types`도 Spot activation scope에서 resolve한다.

```cpp
class create_game_http_handler_t
{
  public:
    using request_type = create_game_http_req_t;
    using reply_type   = create_game_http_res_t;
    static constexpr const char *topic_name = "CreateGame";

    // 1. 의존할 타입들을 순서대로 선언
    using dependency_types =
      zlink::framework::dependency_list_t<
        zlink::framework::request_client_t,
        zlink::framework::logger_t<create_game_http_handler_t>>;

    // 2. 선언 순서대로 생성자가 받는다
    explicit create_game_http_handler_t (
        zlink::framework::request_client_t &client,
        zlink::framework::logger_t<create_game_http_handler_t> &logger)
        : _client (client), _logger (logger) {}

    create_game_http_res_t handle (const create_game_http_req_t &request);

  private:
    zlink::framework::request_client_t &_client;
    zlink::framework::logger_t<create_game_http_handler_t> _logger;
};
```

핸들러는 요청마다 새 인스턴스로 만들어지므로(transient), 주입받은 서비스 참조는
핸들러 수명 안에서 사용한다.

## 4. 프레임워크 내장 서비스

앱 실행 시 프레임워크가 등록하거나, `dependency_types` 처리 중 자동 등록하는 서비스들이다.
필요한 타입을 `dependency_types`에 넣으면 생성자 주입으로 받을 수 있다.

| 서비스 | 설명 |
|--------|------|
| `request_client_t` | 채널 요청 송신 — `request(mesh, channel, msg).submit<TReply>()` |
| `logger_t<TOwner>` | 소유 타입 이름으로 태그된 로거 — `_logger.info(...)` |
| `session_actor_manager_t` | stream session에서 actor 생성·조회·바인딩 |
| `logger_factory_t` | `create("category")` — 카테고리를 동적으로 정할 때(`create<TCategory>()`는 타입명 기반) |

`logger_t<T>`는 `T`의 타입명을 로그 소스 이름으로 자동 태그한다 — 한 앱에 여러 핸들러가 있어도 로그 소스를 구분할 수 있다.

## 5. hosted_service_t에서 직접 꺼내기

앱 수명주기 서비스(`hosted_service_t`)는 `start(service_provider_t &services)` 시점에 컨테이너에서 직접 꺼낸다. 필요한 서비스가 이미 등록되어 있어야 한다.

```cpp
class season_scheduler_t : public zlink::framework::hosted_service_t
{
  public:
    void start (zlink::framework::service_provider_t &services) override
    {
        // start 시점에 컨테이너에서 꺼낸다 — 이전에는 null
        _store = &services.get_required<season_store_t> ();
        _worker = std::thread ([this] { run_schedule (); });
    }
    void stop () noexcept override
    {
        _running = false;
        if (_worker.joinable ()) _worker.join ();
    }

  private:
    season_store_t *_store = nullptr;
    std::atomic<bool> _running{true};
    std::thread _worker;
};
```

## 6. 수명 선택 가이드

| 상황 | 권장 수명 |
|------|----------|
| 공유 상태가 없는 순수 설정·읽기 전용 객체 (topology, config) | **singleton** |
| 연결·클라이언트 같이 앱 전체에서 재사용하는 인프라 | **singleton** + 내부 thread-safe 구현 |
| 요청 단위로 격리해야 하는 상태 (트랜잭션, per-request context) | **scoped** |
| 채널·HTTP 핸들러 — 요청마다 새 인스턴스 필요 | **transient** (dependency_types로 자동 등록) |
| 가변 도메인 상태 (게임 룸, 대화 상태) | **DI 아님 — SPOT으로 관리** ([6장](06-spot.ko.md)) |

## 7. 수명 충돌 주의 — captive dependency

**singleton이 scoped/transient를 주입받으면 안 된다.** singleton은 앱 전체 수명을 가지므로,
더 짧은 수명의 서비스를 참조하면 scoped 객체가 scope 종료 뒤에도 참조될 수 있고,
transient 객체는 요청마다 새로 만들려던 의미가 singleton에 포획되어 사라진다.

```cpp
// 잘못된 예 — singleton이 transient를 참조
options.services ()
    .add_transient<conversation_context_t> ()
    .add_singleton<support_service_t, conversation_context_t> ();
//   ↑ singleton이 transient를 "포획(captive)"해버린다
//     singleton은 처음 주입받은 conversation_context_t 참조를 계속 들고 있게 된다

// 올바른 예
options.services ()
    .add_singleton<conversation_context_t> ()     // 공유해도 안전하면 singleton으로
    .add_singleton<support_service_t, conversation_context_t> ();
```

규칙: **등록하는 서비스의 수명은 주입받는 의존성의 수명을 초과하면 안 된다**.

## 8. singleton 서비스의 동시 접근

singleton은 worker 풀의 여러 스레드가 동시에 접근한다. 가변 상태가 있으면
자체 동기화가 반드시 필요하다.

```cpp
class agent_availability_directory_t
{
  public:
    void set_available (const std::string &agent_id, bool available)
    {
        std::lock_guard lock (_mutex);   // 동시 write 보호
        _availability[agent_id] = available;
    }

    bool is_available (const std::string &agent_id) const
    {
        std::shared_lock lock (_mutex);  // 동시 read 허용
        auto it = _availability.find (agent_id);
        return it != _availability.end () && it->second;
    }

  private:
    mutable std::shared_mutex _mutex;
    std::unordered_map<std::string, bool> _availability;
};
```

읽기 전용 singleton (topology, 설정 struct)은 `const` 메서드만 노출하면 락 없이 안전하다.

## 9. 관련 문서

- 정식 계약: [C++ configuration과 host 공개 계약](../../../common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md)
- 설정 값 읽기: [19. Configuration](19-configuration.ko.md)
- 주입받는 타입 목록: [13. 주요 타입 사용 색인](13-interface-catalog.ko.md)
