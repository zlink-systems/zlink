# Round 169: Spot P04 종료 gate 설계와 현재 달성률

## 목적

S9-P03 성능 목표가 충족된 뒤 S9-P04를 어떤 순서와 증거로 종료할지 고정한다.
현재 통과하지 않은 항목을 완료로 계산하지 않고, 진단 smoke와 정식 gate를
구분한다.

## 현재 달성률

| 구분 | 현재 증거 | 정식 달성률 |
|------|-----------|------------:|
| 72 cell correctness | 2 peer·1초 smoke `72/72 complete` | 진단 72/72, 정식 0/72 |
| deterministic metric·policy | C++ metric 1개와 Python policy 29개 통과 | 30/30 |
| 100 peer paired 처리량 | tcp 64바이트 3패턴 1회 진단 | 0/3 통과 |
| 100 peer paired 전체 matrix | 3패턴 × 4 transport × 6 payload × 5회 | 0/72 정식 판정 |
| full perf 연속 성공 | 같은 source·runtime에서 3회 | 0/3 |
| 남은 child·assertion·timeout | focused 실행은 0, 정식 full 기준 미실행 | 0/3 정식 판정 |
| sanitizer | 관련 Core lifecycle·stress 대상 | 미실행 |
| 비-SPOT 회귀 | 시작 paired median 대비 처리량 -5% 이내 | 미실행 |

tcp 64바이트 1회 matched 진단은 PUBSUB 44.68%, REQREP 81.02%, SENDSEND
56.48%였다. 모두 90% 처리량 gate를 만족하지 않는다. echo one-way 지연 의미를
보정한 뒤 REQREP와 SENDSEND latency도 대응 ROUTER의 1.25배를 넘는다. 일반
PUBSUB은 처리량 절대 하한을 통과했지만 p95와 p99 절대 상한을 통과하지 못했다.
따라서 지금 full 종료 gate를 실행해도 P03에서 실패하며, P04를 시작한 것으로
판정하지 않는다.

## P03 정식 실행

P02에서 유지할 개선이 결정되고 focused gate가 통과하면 먼저 다음 paired 실행을
한 번 수행한다.

```bash
python3 bindings/c/perf/run_spot_paired_gate.py \
  --patterns SPOT_PUBSUB,SPOT_REQREP,SPOT_SENDSEND \
  --transports tcp,tls,ws,wss \
  --msg-sizes 64,256,1024,4096,65536,131072 \
  --runs 5 \
  --clients 100 \
  --duration 5 \
  --time-verbose \
  --tag s9-p03-formal-paired
```

각 cell의 Spot과 ROUTER를 인접 실행하고 반복 순서를 교차한다. 5회 median 처리량
90%, mean·p95·p99 1.25배, multicast drop 0을 모두 만족해야 P03을 통과한다.
runtime 또는 source tree hash가 실행 중 바뀌면 전체 결과를 폐기한다.

## P04 full perf 3회

P03 통과 뒤 같은 source와 runtime에서 Spot 전체 matrix를 독립적으로 세 번
실행한다. 각 실행은 72개 결과가 모두 있어야 하며 fail, skip과 unsupported를
허용하지 않는다.

```bash
for run in 1 2 3; do
  PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh \
    --reuse-build \
    --pattern SPOT_PUBSUB,SPOT_REQREP,SPOT_SENDSEND \
    --transports tcp,tls,ws,wss \
    --msg-sizes 64,256,1024,4096,65536,131072 \
    --duration 5 \
    --clients 100 \
    --runs 1 \
    --server-io-threads 1 \
    --client-io-threads 1 \
    --results-tag "s9-p04-full-${run}"
done
```

Spot client는 모든 child PID에 `waitpid()`를 수행하고 정상 종료 여부를 검사한다.
runner도 server·client의 비정상 종료, assertion과 timeout을 실패로 기록한다.
각 실행 뒤에는 `pgrep -af '[/]comp_src_spot_'` 결과가 없어야 한다. report,
runtime 경로·SHA-256, source tree SHA-256과 process 확인 결과를 한 묶음으로
보존한다.

## sanitizer와 비-SPOT 회귀

sanitizer는 성능 수치를 판정하지 않으므로 `core/build`를 바꾸지 않는 별도
instrumented build에서 관련 lifecycle·stress test를 실행한다. ASAN leak 검출과
UBSAN error가 모두 0이어야 한다. 정식 perf는 계속 `core/build` runtime만 사용한다.

비-SPOT 회귀는 P02 시작 source에서 고정한 같은 조건의 paired median과 비교한다.
공통 C multi 패턴의 처리량이 5% 넘게 낮아지면 실패다. 일반 PUBSUB은 처리량
90% 유지와 함께 p95·p99 목표도 만족해야 한다.

## 판정

현재 P04는 대기 상태다. P03의 72개 cell이 모두 정식 기준을 통과하기 전에는 full
perf, sanitizer 또는 일부 lifecycle 성공을 합쳐 P04 완료율로 계산하지 않는다.
version 변경과 package 배포도 P03·P04가 모두 통과한 뒤 한 번만 수행한다.
