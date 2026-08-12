# .NET DEALER_DEALER 종료 단계 측정 결과

## 대상과 조건

`MULTI_DEALER_DEALER / tcp`만 C를 먼저, .NET을 다음에 단독 실행했다. Release
Core `0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM,
message size `64, 256, 1024, 4096, 65536, 131072B`를 사용했다.

## 결과

| 구현 | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 |
|------|-----------------------------------|---------:|------|
| large-frame teardown idle grace 1초 | 63.18 / 83.37 / 52.35 / 83.84 / 91.29 / 84.24% | 76.38% | 통과 |

기존 64 KiB case는 active window 뒤 50ms idle에서 server가 종료되어 마지막 stop token이
`NOT_ADMITTED`로 실패하고 결과가 partial이었다. large-frame case의 종료 단계만 1초 idle을
허용해 six-size 결과를 완결했다. throughput과 latency는 active window에서만 집계하므로 측정
값에는 종료 단계가 포함되지 않는다.

## 결과 파일

- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_123802_dotnet-dealer-teardown-grace-c.txt`
- .NET: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260812_123817_dealer-teardown-grace.txt`
