# with_stream cppserver 스택 추가 측정 요약

- 실행: `ZLINK_CORE_SOURCE=local ./bindings/c/bench/with_stream/run_benchmarks.sh --stack zlink,asio,cppserver,zmq --size all --ccu 1000 --runs 3 --reuse-build`
- 결과 디렉터리: `bindings/c/bench/with_stream/results/20260908_154828`
- Core: `core/build/lib/libzlink.so.0.17.3`(release --lib-only 재빌드 후 측정; 측정 로그 `core_runtime=...0.17.3` 확인)
- 시각: 측정 시작 2026-09-08 15:48:28, 종료 15:55:44
- **주의(신뢰도 제한)**: PERF_LOCK 대기 중 15:37:01~15:48:28 사이 다른 세션이 같은 with_stream 6-스택 벤치를 먼저 실행해 잠금을 선점했다. 그 job이 끝난 직후(잔여 부하 상태) 본 측정이 시작되어, 측정 시작 시 load average가 6.96 / 5.87 / 5.65로 idle이 아니었다(종료 시 5.20 / 5.42 / 5.58). 그 결과 절대 kops 값이 과거 idle 기준(예: §7.1 65536 B zlink 34.7 kops)보다 크게 낮게 나왔다(잔여 부하로 인한 하향 오염으로 판단). 재측정을 1회 시도했으나(15:58:54 idle window 도달, load 1.01/3.47/4.80) PERF_LOCK을 다른 job(`all3`, perf+TSan ctest)이 즉시 재선점해 16:01:43까지 풀리지 않았고, 1.5 h 상한을 지키기 위해 추가 대기를 중단했다. 아래 수치는 **참고용**이며, 스택 간 상대 비교(같은 오염 조건을 4개 스택이 동일하게 겪음)는 유효하나, 절대값과 §7.1 idle 기준과의 직접 비교는 부정확하다.

## 스택별 build 결과

모든 스택 빌드/실행 성공(`skipped_stacks.csv` 비어 있음). `cppserver`는 사전에 `.gitlinks` 기반 외부 의존성(asio, Catch2, cpp-optparse, CppBenchmark, CppCommon 및 중첩 fmt/HdrHistogram/zlib, CppCMakeScripts)이 저장소에 없어 네트워크로 클론해 채운 뒤 `build-stream`에서 빌드했다(소스 미수정, PERF_LOCK 밖에서 수행).

## 처리량(median kops/s, throughput phase, runs 3)

| size | zlink | asio | cppserver | zmq |
|---|---:|---:|---:|---:|
| 64 B | 109.78 | 143.44 | 128.14 | 111.77 |
| 1024 B | 106.74 | 139.19 | 224.54 | 132.39 |
| 65536 B | 10.71 | 18.72 | 27.30 | 14.80 |

## 비율

| size | zlink/asio | zlink/cppserver | cppserver/asio | zlink/zmq |
|---|---:|---:|---:|---:|
| 64 B | 0.765 | 0.857 | 0.893 | 0.982 |
| 1024 B | 0.767 | 0.475 | 1.613 | 0.806 |
| 65536 B | 0.572 | 0.392 | 1.458 | 0.724 |

## 서버 CPU%(srv cpu%, comparison.md 기준)

| size | zlink | asio | cppserver | zmq |
|---|---:|---:|---:|---:|
| 64 B | 315.48 | 282.69 | 211.53 | 349.11 |
| 1024 B | 330.07 | 287.07 | 261.72 | 368.84 |
| 65536 B | 243.56 | 334.03 | 323.15 | 212.49 |

## load average

- 측정 시작(잠금 획득 직후): 6.96 / 5.87 / 5.65
- 측정 종료: 5.20 / 5.42 / 5.58
- (참고) 측정 개시 전 idle window 확인 자체는 15:36:00~15:37:02(ninja=0, load1 1.05→1.23)에 성립했으나, PERF_LOCK을 다른 job이 먼저 잡아 실제 실행은 그 job 종료 직후로 밀렸다.

## 실패 스택

없음(`skipped_stacks.csv` 비어 있음, 4개 스택 모두 3 sizes × 3 runs PASS).

## §7.1 0.17.2 idle 행과 비교(한 줄)

§7.1의 `0.17.2 idle runs 3`(results/20260908_035802) 행은 zlink/asio = 0.785(64 B) / 0.815(1024 B) / 0.836(65536 B)였는데, 이번 측정은 0.765 / 0.767 / 0.572로 특히 65536 B에서 크게 낮다 — 위에서 밝힌 잔여 부하 오염이 원인일 가능성이 높고, 진짜 idle 조건에서의 재확인이 필요하다(cppserver 스택 자체의 상대 순위: 1024 B·65536 B에서 cppserver가 asio보다도 높게 나온 점은 오염 여부와 무관하게 재확인할 가치가 있다).
