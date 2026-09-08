# 5-run RESULT median 정합

## 결론

.NET Multi와 Go Single·Multi의 최종 `RESULT` 집계를 수정했다. .NET Multi는 같은
`(pattern, transport, size, metric)` key를 반복마다 덮어써 마지막 값을 남겼다. Go는
최종 `Result Data`에 run별 같은 key를 모두 출력했기 때문에, key-value parser가 읽으면
마지막 값이 대표값이 됐다. 두 경로 모두 이제 C와 같이 **metric별 표본 목록의 median**을
최종 `RESULT` 한 줄로 출력한다. 원시 반복값은 report의 run별 표에 남긴다.

측정 duration, timeout, client 수, 측정 코드와 Core·binding library는 변경하지 않았다.
commit과 push도 하지 않았다.

## 1. 결함 지점과 C 기준

- 최상위 .NET 호환 runner는
  `bindings/dotnet/perf/run_comparison.py:96-99`에서 `--runs`를 suite runner에 정상 전달했다.
  각 .NET benchmark process도 run마다 metric 5개를 출력했고, Multi shell runner는 이를
  `result_data.csv`에 모두 추가했다. 의도가 끊긴 곳은 최종 `Result Data` formatter였다.
- 수정 전 `bindings/dotnet/perf/multi/run_benchmarks.sh:2246-2264`는
  `rows[key] = (size, value)`로 같은 key를 덮어썼다. 따라서 5회 실행·원시 25줄·`complete`여도
  최종 metric 5줄은 다섯 번째 run이었다.
- C Multi는 `bindings/c/perf/run_comparison.py:3693-3695`에서 key별 `vals`에
  `statistics.median(vals)`를 적용한다. 수정한 .NET formatter도 같은 key에 값을 append하고
  `statistics.median()`을 적용한다(`bindings/dotnet/perf/multi/run_benchmarks.sh:2242-2266`).
- .NET Single은 기존부터 `bindings/dotnet/perf/single/run_emit.py:1150-1154`에서 각 metric에
  `statistics.median()`을 적용하므로 수정하지 않았다.
- Go Single은 수정 전 raw `RESULT`를 그대로 grep했고, Go Multi는 정렬만 했다. report의 표는
  이미 median이었지만 최종 `RESULT`는 run 수만큼 중복됐다. 공통 formatter에
  `median_result_data_lines()`를 두고 두 Go runner가 이를 사용하도록 수정했다.

수정 전에는 같은 사실에 대해 “표는 median, .NET 최종값은 마지막 run, Go 최종값은 중복 raw”라는
세 규칙이 있었다. 수정 후 대표 `RESULT` 규칙은 “key별 metric 표본의 median” 하나다.

## 2. 7개 언어 감사

Java·Node·Go·Rust·Python에는 `run_comparison.py`가 없으므로, 파일명 grep 결과만으로 판정하지
않고 실제 shell/TypeScript/Python report 진입점을 추적했다. C++는 이번 7개 언어 범위가 아니다.

| 언어 | Single | Multi | 결함·조치 근거 |
|---|---|---|---|
| C | 정상 | 정상 | 기준 구현. Multi `run_comparison.py:3695`의 `statistics.median(vals)` |
| .NET | 정상, 미수정 | **결함, 수정** | Single `run_emit.py:1150-1154`; Multi 최종 key 덮어쓰기를 표본 list + `statistics.median()`으로 변경 |
| Java | 정상, 미수정 | 정상, 미수정 | Single `run_benchmarks.sh:622-640`, Multi `run_benchmarks.sh:1529-1546`에서 metric별 median |
| Node | 정상, 미수정 | 정상, 미수정 | Single `run_benchmarks.ts:464`; Multi `:406-407`에서 raw list를 median 1건으로 교체한 뒤 `:437`의 `list[0]`은 그 median을 사용 |
| Go | **결함, 수정** | **결함, 수정** | 최종 raw 중복 출력을 `perf_report.py:211-238`의 metric별 median으로 교체. `--runs`와 표본 수가 다르면 대표 RESULT를 만들지 않음 |
| Rust | 정상, 미수정 | 정상, 미수정 | 두 shell runner가 Python 공통 `render-single`/`render-multi`를 호출하며, renderer는 `perf_report.py:426`, `:654`에서 metric별 `_median()` 사용 |
| Python | 정상, 미수정 | 정상, 미수정 | Single·Multi runner의 `_median_metrics()`가 `statistics.median()` 사용; 최종 renderer도 metric별 `_median()` 사용 |

## 3. diff 요지

- `bindings/dotnet/perf/multi/run_benchmarks.sh`
  - 최종 formatter의 scalar map을 key별 list로 변경했다.
  - metric별 `statistics.median()`을 C Multi와 같은 정밀도로 출력한다.
- `bindings/python/perf/perf_report.py`
  - 완전한 run 표본만 metric별 median `RESULT`로 만드는 공통 command를 추가했다.
  - Go report가 원시 run 값을 잃지 않도록 `runs > 1`이면 run별 표와 median 표를 모두 출력한다.
- `bindings/go/perf/run_benchmarks.sh`, `bindings/go/perf/run_benchmarks_multi.sh`
  - report table에 `--runs`를 전달하고 최종 `Result Data`를 공통 median formatter로 출력한다.

작업 중 다른 작업이 `bindings/go/perf/run_benchmarks_multi.sh`에 추가한 실패 이유·failure-log
진단 변경은 보존했으며 이번 diff로 계산하지 않았다. 기존 .NET·Go benchmark source 변경도
건드리지 않았다.

## 4. 검증

모든 실제 perf 실행은 `scripts/perf/perf-ticket.sh submit -p 2 -o codex ...`로 제출했다.
고정 Core는 두 5-run ticket log에서
`/home/hep7/.cache/zlink/core-pinned/0.17.2/lib/libzlink.so.0.17.2` 사용을 확인했다.

| 티켓 | 명령 요약 | rc | 결과 |
|---|---|---:|---|
| `2-1788836474-29369-codex-runs-median_parity_dotnet_multi_dd_tcp_6` | .NET Multi DD, tcp, 64 B, 5회, 2초 | 0 | `complete`, 최종 metric 5개 모두 median 일치 |
| `2-1788836474-29361-codex-runs-median_parity_go_multi_dd_tcp_64_5x` | Go Multi DD, tcp, 64 B, 5회, 2초 | 0 | 중간 renderer 검증 통과 |
| `2-1788837180-73088-codex-runs-median_parity_go_final_renderer_dd_` | Go Multi DD, tcp, 64 B, 5회, 2초 | 0 | 최종 renderer: `complete`, run 표·median 표·RESULT 일치 |
| `2-1788836474-29362-codex-dotnet_perf_test_pubsub_large_no_timeout` | .NET PUBSUB large default regression | 0 | 통과 |
| `2-1788836474-29359-codex-dotnet_perf_test_ws_wallclock_latency` | .NET DD ws wallclock regression | 1 | median과 무관한 기존 Auto-HWM assertion 실패: report의 `MsgUnit(B)`가 `?` |

정적 검증은 다음이 모두 통과했다.

- `bash -n` — 수정한 .NET/Go shell runner
- `python3 -m compileall -q bindings/dotnet/perf bindings/python/perf`
- `git diff --check`
- 합성 5-run 표본 — metric별 가운데 값과 formatter 출력 일치

### 4.1 .NET exact 대조

- report:
  `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260908_120548_runs-median-parity-dotnet.txt`
- 원시 자료:
  `bindings/dotnet/perf/results/multi/tmp/perf_dotnet_multi_linux_20260908_120548_runs-median-parity-dotnet.result_data.csv`
- 조건: `MULTI_DEALER_DEALER`, `tcp`, 64 B, 100 clients, duration 2초, 5회

| metric | run 1·2·3·4·5 원시값 | 직접 median | 최종 RESULT |
|---|---|---:|---:|
| throughput | 895938.000, 864560.000, 892359.500, 891724.500, 834481.500 | 891724.500 | 891724.500 |
| bandwidth | 57.340, 55.332, 57.111, 57.070, 53.407 | 57.070 | 57.070 |
| latency | 1.022274, 0.591087, 0.586587, 0.807588, 0.895396 | 0.807588 | 0.807588 |
| latency_p95 | 9.122903, 1.516271, 1.713301, 4.044282, 7.547297 | 4.044282 | 4.044282 |
| latency_p99 | 15.453865, 15.009895, 14.495439, 13.919304, 13.759642 | 14.495439 | 14.495439 |

`META,runs,5`, `status: complete`, `expected_result_lines: 25`,
`actual_result_lines: 25`도 확인했다. 마지막 throughput 834481.500이 아니라 median
891724.500이 최종 대표값이다.

### 4.2 Go report 대조

- report:
  `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260908_121621_runs-median-parity-go-final.txt`
- 조건: `MULTI_DEALER_DEALER`, `tcp`, 64 B, 100 clients, duration 2초, 5회

Go report의 run 표는 rate·latency를 3자리로 표시하고 최종 latency RESULT는 6자리로 보존한다.
아래 median은 run 표의 표시 정밀도에서 직접 계산했으며, 괄호 안은 최종 RESULT의 원래 정밀도다.

| metric | run 1·2·3·4·5 report 값 | 표시 median | 최종 RESULT |
|---|---|---:|---:|
| throughput | 202530, 100517, 208763, 260546, 274535 | 208763 | 208763.000 |
| bandwidth | 12.962, 6.433, 13.361, 16.675, 17.570 | 13.361 | 13.361 |
| latency | 3.478, 1.548, 1.369, 1.177, 0.860 | 1.369 | 1.369489 (→1.369) |
| latency_p95 | 18.664, 7.903, 9.876, 11.271, 3.720 | 9.876 | 9.875878 (→9.876) |
| latency_p99 | 24.254, 8.633, 12.815, 14.983, 5.569 | 12.815 | 12.815139 (→12.815) |

수정 전 같은 형태의 Go 5-run report는 64 B 한 cell에 `RESULT` 25줄을 출력했다. 수정 후
위 report는 최종 `RESULT` 5줄만 출력하며, 원시 5회 값은 `run 1/5`~`run 5/5` 표에 남는다.

### 4.3 남은 실패

`test_multi_dealer_dealer_ws_wallclock_latency.sh`의 rc=1은 이번 median 변경과 무관하다.
해당 실행 자체는 `status: complete`, latency 3.124057 ms였지만 Auto-HWM detail의 client/server
두 행 모두 `MsgUnit(B)`가 `?`여서 테스트의 `64` assertion이 실패했다. 관련 report는
`bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260908_120516_multi_dealer_dealer_ws_wallclock_latency.txt`다.
제약상 측정 조건이나 benchmark/runtime 코드를 바꾸지 않았고, 같은 실패를 재시도하지 않았다.
