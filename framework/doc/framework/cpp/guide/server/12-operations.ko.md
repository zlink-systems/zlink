---
title: "12. 운영 — 런타임 메트릭 · graceful drain · readiness · C++"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/12-operations.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 12. 운영 — 런타임 메트릭 · graceful drain · readiness

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: 17. ZLink의 적용 범위 — 내부 서비스 통신과 실시간 상태 서버](17-alternative.ko.md) | [다음: 옵션과 기본값](16-options.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — **C++** · [C#/.NET](../../../dotnet/guide/server/12-operations.ko.md) · [Java](../../../java/guide/server/12-operations.ko.md) · [Kotlin](../../../kotlin/guide/server/12-operations.ko.md) · [Node/TypeScript](../../../node/guide/server/12-operations.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

> **이 장의 계약 소유 문서** — 공통 스펙
> [Runtime 상태 조회와 운영 진단](../../../common/spec/server/06-observability/01-runtime-monitoring.ko.md),
> [runtime 메트릭](../../../common/spec/server/06-observability/02-runtime-metrics.ko.md)과
> [Graceful Drain & Handoff](../../../common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md)가 소유한다.
> 언어별 표면의 정식 정의는
> [언어별 topology·monitoring 공개 계약](../../../common/spec/server/languages/README.ko.md)이
> 소유한다.
> 이 챕터는 운영 환경에서 실제로 무엇을 붙이고 무엇을 선언하는지 사용법 중심으로 다룬다.

## 0. 제공하는 기능

서비스를 운영에 올리면 `11. Monitoring` 장의 이벤트 관측 외에 다음 항목이
더 필요하다.

1. **메트릭** — CCU, queue 깊이, 요청 지연 같은 수치를 대시보드로 본다.
2. **graceful drain** — 배포·축소로 node를 내릴 때 접속 유저를 튕기지 않고 정리한다.
3. **readiness** — "이 node가 새 요청을 받아도 되는가"를 배포 인프라에 알린다.

framework는 메트릭 계기와 host 종료 시의 drain 절차를 제공한다. application은 meter 이름을 수집
파이프라인에 넣고, 배포 환경이 호출할 readiness endpoint를 공개 runtime 조회 API로 구성한다.

처음 나오는 용어는 다음과 같다.

| 용어 | 한 줄 풀이 |
|---|---|
| Meter / 계기(instrument) | 언어 표준 메트릭 방출 단위. counter·gauge·histogram이 계기다 |
| OpenTelemetry(OTel) | 메트릭·트레이스 수집 표준. Prometheus 등 exporter로 내보낸다 |
| Relocate | stateful object를 compatible target으로 이전하고 host를 `Relocated` 상태로 만드는 operation |
| Shutdown | 새 relocation 없이 local resource를 bounded cleanup하는 operation |
| readiness probe | "새 요청을 받아도 되는가"를 묻는 배포 인프라의 상태 확인 |

## 1. 런타임 메트릭

framework는 `"zlink.framework"` 하나를 정식 meter 이름으로 삼아
모든 계기를 방출한다. application은 이 정식 meter 이름을 수집 파이프라인에 등록한다.

```cpp
// C++은 언어 표준 메트릭 파이프라인이 없으므로 monitoring 표면에서 직접 읽어
// 쓰는 exporter에 넘긴다. 계기 이름은 다른 언어와 같다.
auto status = runtime.status ();
// status의 inbound_dispatch 값을 앱의 exporter로 내보낸다.
```

- zlink 전용 메트릭 API는 없다. 각 언어의 표준 메트릭 API가 그대로 표면이다.
  OTel 없이 수집하려면 `MeterListener`에서 meter 이름 `"zlink.framework"`를 직접 구독한다.
- 어떤 listener도 붙지 않으면 계기 갱신은 최소 비용의 비활성 경로로 끝난다. 계기를
  등록만 해 두고 켜지 않아도 messaging 성능에 영향이 없다.
- 대시보드와 exporter 선택은 application 몫이다. framework는 내장 scrape 서버를 두지 않는다.

계기 카탈로그는 다음과 같다. MeshNode, object·STREAM, location·fanout 계기의 라벨·단위·종류는
[Runtime Metrics §§3~5](../../../common/spec/server/06-observability/02-runtime-metrics.ko.md)가 정하고, drain 계기는
[Host relocation 전체 흐름 §13](../../../common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md#16-관측-정보)이 정한다.

| 계기 | 무엇을 재나 |
|---|---|
| `zlink.stream.connections.active` | 활성 STREAM 연결 수(CCU) |
| `zlink.stream.connections.opened` | 누적 STREAM 연결 시작 수 |
| `zlink.stream.connections.closed` | 누적 STREAM 연결 종료 수 |
| `zlink.spot.count` | 활성 spot 수 |
| `zlink.actor.count` | 활성 Actor 수 |
| `zlink.relocation.started` | Actor·User·Instance Spot relocation 시작 누계 |
| `zlink.relocation.completed` | relocation terminal 결과 누계 |
| `zlink.relocation.duration` | prepare부터 terminal phase까지의 시간 |
| `zlink.relocation.bytes` | 이동한 relocation payload의 encoded 크기 |
| `zlink.instance_spot.activations` | Instance Spot activation 결과 누계 |
| `zlink.instance_spot.activation.duration` | 첫 주소 확인부터 Ready 또는 terminal 실패까지의 시간 |
| `zlink.instance_spot.pending.messages` | activation barrier 앞에서 기다리는 message 수 |
| `zlink.instance_spot.pending.bytes` | activation barrier 앞에서 예약한 payload byte 수 |
| `zlink.instance_spot.claim.conflicts` | Instance location claim 충돌 누계 |
| `zlink.mesh_node.peers.configured` | descriptor에 존재하는 peer 수 |
| `zlink.mesh_node.peers.connected` | transport가 연결된 peer 수 |
| `zlink.mesh_node.peers.ready` | admission과 handler readiness를 통과한 peer 수 |
| `zlink.mesh_node.channels.ready_members` | ChannelName select-one에 사용할 수 있는 member 수 |
| `zlink.mesh_node.channel.selection_failures` | Select-one에 사용할 member가 없었던 횟수 |
| `zlink.mesh_node.requests.inflight` | reply를 기다리는 request 수 |
| `zlink.mesh_node.request.duration` | request submit부터 terminal completion까지의 시간 |
| `zlink.mesh_node.request.timeouts` | request timeout 누계 |
| `zlink.mesh_node.messages.dropped` | Framework가 원인을 확인한 one-way drop 누계 |
| `zlink.fanout.published` | classic fanout publish 누계 |
| `zlink.fanout.received` | classic fanout receive 누계 |
| `zlink.fanout.dropped` | Framework가 원인을 확인한 classic fanout drop 누계 |
| `zlink.location.store.errors` | Redis read·write·lease failure 누계 |
| `zlink.location.owner_lease.renew.failures` | owner lease 갱신 실패 누계 |
| `zlink.location.owner_lease.renew.lateness` | 예정 시각 대비 owner lease 갱신 지연 |
| `zlink.observability.events.overflow` | monitoring·trace observer queue overflow 누계 |
| `zlink.host.state` | 현재 host Framework runtime state |
| `zlink.host.core_hwm.effective_budget` | 시작 시 고정한 유효 Core HWM byte budget |
| `zlink.host.core_hwm.applied` | completion lane을 제외한 일반 방향별 queue HWM 합계 |
| `zlink.host.core_hwm.accounted` | 현재 또는 epoch peak Core accounted byte (`state=current|peak`) |
| `zlink.host.core_hwm.completion_accounted` | 현재 또는 epoch peak completion accounted byte (`state=current|peak`) |
| `zlink.host.core_hwm.blocked_ratio` | ppm 단위 Core blocked ratio |
| `zlink.host.application_job_queue.limit` | 시작 시 고정한 유효 Application Job Queue permit limit |
| `zlink.host.application_job_queue.jobs` | 예약·대기·사용 중·peak permit (`state=reserved|queued|in_use|peak`) |
| `zlink.host.application_job_queue.capacity_waiters` | 현재 permit capacity waiter 수 |
| `zlink.host.application_job_queue.capacity_waits` | 현재 measurement epoch의 permit capacity wait 누계 |
| `zlink.host.application_job_queue.capacity_wait_duration` | 현재 epoch의 permit capacity wait 누적 시간 |
| `zlink.host.application_job_queue.pressure_state` | 현재 `running`·`paused` 상태 (`state`) |
| `zlink.host.application_job_queue.pressure_transitions` | 상태별 전이 누계 (`state=running`·`paused`) |
| `zlink.host.application_job_queue.pause_duration` | 현재·누적 pause 초 (`state=current`·`cumulative`) |
| `zlink.host.application_job_queue.flow_state_config_failures` | Core flow 절대 상태 적용 실패 누계 |
| `zlink.host.relocation.duration` | Host `relocate` 시작부터 terminal result까지의 시간 |
| `zlink.host.relocation.blocked` | `Blocked`로 끝난 host `relocate` 수 |
| `zlink.host.shutdown.duration` | Host `shutdown` 시작부터 terminal result까지의 시간 |
| `zlink.host.shutdown.forced` | Bounded teardown으로 끝난 host `shutdown` 수 |

### 1.1 capacity snapshot과 measurement reset

Host runtime의 capacity snapshot은 Core HWM과 Application Job Queue 상태를 함께 제공한다.
시작 시 고정한 구성·유효 limit과 현재·peak accounted byte, 예약·대기·사용 중 permit,
capacity wait를 연관 지어 볼 때 사용한다. 정확한 type과 member 이름은 해당
[언어별 monitoring 계약](../../../common/spec/server/languages/README.ko.md)에서 확인한다.

Measurement reset은 capacity를 바꾸지 않고 새 epoch를 시작한다. 현재 pressure state와 current
pause duration을 포함한 gauge와 구성은 유지한다. 각 peak는 현재값으로 재설정하고 epoch wait,
pressure transition, cumulative pause duration과 flow-state config failure 누계를 0으로 만든다.
동시에 발생한 event는 정확히 한 epoch에만 속한다. Always-on metric은 의도적으로 모든 job에
timestamp를 찍거나 job별 queue-wait histogram을 만들지 않는다. 그런 분포는 bounded perf
fixture 안에서만 기록한다. 정확한 snapshot·reset 규칙은
[Runtime 상태 조회와 운영 진단](../../../common/spec/server/06-observability/01-runtime-monitoring.ko.md),
metric 이름·단위·label은
[runtime 메트릭](../../../common/spec/server/06-observability/02-runtime-metrics.ko.md)이 소유한다.

## 2. Relocate — 상태를 유지한 채 다른 host로 옮기기

`relocate(...)`는 이 host에서 살아 있는 User Spot·Instance Spot·Actor를 다른 Serving node로
옮긴다. Host 전체를 대상으로 하는 operation이며, 이 호출 자체가 host를 종료하지는 않는다.

옮긴 뒤에도 논리 id와 아직 실행하지 않은 작업과 timer가 그대로 남고, application 상태만
factory에 등록한 adapter가 담아 옮긴다. 무엇이 남고 절차가 어떤 순서로 진행되며 어디까지
되돌릴 수 있는지는 [Relocation](37-relocation.ko.md)이 다룬다.

이 절은 **운영이 호출하는 쪽**을 다룬다 — 언제 호출할 수 있고, 무엇을 설정하며, 언제 종료해도
되는지다.

**호출할 수 없는 경우.** 받을 수 있는 대상이 없으면 이 host의 상태를 바꾸지 않고 막힘으로
끝난다. 이미 옮기는 중이거나 종료 중이면 새 호출을 받지 않는다.

### 2.1 옮기는 단위

무엇을 하나로 묶어 옮기는지는 Spot 종류와 execution mode가 정한다 —
[Relocation](37-relocation.ko.md#4-execution-mode가-정하는-이동-단위)이 그 구분을 다룬다.
운영에서 이것이 중요한 이유는 **묶음이 클수록 한 번에 멈추는 범위가 넓기** 때문이다.

<iframe class="zlink-diagram" src="/common/diagrams/12-relocation.html" title="execution mode별 relocation 이전 단위 — SpotWide vs PerActor" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/12-relocation.html" target="_blank">↗ 크게 보기</a></p>

Actor를 각각 옮기는 User Spot은 Spot 자체가 옮길 상태를 갖지 않으므로, 그 factory는
도착 쪽에서 새로 만드는 정책만 사용할 수 있다. 소속 Actor의 정책은 각 Actor factory가 따로 정한다.

### 2.2 이동 state 전송 설정

이동 state는 source–target mesh 연결로 chunk 단위로 전송되며, 같은 연결의 일반
message 전송이 지연되지 않도록 다음 server 설정으로 조정한다. 기본값으로 시작해도
되고, runtime이 자동으로 조정하지 않으므로 배치별로 관측한 값을 기준으로 바꾼다.

| 설정 | 기본값 | 목적과 조정 기준 |
| --- | --- | --- |
| `RelocationPayloadChunkLimit` | 256 KiB | chunk 하나의 크기. chunk 전송이 같은 연결의 일반 message 지연 목표를 침범하면 낮춘다 |
| `RelocationInFlightPayloadBudget` | 16 MiB | peer 연결 하나에서 동시에 전송 중일 수 있는 relocation byte 총량. 0은 미적용. relocation 전송이 일반 message 대역폭을 잠식하면 낮추고, host 이전 처리량이 이 예산에 막히면 올린다 |
| `RelocationNodeInFlightPayloadBudget` | 0(미적용) | node 전체의 동시 전송 상한. peer 연결이 많은 node에서 총 점유를 제한해야 할 때만 설정한다 |
| `relocation_cutover_wait_timeout` | 1,000ms | target이 cutover 재전송을 기다리는 상한. `cutover_timeout` counter가 0이 아니면 배치의 왕복 시간에 맞게 조정한다 |

예산이 차 있으면 새 relocation unit은 seal 전에 대기하고, 대기하는 동안 그
Actor·Spot은 message를 정상적으로 처리한다 — 예산 때문에 시작하지 못하는 payload
크기는 없다. Chunk 형식·검증 규약 같은 내부 protocol은
[Relocation Flow](../../../common/spec/server/05-location-relocation/04-relocation-flow.ko.md)가 다룬다.

### 2.3 SafeToShutdown — 종료해도 안전한 시점

`Relocated`는 source가 cutover 전송을 마쳤다는 뜻이지, 이전 route를 cache한 호출자가
모두 새 owner를 향하게 됐다는 뜻이 아니다. Source runtime은 모든 unit의 Message Follow를
끝낼 수 있고 cutover 재전송 창이 닫힌 뒤 `SafeToShutdown` 상태를 자기 runtime status에
게시한다. Deployment orchestrator는 `Relocated`를 확인한 뒤 이 상태까지 관찰하고 나서
`shutdown`을 호출하는 것을 권장한다 — 게시 전에 종료해도 되지만, 남아 있던 follow
route가 사라져 이전 route를 cache한 호출자의 request가 `Unavailable`로 끝날 수 있다.
상태 조회·변화 관찰 방법은
[Runtime 상태 조회와 운영 진단](../../../common/spec/server/06-observability/01-runtime-monitoring.ko.md)을 따른다.

Relocation 구간은 세 지표로 나눠 관찰한다 — source 정지(seal부터 cutover 전송까지),
target 재개(target의 owner 확정부터 dispatch 개방까지), route 수렴(cutover 전송부터
Message Follow route를 제거할 수 있을 때까지). Cutover를 기다리다 검증 없이 진행한
fallback 횟수는 `cutover_timeout` counter로 게시된다. 지표 이름·단위·label은
[runtime 메트릭](../../../common/spec/server/06-observability/02-runtime-metrics.ko.md)이 소유한다.

## 3. Shutdown — 옮기지 않고 종료하기

`shutdown(...)`는 이 host를 종료한다. [Relocate](#2-relocate--상태를-유지한-채-다른-host로-옮기기)와 달리 **상태를 다른 node로 옮기지 않는다.**

호출하면 새 relocation을 시작하지 않고, 진행 중인 작업을 주어진 deadline 안에서 끝내거나
실패로 확정한다. 그다음 Entry·User·Instance Spot에 `on_closing`를 `HostShutdown` reason으로
알리고, 그 callback이 끝난 뒤 scope·authority·session·topology resource를 정리한다. deadline을
주지 않으면 30초다.

여기서 정리되는 Spot의 state는 남지 않는다. 배포 자동화가 상태를 살려서 내려야 한다면 종료
전에 `relocate(...)`를 먼저 호출하고 그 결과가 `Relocated`인지 확인한 뒤 이 호출로
넘어간다([운영 호출과 readiness 연결](#4-운영-호출과-readiness-연결)의 예제). 가능하면 `SafeToShutdown` 게시까지 확인한다([SafeToShutdown](#23-safetoshutdown--종료해도-안전한-시점)).

Spot의 수명은 request와 무관하다. 일반 request가 끝났다는 이유만으로 User·Instance Spot을 닫지
않는다. 없는 Instance Spot을 준비시키는 것도 마찬가지로 별도 address나 manager create가 아니라,
SpotId direct 호출에 Instance intent를 붙였을 때만 시작한다([Spot](21-spot.ko.md)).

## 4. 운영 호출과 readiness 연결

앞의 두 operation은 자동으로 일어나지 않는다. Application이 framework runtime으로 직접
호출한다. 이 interface는 host maintenance를 소유하는 DI singleton이다.

배포에서 사용하는 순서는 "먼저 옮기고, 성공했으면 종료한다"다. `Relocated`를 확인한 뒤
`SafeToShutdown` 게시([SafeToShutdown](#23-safetoshutdown--종료해도-안전한-시점))까지 관찰하고 종료하면 이전 route를 cache한 호출자의 실패를
피할 수 있다.

```cpp
relocation_options_t relocation;
relocation.mode = relocation_mode_t::rolling_update;
// 지정한 새 버전의 eligible node만 사용한다.
relocation.target_application_version = 12;
relocation.deadline = std::chrono::seconds (25);

auto result = co_await runtime.relocate (relocation);
if (result.outcome == relocation_outcome_t::relocated)
    co_await runtime.shutdown (std::chrono::seconds (10));
else
    _logger.error ("host relocation blocked");
```

`PlannedMaintenance`는 source와 같은 application version의 target만 사용한다.
`RollingUpdate`는 source보다 큰 `target_application_version`을 요구하고 그 version과 정확히 같은 target만
사용한다. Eligible target이 없으면 deadline까지 기다린 뒤 `Blocked/TargetUnavailable`을 반환한다.
Cancellation은 해당 waiter만 끝내며 이미 시작한 shared lifecycle operation은 계속 실행된다.

Readiness는 host framework runtime의 준비 여부와 업무에 필요한 component runtime의 readiness를 함께
확인해 기존 HTTP endpoint에 연결한다.

```cpp
// readiness endpoint는 host runtime의 상태 하나만 본다.
const bool ready = runtime.status ().is_ready;
// ready가 false면 503을 응답한다.
```

Kubernetes 배포에 연결하면 다음 개념이 된다.

```yaml
# readiness probe → /healthz/ready — Draining 진입 즉시 신규 트래픽 대상에서 제외
# preStop hook + terminationGracePeriodSeconds ≥ drain deadline — 자동 drain이 끝날 시간을 확보
```

### 4.1 다시 부르거나 겹쳐 불렀을 때

배포 자동화는 실패하면 재시도한다. 그래서 **같은 호출을 두 번 하면 어떻게 되는지**가
계약으로 정해져 있다.

| 상황 | 결과 |
| --- | --- |
| 같은 mode로 `relocate`를 겹쳐 부름 | 최초 operation과 deadline을 공유한다. 뒤의 호출이 deadline을 늘리지 않는다 |
| 다른 mode로 `relocate`를 겹쳐 부름 | 기다리지 않고 `Blocked` — 진행 중인 operation이 있다는 뜻이다 |
| `Blocked` 뒤에 다시 `relocate` | `Blocked`는 저장하지 않으므로 host 조건을 처음부터 다시 검사한다. **재시도가 의미 있는 유일한 결과다** |
| `Relocated`에서 다시 `relocate` | 최초 성공 결과를 그대로 돌려준다. 다시 옮기지 않는다 |
| `shutdown`을 겹쳐 부름 | 같은 operation을 공유하고 terminal 결과를 저장한다 |
| `Stopped`에서 다시 `shutdown` | 저장한 결과를 돌려준다 |
| 시작 중이거나 오류·정지 상태에서 `relocate` | admission을 건드리지 않고 `Blocked`다 |

**호출자 취소는 그 호출만 끝낸다.** 공유하고 있는 operation 자체는 취소되지 않는다.

**`shutdown`은 막히지 않는다.** target이 없어도, capacity가 부족해도, Relocation Store가
없어도 진행한다. 그래서 `relocate`를 기다리는 중에 `shutdown`이 확정되면 기다리던 쪽이
`Blocked`로 끝난다 — **먼저 옮기고 성공을 확인한 뒤 종료한다**는 순서를 지켜야 하는
이유다.

`shutdown`이 deadline 안에 끝나지 않으면 제한된 정리만 하고 강제 종료 결과로 끝난다.
deadline 초과와 callback 실패는 서로 다른 결과값으로 구분된다.

### 4.2 전이 중에도 살아 있는 것

`Relocating` · `Relocated` · `Draining`은 "아무것도 안 받는 상태"가 아니다. **새로
시작하는 것만 막고 이미 수락한 것은 끝까지 처리한다.**

| | `Relocating` | `Relocated` | `Draining` |
| --- | --- | --- | --- |
| channel 이름으로 고르기 | 새 선택에서 제외. 기존 owner 경로는 유지 | 새 선택에서 제외 | 새 admission 닫음 |
| node를 직접 지정한 요청 | unit seal 전까지 수락 | 받지 않음 | shutdown 결과로 끝냄 |
| Spot · Actor 생성과 join | 거부 | 거부 | 거부 |
| STREAM | 새 binding 제외. 기존 session은 barrier로 처리 | 새 binding 제외 | 새 session 받지 않음 |
| 이미 수락한 request | reply · error · timeout · shutdown 중 하나로 **한 번만** 끝난다 | 〃 | 〃 |

**monitoring이나 observer callback은 종료를 붙잡지 않는다.** 상태를 관찰하는 코드가
오래 돌아도 maintenance가 그것을 기다리지 않는다.

## 5. Location readiness와 운영 조회

운영 코드는 location readiness 표면으로 필요한 peer가 Ready인지 확인한다. 전체 상태와 paged
topology는 location runtime query로 조회한다.

```cpp
auto status = co_await query.get_status ();
auto page = co_await query.list_topology (
  location_topology_filter_t{.mesh_name = "play"},
  location_page_request_t{.page_size = 100});

auto object_peer_ready = co_await readiness.is_peer_ready ("play", location_role_t::spot);

// status.store_healthy · status.owner_lease_healthy · object_peer_ready · page.items를
// 운영 endpoint의 응답으로 직접 만든다.
```

운영 query는 health와 사람이 확인할 topology만 반환한다. Store key, authority version, owner token과
relocation record는 Framework 내부 정보이므로 반환하지 않는다. `NodeRid`는 실제 transport node를
운영 정보와 대응할 때만 사용한다.

## 6. MeshNode runtime 제어와 관측

RouteMesh로 등록한 MeshNode는 runtime 옵션과 상태 조회 표면으로 운영한다 — 등록 호출과 주입 이름은 언어를 따른다.

**runtime 옵션.** serving 중에 바꿀 수 있는 값은
다음과 같다. 나머지 소켓 옵션(HWM·timeout)은 시작 전 `configure_router_socket()`
전용이다.

```cpp
mesh_options.placement_weight (0);              // 새 object 배치 대상에서 제외
mesh_options.channel ("game.room").weight (0);  // 새 channel select-one 대상에서 제외
```

두 weight는 독립적이며 실행 중 새 선택에 반영된다. Placement weight는 Actor·Spot create와 relocation
target 선택에만 사용한다. Channel weight는 해당 server membership의 새 select-one 대상 선택에만
사용한다. 등록되지 않은 mesh나 membership을 조회하면 설정 오류다.

**상태 조회 — RouteMesh runtime.** Mesh 하나에 대해 일관된 snapshot 한 장과 순서 있는
component 이벤트 스트림을 제공한다. Host termination은 framework runtime이 소유한다.

```cpp
// 노드·peer·channel의 immutable 현재 상태
auto snapshot = mesh_runtime.snapshot ("game.room");
const bool ready = mesh_runtime.is_ready ("game.room");

// state/peer 전이가 순서대로 온다. capacity를 넘기면 느린 관찰자는 건너뛴다.
auto observation = mesh_runtime.observe (
  "game.room", 64,
  [] (const observed_status_t<mesh_node_snapshot_t> &observed) {
      record (observed.status);
      //  observed.loss가 이 관찰자가 놓친 개수다.
  });
```

## 7. Host lifecycle

Framework runtime은 host의 **수명주기 서비스**로 시작·종료에 묶인다.
channel·SPOT·STREAM runtime은 startup에서 등록한 역할을 보고 생성되어 shutdown에서
정리된다.

<iframe class="zlink-diagram" src="/common/diagrams/12-lifecycle.html" title="Host lifecycle — 구성·서비스·종료" loading="lazy" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/12-lifecycle.html" target="_blank">↗ 크게 보기</a></p>

- **구성 단계** — `app.Run()` 전에 모든 선언을 끝낸다. 잘못된 구성은 host
  startup에서 예외로 거부된다.
- **종료** — host shutdown 신호가 오면 hosted service `stop()` → channel/SPOT/STREAM
  runtime 정리 순으로 내려간다.
- 백그라운드 작업은 host의 표준 수명주기 서비스로 같은 수명주기에 편입시킨다.

### 7.1 상태 관측

Host `relocate`·`shutdown` 상태 전이는 framework runtime의 bounded status stream에서 관측한다. MeshName별
runtime은 component snapshot을 제공하지만 별도 termination authority나 partial drain operation을 만들지 않는다.

```cpp
// Host 전체 state, effective intent와 terminal outcome을 순서대로 기록한다.
auto observation = runtime.observe (
  64, [&] (const observed_status_t<framework_runtime_status_t> &observed) {
      _logger.info ("host lifecycle state changed");
      //  observed.status가 상태, observed.loss가 놓친 개수다.
  });
```

host lifecycle 상태 일곱(preparing · serving · relocating · relocated · draining ·
stopped · error)을 그대로 관측한다. 표기는 언어를 따른다. Status의 relocation·termination 결과는 해당 operation의 terminal 결과와 같아야 한다.
수치로 보려면 [런타임 메트릭](#1-런타임-메트릭)의 `zlink.host.*` 계기를 사용한다.

## 8. 관련 문서

- 이 챕터 계약의 실행 검증 예문: [주요 타입 사용 색인](13-interface-catalog.ko.md) — 검증 클래스 `FrameworkRuntimeContracts`
- 정식 계약: [Host relocation 전체 흐름](../../../common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md) · [Runtime Metrics](../../../common/spec/server/06-observability/02-runtime-metrics.ko.md)
- 상태 관측과 진단: `11. Monitoring` 장
- relocation 경계를 application이 정하는 Spot: [상태를 담는 시점](37-relocation.ko.md#3-상태를-담는-시점--factory-등록이-정한다)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=Math.max(d.body?d.body.scrollHeight:0,d.documentElement?d.documentElement.scrollHeight:0);if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
