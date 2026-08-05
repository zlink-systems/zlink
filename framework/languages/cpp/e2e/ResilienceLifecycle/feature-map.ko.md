# C++ ResilienceLifecycle E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-5-resilience-lifecycle.ko.md`

현재 C++ `ResilienceLifecycle`은 Redis location store를 공유하는 Provider, Consumer, Client target으로 recovery 흐름을 실행한다. 10.0.0에서는 Consumer/Provider의 MeshNode와 ChannelName client로 같은 scenario를 다시 연결한다. Client target은 HTTP-only dispatcher이며 Provider admin endpoint는 drain/restore/weight/wait marker를 보존한다.

최신 full runner proof는 `logs/20260708-133049-101113`이다. 이 실행은 `RL-B2`의 `kill -9`와
`RL-C2`의 SIGABRT crash처럼 시나리오가 의도한 failure injection만 허용하고, 그 외 provider
비정상 종료는 runner 실패로 드러내도록 보강한 뒤 통과했다.

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| `RL-A1` | 구현 | 전용 runner가 provider를 같은 endpoint와 같은 rid로 재시작하고, 실행 중인 client가 `rl_a1_provider_restart_scenario.hpp` 경로로 follow-up request를 성공시키는지 검증한다. |
| `RL-A2` | 구현 | 전용 runner가 같은 rid를 다른 endpoint로 재기동하고, 실행 중인 client가 `rl_a2_provider_endpoint_remap_scenario.hpp` 경로로 stale endpoint 대신 replacement provider로 전환되는지 검증한다. |
| `RL-A3` | 구현 | `rl_a3_reconnect_storm_scenario.hpp`가 Consumer HTTP `/profile/request/new-client`를 24번 호출해 reconnect storm 중 요청마다 새 client host를 만들고, reply provider id와 provider evidence marker를 검증한다. |
| `RL-A4` | 구현 | `rl_a4_drain_and_green_endpoint_scenario.hpp`가 provider B `/admin/drain`, green provider endpoint 시작, original provider shutdown, Consumer `/topology/wait` Ready 1, green provider evidence, green shutdown, original provider 복구, restored evidence를 `.NET`처럼 검증한다. |
| `RL-A5` | 구현 | runner가 provider B stop/restart를 3회 반복하고, `rl_a5_provider_flapping_scenario.hpp`가 down window의 Consumer HTTP request `api-a` 수렴, up window의 request 성공, provider B evidence prefix를 검증한다. |
| `RL-B1` | 구현 | runner가 Consumer HTTP `/profile/request/timeout/100`으로 timeout request를 보내 `TimeoutException`을 확인하고, 같은 consumer의 후속 request가 정상화되는지 검증한다. |
| `RL-B2` | 구현 | `inflight-crash` client가 Consumer HTTP `/profile/request/manual-b` slow request를 열고 provider B file evidence에서 start marker를 확인한 뒤 provider B crash를 관찰한다. 이후 Consumer `/topology/wait`가 `api-b` Ready 0개로 수렴하는지, in-flight request가 실패하는지, `api-a` follow-up과 provider B 재기동 뒤 restored request evidence가 남는지 검증한다. |
| `RL-B3` | 구현 | 전용 runner가 provider 하나를 정상 종료하고, 실행 중인 client가 `rl_b3_graceful_shutdown_scenario.hpp` 경로로 남은 provider에 request를 성공시키는지 검증한다. |
| `RL-B4` | 구현 | `rl_b4_runtime_drain_scenario.hpp`가 provider B의 `/admin/drain`, `/admin/restore`, `/admin/weight/wait` 경로를 사용해 신규 request가 A로만 가는지, drained provider evidence가 늘지 않는지 검증한다. restore 뒤 이름 있는 첫 request를 재시도 없이 한 번 보내 즉시 성공을 단언하고, 이어서 provider B의 부하 복귀를 확인한다. |
| `RL-B5` | 구현 | `rl_b5_drain_inflight_scenario.hpp`가 Consumer HTTP slow request를 열고 실제 slow provider를 evidence file로 찾은 뒤 해당 provider를 `/admin/drain`한다. 신규 request가 healthy provider로 가는지, in-flight reply가 drained provider에서 끝나는지, drained provider evidence가 새 request를 받지 않는지, restore 뒤 evidence가 회복되는지 `.NET`처럼 검증한다. |
| `RL-B6` | 구현 | provider B의 gray fault mode를 켠 뒤 gray request의 `RequestFailed`와 healthy provider 성공을 함께 관찰하고, fault mode 해제 뒤 follow-up request가 정상화되는지 검증한다. |
| `RL-C1` | 구현 | `rl_c1_client_host_lifecycle_scenario.hpp`가 Consumer HTTP `/profile/request/new-client`로 요청마다 새 client host를 만들고, 반복 request와 cleanup follow-up marker가 provider evidence에 남는지 검증한다. 이어서 같은 MeshName과 RID를 유지한 RouteMesh host를 12번 순차 재생성한다. 각 host는 `tcp://127.0.0.1:0`에서 새 실제 endpoint와 generation을 얻고, Redis reciprocal auto-connect로 기존 `api-a`를 ready peer로 확인한 뒤 공개 targeted request를 완료한다. 최신 일반 실행은 12회 요청과 전체 cleanup이 통과했다(`logs/20260720-022259-1836671`). |
| `RL-C2` | 구현 | provider B의 `/admin/crash`를 호출한 뒤 Consumer `/topology/wait`가 `api-b` Ready 0개로 수렴하는지 확인하고, Consumer HTTP `/profile/request/new-client`가 정상 provider `api-a`로 수렴하는지 확인한다. provider B 재기동 뒤 일반 request가 `api-b` evidence까지 회복되는지도 검증한다. |
| `RL-C3` | 구현 | `rl_c3_node_pause_recovery_scenario.hpp`가 provider B `/shutdown`, Consumer HTTP `/profile/request`의 `api-a` 수렴, provider B 재기동 뒤 Consumer `/topology/wait` Ready 1, recovered request evidence를 `.NET`처럼 검증한다. |
| `RL-C4` | 구현 | `rl_c4_location_store_outage_scenario.hpp`가 Redis location store outage 전 Consumer `/profile/request/manual`을 호출하고, runner가 Redis container를 pause한 상태에서도 Consumer role의 established manual channel request가 성공하고 provider evidence가 남는지 검증한다. Redis 복구와 provider A 재기동 뒤 Consumer `/profile/request/new-client`가 `rl-c4-after-restart` request와 provider evidence를 성공시키는지 확인한다. |
| `RL-D1` | 구현 | runner가 Consumer HTTP `/profile/request`로 120개 request burst를 만들고 provider evidence에서 `rl-d1-` marker가 남는지 검증한다. |
| `RL-D2` | 전환 필요 | observer fault 격리에 더해 public `runtime_error_sink_t`가 `zlink.runtime_error`/`observer_failed`/`message_flow_observer` event를 한 번 받는지 검증해야 한다. |
| `RL-D3` | 전환 필요 | provider flow evidence를 `outcome=failed`, `reason=no_handler`, `action=reply_error`, `packet_name`으로 재정렬해야 한다. |
| `RL-D4` | 구현 | 실제 provider/consumer E2E는 missing request의 `HandlerNotFound` public failure와 provider dispatch error evidence를 확인한다. runtime unit gate는 같은 channel error reply의 raw header에서 `Error=5`, camelCase `errorCode`/`errorMessage`, `status` 부재와 성공 `Response=2`를 직접 검증한다. |
| `RL-D5` | deferred | 공통 문서가 요구하는 동시 다수 client, 수 분 지속, request/send 혼합, latency drift 관측을 제공하는 soak harness가 없다. 기존 120회 순차 mixed burst는 이 계약을 검증하지 못하므로 scenario PASS 경로에서 제거했다. |
| Consumer role smoke | 구현 | runner가 전용 Consumer HTTP role을 시작하고 `/profile/request`, `/profile/request/manual`, `/profile/request/manual-b`, `/profile/request/new-client`, `/profile/request/timeout/100`, `/profile/request/missing`, `/profile/command`, `/profile/command/missing`, `/topology`, `/topology/wait`으로 provider request, established manual request, transient client host request, timeout cleanup, missing request/send, 정상 command 흐름, location store topology 조회를 확인한다. |

## 현재 검증 구조

- Client는 HTTP-only dispatcher이고 framework channel client는 Consumer와 Provider role이 소유한다.
  Provider admin endpoint는 weight 0/복원과 대기 상태를 제공한다.
- `run_e2e.sh`는 provider process 시작, health 대기, 종료와 stdout/stderr 로그 저장을 담당한다. Redis는
  runner가 loopback container로 시작하며 외부 Redis endpoint를 공유하지 않는다.
- Consumer host는 Redis location store를 조회해 topology endpoint를 제공한다. Profile request/reply/send
  DTO의 marker가 비어 있으면 value 또는 command id를 evidence marker로 사용한다.
- 위 RL-D4 검증은 client scenario와 raw envelope unit gate를 함께 사용한다. RL-D5는 공통 계약의 지속 부하
  harness가 마련될 때까지 `deferred`다. `/profile/request/new-client`는 요청마다 transient client host를
  만들고 별도 `storm-...-flow.log`를 남긴다.
