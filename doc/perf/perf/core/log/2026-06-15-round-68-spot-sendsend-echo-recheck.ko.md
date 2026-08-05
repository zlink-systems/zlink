# Round 68: SPOT_SENDSEND echo 64B 재현성 확인

- goal:
  - `MULTI_SPOT_SENDSEND` 64B echo 계열이 현재도 May26 기준 대비 반복 회귀인지 확인한다.
  - 완료 기준: standalone/current 반복에서 `-10%` 이상 하락이 재현되면 core routed-send hot path
    후보를 찾고, 재현되지 않으면 run-order/load 영향으로 분리한다. source 후보가 생기면 관련 CTest와
    targeted perf를 통과해야 한다.
- 시작 시각: 2026-06-15 12:11:38 KST
- 기준 commit: `7ce06becc`
- 시작 git status:
  - core source diff는 SPOT logical queue 및 part-helper restore 계열만 남아 있다.
  - `framework/languages/dotnet/doc/guide/01-overview.ko.md` 변경과 `_workspace/`,
    기존 perf log untracked 파일은 이번 라운드 범위 밖이다.
- corrected baseline:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- historical baseline:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - 테스트 기준이 달랐을 수 있어 판정 기준으로 쓰지 않는다.
- 문제 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 retained 기준 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_104531_round65_final_spot_restore_all64_reduced_full.txt`

## 현재 관찰

- round65 standalone:
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_103459_round65_spot_sendsend_tcp_tls_after_pubsub_candidate_revert.txt`
  - May26 full 대비:
    - `MULTI_SPOT_SENDSEND/tcp/64`: `-7.15%`
    - `MULTI_SPOT_SENDSEND/tls/64`: `-1.77%`
  - 판정: standalone 기준으로 `-10%` 반복 회귀가 아니었다.
- round65 reduced full:
  - report:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_104531_round65_final_spot_restore_all64_reduced_full.txt`
  - May26 full 대비 worst에 `MULTI_SPOT_SENDSEND/tcp`가 남았다.
  - 판정: full run-order/load 영향인지, routed-send echo hot path 회귀인지 분리해야 한다.

## 가설

- 가설 1:
  - `SPOT_SENDSEND` reduced full 하락은 run-order/load 영향이다. standalone 반복에서는 May26 full 대비
    `-10%` 이상 하락이 재현되지 않는다.
- 가설 2:
  - routed-send data-plane queue 또는 encoded-size 계산이 echo 64B에서 여전히 병목이다.
    단, round65 `encoded_bytes` cache 후보는 standalone `tcp -0.66%`, `tls -0.38%`라 배제됐다.
- 가설 3:
  - SPOT restore가 one-way SPOT은 회복했지만, sendsend echo는 request/reply routing-id frame 또는
    reply-side part-helper 경로에서 별도 비용을 남긴다.
- 먼저 검증할 가설:
  - 가설 1. source 변경 전 `SPOT_SENDSEND tcp,tls,ws,wss 64B` standalone 반복을 실행해 회귀 재현성부터
    확인한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 아직 source 변경 전이다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - source 후보가 생기면 기록한다.

## Standalone 재측정

- command:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT_SENDSEND --transports tcp,tls,ws,wss --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round68_spot_sendsend_all_transport_recheck`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_121208_round68_spot_sendsend_all_transport_recheck.txt`
- load_avg:
  `0.26 1.39 3.91`
- completion:
  `success=4`, `fail=0`, `status=complete`

| 항목 | May26 full | round68 standalone | May26 대비 | problem 대비 |
|------|------------|--------------------|------------|--------------|
| `MULTI_SPOT_SENDSEND/tcp/64` | `271,206.0` | `251,074.4` | `-7.42%` | `+1.25%` |
| `MULTI_SPOT_SENDSEND/tls/64` | `254,009.6` | `252,953.6` | `-0.42%` | `+7.18%` |
| `MULTI_SPOT_SENDSEND/ws/64` | `240,791.0` | `238,693.6` | `-0.87%` | n/a |
| `MULTI_SPOT_SENDSEND/wss/64` | `252,557.8` | `254,324.6` | `+0.70%` | n/a |

## Call path 확인

- perf runner/client/server는 수정하지 않았다. 아래는 읽은 경로다.
- `bindings/c/perf/multi/src/perf_multi_spot_sendsend_client.cpp`,
  `bindings/c/perf/multi/src/perf_multi_spot_sendsend_server.cpp`
  - 패턴은 echo 계열이며 client가 routed send를 보내고 server가 reply를 보낸다.
- `core/src/api/socket/socket_message_send_api.cpp`
  - routed multipart helper는 첫 frame에서 `send_routed_scoped()`를 호출한다.
- `core/src/api/spot/request_reply/service_spot_request_reply_routed_delivery.cpp`
  - SPOT runtime routed send는 data-plane queue에 entry를 넣고 data-plane에서 drain한다.
  - round65 `encoded_bytes` cache 후보는 이 경로의 byte-size 재계산만 줄였지만
    standalone `tcp -0.66%`, `tls -0.38%`로 효과가 없었다.

## Round 68 판정

- `SPOT_SENDSEND`는 standalone low-load 조건에서 May26 full 대비 반복 `-10%` 회귀가 아니다.
- reduced full의 `SPOT_SENDSEND/tcp,tls` 큰 하락은 run-order/load 영향 후보로 분리한다.
- 이 경로에 local shortcut을 넣으면 data-plane routed send queue의 순서/재시도 의미를 바꿀 수 있어
  POSD 기준으로도 좋은 후보가 아니다.
- source 변경 없음. 추가 build/CTest는 필요하지 않다.
- 다음 후보는 최신 retained 기준에서 남은 one-way gap인 `MULTI_PUBSUB/tcp,tls,wss`다.
