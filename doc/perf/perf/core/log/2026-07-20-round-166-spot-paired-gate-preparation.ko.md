# Round 166: Spot paired gate 준비

## 목표

S9-P02의 100 peer·5초 병목 측정과 S9-P03의 정량 판정을 시작하기 전에
현재 runner가 계획 문서의 paired run 조건을 그대로 실행할 수 있는지 확인한다.
다른 framework E2E가 실행 중인 동안에는 성능 수치를 측정하지 않는다.

## 정적 점검 결과

`run_benchmarks_multi.sh --runs 5`는 한 패턴의 다섯 번 실행을 끝낸 뒤 다음
패턴을 실행한다. 이 방식은 계획에서 요구하는 Spot과 ROUTER 실행 순서 교차를
보장하지 않는다.

또한 Spot peer MeshNode는 I/O thread를 1개 사용하지만 기존 ROUTER req/rep과
send/send 기본값은 4개다. 별도 지정 없이 두 결과를 나누면 자원 조건이 달라
정식 비율로 사용할 수 없다.

확인 시점의 Core runtime은 다음과 같다.

- runtime:
  `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.10.6.0`
- SHA-256:
  `345610491e3073f8984a3e6c8bf4eac4cd3b22a117aa1fb1d1cc4d8edb33d755`
- Core 및 C perf 측정 source SHA-256:
  `8c848ca4a957d2d71414f96f2dc7b27ea8c4f55941bf75a3aba095f953ef3253`
- freshness:
  `core/src`와 `core/include`에 runtime보다 새로운 파일 없음

## 변경

`bindings/c/perf/run_spot_paired_gate.py`를 추가했다.

- 각 cell에서 Spot-first와 ROUTER-first 순서를 번갈아 실행한다.
- Spot과 ROUTER의 server·client I/O thread를 모두 1개로 고정한다.
- 기본 조건은 100 peer, 5초 active duration, cell당 5회다.
- pub/sub, req/rep과 send/send를 각각 방향이 일치하는 ROUTER 기준과 비교한다.
- throughput 90%와 mean·p95·p99 1.25배 조건을 cell별로 판정한다.
- raw output, JSON과 Markdown 판정표를 함께 남긴다.
- 선택한 benchmark target을 공식 `bindings/c/build`에서 먼저 다시 만들어
  오래된 harness 실행 파일을 재사용하지 않는다.
- Core runtime 경로와 SHA-256, Core 및 C perf source tree SHA-256을 기록한다.
- 측정 도중 Core runtime 또는 source가 바뀌면 결과를 폐기하고 실패한다.
- Spot active window가 끝난 뒤 MeshNode status를 한 번 읽어 hub와 전체 peer의
  수신 건수, pending message·byte와 multicast submit·drop을 `SPOT_DIAG`로 남긴다.
  이 값은 throughput·latency 계산에 포함하지 않는다.

Linux `perf`가 있는 환경에서는 좁은 pattern·transport·payload 범위에
`--perf-record`를 사용할 수 있다. 현재 WSL 환경에는 `perf`가 없고 GNU
`time`과 Valgrind가 있다. `--time-verbose`는 각 실행의 process tree 전체에
대한 CPU 시간, 최대 RSS, page fault와 context switch를 별도 파일에 기록한다.

이 증거의 역할은 다음처럼 구분한다.

1. paired throughput·latency 결과는 정량 gate와 병목 크기를 판정한다.
2. GNU `time -v`는 CPU 포화, 메모리와 context switch 차이를 낮은 간섭으로
   확인하지만 함수별 병목을 증명하지 않는다.
3. MeshNode status와 monitor counter는 제출, backpressure, multicast drop과
   종료 시 pending work를 확인하는 correctness 진단이다. CPU 병목 위치를
   증명하지 않는다.
4. Callgrind는 함수별 비용 후보를 찾는 보조 수단이다. 계측 오버헤드가 크므로
   정식 성능 수치에는 사용하지 않고, paired red에서 좁혀진 한 cell을 한 번만
   조사할 때 사용한다.

## 검증

다음 저부하 검증만 실행했다.

```bash
python3 -m unittest \
  bindings/c/perf/multi/tests/test_spot_paired_gate.py
python3 -m unittest discover \
  -s bindings/c/perf/single/tests \
  -p 'test_multi_run_comparison_policy.py'
python3 bindings/c/perf/run_spot_paired_gate.py \
  --dry-run \
  --patterns SPOT_REQREP \
  --transports tcp \
  --msg-sizes 64 \
  --runs 2 \
  --clients 100 \
  --duration 5
```

paired gate test는 `5/5`, 기존 multi runner policy test는 `22/22` 통과했다.
dry run은 첫 반복에서 Spot 다음 ROUTER, 둘째 반복에서 ROUTER 다음 Spot 순서와
양쪽 I/O thread 1개를 출력했다.

## 다음 판단

이 라운드는 측정 준비이므로 S9-P02를 완료하지 않는다. framework의 시간 민감
E2E가 끝나고 host가 안정된 뒤 먼저 tcp 64바이트의 세 paired cell을
100 peer·5초 조건으로 실행한다. `--time-verbose` 결과와 Core counter를 함께
확인해 처리량 차이가 CPU 포화, context switch, backpressure 또는 pending
queue 중 어느 현상과 함께 나타나는지 분리한다.

첫 focused 명령은 다음과 같다.

```bash
python3 bindings/c/perf/run_spot_paired_gate.py \
  --patterns SPOT_PUBSUB,SPOT_REQREP,SPOT_SENDSEND \
  --transports tcp \
  --msg-sizes 64 \
  --runs 5 \
  --clients 100 \
  --duration 5 \
  --time-verbose \
  --tag s9-p02-spot-router-tcp64
```

이 실행과 인접하게 일반 pub/sub tcp 64바이트 5회와 비-SPOT 64바이트 시작
snapshot을 고정한다. 이후 Core 성능 변경이 있으면 같은 명령으로 다시 실행해
변경 전후를 비교한다. 현재 source 시작 결과가 없으면 과거 다른 commit의 report를
회귀 분모로 사용하지 않는다.

함수별 profile이 꼭 필요하면 Callgrind focused 1회를 보조로 사용하되, 이 결과로
throughput이나 latency gate를 판정하지 않는다. `perf`가 제공되는 환경에서는 같은
focused cell을 Linux sampling profile로 다시 확인한다.
