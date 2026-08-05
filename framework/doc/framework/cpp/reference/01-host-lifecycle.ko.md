# 01. Host lifecycle

[레퍼런스 목차](README.ko.md)

이 category는 `app_t`가 제공하는 host 등록·이전·종료·조회 진입점과 `app_t::health()`가 제공하는
health 등록을 다룬다. 정확한 signature는
[Configuration과 host exact interface](../../common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md)와
[Monitoring exact interface](../../common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md)가
소유한다.

---

## `app_t::add_zlink_framework` (구성 시점)

Framework root를 앱에 한 번 등록한다. 다른 모든 항목의 전제 조건이다.

```cpp
zlink::framework::app_t app = zlink::framework::app_t::create();

app.add_zlink_framework([&](zlink::framework::zlink_framework_options_t &options) {
    auto play = options.add_route_mesh("play")
      .listen(5501)
      .set_automatic_routing_id_prefix("play")
      .set_placement_weight(100);
});

return app.run(argc, argv);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `configure: std::function<void(zlink_framework_options_t &)>` | 필수 | topology, handler, Location Store 등 모든 등록의 진입점이다 |
| `add_zlink_framework<TModule, TArgs...>(args...)` | — | `module_t`를 구현한 타입을 생성자 인자와 함께 등록하는 overload. `module_t::configure(options)`가 위 콜백과 같은 일을 한다 |

**완료 결과.** 반환값 없이 동기로 등록된다. `app.run(argc, argv)`가 network bind 전 구성을
검증하고, 실패하면 configuration error로 startup 자체를 실패시킨다 — 잘못된 구성이 message 처리
중에 처음 나타나지 않는다.

**선택 기준.** 모든 host가 정확히 한 번 호출한다. `zlink_framework_options_t`의 topology·handler
등록 세부는 topology-discovery category를 참고한다.

---

## `relocate`

현재 host가 들고 있는 stateful object(User Spot·Actor)를 다른 eligible node로 이전한다. 계획된
점검이나 rolling update 전에 호출한다.

```cpp
zlink::framework::relocation_options_t options{
    .mode = zlink::framework::relocation_mode_t::rolling_update,
    .target_application_version = 2,
    .deadline = std::chrono::minutes{5},
};

zlink::framework::relocation_result_t result = co_await app.relocate(options);

if (result.outcome == zlink::framework::relocation_outcome_t::relocated) {
    co_await app.shutdown();
}
```

**옵션.** `relocation_options_t`의 field는 다음과 같다.

| Field | 기본값 | 의미 |
| --- | --- | --- |
| `mode` | 필수 | `planned_maintenance`(source와 같은 application version만 target) 또는 `rolling_update`(지정한 version만 target) |
| `target_application_version` | `planned_maintenance`에서는 지정하지 않음(source 값 사용), `rolling_update`에서는 필수 | 목표 application version. 조합이 맞지 않으면 시작 전 `std::invalid_argument`로 거부한다 |
| `deadline` | 없으면 30초 | eligible target 수렴을 기다리는 상한 |
| `wait_cancellation`(`relocate(options, stop_token)`) | 없음 | waiter만 중단한다. 이미 시작한 shared operation은 취소하지 않는다 |

**완료 결과.** `relocation_result_t::outcome`이 `relocated`면 모든 object 이전이 끝나고 host는
`relocated` 상태가 된다(새 operation은 받지 않지만 infrastructure는 유지한다). `blocked`면
`reason`에 `target_unavailable`·`store_unavailable`·`deadline_exceeded` 등이 담기고, host는 처리
중이던 local object가 남아 있으면 `serving`으로 복귀한다.

**선택 기준.** 배포 전 무중단 이전이 필요할 때 쓴다. 이전 없이 바로 종료하려면 `shutdown`을
직접 호출한다. 같은 `relocation_options_t`로 중복 호출하면 진행 중인 operation에 합류하고, 다른
값으로 호출하면 `blocked/operation_in_progress`로 완료한다.

---

## `shutdown`

Host를 종료한다. Relocation을 시작하지 않는다 — 이전이 필요하면 먼저 `relocate`를 호출한다.

```cpp
zlink::framework::termination_result_t result =
  co_await app.shutdown(std::chrono::seconds{30});
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `deadline` | 30초 | 종료 정리 상한. 초과하면 `force_stopped`로 완료한다 |
| `wait_cancellation` | 없음 | waiter만 중단한다 |

**완료 결과.** `termination_result_t::outcome`이 `stopped`(정상 정리) 또는
`force_stopped`(deadline 초과·정리 실패)다. `serving`에서 호출하면 남은 application 처리와
resource를 정리하고, `relocated`에서 호출하면 infrastructure 연결만 정리한다. 두 경우 모두 끝나면
`stopped` 상태가 된다.

**선택 기준.** Host를 종료할 때 항상 호출한다. `relocating` 도중 호출하면 진행 중인 atomic
relocation unit의 결과만 확정하고 나머지는 시작하지 않는다 — 그 relocation을 기다리던 호출자는
`blocked/shutdown_requested`를 받는다.

---

## `run` / `stop` / `request_stop`

Host process를 실행하고 정지 신호를 보낸다.

```cpp
int exit_code = app.run(argc, argv); // block하며 host lifecycle 전체를 실행한다

// 다른 thread나 signal handler에서 정지를 요청하는 경우
app.request_stop();
```

**옵션.** 세 호출 모두 modifier가 없다.

**완료 결과.** `run(argc, argv)`는 host가 완전히 정지한 뒤 process exit code로 쓸 수 있는
`int`를 반환하는 blocking 호출이다. Handler 예외, runtime 오류, signal shutdown은 host가 수집하고
종료 경로를 닫는다. `stop()`은 즉시 정지를 시작하고, `request_stop()`은 graceful shutdown 신호만
보낸다.

**선택 기준.** `run`은 일반적인 `main()` 진입점에서 한 번 호출한다. `request_stop`은 signal handler
같은 외부 코드에서 host 종료를 시작할 때 쓴다.

---

## `is_ready` / `set_message_flow_mode` (읽기·변경)

Host가 준비됐는지 확인하고, 실행 중 message-flow diagnostics mode를 바꾼다.

```cpp
bool ready = app.is_ready();
app.set_message_flow_mode(zlink::framework::message_flow_log_mode_t::verbose);
```

**옵션.** 이 진입점에는 두 개의 독립된 property가 있다.

| Property | 기본값 | 의미 |
| --- | --- | --- |
| `is_ready()` | — | `framework_runtime_t::status().state == serving`일 때만 `true` |
| `message_flow_mode()` / `set_message_flow_mode(...)` | `configure_dispatch().message_flow(...)`로 등록한 값 | 실행 중인 diagnostics 상세도 |

**완료 결과.** 두 호출 모두 동기 get/set이다. `is_ready()`가 `false`여도 `status()`(host-lifecycle이
아니라 `framework_runtime_t`, observability-diagnostics category 참고)로 더 자세한 상태를 확인할
수 있다.

**선택 기준.** 배포를 다시 하지 않고 특정 시점에만 diagnostics 상세도를 올리거나 내릴 때
`set_message_flow_mode`를 쓴다. Diagnostics 시작값 등록은 observability-diagnostics category의
`configure_dispatch()` 항목을 쓴다.

---

## Health 등록 (`app_t::health()`, 구성 시점)

Host readiness·liveness probe에 노출할 health check를 등록한다.

```cpp
app.health()
  .add_zlink_runtime_check()
  .add_channel_check("play.api")
  .add_location_check("location-store");

zlink::framework::health_report_t report = app.health().report();
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.add_zlink_runtime_check(name = "zlink.runtime")` | — | Framework runtime 자체의 상태를 check로 추가 |
| `.add_channel_check(name)` | — | 지정한 이름의 channel 가용성 check 추가 |
| `.add_location_check(name)` | — | Location Store 연결 check 추가 |
| `.add_stream_endpoint_check(name)` | — | STREAM listener 상태 check 추가 |
| `.add_hosted_service_check(name)` | — | hosted service 상태 check 추가 |
| `.set_status(name, status, message)` | — | application이 직접 판단한 상태를 이름으로 등록 |

**완료 결과.** `report()`는 동기 호출이며 `health_report_t`를 반환한다. `readiness`와 `liveness`를
분리해 판단하며, `degraded`는 `ready()`·`live()`를 막지 않는다.

**선택 기준.** HTTP `/health`, `/readiness`, `/liveness` endpoint(HTTP hosting 확장을 쓰는 경우
`http().map_health(...)` 등)나 외부 오케스트레이터가 조회할 상태를 구성할 때 쓴다.

---

전체 근거는
[Configuration과 host exact interface](../../common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md)와
[Monitoring exact interface](../../common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md)를
참고한다.
