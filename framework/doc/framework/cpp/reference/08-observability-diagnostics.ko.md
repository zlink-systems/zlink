# 08. Observability diagnostics

[레퍼런스 목차](README.ko.md)

이 category는 trace·metric·log 기록 수준을 구성하는 `dispatch_options_t`/
`dispatch_diagnostics_options_t`, host·topology 상태를 읽는 `framework_runtime_t`, structured
logging을 구성하는 `logging_builder_t`, 그리고 모든 category의 실패를 판단하는
`framework_error_kind_t` 대응표를 다룬다. 정확한 signature는
[Monitoring exact interface](../../common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md)와
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md)가
소유한다.

---

## `configure_dispatch().diagnostics` (구성 시점)

Trace·metric 기록 수준과 sampling을 설정한다.

```cpp
options.configure_dispatch()
  .message_flow(zlink::framework::message_flow_log_mode_t::key_transitions)
  .trace_sample_rate(0.1)
  .include_message_sizes(true);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.message_flow(message_flow_log_mode_t)` | `off` | `off`/`errors_only`/`key_transitions`/`verbose`/`diagnostic` 중 기록할 상세도 |
| `.trace_sample_rate(double)` | 구현 기본값 | `0.0`..`1.0`. NaN이거나 범위를 벗어나면 configuration error |
| `.include_message_sizes(bool)` | `false` | Payload 크기 분포를 telemetry에 포함할지 여부. Payload 내용 자체는 절대 기록하지 않는다 |
| `.trace_log_file(path)` | 없음 | Diagnostics 기록을 남길 파일 경로 |
| `.trace_label(id)` | 없음 | Diagnostics 기록에 붙일 label |
| `.set_message_flow_observer(shared_ptr<message_flow_observer_t>)` / `.set_message_flow_observer(function<void(const message_flow_event_t&)>)` | 없음 | `message_flow_event_t`(§2)를 받는 observer 등록 |
| `.message_flow_live(shared_ptr<atomic<message_flow_log_mode_t>>)` | 없음 | 실행 중 mode 변경을 위한 공유 atomic 연결. `app_t::set_message_flow_mode(...)`가 이 값을 갱신한다 |

각 modifier는 `dispatch_options_t`를 반환하는 동기 fluent 호출이다 — 반환값 없는 등록이 아니다.

**완료 결과.** Trace·metric·log 기록 대상(exporter, 원격 backend)은 application이 별도로 구성한다.
`send`와 `publish`는 reply path가 없으므로 unhandled 정책에 `reply_error`를 쓸 수 없다.

**선택 기준.** Startup 시점에 기본 기록 수준을 정할 때 쓴다. 실행 중 level만 바꾸려면
host-lifecycle category의 `app_t::set_message_flow_mode`를 쓴다.

---

## `framework_runtime_t::status` / `observe` (읽기·관찰)

Host 전체 상태(lifecycle state, relocation·termination 결과, inbound dispatch backpressure)를
조회하거나 관찰한다.

```cpp
zlink::framework::framework_runtime_status_t status = framework_runtime.status();
bool can_accept_new_operations = status.is_ready && status.accepting_work;

auto observation = framework_runtime.observe(
  /*capacity=*/64,
  [](const auto &observed) {
      // observed.status.inbound_dispatch, observed.status.state를 확인한다
  });
```

**옵션.** 이 진입점에는 modifier가 없다.

**완료 결과.** `status()`는 즉시 값을 반환하는 동기 호출이다. `observe(...)`는
`observed_status_t<framework_runtime_status_t>`를 콜백으로 전달하며 `loss` field로 관찰 유실
여부를 판단한다. `framework_runtime_status_t::inbound_dispatch`(`inbound_dispatch_status_t`)로
application HWM 사용량과 backpressure 상태를 확인한다.

**선택 기준.** Host 전체의 lifecycle 상태나 inbound backpressure를 진단할 때 쓴다. 특정
MeshName·ChannelName의 가용성은 topology-discovery category의 상태 조회 항목을 쓴다.

---

## Logging 구성 (`app_t::logging()`, 구성 시점)

Structured logging provider(console, file, callback sink)를 구성한다.

```cpp
app.logging()
  .use_console()
  .use_rotating_file("logs/app.log")
  .set_min_level(zlink::framework::log_level_t::info)
  .use_async();
```

**옵션.** 자주 쓰는 modifier는 다음과 같다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.use_console()` | 비활성 | 표준 출력에 로그 기록 |
| `.use_file(path)` / `.use_rotating_file(path, options)` | 없음 | 파일에 로그 기록. Rotating 옵션은 `max_file_size`/`max_files` |
| `.use_callback_sink(sink)` / `.use_provider(name, sink)` | 없음 | Application이 제공한 sink로 로그 전달 |
| `.use_async(options)` | 동기 | Async queue(용량·overflow 정책)를 통해 로그 기록 |
| `.set_min_level(log_level_t)` | 구현 기본값 | 이 레벨 미만은 기록하지 않는다 |

**완료 결과.** 반환값 없이 동기로 등록된다. Handler는 `logger_t<THandler>`를 dependency로 선언해
category logger를 DI로 주입받는다 — 별도 service 등록이 필요 없다.

**선택 기준.** 표준 logging provider와 health 표면을 구성할 때 쓴다. Handler별 custom category가
필요하면 `logger_factory_t`를 dependency로 받아 만든다.

---

## `framework_error_kind_t` 대응표

Framework operation이 실패하면 `framework_exception_t::kind()`로 원인 계열을 판단한다. 이 표는
모든 category의 완료 kind 설명이 공유하는 근거다.

| Kind | Application에서 확인할 내용 |
| --- | --- |
| `not_found` | 요청한 Actor, Spot, handler, route 또는 target이 존재하는지 확인한다 |
| `already_exists` | create와 registration이 멱등하게 처리되어야 하는지 확인한다 |
| `type_mismatch` | stable type과 요청한 application type이 일치하는지 확인한다 |
| `not_configured` | 필요한 role, handler, Store 또는 object client가 startup에 등록되었는지 확인한다 |
| `rejected` | Typed 결과가 없는 Framework admission, filter 또는 runtime policy가 operation을 거부했다 |
| `unavailable` | target, route, Store 또는 worker가 현재 operation을 처리할 수 없다 |
| `capacity_exceeded` | placement, queue 또는 bounded resource의 여유가 없다 |
| `deadline_exceeded` | operation이 정한 deadline 안에 완료되지 않았다. 결과의 side effect 여부는 해당 operation 계약을 따른다 |
| `shutting_down` | runtime이 신규 admission을 받지 않는 상태다. 다른 serving instance를 사용해야 한다 |
| `protocol_error` | peer와 protocol 또는 reply 계약이 일치하는지 확인한다 |
| `invalid_operation` | 현재 object·session·runtime 상태에서는 요청한 operation이 허용되지 않는다 |
| `data_lost` | 공개된 relocation payload를 찾을 수 없거나 검증에 실패했다. 이전 owner로 임의 rollback하지 않는다 |
| `internal_failure` | 위 분류로 표현할 수 없는 Framework 실패다. Log와 trace의 correlation 정보로 원인을 확인한다 |

**완료 결과.** `framework_exception_t`는 Framework만 생성하며 `what()`은 사람이 진단하기 위한
설명이지 programmatic 분기 대상이 아니다. `code()`는 timeout이나 transport처럼 platform 원인이
있을 때 진단 정보를 추가하지만 공통 오류 분류를 대신하지 않는다. Configuration 검증 실패(startup
전 `std::invalid_argument` 등)와 인자 오류는 이 kind 분류와 다른 층이다. 재시도 여부는 이 kind가
알려주지 않는다 — operation의 완료 조건, idempotency와 업무 상태를 확인해 application이 직접
판단한다.

**선택 기준.** 각 category 항목의 "완료 결과"에 나온 kind를 이 표로 되짚어 대응 방법을 정할 때
쓴다.

---

전체 근거는
[Monitoring exact interface](../../common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md)와
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md)를
참고한다.
