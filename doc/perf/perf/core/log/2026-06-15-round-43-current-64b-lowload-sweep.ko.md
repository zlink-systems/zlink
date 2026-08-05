# Round 43: current 64B low-load sweep

- goal: 낮은 부하 상태에서 current clean core 64B sweep를 다시 실행해 반복 가능한
  전체 64B hot path 후보를 재선정한다.
- 완료 기준:
  - core/perf source diff가 없는 상태에서 64B sweep 실행
  - 문제 report와 baseline 대비 64B 통계를 다시 계산
  - 10% 이상 반복 gap이 확인된 항목만 다음 core 후보로 선정
- 기준 commit: `72d893595`
- 시작 git status:
  - `core/src`, `core/include`, `core/tests`, `bindings/c/perf` source diff 없음
  - perf 로그 파일 untracked 다수 존재
- 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 기존 current 64B sweep:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_235057_round29_current_64b_sweep.txt`

## 이전 후보 배제

- STREAM 계열:
  - packet view/copy fast path, dispatch inflight relaxed, current EAGAIN short-circuit,
    direct xsend, native send, read-drain/batch 조정은 400k 또는 clean 대비 개선을 만들지 못해 원복했다.
- PUBSUB 계열:
  - `zlink_publish_part()` FINAL fast path 후보는 `MULTI_PUBSUB` 64B에서 10% 개선이 없어서 원복했다.
- SPOT 계열:
  - ingress direct forward, raw part move, encoded byte combine, DONTWAIT admission,
    local fanout refresh 조건화, mesh publish skip은 실패했거나 반복 개선이 아니어서 원복했다.
- public send command poll:
  - 단순 제거는 계약 테스트 실패, throttle 조정은 clear win 없음.

## 가설

- 가설 1: round29의 worst 항목 일부는 부하/순서/variance 영향이며, 낮은 부하 재측정에서
  문제 report 대비 10% 이상 반복 gap이 줄어든다. 이 경우 source 변경보다 다음 반복 gap을
  새로 골라야 한다.
- 가설 2: 낮은 부하에서도 SPOT/PUBSUB/DEALER_DEALER one-way 중 하나 이상이 문제 report 대비
  10% 이상 낮게 반복된다. 이 경우 해당 pattern의 core data path를 다음 후보로 삼는다.
- 선택한 가설: 먼저 가설 1과 2를 측정으로 분리한다.

## 실행 전 부하

- `uptime`: load average `1.56 7.58 9.16`
- 남은 benchmark 프로세스: 없음.

## 실행 명령

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,SPOT_REQREP,SPOT_SENDSEND,STREAM \
  --transports tcp,tls,ws,wss \
  --duration 5 \
  --runs 5 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round43_current_64b_lowload_sweep
```

## 실행 결과

- report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_025722_round43_current_64b_lowload_sweep.txt`
- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- completion: `success=32`, `fail=0`, `status=complete`
- total benchmark time: `29m 40s`

## 문제 report 대비 판정

- 문제 report:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 이번 current sweep에서 문제 report 대비 10% 이상 낮은 64B 항목: 없음.
- 주요 항목:
  - `MULTI_DEALER_DEALER/tcp/64`: `3,045,747.2 -> 2,906,389.0`, `-4.6%`
  - `MULTI_PUBSUB/tcp/64`: `2,628,104.8 -> 2,490,019.2`, `-5.3%`
  - `MULTI_SPOT/tcp/64`: `3,896,078.6 -> 3,515,067.8`, `-9.8%`
  - `MULTI_SPOT_REQREP/tcp/64`: `246,283.6 -> 261,351.4`, `+6.1%`
  - `MULTI_SPOT_SENDSEND/tcp/64`: `247,978.4 -> 243,517.6`, `-1.8%`
  - `MULTI_STREAM/tcp/64`: `299,395.0 -> 320,996.6`, `+7.2%`

## historical baseline 대비 판정

- historical baseline:
  `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 사용자 정정 기준:
  `400kops` 목표는 `MULTI_STREAM/tcp/64B`이며 WS/WSS 기준이 아니다.
- `MULTI_STREAM/tcp/64`: `400,124.6 -> 320,996.6`, `-19.8%`
- `MULTI_STREAM/ws/64`: `366,639.2 -> 291,834.2`, `-20.4%`
- `MULTI_STREAM/tls/64`: `242,016.4 -> 230,458.8`, `-4.8%`
- `MULTI_STREAM/wss/64`: `206,699.2 -> 189,754.4`, `-8.2%`

## 다음 후보 선정

- 문제 report 대비 10% 이상 반복 gap은 이번 sweep에서 사라졌다.
- 다만 사용자 기준인 `MULTI_STREAM/tcp/64B` historical baseline `400,124.6 ops/s`
  대비 current `320,996.6 ops/s`는 아직 `-19.8%`로 목표 미달이다.
- 이전 round35/41 근거:
  - baseline commit replay는 `381,021.6 ops/s`까지 재현됐다.
  - perf helper의 `send_mutex` 제거 진단은 `375,799.2`에서 `383,141.6 ops/s`까지 회복했다.
  - baseline commit의 core stream dispatch 구현은 현재와 의미상 큰 차이가 없었다.
- 따라서 다음 source 후보는 STREAM packet dispatch/send의 core-only 비용 중
  이미 원복한 후보(packet view/copy, inflight relaxed, current EAGAIN, direct xsend)를 제외하고
  찾아야 한다. perf helper의 `send_mutex` 제거는 400k gap 설명력은 높지만 이번 core-only
  목표의 유지 변경으로 채택하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음. 측정-only 라운드.
- 보안 의미를 유지한 근거: source 변경 없음.
- 추가로 실행한 회귀 테스트: source 후보가 생기면 기록한다.
