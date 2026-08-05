---
title: "11. Monitoring — 상태 관측과 진단 · C++"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: Location](10-location.ko.md) | [다음: 운영 — 메트릭 · drain · readiness](12-operations.ko.md)
<!-- framework-adapter-nav:end -->

# 11. Monitoring — 상태 관측과 진단

> **이 장의 계약 소유 문서** — [C++ monitoring 공개 계약](../../../common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md)이
> 다룬다. 이 챕터는 그 계약이 노출하는 네 관측 표면을 사용법 중심으로 설명한다.

handler 호출만으로는 운영을 다 볼 수 없다. 연결이 준비되었는지, 어느 peer가 빠졌는지,
메시지가 어디서 실패했는지도 framework 표면에서 읽어야 한다. C++ framework는 이를 **네
갈래**로 노출한다 — 상태 snapshot과 observation, 메시지 흐름 관측, health check,
표준 logging provider다.

runtime event를 DI handler로 받는 표면은 없다. 관측은 전부 아래 네 갈래를 통한다.

## 1. 관측 표면

| 무엇을 보나 | 표면 | 어디서 다루나 |
|---|---|---|
| Host lifecycle(relocate·drain·readiness) | `framework_runtime_t::status ()` · `observe (...)` | [12. 운영](12-operations.ko.md) §6.1 |
| MeshNode의 node·peer·channel 준비 상태 | `route_mesh_runtime_t::snapshot (...)` · `observe (...)` | [12. 운영](12-operations.ko.md) §5 |
| Location store 상태와 topology | `location_runtime_query_t` | [10. Location](10-location.ko.md) §4 |
| 메시지 수신·dispatch·실패와 흐름 | `configure_dispatch ()`의 message flow | 이 챕터 §3 |
| readiness · liveness 판정 | `health_report_t` | 이 챕터 §4 |
| CCU·큐 깊이 같은 수치 | 표준 logging provider와 metric 규약 | [12. 운영](12-operations.ko.md) §1 |

네 갈래는 소비 방식이 다르다. **상태 표면**은 지금 값을 읽거나 변화를 순서대로 받을 때,
**메시지 흐름**은 개별 메시지가 어디서 어떻게 끝났는지 추적할 때, **health**는 로드
밸런서와 오케스트레이터에 붙일 때, **logging provider**는 이 셋을 밖으로 내보낼 때 쓴다.

## 2. 상태 snapshot과 observation

상태 표면은 모두 같은 모양이다 — `snapshot(...)`이 immutable 값 한 장을,
`observe(...)`가 그 이후 변화를 순서대로 준다.

```cpp
// 지금 값 한 장.
auto snapshot = mesh_runtime.snapshot ("game.room");
const bool ready = mesh_runtime.is_ready ("game.room");

// 변화 스트림. capacity를 넘기면 느린 관찰자는 건너뛴다.
auto observation = mesh_runtime.observe (
  "game.room", 64, [] (const mesh_node_snapshot_t &next) {
      // 변경 뒤의 완전한 snapshot이 온다. 바뀐 field만 오는 event가 아니다.
      record (next);
  });
```

**observation 객체가 구독 수명이다.** `observe(...)`가 돌려주는
`std::unique_ptr<mesh_runtime_observation_t>`를 살려 두는 동안만 callback이 호출된다.
버리면 구독이 끝난다. 로컬 변수로 받아 두고 잊으면 그 자리에서 구독이 사라진다.

callback은 **runtime 스레드에서 실행된다.** 안에서 blocking 호출을 하거나 다시 framework
표면을 부르지 않는다. 값만 꺼내 자기 자료구조에 넘기고 즉시 반환한다.

| | `snapshot (...)` | `observe (...)` |
| --- | --- | --- |
| 무엇을 주나 | 호출 시점의 값 하나 | 변경마다 완전한 값 |
| 언제 쓰나 | 운영 endpoint 응답, 단발 확인 | 상태 전이를 기록하거나 반응할 때 |
| 놓칠 수 있나 | 해당 없음 | capacity를 넘기면 중간 값을 건너뛴다 |

Peer 상태는 Node RID, 현재 상태와 사용할 수 없는 이유만 담는다. 재연결 시도 횟수나
socket 내부 상태는 공개 계약이 아니다.

## 3. 메시지 흐름 추적

메시지 하나가 어디서 어떻게 끝났는지는 message flow로 본다. 수준은 `configure_dispatch ()`가
정한다.

```cpp
options.configure_dispatch ()
  .message_flow (message_flow_log_mode_t::errors_only) // 기본값 — error와 backpressure만.
  .trace_sample_rate (1.0)                            // 표본 비율.
  .include_message_sizes (true)                       // payload byte를 함께 남긴다.
  .trace_log_file ("logs/flow.jsonl");                // 앱 로그와 분리해 따로 쓴다.
```

| 수준 | 남기는 것 |
| --- | --- |
| `off` | 남기지 않는다 |
| `errors_only`(기본) | dispatch 실패와 backpressure |
| `key_transitions` | 위 + 수신·dispatch·완료 같은 주요 전이 |
| `verbose` | 위 + 개별 메시지 단위 기록 |

**운영에서는 `errors_only`로 두고 필요할 때만 올린다.** `verbose`는 메시지마다 기록을
남기므로 처리량이 많은 구간에서 그 자체가 부하가 된다.

기록을 프로그램에서 받으려면 observer를 등록한다.

```cpp
class flow_recorder_t : public message_flow_observer_t
{
  public:
    void on_message_flow (const message_flow_event_t &event) override
    {
        // outcome · surface · packet_name · flow_id 등을 자기 저장소로 넘긴다.
        // 이 callback도 runtime 스레드에서 실행된다 — blocking 금지.
        _sink.append (event.outcome, event.packet_name.value_or ("-"));
    }
};

options.configure_dispatch ().set_message_flow_observer (
  std::make_shared<flow_recorder_t> (sink));

// 짧은 기록이라면 class 대신 함수 하나를 넘겨도 된다.
options.configure_dispatch ().set_message_flow_observer (
  [&sink] (const message_flow_event_t &event) { sink.append (event.outcome); });
```

`flow_id`와 `flow_origin`은 한 요청이 여러 node를 거칠 때 그 조각들을 하나로 묶는
식별자다. 상관관계 규칙은
[흐름 상관관계](../../../common/spec/27-flow-correlation.ko.md)가 소유한다.

## 4. Health check

readiness와 liveness는 `health_report_t` 하나로 판정한다. HTTP hosting을 쓰면
endpoint에 바로 연결한다.

```cpp
options.http ()
  .listen ("http://0.0.0.0:8080")
  .map_readiness ("/healthz/ready")   // report.ready ()  — readiness != unhealthy
  .map_liveness ("/healthz/live");    // report.live ()   — liveness  != unhealthy
```

| 판정 | 뜻 | 로드 밸런서 동작 |
| --- | --- | --- |
| `healthy` | 정상 | 트래픽을 보낸다 |
| `degraded` | 일부 기능이 떨어졌지만 처리는 된다 | 계속 보낸다 |
| `unhealthy` | 처리할 수 없다 | 대상에서 뺀다 |

**scope를 나눠 등록한다.** `readiness`는 "지금 트래픽을 받아도 되나", `liveness`는
"프로세스를 재시작해야 하나"다. Location store 연결처럼 일시적으로 끊길 수 있는 의존성은
readiness에만 넣는다 — liveness에 넣으면 store가 잠깐 끊겼을 때 오케스트레이터가
프로세스를 죽인다.

## 5. Structured logging

runtime 상태 변화와 진단은 표준 logging provider로 나간다. provider 구성은
[19. Configuration](19-configuration.ko.md)이 다룬다.

공개 계약이 **아닌** 것들이 있다. 아래를 직접 소비하는 코드를 쓰지 않는다.

- socket · Spot · Actor · STREAM별 raw event DTO
- raw event handler와 source 등록 builder
- metric sample DTO와 application callback
- exporter lifecycle, registry와 provider 내부 상태

metric 이름·종류·단위·label은
[Runtime metric과 집계 규칙](../../../common/spec/25-runtime-metrics.ko.md)이 소유한다.

## 6. 자주 발생하는 문제

- **`observe(...)`를 걸었는데 callback이 오지 않는다** → 돌려받은 observation 객체를
  버렸을 가능성이 크다. 구독 수명이 그 객체다. 멤버로 보관한다.
- **callback 안에서 데드락이 난다** → runtime 스레드에서 실행된다. 안에서 framework
  표면을 다시 부르거나 blocking 대기를 하지 않는다.
- **상태 전이 일부가 안 보인다** → `observe(...)`의 capacity를 넘겨 건너뛴 것이다.
  모든 전이가 필요하면 capacity를 늘리고 callback을 더 빨리 반환시킨다.
- **flow 기록이 비어 있다** → 기본 수준이 `errors_only`라 정상 흐름은 남지 않는다.
  `key_transitions` 이상으로 올린다.
- **store가 잠깐 끊겼는데 프로세스가 재시작된다** → store 의존성이 liveness에 들어가
  있다. readiness로 옮긴다.

## 7. 관련 문서

- 정식 계약: [C++ monitoring 공개 계약](../../../common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md)
- 메트릭과 drain·readiness 운영: [12. 운영](12-operations.ko.md)
- logging provider 구성: [19. Configuration](19-configuration.ko.md)
- HTTP endpoint 등록: [20. HTTP Hosting](20-http-hosting.ko.md)
