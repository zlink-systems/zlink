# `core` 성능 개선 실행 가이드

> 상태: active
> 기준 baseline:
> - `core/tools/perf/perf_linux_recv_20260323_094627.txt`
> - `core/tools/perf/perf_linux_callback_20260323_082648.txt`
> 실행 루프: `core/tools/perf/run_perf_improvement_loop.sh`
> 상위 supervisor: `core/tools/ralphloop/run_codex_execution_guide_loop.sh`
> 로그 디렉터리: `core/tools/perf/logs/`
> 대상 범위: `core/`, `core/tests/`, `core/tools/perf/`
> 목적: baseline 초과까지 중단 없이 반복 수행할 실전 실행 규칙 고정

`run_perf_improvement_loop.sh`는 별도 락으로 막지 않는다.
대신 `core/tools/perf/logs/` 아래 로그 파일이 실제로 갱신되는 `perf` 실행이 감지되면
그 로그를 따라가며 대기하고, 완료되면 다음 iteration을 시작한다.
프로세스는 남아 있는데 로그 갱신이 멈춘 경우에는 stale로 간주하고
해당 `perf` 프로세스를 정리한 뒤 루프를 계속 진행한다.

## 1. 문서 목적

이 문서는 `core` 성능 개선 작업의 단일 authority다.
기존의 "마스터 플랜"과 "실행 가이드" 역할을 이 문서 하나에 통합한다.

이 문서의 핵심 목표는 아래 한 줄이다.

```text
baseline 두 파일에 기록된 모든 공식 측정 항목보다 현재 결과가 낮은 상태로 종료하지 않고,
모든 항목을 상회하는 증거를 남길 때까지 개선을 반복한다.
```

여기서 "모든 항목"은 다음 의미로 고정한다.

- `recv` baseline의 모든 패턴/transport/msg_size 조합
- `callback` baseline의 모든 패턴/transport/msg_size 조합
- 공식 결과 파일에 실제로 표로 기록된 throughput/bandwidth/latency 지표
- queue pending, timeout, skip, crash 같은 품질 신호

즉 일부 대표 tuple만 좋아진 상태, 특정 transport만 복구된 상태,
평균만 개선되고 tail latency가 악화된 상태는 완료로 보지 않는다.

## 2. authority와 범위

이 문서의 해석 규칙은 아래로 고정한다.

- 구현 방향, 실행 순서, 중단 규칙, 종료 판정 모두 이 문서가 authority다.
- 이 실행은 단일 문서 체계로 운영한다. 별도 main/master/gap/residual 보조 문서를 만들지 않는다.
- baseline 자체는 위 두 성능 결과 파일이 authority다.
- 성능 개선 요청이라도 perf-only shortcut으로 닫지 않는다.
- 구조 문제로 판단되면 POSD 원칙에 따라 `core` 내부 복잡도를 줄이는 방향으로 고친다.

기본 작업 범위는 아래로 고정한다.

- 허용:
  - `core/`
  - `core/tests/`
  - `core/tools/perf/`
- 기본 비허용:
  - `core/bench/`
  - `core/perf/`를 workaround surface로 바꾸는 수정
  - bindings 전용 우회

단, 공식 perf 실행기 자체의 버그 때문에 baseline 비교가 불가능한 경우에만
`core/perf/` 수정이 허용된다.
그 경우에도 목적은 harness 우회가 아니라 공식 측정 surface 복구여야 한다.

## 3. 설계 원칙

성능 작업은 아래 원칙을 동시에 만족해야 한다.

1. 성능 수치가 아니라 hot path 설명이 먼저 단순해져야 한다.
2. perf 전용 hidden fast path를 추가하지 않는다.
3. bench/perf에서만 켜지는 내부 shortcut으로 결과를 만들지 않는다.
4. queue/HWM/ready/monitor 계약을 약화해서 수치를 올리지 않는다.
5. 한 병목을 고치며 다른 tuple을 깨뜨리면 종료하지 않는다.
6. 복잡도를 늘리는 얇은 wrapper보다, ownership과 lifecycle을 더 명확히 만드는 deep module을 선호한다.

성능 개선의 우선 순위는 아래처럼 둔다.

1. 측정 신뢰도 복구
2. worst tuple 식별
3. 공통 hot path 단순화
4. transport/pattern별 잔여 병목 제거
5. full baseline 초과 재검증

## 4. baseline 해석 규칙

baseline은 archive가 아니라 active acceptance 기준이다.

- `recv`는 `MULTI_*` 표면을 기준으로 본다.
- `callback`은 single callback 표면을 기준으로 본다.
- 한 파일 안의 모든 표 행이 acceptance 대상이다.
- 특정 항목이 현재 실행에서 skip/fail되면 baseline 미달로 간주한다.
- throughput/bandwidth는 baseline 초과가 목표다.
- latency 계열(`Lat.Mean`, `Lat.P95`, `Lat.P99`)은 baseline 이하가 목표다.
- queue pending 관련 최대치/종료 잔량은 baseline 이하가 목표다.
- 동일 항목이 수치상 비슷해 보여도 오차 핑계로 닫지 않는다. baseline 초과를 명시적으로 확인해야 한다.

## 5. 운영 원칙

핵심 원칙은 아래 두 줄로 고정한다.

```text
가장 나쁜 tuple부터 고치되, baseline 미달 항목이 남아 있으면 종료하지 않는다.
perf 수치만 올리는 우회가 아니라 core hot path를 더 단순하게 설명하는 수정으로만 닫는다.
```

중단 허용 조건은 아래뿐이다.

1. 사용자 결정이 필요한 공개 계약 변경
2. 로컬 환경 자체가 깨져 공식 perf 실행이 불가능한 상태
3. 사용자 변경과 직접 충돌하는 dirty worktree

위 세 경우가 아니면 작업을 멈추지 않는다.

## 6. 세션 시작 절차

매 세션 시작 시 아래를 순서대로 수행한다.

1. 이 문서 전체를 다시 읽는다.
2. `14.0 상태표`에서 `완료`가 아닌 첫 항목을 고른다.
3. baseline 두 파일에서 해당 항목의 기준 수치를 다시 확인한다.
4. 최신 결과와 비교해 실제 worst tuple을 정한다.
5. 구조 병목 가설을 적고 바로 코드/테스트/재측정으로 진행한다.

## 7. 공식 실행 명령

빌드와 공식 측정은 아래 명령만 사용한다.

```bash
cmake -S . -B core/build -DZLINK_BUILD_TESTS=ON
cmake --build core/build -j"$(nproc)"

ctest --test-dir core/build --output-on-failure -L unittest -j"$(nproc)"
ctest --test-dir core/build --output-on-failure -L integration -j1

./core/perf/run_benchmarks_multi.sh \
  --build-dir core/build \
  --reuse-build \
  --recv recv

./core/perf/run_benchmarks_multi.sh \
  --build-dir core/build \
  --reuse-build \
  --recv callback \
  --pattern SPOT

./core/perf/run_benchmarks.sh \
  --build-dir core/build \
  --reuse-build \
  --recv callback
```

해석 규칙:

- `recv` baseline 검증은 `run_benchmarks_multi.sh --recv recv`가 공식이다.
- multi callback은 현재 공식 surface 제약 때문에 `SPOT`/`STREAM` 중심 spot 확인에만 쓴다.
- `callback` baseline 검증은 `run_benchmarks.sh --recv callback`이 공식이다.
- 최종 acceptance는 `recv full + callback full` 둘 다 있어야 한다.

## 8. 권장 spot 재측정

전체 full run 전에는 spot 재측정으로 병목을 줄인다.

권장 명령:

```bash
./core/perf/run_benchmarks_multi.sh \
  --build-dir core/build \
  --reuse-build \
  --recv recv \
  --pattern DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,PUBSUB,SPOT,STREAM \
  --transports tcp,tls,ws,wss \
  --msg-sizes 64,256,1024,65536,131072,262144

./core/perf/run_benchmarks.sh \
  --build-dir core/build \
  --reuse-build \
  --recv callback \
  --pattern PAIR,PUBSUB,DEALER_DEALER,DEALER_ROUTER,ROUTER_ROUTER,GATEWAY,SPOT \
  --transports inproc,ipc,tcp,tls,ws,wss \
  --msg-sizes 64,256,1024,65536,131072,262144
```

spot 재측정도 baseline보다 낮은 tuple을 찾기 위한 수단이지,
full acceptance를 대체하지 않는다.

## 9. 수정 단위 규칙

하나의 iteration은 아래 묶음으로 닫는다.

1. worst tuple 선정
2. 원인 코드 읽기
3. 필요한 회귀 테스트 추가 또는 기존 테스트 보강
4. `core/` 수정
5. build + 관련 test
6. spot perf 재측정
7. 상태표 갱신

금지:

- 여러 병목을 한 커밋에 섞어 원인 추적 불가 상태 만들기
- perf 결과만 보고 구조 설명 없이 문서 닫기
- 테스트를 perf-friendly하게 약화
- 공식 runner 대신 비공식 실행 결과로 acceptance 판정

반복 루프는 아래 순서로 고정한다.

1. baseline과 최신 결과를 비교해 worst tuple을 고른다.
2. 그 tuple이 드러내는 공통 병목을 코드에서 설명한다.
3. `core/tests/` 범위에서 계약 회귀가 필요한지 먼저 추가한다.
4. `core/`를 수정한다.
5. spot 재측정으로 영향 범위를 확인한다.
6. 좋아졌더라도 다른 tuple 악화 여부를 확인한다.
7. 충분한 개선이 보이면 full recv/callback 재측정으로 baseline을 다시 비교한다.
8. 문서와 로그를 갱신하고 다음 worst tuple로 이동한다.

핵심은 "한 번의 큰 실험"이 아니라
"작은 구조 수정 + 재측정 + baseline 재비교"의 반복이다.

## 10. 장시간 실행 규칙

full perf는 오래 걸릴 수 있으므로 아래 규칙을 따른다.

- 로그는 반드시 파일과 콘솔에 동시에 남긴다.
- 장시간 실행 시작 후에는 완료 여부 확인 외 다른 작업으로 넘어가지 않는다.
- 실패하면 같은 항목을 owner로 유지하고 즉시 수정 루프로 되돌아간다.
- skip/timeout/crash도 baseline 미달로 취급한다.

권장 로그 예시:

```bash
mkdir -p core/tools/perf/logs

./core/perf/run_benchmarks_multi.sh \
  --build-dir core/build \
  --reuse-build \
  --recv recv \
  2>&1 | tee core/tools/perf/logs/perf_recv_full_$(date +%Y%m%d_%H%M%S).log

./core/perf/run_benchmarks.sh \
  --build-dir core/build \
  --reuse-build \
  --recv callback \
  2>&1 | tee core/tools/perf/logs/perf_callback_full_$(date +%Y%m%d_%H%M%S).log
```

## 11. 종료 판정

아래 조건을 모두 만족할 때만 이 문서 기준으로 종료할 수 있다.

1. `14.0 상태표` 전 행이 `완료`
2. 최신 `recv` 공식 결과 파일 확보
3. 최신 `callback` 공식 결과 파일 확보
4. 두 결과 모두 baseline 상회 판정 메모 기록
5. skip/fail tuple 없음

종료 직전에는 아래를 반드시 다시 확인한다.

1. baseline 두 파일을 다시 읽는다.
2. 최신 결과 파일 경로가 문서에 적혀 있는지 확인한다.
3. `완료`가 아닌 행이 없는지 확인한다.
4. full run이 아니라 spot run만 있는 행이 없는지 확인한다.

아래는 완료가 아니다.

- 일부 대표 transport만 개선
- 평균 latency만 개선
- callback만 개선하고 recv가 퇴행
- 단일 실험 옵션에서만 baseline 초과
- 문서와 로그 없이 "재측정해보니 좋아 보임" 상태

## 12. 금지 규칙

- baseline보다 낮은 항목이 남아 있는데 종료하지 않는다.
- 성능 문제를 테스트 약화로 숨기지 않는다.
- 대용량 tuple 악화를 소형 tuple 개선으로 상쇄했다고 해석하지 않는다.
- transport별 임시 ifdef/환경변수 우회로 닫지 않는다.
- ad-hoc binary나 `/tmp` repro로 근거를 대체하지 않는다.
- `core/build/` 외 다른 build 디렉터리를 쓰지 않는다.

## 13. 산출물

이 루프의 필수 산출물은 아래와 같다.

- 최신 공식 perf 결과 파일 경로
- worst tuple과 원인 메모
- 적용한 코드 변경
- 관련 회귀 테스트 또는 기존 회귀 재사용 근거
- baseline 상회 여부를 적은 상태표
- 루프 로그 디렉터리

## 14. 체크리스트

## 14.0 상태표

상태 값은 아래 네 개만 사용한다.

- `미착수`
- `진행중`
- `검증중`
- `완료`

| ID | 항목 | 상태 | 실행 내용 | 검증 증거 |
|---|---|---|---|---|
| P1 | baseline authority와 비교 규칙 고정 | 완료 | baseline 두 파일과 단일 authority 문서 생성 | `core/tools/perf/core-perf-optimization-execution-guide.ko.md` |
| P2 | recv worst tuple 식별 및 1차 병목 제거 | 진행중 | 기존 `16 MiB` large-message byte-budget cap과 topology-stable bootstrap re-broadcast suppression 위에, `core/src/services/spot/spot_node_control.cpp`의 secure transport subscription replay retry 창을 warmup 구간 쪽으로 압축했다. `subscription_replay_attempt_count`를 `tls: 150 -> 20`, `wss: 300 -> 40`, `ws/default: 50 -> 10`으로 줄이고, 재시도 holdoff를 transport-aware `2/5 tick` helper로 바꿔 repeated control replay가 active phase까지 길게 끌리지 않도록 정리했다. 관련 회귀(`test_multi_spot_benchmark_process`, `test_spot_service_introspection_member_peers`, `unittest_spot_data_plane_protocol`, `unittest_spot_data_plane_budget`)는 clean pass했고, `sync_mesh_peer_monitor_state`의 endpoint-membership 분류 helper와 unit coverage도 추가했다. accepted 상태의 최신 targeted 공식 run `perf_linux_recv_20260327_103525.txt`는 `MULTI_SPOT/tls/131072`에서 `27.876 Kmsg/s`, `3653.816 MB/s`, `mean/P95/P99 181.487/229.897/243.824 ms`를 기록해 partial fail은 재현되지 않았지만 baseline latency(`108.176/153.935/172.448 ms`) 대비 여전히 크게 높다. 같은 iteration에서 connected-peer replay deferral 실험을 했지만 `perf_linux_recv_20260327_101759.txt`, `perf_linux_recv_20260327_101807.txt`, `perf_linux_recv_20260327_101816.txt`가 latency를 더 악화했고 process regression도 깨뜨려 같은 iteration 안에서 되돌렸다. 이어서 mesh monitor ready-count-only wake suppression 실험도 했지만 `perf_linux_recv_20260327_105121.txt`, `perf_linux_recv_20260327_105137.txt`가 각각 `mean/P95/P99 679.475/1245.451/1357.077 ms`, `302.037/444.207/473.449 ms`로 baseline과 best-known 대비 모두 악화돼 runtime 동작은 같은 iteration 안에서 되돌렸다. 이번 iteration의 ingress/mesh contention-only `4 MiB` batch split 실험도 targeted 공식 run `perf_linux_recv_20260327_110714.txt`에서 `10.042 Kmsg/s`, `1316.173 MB/s`, `mean/P95/P99 292.937/378.565/396.995 ms`로 baseline과 best-known 대비 즉시 악화돼 같은 iteration 안에서 되돌렸고, 현재 코드는 replay window 압축 + 기존 `16 MiB` cap 상태를 유지한다. full recv는 계속 보류하고 다음 iteration에서는 replay scheduling/control-task wake/batch split 재실험 없이 client active-phase 진입 편차 자체를 직접 좁히는 쪽으로 이동해야 한다 | `core/src/services/spot/spot_node_control.cpp`; `core/src/services/spot/spot_data_plane_forwarding.cpp`; `core/src/services/spot/spot_data_plane_loop.cpp`; `core/src/services/spot/spot_data_plane_protocol.cpp`; `core/src/services/spot/spot_data_plane_internal.hpp`; `core/tests/CMakeLists.txt`; `core/tests/integration/monitoring/test_multi_spot_benchmark_process.cpp`; `core/tests/unittest/unittest_spot_data_plane_protocol.cpp`; `core/tests/unittest/unittest_spot_data_plane_budget.cpp`; `core/perf/results/multi/report/perf_linux_recv_20260327_094749.txt`; `core/perf/results/multi/report/perf_linux_recv_20260327_095214.txt`; `core/perf/results/multi/report/perf_linux_recv_20260327_095302.txt`; `core/perf/results/multi/report/perf_linux_recv_20260327_100924.txt`; `core/perf/results/multi/report/perf_linux_recv_20260327_101006.txt`; `core/perf/results/multi/report/perf_linux_recv_20260327_103525.txt`; `core/perf/results/multi/report/perf_linux_recv_20260327_105121.txt`; `core/perf/results/multi/report/perf_linux_recv_20260327_105137.txt`; `core/perf/results/multi/report/perf_linux_recv_20260327_110714.txt` |
| P3 | callback worst tuple 식별 및 1차 병목 제거 | 미착수 | 첫 full callback 재측정 후 worst tuple 선정 | 미기록 |
| P4 | cross-check: recv 개선이 callback을 깨지 않는지 확인 | 미착수 | 상호 영향 검증 | 미기록 |
| P5 | cross-check: callback 개선이 recv를 깨지 않는지 확인 | 미착수 | 상호 영향 검증 | 미기록 |
| P6 | full recv 결과가 baseline 전체를 상회 | 미착수 | 공식 full recv 재측정 및 비교 메모 | 미기록 |
| P7 | full callback 결과가 baseline 전체를 상회 | 미착수 | 공식 full callback 재측정 및 비교 메모 | 미기록 |
| P8 | 최종 문서/로그/증거 정리 | 미착수 | 결과 경로와 판단 근거 갱신 | 미기록 |

## 14.1 항목 진행 규칙

- 항상 `완료`가 아닌 첫 항목부터 진행한다.
- `검증 증거`가 비어 있으면 `완료`로 바꾸지 않는다.
- `P6`, `P7`은 반드시 full run 결과를 요구한다.
- 한 항목이 끝나면 즉시 다음 항목으로 이동한다.

## 14.2 iteration 종료 시 문서 갱신

매 iteration 끝에는 아래를 반영한다.

1. 방금 건드린 병목과 관련 파일
2. 실행한 test/perf 명령
3. 생성된 로그/결과 파일 경로
4. baseline 대비 남은 부족 항목
5. 다음 iteration의 첫 타깃

## 14.3 진행 메모

- 2026-03-27: 공식 full recv `core/perf/results/multi/report/perf_linux_recv_20260327_064255.txt`에서 `MULTI_DEALER_DEALER/tls`와 `MULTI_SPOT/tcp`가 partial/fail로 확인됨.
- 2026-03-27: `core/tests/integration/monitoring/test_multi_spot_benchmark_process.cpp`에 `tcp 131072` 및 `tcp 64..262144` sequence 회귀를 추가했고 둘 다 통과했다. `MULTI_SPOT/tcp`는 단일 pattern targeted 공식 run `perf_linux_recv_20260327_071822.txt`에서 clean이므로 다음 full recv에서 다시 cross-pattern 영향을 확인해야 한다.
- 2026-03-27: `core/tests/integration/monitoring/test_multi_dealer_dealer_benchmark_process.cpp`를 추가해 split process contract를 고정했다. `core/perf/multi/src/perf_multi_dealer_dealer_server.cpp`와 `core/perf/multi/src/perf_multi_dealer_dealer_client.cpp`에 runner용 start/done 동기화를 추가한 뒤 targeted 공식 run `perf_linux_recv_20260327_071721.txt`가 clean으로 복구됐다.
- 2026-03-27: 공식 full recv `core/perf/results/multi/report/perf_linux_recv_20260327_072300.txt`는 `DEALER_DEALER/tls` fail 없이 complete로 종료됐다. baseline 비교 결과 새 worst tuple은 `MULTI_SPOT/tls/131072`였고, tail latency(`P95/P99`)와 secure SPOT bandwidth가 주된 잔여 미달 항목으로 남았다.
- 2026-03-27: `core/perf/multi/src/perf_multi_spot_server.cpp`에 pub delivery-ready quorum gate를 추가하고 `core/tests/integration/monitoring/test_multi_spot_benchmark_process.cpp`에 `tls 65536,131072,262144` sequence 회귀를 추가했다. `ctest --test-dir core/build --output-on-failure -R test_multi_spot_benchmark_process -j1`는 120.07초에 통과했다.
- 2026-03-27: targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_075123.txt`에서 `MULTI_SPOT/tls/131072` throughput은 `12.577 Kmsg/s -> 19.418 Kmsg/s`, bandwidth는 `1648.519 MB/s -> 2545.156 MB/s`, mean latency는 `821.785 ms -> 404.103 ms`로 개선됐다. 다만 `P95/P99`는 baseline 대비 여전히 크게 높아 full recv 재실행 전 추가 구조 수정이 더 필요하다.
- 2026-03-27: `core/src/services/spot/spot_mesh_pub_budget.cpp`에서 secure multi-peer mesh pub budget을 `tls: 768 -> 2048`, `wss: 128 -> 512`로 키우고 `core/tests/unittest/unittest_spot_data_plane_budget.cpp` 기대값을 갱신했다. `ctest --test-dir core/build --output-on-failure -R unittest_spot_data_plane_budget -j1`, `ctest --test-dir core/build --output-on-failure -R test_spot_service_introspection -j1`, `ctest --test-dir core/build --output-on-failure -R test_multi_spot_benchmark_process -j1`가 모두 통과했다.
- 2026-03-27: targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_080510.txt`에서 `MULTI_SPOT/tls/131072` throughput은 `15.988 Kmsg/s`, bandwidth는 `2095.579 MB/s`, mean latency는 `293.634 ms`, `P95/P99`는 `376.334/392.574 ms`로 추가 개선됐다. 이전 targeted run 대비 throughput은 일부 내려왔지만 tail latency와 server memory(`665.110 MB -> 470.120 MB`)는 개선됐다. baseline 대비 `throughput/bandwidth`는 여전히 상회하지만 `mean/P95/P99 latency`는 아직 미달이다.
- 2026-03-27: `core/src/services/spot/spot_data_plane_forwarding.cpp`와 `core/src/services/spot/spot_data_plane_protocol.cpp`에 large-message byte-budget cap(`16 MiB`)을 추가했다. 관련 process regression `ctest --test-dir core/build --output-on-failure -R test_multi_spot_benchmark_process -j1`는 124.10초에 통과했다.
- 2026-03-27: targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_084159.txt`에서 `MULTI_SPOT/tls/131072`는 throughput `12.086 Kmsg/s`, bandwidth `1584.084 MB/s`, mean latency `135.180 ms`, `P95/P99 172.356/179.641 ms`를 기록했다. baseline 대비 throughput/bandwidth는 여전히 상회했고 latency는 `mean +27.004 ms`, `P95 +18.421 ms`, `P99 +7.193 ms` 차이까지 줄었다.
- 2026-03-27: 같은 구조를 `12 MiB`까지 더 조인 실험(`core/perf/results/multi/report/perf_linux_recv_20260327_084450.txt`)은 `mean 266.564 ms`, `P95 425.514 ms`, `P99 468.609 ms`로 다시 퇴행해 폐기했고, 현재 코드는 `16 MiB` byte-budget cap 상태를 유지한다.
- 2026-03-27: `core/src/services/spot/spot_data_plane_forwarding.cpp`와 `core/src/services/spot/spot_data_plane_protocol.cpp`에 intra-batch socket service 지점을 추가하는 실험을 했지만 targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_085126.txt`가 `mean 364.771 ms`, `P95 463.533 ms`, `P99 474.745 ms`로 즉시 퇴행했다. 해당 수정은 같은 iteration 안에서 되돌렸고 관련 결과는 폐기한다.
- 2026-03-27: best-known `16 MiB` 상태로 되돌린 뒤 process regression `ctest --test-dir core/build --output-on-failure -R test_multi_spot_benchmark_process -j1`는 120.95초에 다시 통과했다. 다만 targeted 공식 rerun `core/perf/results/multi/report/perf_linux_recv_20260327_085505.txt`는 `mean 530.688 ms`, `P95 679.660 ms`, `P99 694.162 ms`로 큰 편차를 보여 현 상태가 acceptance에 충분히 안정적이지 않음이 확인됐다.
- 2026-03-27: `core/src/services/spot/spot_data_plane_loop.cpp`에서 `peer_ctrl_sub` 선행 drain을 제거해 control path fairness를 높이는 실험을 했고, 선택 빌드(`test_multi_spot_benchmark_process`, `unittest_spot_data_plane_budget`, `perf_spot`, `comp_src_spot_server`, `comp_src_spot_client`)와 regression `ctest --test-dir core/build --output-on-failure -R 'unittest_spot_data_plane_budget|test_multi_spot_benchmark_process' -j1`는 통과했다. 하지만 targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_091601.txt`가 `mean 447.950 ms`, `P95 765.109 ms`, `P99 809.513 ms`로 즉시 크게 퇴행해 해당 수정은 같은 iteration 안에서 되돌렸고 결과는 폐기한다.
- 2026-03-27: `core/src/services/spot/spot_data_plane_protocol.cpp`에서 `mesh_xsub` fanout byte-budget만 `12 MiB`로 낮추는 분리 실험을 했다. 관련 선택 빌드와 regression `ctest --test-dir core/build --output-on-failure -R 'unittest_spot_data_plane_budget|test_multi_spot_benchmark_process' -j1`는 통과했지만, 병렬 실행으로 오염된 `core/perf/results/multi/report/perf_linux_recv_20260327_091756.txt`는 폐기했고 단독 targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_092003.txt`도 `mean 339.649 ms`, `P95 440.518 ms`, `P99 457.334 ms`로 baseline과 best-known 대비 모두 퇴행해 수정은 같은 iteration 안에서 되돌렸다.
- 2026-03-27: `core/src/services/spot/spot_data_plane_forwarding.cpp`에서 `mesh_pub + fanout` dual-forward ingress burst만 `14 MiB`로 낮추는 분리 실험을 했다. 회귀 `ctest --test-dir core/build --output-on-failure -R 'test_multi_spot_benchmark_process|unittest_spot_data_plane_budget|unittest_spot_data_plane_protocol' -j1`는 121.22초에 통과했지만, targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_093448.txt`가 `mean 479.873 ms`, `P95 610.462 ms`, `P99 626.596 ms`로 best-known과 baseline 대비 즉시 크게 퇴행했다. 해당 수정은 같은 iteration 안에서 되돌렸고, 현재 코드는 다시 대칭 `16 MiB` byte-budget cap 상태를 유지한다.
- 2026-03-27: `test_multi_spot_benchmark_process`는 새 sequence 케이스가 누적된 현재 상태에서 executable 내부 케이스들은 모두 PASS하면서도 CTest `TIMEOUT 180`에 걸렸다. 이는 제품 버그가 아니라 테스트 표면 확장과 timeout 계약 불일치이므로 `core/tests/CMakeLists.txt`에서 timeout을 `360`으로 올렸고, 단독 회귀 `ctest --test-dir core/build --output-on-failure -R '^test_multi_spot_benchmark_process$' -j1`는 120.22초에 다시 clean pass 했다.
- 2026-03-27: `core/src/services/spot/spot_data_plane_loop.cpp`, `core/src/services/spot/spot_data_plane_protocol.cpp`, `core/tests/unittest/unittest_spot_data_plane_protocol.cpp`에 topology-stable bootstrap re-broadcast suppression을 추가했다. 관련 회귀 `ctest --test-dir core/build --output-on-failure -R 'unittest_spot_data_plane_protocol|test_multi_spot_benchmark_process|test_spot_service_introspection_member_peers' -j1`는 124.84초에 통과했다.
- 2026-03-27: bootstrap gating 적용 전 targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_094749.txt`는 `MULTI_SPOT/tls/131072`에서 `mean 601.024 ms`, `P95 770.346 ms`, `P99 803.529 ms`로 크게 퇴행했다.
- 2026-03-27: 같은 수정 적용 후 targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_095214.txt`는 `mean 273.197 ms`, `P95 446.600 ms`, `P99 467.677 ms`로 회복했고, rerun `core/perf/results/multi/report/perf_linux_recv_20260327_095302.txt`도 `mean 283.077 ms`, `P95 405.566 ms`, `P99 433.502 ms`를 기록해 기존 bad run 대비 편차를 크게 줄였다. throughput/bandwidth는 각각 `16.870/2211.132`, `26.730/3503.502`로 baseline 위를 유지했다.
- 2026-03-27: local fanout `SNDHWM`을 `16 -> 64`로 늘리는 실험은 `test_multi_spot_benchmark_process`의 `test_multi_spot_process_recv_many_clients_wss_perf_window_sequence`를 깨뜨려 폐기했다. 현재 코드는 다시 `16` 상태를 유지한다.
- 2026-03-27: `core/src/services/spot/spot_node_control.cpp`에서 secure transport subscription replay retry 창을 압축했다. `subscription_replay_attempt_count`를 `tls: 150 -> 20`, `wss: 300 -> 40`, `ws/default: 50 -> 10`으로 줄이고, retry holdoff를 transport-aware helper(`secure/ws: 2 tick`, default `5 tick`)로 바꿨다. 관련 회귀 `ctest --test-dir core/build --output-on-failure -R '^test_multi_spot_benchmark_process$|^test_spot_service_introspection_member_peers$|^unittest_spot_data_plane_protocol$|^unittest_spot_data_plane_budget$' -j1`는 128.06초에 clean pass했다.
- 2026-03-27: 같은 수정 뒤 targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_100924.txt`는 `MULTI_SPOT/tls/131072`에서 throughput `11.874 Kmsg/s`, bandwidth `1556.296 MB/s`, mean latency `130.817 ms`, `P95/P99 174.168/188.170 ms`를 기록했다. baseline 대비 throughput/bandwidth는 상회했고 latency는 기존 best-known보다 더 낮아졌지만 `P95/P99`는 아직 `+20.233/+15.722 ms` 남는다.
- 2026-03-27: rerun `core/perf/results/multi/report/perf_linux_recv_20260327_101006.txt`는 `CLIENT_READY,131072` 직후 partial fail(`status=partial`)로 끊겨 현재 수정이 tail latency는 낮췄지만 official targeted gate 안정성까지는 아직 닫지 못했다.
- 2026-03-27: connected-peer 변화 때마다 immediate `replay_subscriptions`를 defer하는 실험을 했지만 targeted 공식 rerun `core/perf/results/multi/report/perf_linux_recv_20260327_101759.txt`, `core/perf/results/multi/report/perf_linux_recv_20260327_101807.txt`, `core/perf/results/multi/report/perf_linux_recv_20260327_101816.txt`가 partial fail은 막는 대신 latency를 각각 `170.211/219.291/233.937 ms`, `207.880/386.940/407.577 ms`, `689.368/996.462/1055.183 ms`까지 흔들었다. process regression도 같은 iteration에서 `tcp`/`wss` sequence를 깨뜨려 이 실험은 즉시 되돌렸고 결과는 폐기한다.
- 2026-03-27: 실험 되돌림 후 rebaseline targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_103525.txt`는 `MULTI_SPOT/tls/131072`에서 throughput `27.876 Kmsg/s`, bandwidth `3653.816 MB/s`, mean latency `181.487 ms`, `P95/P99 229.897/243.824 ms`를 기록했다. partial fail은 재현되지 않았지만 baseline latency 대비 `+73.311/+75.962/+71.376 ms`로 여전히 미달이며, 현재 병목은 replay scheduling보다 active-phase 편차나 large-message receive/fanout fairness 쪽일 가능성이 더 높다.
- 2026-03-27: `sync_mesh_peer_monitor_state`에 endpoint-membership 분류 helper와 unit coverage를 추가한 뒤, ready-count-only monitor change에서는 control task wake를 생략하는 실험을 했다. 관련 회귀 `ctest --test-dir core/build --output-on-failure -R '^unittest_spot_data_plane_protocol$|^unittest_spot_data_plane_budget$|^test_spot_service_introspection_member_peers$|^test_multi_spot_benchmark_process$' -j1`는 단독 rerun에서 clean pass했지만 targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_105121.txt`는 `load_avg 20.28`에서 `mean/P95/P99 679.475/1245.451/1357.077 ms`, rerun `core/perf/results/multi/report/perf_linux_recv_20260327_105137.txt`도 `302.037/444.207/473.449 ms`로 baseline과 best-known 대비 모두 악화했다. runtime wake suppression은 같은 iteration 안에서 되돌렸고 helper/test coverage만 유지한다.
- 2026-03-27: 되돌림 직후 combined regression은 `test_multi_spot_benchmark_process`의 `tls_large_sequence` 한 케이스에서 단발성 끊김을 보였지만, 같은 binary 상태에서 `ctest --test-dir core/build --output-on-failure -R '^test_multi_spot_benchmark_process$' -j1` rerun은 130.63초에 clean pass했다. 현재는 wake-throttle 실험을 폐기한 상태를 유지한다.
- 2026-03-27: ingress와 mesh fanout이 같은 poll cycle에서만 `4 MiB`로 더 잘게 번갈아 빠지게 하는 contention-only batch split 실험을 했다. 단위/프로세스 회귀(`unittest_spot_data_plane_protocol`, `unittest_spot_data_plane_budget`, `test_multi_spot_benchmark_process`)는 통과했지만 targeted 공식 run `core/perf/results/multi/report/perf_linux_recv_20260327_110714.txt`가 `MULTI_SPOT/tls/131072`에서 `10.042 Kmsg/s`, `1316.173 MB/s`, `mean/P95/P99 292.937/378.565/396.995 ms`로 baseline과 best-known 대비 모두 악화했다. 해당 runtime 수정은 같은 iteration 안에서 되돌렸고 현재 코드는 기존 `16 MiB` cap 상태를 유지한다.
- 다음 iteration 첫 타깃: `subscription replay` 창 압축은 유지하되 replay scheduling, control-task wake, batch split 실험은 다시 건드리지 않는다. `MULTI_SPOT/tls/131072`의 client active-phase 진입 시점 편차를 직접 줄일 수 있는 bootstrap/ready-ack/first-delivery 경계 쪽 병목을 먼저 읽고, targeted 공식 run으로 latency 분산이 실제 줄었는지 다시 확인한다.

## 15. Codex supervisor 종료 메시지 규칙

반복 루프에서 아래 세 형식만 쓴다.

- 더 이상 미적용 사항이 없을 때:

```text
미적용 사항이 없습니다.
```

- 사용자 결정이 필요할 때:

```text
사용자 입력 필요: <한 줄 이유>
```

- 그 외 계속 진행해야 할 때:

```text
계속 진행 필요
```
