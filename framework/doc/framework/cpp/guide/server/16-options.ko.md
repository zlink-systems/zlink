---
title: "옵션과 기본값 · C++"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/16-options.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 옵션과 기본값

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: 12. 운영 — 런타임 메트릭 · graceful drain · readiness](12-operations.ko.md) | [다음: 14. 샘플 고르기 — 내 문제에 가까운 예제부터](14-samples.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — **C++** · [C#/.NET](../../../dotnet/guide/server/16-options.ko.md) · [Java](../../../java/guide/server/16-options.ko.md) · [Kotlin](../../../kotlin/guide/server/16-options.ko.md) · [Node/TypeScript](../../../node/guide/server/16-options.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    무엇을 정할 수 있는지, 정하지 않으면 어떤 값으로 동작하는지, 시작한 뒤에 바꿀 수 있는
    값이 무엇인지 안다.

옵션의 이름과 기본값은 다섯 언어가 같다. 언어마다 다른 것은 표기와 지정 방법이며 각 절의
탭이 그것을 보여준다. **대부분의 옵션은 지정하지 않아도 동작한다.** 바꿀 이유가 생겼을 때
해당 줄의 기본값을 확인하고, 그전에는 그대로 사용한다.

## 1. 설정 자리와 적용 범위

같은 값이라도 어디에 지정하느냐에 따라 적용 범위와 바꿀 수 있는 시점이 달라진다.

| 자리 | 적용 범위 | 변경 시점 |
| --- | --- | --- |
| 루트 옵션 | 이 process 전체의 기본값 | host 시작 전 |
| node builder | 그 MeshNode · channel · STREAM node 하나 | host 시작 전 |
| runtime option | 실행 중에 바꿀 수 있는 값 | 실행 중([§9](#9-실행-중-바꿀-수-있는-값)) |

```cpp
app.add_zlink_framework ([] (zlink_framework_options_t &options) {
    options.configure_network ().set_bind_host ("0.0.0.0");   // 루트 옵션
    auto mesh = options.add_route_mesh ("play");              // node builder
    mesh.listen ("tcp://0.0.0.0:5555").set_placement_weight (100);
    mesh.channel_name ("room").server ();
});
```

host가 시작된 뒤에 builder를 다시 호출하는 표면은 없다. 잘못된 조합은 첫 호출까지 미루지
않고 **시작 단계에서 설정 오류로 끝난다.**

## 2. 루트 옵션

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| codec 등록 | payload 직렬화 형식 | 내장 JSON |
| `bind_host` | listener가 bind할 주소 | `127.0.0.1` |
| `advertise_host` | 상대에게 알릴 주소 | 지정 안 함 — bind 주소를 사용 |
| `default_request_timeout` | request가 응답을 기다리는 상한 | 30초 |
| `session_replacement_callback_timeout` | session 교체 callback이 끝나기를 기다리는 상한 | 30초 |
| stream compression | STREAM payload 압축 | LZ4 사용 |
| worker `MinThreads` · `max_threads` | CPU worker 풀의 스레드 수 | 0 · 프로세서 수의 두 배 |
| worker `idle_timeout` | 유휴 스레드를 정리하기까지의 시간 | 30초 |
| `application_version` · `maintenance_wave` | rolling update가 비교할 버전과 점검 묶음 | 0 · 지정 안 함 |
| handler 탐색 · filter · metadata 정책 | 등록 대상과 전달 허용 key | 등록한 것만 |
| location store · relocation store | 위치 결정과 상태 이전 저장소 | 없으면 단일 node 구성 |

- **bind 주소의 기본값은 loopback이다.** 다른 host의 node나 client가 접속해야 한다면 bind
  주소와 광고 주소를 각각 지정한다.
- **STREAM 압축은 켜진 상태로 시작한다.** 끄려면 압축 설정에서 명시적으로 끈다.
- **CPU worker 풀에는 대기열 상한이 없다.** 유입을 제한하는 것은 Application job queue다(§3).
  `default_request_timeout`은 `0` 이하를 거부한다.

## 3. Core HWM과 Application job queue 상한

Core HWM은 ordinary queue가 보유한 byte를, Application job queue는 handler 시작을 기다리는 job
수를 host 전체에서 제한한다. 두 상한의 동작은
[Backpressure](33-backpressure.ko.md#1-core-hwm과-application-job-queue)가 다룬다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `core_hwm_memory_limit_bytes` | Core budget 계산에 전달할 memory limit hint | 지정 안 함 |
| `core_hwm_budget_bytes` | profile 계산보다 우선하는 manual Core budget | 지정 안 함 |
| `CoreHwmProfile` | Core Auto-budget profile | `Balanced` |
| `ApplicationJobQueueProfile` | job 상한을 계산할 profile | `Balanced` |
| `max_queued_application_jobs` | profile 계산을 대체하는 정확한 job 상한 | 지정 안 함 |
| `set_application_job_queue_pause_threshold_percent` | 유입을 멈추는 사용률 | 80 |
| `set_application_job_queue_resume_threshold_percent` | 유입을 다시 여는 사용률 | 60 |

Memory limit과 Core budget은 양수만 허용한다. Manual job 상한의 범위는 `1..2,147,483,647`이며
`0`은 무제한이 아니라 시작 단계 설정 오류다. 사용률은 각각 `1..100`과 `0..99`이고 다시 여는 값이
멈추는 값보다 작아야 한다. 두 profile은 같은 label을 쓰지만 독립된 값이며 `Balanced`는
프로세서당 128 job이다.

## 4. 진단 기록 설정

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| 기록 수준 | `off` · `errors` · `Normal` · `Detailed` | `errors` |
| `TraceSampleRate` | 정상 흐름을 기록할 비율. 범위는 `0.0..1.0` | 1.0 |
| `include_message_sizes` | payload byte 크기를 함께 남길지 | 남기지 않음 |

handler가 없는 packet이 도착했을 때의 동작도 같은 자리에서 정한다. request는 보낸 쪽에 오류
응답을 보내고, send와 publish는 기록을 남기고 버린다. 응답 경로가 없는 send와 publish에는
오류 응답 동작을 지정할 수 없다. 이 설정은 C++에 없으며 기본 동작만 적용된다. 수준별로
무엇이 남는지는 [모니터링](26-monitoring.ko.md#4-진단-수준-정하기)이 다룬다.

!!! warning "message 크기 기록의 기본값은 JVM에서만 다르다"

    Java와 Kotlin은 `include_message_sizes`가 켜진 상태로 시작하고 나머지 언어는 꺼진 상태로
    시작한다. 언어를 섞은 구성에서 기록의 양을 맞추려면 값을 명시한다.

## 5. MeshNode 옵션

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `listen` | 다른 node가 접속할 자기 주소 | 지정해야 한다 |
| `bind_host` · `advertise_host` | 이 node만의 bind · 광고 주소 | 루트 값 |
| `RoutingId` · `RoutingIdPrefix` | 이 node의 식별자 | 자동 생성 |
| `object_role` | Spot · Actor 배치 참여 여부 | 아래 주의 |
| `placement_weight` | 새 배치 대상으로 선택되는 비중. 범위는 `0..10000` | 100 |
| `actor_limit` · `spot_limit` | 이 node가 동시에 담을 수 있는 상한 | `0` — 제한 없음 |
| `ActivationConcurrency` | 동시에 진행할 cold activation 수 | 128 |
| `instance_spot_idle_timeout` | 유휴 Instance Spot을 정리하기까지의 시간 | `0` — 정리하지 않음 |
| `default_request_timeout` | 이 node에서 나가는 request의 상한 | 루트 값(30초) |
| peer connection | 수동으로 연결할 상대 | 없음 — location store가 찾아낸다 |

두 limit의 `0`은 제한 없음이고 양수는 `1..2,147,483,647`이다. `ActivationConcurrency`는 반대로
`0`을 거부한다 — object 수가 아니라 동시에 진행되는 활성화를 제한하는 값이기 때문이다.

!!! warning "배치 참여의 기본값은 C++만 다르다"

    C++은 `object_role`을 지정하지 않으면 배치를 받는 `Server`로 시작하고 나머지 언어는
    배치에 참여하지 않는다. Spot과 Actor를 두지 않을 node라면 C++에서는 역할을 명시한다.

## 6. 송신 대기와 socket 상한

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `send_timeout` | 보낼 자리가 나기를 기다리는 상한 | 1초 |
| `receive_timeout` | 받는 방향의 대기 상한 | 지정 안 함 |
| `SendHighWaterMark` · `ReceiveHighWaterMark` | 상대별로 보관할 byte. `0`은 무제한 | 지정 안 함 — Core가 계산 |

상한에 도달하면 보내는 쪽이 `send_timeout`까지 기다리고, 끝까지 자리가 나지 않으면 그 호출은
deadline 초과로 끝난다. 자동으로 다시 보내지 않으므로 재시도 여부는 application이 정한다.
**MeshNode 사이의 연결에는 message 크기 상한 설정이 없다** — 그 상한은 STREAM node와
ClientServer listener가 소유한다([Backpressure](33-backpressure.ko.md)).

!!! warning "manual HWM을 지정하는 자리는 언어마다 다르다"

    방향별 byte 상한을 직접 지정하는 표면은 일부 언어에만 있고 C++에는 없다. 지정하지 않으면
    Core가 physical queue를 근거로 계산하므로 값을 정하지 않는 것이 기본 구성이다. 실행 단위
    하나의 mailbox를 message 수나 byte로 제한하는 설정은 어느 언어에서도 동작하지 않는다.

## 7. Location 옵션

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `owner_lease_renew_interval` | 소유권을 갱신하는 주기 | 5초 |
| `owner_lease_ttl` | 갱신이 끊긴 소유권이 만료되는 시간 | 15초 |
| `owner_lease_renew_timeout` | 갱신 시도 하나의 상한 | 3초 |
| `owner_lease_fencing_margin` | 만료 전에 권한을 반납하는 여유 | 5초 |
| `polling_interval` | 변경 알림이 없는 store를 다시 읽는 주기 | 1초 |
| `store_failure_grace` | store 장애를 견디는 기간 | 30초 |
| `route_cache_max_age` | 조회한 위치를 재사용하는 기간 | 15초 |
| `message_follow_duration` | 이전 owner가 새 owner로 message를 전달하는 기간 | 30초 |
| `session_relocation_seal_timeout` | session 경로 갱신을 기다리는 상한 | 3초 |
| `RelocationPayloadChunkLimit` | relocation payload chunk 하나의 크기 상한 | 256 KiB |
| `RelocationInFlightPayloadBudget` | 연결 하나가 동시에 전송하는 chunk byte 상한 | 16 MiB |
| `RelocationNodeInFlightPayloadBudget` | 같은 상한의 node 전체 값 | `0` — 적용하지 않음 |
| `relocation_cutover_wait_timeout` | cutover를 기다리는 시간 | 1초 |

**lease 값은 함께 움직인다.** `OwnerLeaseRenewInterval + OwnerLeaseRenewTimeout`이
`OwnerLeaseTtl - OwnerLeaseFencingMargin`보다 작아야 한다. 기본값은 8초와 10초로 이 관계를
만족하며 갱신이 한 번 실패해도 소유권이 유지된다. `route_cache_max_age`는 `message_follow_duration`
보다 5초 이상 작아야 하고, 각각 `0`이면 경로 캐시와 message 전달을 끈다. 위치 결정과 이전
동작은 [Location](25-location.ko.md)과 [Relocation](37-relocation.ko.md)이 다룬다.

## 8. STREAM node 옵션

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `bind` | client가 접속할 주소 | 지정해야 한다 |
| `bind_host` · `advertise_host` | bind · 광고 주소 | 루트 값 |
| session 등록 | 연결마다 만들 session 타입 | 지정해야 한다 |
| `max_message_size` | client가 보내는 message 하나의 byte 상한 | 64 KiB |
| TLS 설정 | 서버 인증서와 client 인증서 요구 여부 | 평문, client 인증서를 요구하지 않음 |
| Actor dispatch | 들어온 packet을 bind된 Actor로 전달 | 꺼져 있음 |

`max_message_size`는 client에서 server로 오는 방향에만 적용하고 `0`은 상한을 두지 않는다는 뜻이다.
크기를 넘긴 message는 handler에 일부도 전달하지 않고 server가 연결을 끊는다. Actor dispatch는
STREAM node마다 한 번만 활성화하며 두 번 호출하면 오류가 난다. 어느 mesh에서 Actor를 찾을지는
인자가 아니라 global ActorId가 정하므로 mesh 이름을 함께 지정하지 않는다. 등록 코드는
[STREAM](23-stream.ko.md)과 [Session과 Actor 연결](24-actor-session.ko.md)이 다룬다.

## 9. 실행 중 바꿀 수 있는 값

시작한 뒤에 바꿀 수 있는 값은 선택 비중이다. 나머지는 시작 시점에 확정된다.

| 값 | 무엇에 쓰나 |
| --- | --- |
| channel weight | 이 node가 새 request · send 대상으로 선택되는 비중 |
| placement weight | 새 Spot · Actor가 이 node에 배치되는 비중 |

```cpp
runtime_options.placement_weight (0);
runtime_options.channel ("room").weight (0);
```

두 값의 범위는 `0..10000`이고 기본값은 100이다. `0`으로 두면 **새 배정만 멈춘다** — 이미 있는
object와 연결은 유지된다. 무중단 배포에서 이 node로 새 트래픽이 가지 않게 한 뒤 relocation을
시작하는 순서로 사용한다([운영과 lifecycle](12-operations.ko.md#4-운영-호출과-readiness-연결)).

## 10. 반드시 지정하는 값

기본값이 없어 지정하지 않으면 시작이 실패하거나 의도한 동작이 나오지 않는 값이다.

| 값 | 빠뜨렸을 때 |
| --- | --- |
| MeshNode의 `listen` 주소 | 시작 단계에서 설정 오류 |
| MeshNode의 channel 역할 또는 object 역할 하나 이상 | 시작 단계에서 설정 오류 |
| STREAM node의 `bind` 주소와 session 타입 | 시작 단계에서 설정 오류 |
| Spot · Actor factory의 relocation 정책 정확히 하나 | 시작 단계에서 설정 오류 |
| 상태를 옮기는 factory나 Instance Spot이 있을 때의 relocation store | 시작 단계에서 설정 오류 |
| 여러 node를 사용할 때의 location store | 연결할 상대를 찾지 못한다 |
| 연결과 Actor 사이로 전달할 metadata key | 오류 없이 값만 전달되지 않는다 |

## 11. 자주 발생하는 문제

- **설정을 바꿨는데 반영되지 않는다** — 대부분의 옵션은 host 시작 시점에 확정된다. 실행 중
  바꿀 수 있는 값은 [§9](#9-실행-중-바꿀-수-있는-값)에 정리되어 있다.
- **다른 host의 node가 접속하지 못한다** — bind 주소의 기본값은 loopback이다. bind 주소와
  광고 주소를 각각 지정한다.
- **`0`으로 두었더니 memory가 계속 늘어난다** — byte 상한의 `0`은 무제한이다. Core 계산에
  맡기려면 값을 지정하지 않는다.
- **소유권을 자꾸 잃는다** — 갱신 주기와 갱신 상한의 합이 유효 기간에서 여유를 뺀 값보다 크다.
- **client가 보낸 큰 message에서 연결이 끊긴다** — STREAM의 크기 상한은 64 KiB다. 큰 payload를
  받는 node라면 상한을 올린다.
- **weight를 `0`으로 했더니 기존 연결도 끊길 것이라 예상했다** — weight는 새 배정만 막고 기존
  object와 연결은 유지된다.

## 12. 관련 문서

- 상한이 무엇을 바꾸는지 — [Backpressure](33-backpressure.ko.md)
- 기록을 읽는 방법 — [모니터링](26-monitoring.ko.md)
- weight로 트래픽을 빼는 절차 — [운영과 lifecycle](12-operations.ko.md)
- 언어별 표면 이름 — [주요 타입 사용 색인](13-interface-catalog.ko.md)
