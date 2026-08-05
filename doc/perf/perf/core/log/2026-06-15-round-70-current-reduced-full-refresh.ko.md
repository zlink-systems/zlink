# Round 70: 현재 retained 변경 64B reduced full 재검증

- goal:
  - SPOT restore 이후 현재 retained source 기준으로 64B reduced full을 다시 실행해 목표 수치와
    남은 gap을 갱신한다.
  - 완료 기준: `DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM`
    / `tcp,tls,ws,wss` / `64B` reduced full이 실패 0개로 끝나고, 문제 report 및 May26 corrected
    baseline 대비 평균/중앙값을 다시 계산한다.
- 시작 시각: 2026-06-15 12:30:50 KST
- 기준 commit: `7ce06becc`
- 시작 load_avg:
  - `/proc/loadavg`: `6.12 16.71 12.23`
- 시작 git status:
  - core source diff는 SPOT logical queue 및 part-helper restore 계열만 남아 있다.
  - framework dotnet/java 문서 변경과 `_workspace/`, 기존 perf log untracked 파일은 이번 라운드 범위 밖이다.
- corrected baseline:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- historical baseline:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - 테스트 기준이 달랐을 수 있어 판정 기준으로 쓰지 않는다.
- 문제 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 직전 retained 기준 reduced full:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_104531_round65_final_spot_restore_all64_reduced_full.txt`
  - 시작 load_avg: `22.48 15.02 10.96`

## 가설

- 가설 1:
  - 직전 reduced full의 일부 worst 항목은 높은 시작 load와 run-order 영향이 섞였다.
    낮은 load에서 재실행하면 standalone에서 회복된 `SPOT_SENDSEND`, `PUBSUB tcp/ws/wss`처럼
    전체 평균/중앙값도 올라갈 수 있다.
- 가설 2:
  - `PUBSUB/tls`는 standalone에서도 반복 하락이 확인됐으므로 reduced full에서도 남는다.
    하지만 단일 항목이므로 전체 64B 목표에는 제한적으로만 영향을 준다.
- 가설 3:
  - SPOT restore는 유지할 core 개선이지만 전체 64B 중앙값 +10%까지는 부족하다.
    이번 reduced full은 다음 후보를 고르는 최신 기준선으로 쓴다.
- 먼저 검증할 가설:
  - 가설 1. source 변경 없이 reduced full을 재실행한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - source 변경 없음.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - source 변경 없음. 직전 후보 원복 후 `cmake --build core/build -j$(nproc)`는 통과했다.

## 실행

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM \
  --transports tcp,tls,ws,wss \
  --duration 5 \
  --runs 3 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round70_current_reduced_full_refresh
```

- runner runtime:
  - `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- runner meta load_avg:
  - `2.96 14.39 11.66`
- result report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`
- completion:
  - success: 32
  - unsupported: 0
  - skip: 0
  - fail: 0
  - status: complete
  - expected_result_lines: 160
  - actual_result_lines: 160

## 비교 결과

### May26 full corrected baseline 대비

- baseline:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- common 64B 항목: 32
- 전체:
  - 평균 `-0.51%`
  - 중앙값 `-0.35%`
- one-way:
  - 평균 `-3.85%`
  - 중앙값 `-3.61%`
- echo:
  - 평균 `+1.50%`
  - 중앙값 `+1.65%`
- best:
  - `MULTI_SPOT/tls`: `+16.27%` (`5,939,903.4` -> `6,906,405.8`)
  - `MULTI_STREAM/ws`: `+12.34%` (`251,311.4` -> `282,312.6`)
  - `MULTI_STREAM/tcp`: `+8.87%` (`305,177.4` -> `332,250.4`)
  - `MULTI_DEALER_ROUTER/ws`: `+5.90%` (`394,390.8` -> `417,659.6`)
  - `MULTI_DEALER_ROUTER/tls`: `+5.04%` (`381,698.8` -> `400,949.2`)
- worst:
  - `MULTI_SPOT/wss`: `-14.70%` (`6,776,300.6` -> `5,780,216.2`)
  - `MULTI_PUBSUB/tls`: `-13.67%` (`2,623,065.0` -> `2,264,552.0`)
  - `MULTI_PUBSUB/wss`: `-9.23%` (`2,760,571.0` -> `2,505,671.8`)
  - `MULTI_SPOT_SENDSEND/tls`: `-7.74%` (`254,009.6` -> `234,358.8`)
  - `MULTI_PUBSUB/tcp`: `-7.25%` (`2,661,635.6` -> `2,468,643.4`)
  - `MULTI_SPOT_SENDSEND/tcp`: `-7.24%` (`271,206.0` -> `251,575.8`)
  - `MULTI_PUBSUB/ws`: `-5.53%` (`2,201,277.0` -> `2,079,550.8`)
  - `MULTI_STREAM/wss`: `-3.70%` (`184,722.2` -> `177,889.2`)

### May26 smoke corrected baseline 대비

- baseline:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
- common 64B 항목: 32
- 전체:
  - 평균 `-0.16%`
  - 중앙값 `-0.78%`
- one-way:
  - 평균 `-3.35%`
  - 중앙값 `-3.75%`
- echo:
  - 평균 `+1.75%`
  - 중앙값 `+3.00%`
- best:
  - `MULTI_SPOT/tls`: `+19.83%`
- worst:
  - `MULTI_SPOT/tcp`: `-17.17%`

### 문제 report 대비

- baseline:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- common 64B 항목: 26
- 전체:
  - 평균 `+4.59%`
  - 중앙값 `+2.09%`
- one-way:
  - 평균 `+3.95%`
  - 중앙값 `-5.48%`
- echo:
  - 평균 `+4.98%`
  - 중앙값 `+4.20%`
- best:
  - `MULTI_SPOT/tls`: `+84.71%` (`3,739,003.6` -> `6,906,405.8`)
  - `MULTI_STREAM/ws`: `+15.87%` (`243,650.8` -> `282,312.6`)
  - `MULTI_STREAM/tcp`: `+10.97%` (`299,395.0` -> `332,250.4`)
  - `MULTI_ROUTER_ROUTER/wss`: `+8.85%` (`337,912.6` -> `367,805.4`)
  - `MULTI_DEALER_ROUTER/tcp`: `+8.78%` (`395,963.2` -> `430,745.4`)
- worst:
  - `MULTI_PUBSUB/tls`: `-7.44%` (`2,446,707.8` -> `2,264,552.0`)
  - `MULTI_PUBSUB/wss`: `-6.50%` (`2,679,903.2` -> `2,505,671.8`)
  - `MULTI_PUBSUB/ws`: `-6.34%` (`2,220,372.2` -> `2,079,550.8`)
  - `MULTI_PUBSUB/tcp`: `-6.07%` (`2,628,104.8` -> `2,468,643.4`)
  - `MULTI_DEALER_DEALER/ws`: `-5.86%` (`3,156,838.0` -> `2,971,726.6`)
  - `MULTI_DEALER_DEALER/wss`: `-5.10%` (`3,265,432.2` -> `3,098,880.0`)
  - `MULTI_DEALER_DEALER/tcp`: `-5.09%` (`3,045,747.2` -> `2,890,774.8`)
  - `MULTI_DEALER_DEALER/tls`: `-4.77%` (`3,162,931.4` -> `3,011,993.8`)

## 판정

- 채택 유지:
  - SPOT logical queue 및 global part-helper restore는 retained 상태로 유지한다.
  - `SPOT/tls`와 `STREAM/tcp`, `STREAM/ws`는 May26 full 대비 의미 있는 상승이 있고,
    이 변경은 이전에 성능 하락을 만든 SPOT-owned helper 상태를 제거하는 방향이라 POSD의
    정보 은닉과 복잡성 감소에도 맞다.
- 목표 미달:
  - May26 full 대비 전체 평균/중앙값은 아직 음수다.
  - problem report 대비도 전체 평균 `+5%`에 못 미치고, one-way 중앙값은 음수다.
  - `STREAM/tcp`는 `332,250.4 ops/s`로 problem 대비 `+10.97%`, May26 full 대비 `+8.87%`지만
    사용자가 처음 말한 `400kops`에는 아직 못 미친다.
- 추가 채택 불가:
  - 이번 round에는 source 변경이 없었다.
  - 앞선 후보 중 `PUBSUB` empty-subscription active pipe, TLS speculative write, ASIO handler allocator
    확대처럼 상태나 분기를 늘리는 변경은 반복 수치가 작거나 하락 항목이 있어 유지하지 않는다.
  - 1~2% 개선이라도 하락 항목 없이 단순성이 유지되면 묶음 후보가 될 수 있지만, 이번에 확인된
    후보들은 POSD 관점에서 복잡도 증가 비용을 정당화하지 못했다.
- 다음 후보 선택 기준:
  - `PUBSUB/tls`는 standalone 반복에서도 하락이 재현됐으므로 다음 source 조사는 이 항목을 우선한다.
  - 다만 안전 가드 제거, 계약 변경, perf runner/client/server 조작은 제외한다.
