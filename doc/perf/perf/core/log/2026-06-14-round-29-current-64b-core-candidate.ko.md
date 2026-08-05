# Round 29: current 64B core 후보 재선정

- goal: 최신 clean core source와 현재 runner 조건에서 64B 전체 sweep를 다시 실행해 실패 항목과 반복 가능한 core hot path 후보를 선정한다.
- 완료 기준:
  - `core/build` runtime으로 current 64B sweep를 실행한다.
  - 문제 report 대비 10% 이상 하락 항목이 있으면 해당 pattern의 core call path를 읽고 최소 core 후보를 검토한다.
  - 반복 후보가 없으면 source 변경을 하지 않고, full failure gate를 다음 후보로 남긴다.
- 시작 시각: 2026-06-14 23:50 +0900
- 시작 git status:
  - core source diff 없음.
  - unrelated `.NET` 문서 변경과 기존 perf log untracked 파일이 있음.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 직전 관련 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_183228_round21_current_64b_sweep.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_225458_round27_current_oneway_send_poll_base.txt`
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_234533_round28_stream_tcp_connect128_clean.txt`

## 기준 비교

- 문제 report 기준 큰 64B 하락:
  - `MULTI_SPOT tcp`: `-47.21%`
  - `MULTI_SPOT tls`: `-46.00%`
  - `MULTI_PUBSUB tls`: `-26.61%`
  - `MULTI_PUBSUB tcp`: `-25.30%`
  - `MULTI_DEALER_DEALER tcp/tls/ws/wss`: 약 `-21.78%` ~ `-23.20%`
  - `MULTI_STREAM tcp`: `-25.17%`
- round28에서 `MULTI_STREAM tcp 64B` 하락의 큰 부분은 perf helper의 send serialization 영향으로 분리했다.
- 따라서 이번 round의 core 후보는 STREAM 단독이 아니라 64B 전체 sweep에서 다시 선정한다.

## 가설

- 가설 1: 최신 clean source의 64B sweep에서 문제 report 대비 10% 이상 하락이 다시 나타나고, 해당 pattern의 core hot path가 최소 변경 후보가 된다.
- 가설 2: 최신 clean source의 64B sweep은 문제 report 대비 10% 이상 안정 gap이 없고, 남은 장기 하락은 runner/측정 의미 변화와 run-to-run variance가 커서 바로 core 변경 후보가 아니다.
- 가설 3: failure가 다시 나오면 성능 개선보다 실패 안정화가 우선이며, 해당 transport/size를 단독 반복해 core runtime 실패인지 분리해야 한다.
- 선택한 가설: 먼저 가설 1과 3을 확인한다. 계획상 실패 0이 우선이고, source 후보는 같은 runner 조건에서 10% 이상 gap이 재현될 때만 정당화된다.

## 읽을 코드와 조건

- sweep에서 10% 이상 gap 또는 failure가 나온 pattern만 core call path를 읽는다.
- 이미 실패한 후보는 반복하지 않는다:
  - load-balancer one-active final helper
  - public send command poll 제거/throttle 확대
  - publish 단일 FINAL fast path
  - STREAM packet body view/direct xsend/inflight relaxed/read drain/native send/batch류
  - PUBSUB distributor final helper, small LMSG pool, SPOT ingress 직접 forward

## 변경

- core 소스 변경: 없음
- perf 소스 변경: 없음
- 변경 이유: source 후보 선정 전 current evidence를 갱신한다.
- perf 전용 변경이 아닌 이유: perf 코드는 수정하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: 측정 라운드이며 WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
- 추가로 실행한 회귀 테스트: 소스 변경이 없으면 별도 test는 실행하지 않는다.

## 검증 예정

- build:
  - `cmake --build core/build -j$(nproc)`
- current 64B sweep:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round29_current_64b_sweep`

## 결과: current 64B sweep

- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- current 64B sweep:
  - command:
    - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round29_current_64b_sweep`
  - runner 확인:
    - `Perf runtime libzlink: /home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - report:
    - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_235057_round29_current_64b_sweep.txt`
  - `META,commit`: `72d893595`
  - load_avg: `0.38 3.58 7.41`
  - completion: success `32`, fail `0`, status `complete`
- 문제 report 대비:
  - 공통 64B 26개 평균: `+0.91%`
  - 공통 64B 26개 중앙값: `+1.12%`
  - one-way 평균: `-5.78%`
  - one-way 중앙값: `-5.31%`
  - echo 평균: `+5.10%`
  - echo 중앙값: `+4.05%`
- 문제 report 대비 10% 이상 하락:
  - `MULTI_SPOT tcp 64B`: `3,896,078.6 -> 3,276,035.2`, `-15.91%`
- 기타 하락:
  - `MULTI_SPOT tls 64B`: `-9.35%`
  - `MULTI_PUBSUB tls 64B`: `-7.12%`
  - `MULTI_DEALER_DEALER tcp/ws/wss`: 약 `-5.29%` ~ `-5.62%`

## 판정

- 64B sweep는 failure 0이다.
- 문제 report 대비 10% 이상 하락한 항목은 `MULTI_SPOT tcp 64B` 하나다.
- 이 항목은 round21에서도 single sweep에서 후보가 됐지만 round22 standalone repeat에서 10% 이상 결손으로 재현되지 않았다.
- 따라서 source 수정 전 `MULTI_SPOT tcp 64B`를 5-run repeat로 다시 확인한다.

## 다음 검증

- `MULTI_SPOT tcp 64B` repeat:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round29_spot_tcp_repeat`

## 결과: MULTI_SPOT tcp 64B repeat

- command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round29_spot_tcp_repeat`
- runner 확인:
  - `Perf runtime libzlink: /home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_235824_round29_spot_tcp_repeat.txt`
- load_avg: `1.45 2.58 5.41`
- completion: success `1`, fail `0`, status `complete`
- runs:
  - `3,351,901`
  - `3,467,808`
  - `3,459,583`
  - `3,528,709`
  - `3,280,716`
- median:
  - `3,459,582.6 msg/s`
- 문제 report 대비:
  - `3,896,078.6 -> 3,459,582.6`
  - delta: `-11.20%`

## 갱신 판정

- `MULTI_SPOT tcp 64B`는 이번 round에서 10% 이상 결손으로 반복 재현됐다.
- source 후보 선정 조건을 만족하므로 SPOT data-plane publish/local fanout 경로를 읽는다.
- 이미 실패한 SPOT 후보인 publish ingress 직접 forward, per-message pump 제거, owned frame 단순 vector 전환은 반복하지 않는다.

## 2026-06-15 00:03 KST 후보: 준비된 mesh peer가 없을 때 SPOT mesh publish 생략

근거:
- `MULTI_SPOT tcp 64B` 반복 중앙값은 문제 리포트 대비 -11.20%로 10% 이상 하락했다.
- 단일 노드 SPOT publish도 `runtime_->mesh_pub != NULL` 조건 때문에 local fanout 뒤에 mesh PUB publish를 한 번 더 수행한다.
- `mesh_pub_ready_peer_count`는 remote subscription readiness에 의해 갱신되는 기존 런타임 상태다. 준비된 peer가 0이면 user payload mesh publish 수신자가 없으므로 데이터 메시지 mesh publish를 생략하는 후보를 검증한다.

변경:
- `spot_data_plane_forwarding.cpp`에 `mesh_publish_needed_local()` 추가.
- publish ingress queue 및 pub ingress socket 경로의 `need_mesh` 판단을 `runtime_->mesh_pub != NULL`에서 `mesh_pub_ready_peer_count(...) > 0`로 변경.

검증 예정:
- core build
- SPOT 관련 CTest
- `MULTI_SPOT tcp 64B` 반복 perf

결과:
- build: 통과
- SPOT 관련 CTest: 실패
  - `test_spot_pubsub_scenario` 6개 case 실패
  - 대표 실패: `test_spot_peer_tcp_reverse_publish`, `test_spot_multi_publisher`, `test_spot_aggregate_subscription_refcount`, `test_spot_node_direct_local_and_child_interop`

판정:
- readiness 카운트만으로 mesh publish를 생략하면 현재 SPOT interop 계약을 깨뜨린다.
- 후보는 폐기하고 원복했다.

## 후보: SPOT publish ingress 성공 경로에서 raw part copy를 move로 전환

근거:
- `zlink_spot_publish_part(..., ZLINK_PART_FINAL)` fast path는 성공 시 caller part를 소비하는 submit API다.
- 기존 `enqueue_publish_ingress()`는 큐 입장 전에 `zlink_msg_copy()`로 owned parts를 만들고, 성공 후 원본 parts를 닫았다.
- 실패 시 재시도 의미를 보존하려면 큐 room 확인 전에는 원본 part를 건드리면 안 된다.

변경:
- raw parts의 encoded byte 수를 복사 없이 계산한다.
- queue room/closed 확인 후, 성공 경로에서만 `zlink_msg_move()`로 staged entry에 소유권을 넘긴다.
- 성공 후 기존처럼 caller parts 배열을 `zlink_multipart_close()`로 닫는다.

성능 결과:
- command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern SPOT --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round29_spot_tcp_move_ingress_parts`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_000738_round29_spot_tcp_move_ingress_parts.txt`
- runs: 3,476,856 / 5,120 / 3,821,340 / 3,879,832 / 3,982,762 ops/s
- median: 3,821,340.0 ops/s
- problem report 대비: 3,896,078.6 -> 3,821,340.0, -1.92%
- round29 clean repeat median 대비: 3,459,582.6 -> 3,821,340.0, +10.46%

주의:
- run 2에 5K ops/s outlier가 있다. median에는 영향이 없지만 sweep 전 관련 CTest를 재실행하고 필요하면 조용한 상태에서 재측정한다.

full 64B sweep 결과:
- command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --duration 5 --runs 1 --connect-ready-timeout-ms 5000 --results-tag round29_move_ingress_parts_64b_sweep`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_001016_round29_move_ingress_parts_64b_sweep.txt`
- completion: success 29, fail 3, status partial
- 실패: `MULTI_STREAM tls 64B`, fail-fast로 STREAM ws/wss는 미실행

주요 값:
- `MULTI_SPOT tcp 64B`: 3,832,600.0 ops/s
  - 문제 report 3,896,078.6 대비 -1.63%
  - round29 clean sweep 3,276,035.2 대비 +16.99%
- `MULTI_STREAM tcp 64B`: 325,290.8 ops/s
  - 문제 report 299,395.0 대비 +8.65%
  - 2026-05-13 baseline 400,124.6 대비 -18.70%

문제 report 대비 공통 23개 비교:
- 전체 평균 +0.92%, 중앙값 +0.96%
- one-way 평균 -3.27%, 중앙값 -4.74%
- echo 평균 +4.15%, 중앙값 +3.01%
- 최저: `MULTI_PUBSUB ws` -8.74%
- 최고: `MULTI_ROUTER_ROUTER wss` +10.05%

판정:
- SPOT/tcp 회복에는 유효하나 전체 목표에는 부족하다.
- full gate도 STREAM/tls 실패 때문에 아직 통과하지 않았다.
- 사용자가 정정한 STREAM/tcp 64B baseline 목표 400kops 기준으로 STREAM/tcp core 병목을 계속 추적한다.

STREAM 추가 확인:
- `perf` 도구는 설치되어 있지 않아 `perf record/report`로 CPU hotspot을 볼 수 없다.
- STREAM/tls 단독 재실행:
  - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tls --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round29_stream_tls_retry_after_move_ingress`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_001908_round29_stream_tls_retry_after_move_ingress.txt`
  - completion: success 1, fail 0, status complete
  - `MULTI_STREAM tls 64B`: 233,848.8 ops/s
  - 판정: full sweep의 STREAM/tls 실패는 일시 실패로 분리한다.
- STREAM/tcp 단독 반복:
  - command: `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-concurrency 128 --connect-ready-timeout-ms 5000 --results-tag round29_stream_tcp_repeat_after_move_ingress`
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_001927_round29_stream_tcp_repeat_after_move_ingress.txt`
  - completion: success 1, fail 0, status complete
  - `MULTI_STREAM tcp 64B`: 327,576.8 ops/s
  - baseline 400,124.6 대비 -18.13%
  - problem report 299,395.0 대비 +9.41%

SPOT move 후보 최종 판정:
- 방어 검사 없는 중간 형태에서는 `MULTI_SPOT tcp 64B`가 3.82M까지 보였으나, 기존 invalid frame 방어 의미를 보존하려면 raw size 계산 전에 frame 유효성 검사가 필요하다.
- 방어 검사 포함 최종 형태 재측정:
  - report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_002508_round29_spot_tcp_move_ingress_parts_validated_quiet.txt`
  - runs: 3,299,104 / 3,359,571 / 3,488,425 / 3,472,479 / 3,500,312 ops/s
  - median: 3,472,479.2 ops/s
  - round29 clean repeat median 3,459,582.6 대비 +0.37%
- 판정: clear win이 아니므로 폐기했다.
- `spot_data_plane_forwarding.cpp`는 원복했다.
