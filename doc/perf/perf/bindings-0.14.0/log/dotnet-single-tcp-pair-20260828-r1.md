# .NET paired measurement: Single / tcp / PAIR

- C report: `bindings/c/perf/results/single/report/perf_c_single_linux_20260828_090218_dotnet0140-single-tcp-pair-final-r1.txt` (complete)
- .NET report: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260828_090248_dotnet0140-single-tcp-pair-final-r1.txt` (partial)
- status: 미측정
- runs=1. This is a terrain-reading single run, not a final five-run median.

## 측정 불가
- .NET 65536B, 131072B, 262144B phase가 wire-level stop token을 수신하지 못해 timeout으로 partial report가 되었다.
- C one-way stop retry와 .NET `POLLOUT` wait 차이를 bounded 1ms retry로 정렬하고 transient send retry도 추가했으나 final paired report는 여전히 partial이었다.
- complete paired report가 아니므로 부분 RESULT 값은 표·xlsx 판정에 사용하지 않았다. 다음 단계 후보: large-message PAIR stop-token delivery와 receiver drain parity를 조사한다.
