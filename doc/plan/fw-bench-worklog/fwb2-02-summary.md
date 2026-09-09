# fwb2-02 공용 집계기 S2S 셀 지원 결과

## 결과

공용 집계기가 기존 client-driven 원본과 규격 §4의 server-driven 원본을 한 번에 읽는다.
server-driven source A와 target B가 별도 JSON을 쓰면 `trigger.runId`와 `trigger.cellId`로 합친다.
target 자료가 없는 셀은 `incomplete`로 남기고 중앙값, `RESULT`, 판정에서 제외한다.

표는 내부 metric 이름인 `client_*`와 `server_*`를 유지하면서 머리글만 `Source`와 `Target`으로
표시한다. request 계열은 `KOPS`, `send-saturation`은 `KMSG/s`를 사용한다. 언어 문서에 옮길 수
있는 Markdown 표는 `--format doc-table`로 출력한다.

최종 재검증 기준은 branch `main`, HEAD `91caaa753d`다. 작업 중 다른 job의 commit으로 HEAD가
전진했으며, 이 job은 commit이나 push를 수행하지 않았다.

## 스키마와 병합 규칙

| 코드 | 규칙 |
|---|---|
| `framework/bench/grpc/tools/benchagg/model.py:94` | `role`, `run_id`, `cell_id`, `trigger`, `streams`, `target_stats`, `status`를 정규화한 셀 하나에 둔다. `role`이 없으면 client-driven 원본이다. |
| `framework/bench/grpc/tools/benchagg/readers.py:355` | 규격 §4의 `trigger` 필드 이름을 그대로 요구한다. 별칭이나 기본값을 만들지 않는다. |
| `framework/bench/grpc/tools/benchagg/readers.py:389` | `target_stats.received/errors/drainMs`를 숫자로 읽고 음수와 누락을 거부한다. |
| `framework/bench/grpc/tools/benchagg/readers.py:425` | `with-grpc-cell-v1`의 `cells` 배열 안에 `role`이 있는 형식과 기존 role 없는 형식을 함께 읽는다. |
| `framework/bench/grpc/tools/benchagg/readers.py:473` | 기존 `results` container의 최상위에 §4 필드를 추가한 셀별 JSON도 같은 정규화 경로로 읽는다. |
| `framework/bench/grpc/tools/benchagg/readers.py:576` | source가 workload·source 자원·trigger·streams를 소유하고 target이 target 자원·`target_stats`를 제공한다. `send-saturation` 처리량은 `received / durationMs`로 계산한다. |
| `framework/bench/grpc/tools/benchagg/readers.py:623` | A/B 병합 key는 `(runId, cellId)`뿐이다. 같은 역할이 중복되거나 source와 target의 셀 key가 다르면 입력 오류다. |
| `framework/bench/grpc/tools/benchagg/readers.py:600` | target 레코드와 source에 합쳐진 `target_stats`가 모두 없으면 `incomplete`다. target만 있어도 같은 상태로 남긴다. |
| `framework/bench/grpc/tools/benchagg/model.py:271` | 오염 셀과 incomplete 셀은 중앙값 입력에서 제외한다. |
| `framework/bench/grpc/tools/benchagg/analysis.py:267` | incomplete, G5 실패, `abandoned`, target 오류, source 포화, client-counted send를 같은 판정 경로에서 `unsupported`로 처리한다. |

파일 이름이나 §9 포트로 A/B를 추정하는 방법과 `(runId, cellId)`만 사용하는 방법을 비교했다.
전자는 파일 배치와 포트 표를 병합 규칙으로 복제한다. 후자는 셀 원본의 식별자 하나만 사용하므로
후자를 적용했다.

## 변경 파일과 diff 요지

- `framework/bench/grpc/tools/bench_aggregate.py`: `doc-table` format, server-driven 동반 정보와
  incomplete 목록, JSON aggregate의 streams·오류·incomplete 정보를 추가했다.
- `framework/bench/grpc/tools/benchagg/model.py`: S2S identity와 원본 필드, complete 상태를 추가했다.
- `framework/bench/grpc/tools/benchagg/readers.py`: §4 필드 검증, `cells`/`results` container 읽기,
  여러 파일과 여러 run 디렉터리에 걸친 A/B 병합, send target-count 정규화를 추가했다. 기존
  `report.txt` bandwidth 기반 단위 판별은 그대로 유지했다.
- `framework/bench/grpc/tools/benchagg/analysis.py`: incomplete 제외, streams의 3-run 합의값,
  `target_stats.errors`와 `abandoned` 판정 차단을 추가했다. G5 10%와 §7.2 0.80은 바꾸지 않았다.
- `framework/bench/grpc/tools/benchagg/render.py`: Source/Target 머리글, KOPS/KMSG/s,
  §7.1 동반 정보, 문서용 표를 추가했다.
- `framework/bench/grpc/tools/tests/test_server_driven.py`,
  `framework/bench/grpc/tools/tests/fixtures/s2s/`: 별도 A/B 원본 3-run, 두 payload,
  target 누락, client-driven 혼합, 네 format의 입력을 추가했다.
- `framework/bench/grpc/tools/README.ko.md`: 입력 container, 병합 key, 필드 소유권,
  incomplete 처리와 `doc-table` 사용법을 기록했다.

입력 분기 규칙 수는 수정 전과 후가 `3 → 3`이다. 수정 전의 `cells.json → declared results.json →
stdout`을 `구조화 셀 JSON → legacy results.json → stdout`으로 일반화했다. 새 S2S 내부에는
`(runId, cellId)` 병합 규칙 하나만 있으며 파일명, 디렉터리명, 포트별 예외는 없다.

## 검증

| 명령 | rc | 결과 |
|---|---:|---|
| `python3 -m unittest discover -s framework/bench/grpc/tools/tests -p 'test_*.py'` | 0 | 기존 50개와 신규 10개, 합계 60개 통과 |
| `python3 -m py_compile framework/bench/grpc/tools/bench_aggregate.py framework/bench/grpc/tools/benchagg/*.py framework/bench/grpc/tools/tests/test_*.py` | 0 | syntax 오류 없음 |
| `git diff --check` | 0 | whitespace 오류 없음 |
| gated2 `spec4`를 변경 전 출력의 `Client/Server` 머리글만 `Source/Target`으로 바꾼 결과와 `cmp` | 0 | 1차 표의 행·값·순서 동일 |
| gated2 변경 전/후 `result` 출력을 `cmp` | 0 | byte 단위 동일 |
| S2S fixture의 `--format spec4`, `result`, `judgement`, `doc-table` | 0 | 네 format 모두 생성 |

회귀 비교 파일과 아래 예시 전체는 `/tmp/zlink-sol-fwb2-02/`에 있다.

## 예시 출력

### `--format spec4`

```text
  > Benchmarking current for request-window...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      | zlink-dotnet            | 1024B    |      100.00 KOPS |  102.40 MB/s |     0.500 ms |     0.800 ms |     1.000 ms |      10.0% |   100.0 MB |       4.0% |    80.0 MB |
      | zlink-framework-dotnet  | 4096B    |       70.00 KOPS |  286.72 MB/s |     1.000 ms |     1.400 ms |     1.800 ms |      11.5% |   114.0 MB |       5.5% |    94.0 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | zlink-dotnet            | 1024B    |     50.00 KMSG/s |   51.20 MB/s |     0.100 ms |     0.200 ms |     0.300 ms |      12.0% |   105.0 MB |       6.0% |    85.0 MB |
```

### `--format result`

```text
RESULT,current,zlink-dotnet-request-window,local,1024,throughput,100000.000
RESULT,current,zlink-dotnet-request-window,local,1024,latency_p95,0.800
RESULT,current,zlink-framework-dotnet-request-window,local,4096,throughput,70000.000
RESULT,current,zlink-framework-dotnet-request-window,local,4096,server_memory_mb,94.000
RESULT,current,zlink-dotnet-send-saturation,local,1024,throughput,50000.000
RESULT,current,zlink-dotnet-send-saturation,local,1024,bandwidth,51.200
```

### `--format judgement`

```text
| Formula | Payload | Value | Status | Verdict (>= 0.80) | Reason |
|---|---|---|---|---|---|
| `zlink-dotnet / zlink-c` | 1024 | n/a | **unsupported** | — | denominator zlink-c-request-window@1024 was not measured |
| `zlink-dotnet / zlink-c` | 4096 | n/a | **unsupported** | — | denominator zlink-c-request-window@4096 was not measured |
| `zlink-framework-dotnet / zlink-dotnet` | 1024 | 0.900 | published | pass | both rows pass G5 |
| `zlink-framework-dotnet / zlink-dotnet` | 4096 | 0.875 | published | pass | both rows pass G5 |
```

fixture에 C 기준 원본을 넣지 않았으므로 첫 번째 식은 `unsupported`다. 이 상태를 통과로 바꾸지
않고 그대로 출력한다.

### `--format doc-table`

```text
| Language | Pattern | Payload | Implementation | Throughput (3-run median) | Unit | Lat.Mean(ms) | Lat.P95(ms) | Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem | Runs | G5 |
|---|---|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| dotnet | request-window | 1024B | `zlink-dotnet` | 100.000 | KOPS | 0.500 | 0.800 | 1.000 | 10.0% | 100.0 MB | 4.0% | 80.0 MB | 3 | pass |
| dotnet | request-window | 4096B | `zlink-framework-dotnet` | 70.000 | KOPS | 1.000 | 1.400 | 1.800 | 11.5% | 114.0 MB | 5.5% | 94.0 MB | 3 | pass |
| dotnet | send-saturation | 1024B | `zlink-dotnet` | 50.000 | KMSG/s | 0.100 | 0.200 | 0.300 | 12.0% | 105.0 MB | 6.0% | 85.0 MB | 3 | pass |
```

### target 누락

```text
| Run | Cell | Reason |
|---|---|---|
| missing-target | `grpc-dotnet-request-serial@1024` | target record or embedded target_stats is missing |
```

## BLOCKERS

1. 규격 §7.1은 trigger endpoint를 동반 정보로 요구하지만 §4의 `trigger`에는 endpoint 필드가 없다
   (`framework/bench/grpc/README.ko.md:235`, `framework/bench/grpc/README.ko.md:373`). 집계기는
   §9 포트로 값을 추정하지 않고 `n/a`를 출력한다. §4가 필드 위치와 이름을 정하기 전에는 실제
   endpoint를 원본에서 출력할 수 없다.
2. 동시에 수정 중인 .NET producer의 `BenchTriggerObservation`은 `receivedAtUnixMs`를 쓰고 `warmup`과
   `receivedAt`이 없다
   (`framework/languages/dotnet/perf/ZLink.Framework.Perf.ServerSupport/BenchHttpApplication.cs:23`,
   `framework/bench/grpc/dotnet/run_local.sh:131`). 이 job은 금지 범위인 .NET runner를 수정하지
   않았다. §4의 필드로 producer가 맞춰지기 전까지 집계기는 해당 초안 원본을 입력 오류로
   거부한다. `receivedAtUnixMs → receivedAt` 같은 별칭은 추가하지 않았다.

## 감독자 재검증 명령

```bash
python3 -m unittest discover -s framework/bench/grpc/tools/tests -p 'test_*.py'
python3 -m py_compile framework/bench/grpc/tools/bench_aggregate.py framework/bench/grpc/tools/benchagg/*.py framework/bench/grpc/tools/tests/test_*.py
git diff --check

python3 framework/bench/grpc/tools/bench_aggregate.py \
  --lang dotnet --judgement-pattern request-window \
  --runs-glob 'framework/bench/grpc/tools/tests/fixtures/s2s/paired-*' \
  --format spec4
python3 framework/bench/grpc/tools/bench_aggregate.py \
  --lang dotnet --judgement-pattern request-window \
  --runs-glob 'framework/bench/grpc/tools/tests/fixtures/s2s/paired-*' \
  --format result
python3 framework/bench/grpc/tools/bench_aggregate.py \
  --lang dotnet --judgement-pattern request-window \
  --runs-glob 'framework/bench/grpc/tools/tests/fixtures/s2s/paired-*' \
  --format judgement
python3 framework/bench/grpc/tools/bench_aggregate.py \
  --lang dotnet --judgement-pattern request-window \
  --runs-glob 'framework/bench/grpc/tools/tests/fixtures/s2s/paired-*' \
  --format doc-table

python3 framework/bench/grpc/tools/bench_aggregate.py \
  --lang dotnet --judgement-pattern request-window \
  --runs-glob 'framework/bench/grpc/tools/tests/fixtures/gated2/c-router-*' \
  --runs-glob 'framework/bench/grpc/tools/tests/fixtures/gated2/dotnet-router-*' \
  --format full
```
