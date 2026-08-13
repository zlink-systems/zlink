# .NET DEALER/DEALER latency reservoir 정렬 결과

## 변경

`.NET MULTI_DEALER_DEALER`는 C의 `PERF_MULTI_LATENCY_SAMPLE_CAP`을 사용하지 않고
최대 400만 개 latency sample을 계속 저장했다. C와 같은 cap·LCG reservoir sampling으로
바꾸고, mean은 전체 sample의 합과 개수로 계산한다. public interface는 변경하지 않았다.

## 결과

Release Core `0.10.1`, `tcp`, clients `100`, duration `1초`, runs `1`, balanced
auto-HWM에서 C를 먼저, .NET을 다음에 단독 실행했다.

| 구현 | C 대비 throughput 비율 (size 순서) | 산술평균 |
|---|---|---:|
| 변경 전 | 33.88 / 59.75 / 104.14 / 58.64 / 96.57 / 88.56% | 73.59% |
| C와 같은 reservoir | 35.58 / 59.43 / 111.34 / 75.79 / 88.95 / 101.73% | 78.80% |

목표 `85%`에는 아직 미달이지만 측정 의미와 throughput이 함께 개선됐으므로 변경을 유지한다.

## 결과 파일

- C: `/tmp/zlink-dotnet-dd-current-c/multi/report/perf_c_multi_linux_20260812_222919.txt`
- 변경 전 .NET: `/tmp/zlink-dotnet-dd-current-dotnet/multi/report/perf_dotnet_multi_linux_20260812_222935.txt`
- 변경 후 .NET: `/tmp/zlink-dotnet-dd-reservoir-dotnet/multi/report/perf_dotnet_multi_linux_20260812_223107.txt`
