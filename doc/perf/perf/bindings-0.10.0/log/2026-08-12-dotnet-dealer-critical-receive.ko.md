# .NET DEALER critical receive 후보 측정 결과

## 대상과 변경

`MULTI_DEALER_DEALER / tcp`의 첫 `DONT_WAIT` receive가 non-blocking Core 호출인지
검토하기 위해,
blocking receive import와 별도의 private P/Invoke entry point를 두고 `SuppressGCTransition`을
적용했다. multipart의 뒤 part와 blocking receive는 기존 import를 그대로 사용한다.

공개 interface와 Core ABI는 변경하지 않았다. `DontWait` error mapping, multipart 수신 순서,
Message ownership도 유지했고 `Zlink.Tests` 149개가 통과했다. 그러나 Sol review는 Core receive가
mutex와 multipart staging을 수행하므로 호출 시간이 제한되지 않는다고 확인했다. 따라서
`SuppressGCTransition`은 채택하지 않았고 일반 P/Invoke를 복원했다.

Core release `0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM,
64·256·1024·4096·65536·131072B 조건에서 C를 먼저, .NET을 다음에 단독 실행했다.

## 결과

| Message size | C throughput | .NET throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,933,856 msg/s | 1,258,530 msg/s | 65.08% |
| 256B | 1,356,722 msg/s | 1,015,661 msg/s | 74.86% |
| 1024B | 1,131,153 msg/s | 634,057 msg/s | 56.05% |
| 4096B | 296,504 msg/s | 268,899 msg/s | 90.69% |
| 65536B | 97,869 msg/s | 80,203 msg/s | 81.95% |
| 131072B | 48,387 msg/s | 41,598 msg/s | 85.97% |
| 산술평균 | - | - | 75.77% |

후보의 산술평균은 75.77%였지만, GC transition을 억제할 전제를 충족하지 않아 결과를
공식 paired 기준이나 전체 평균에 사용하지 않는다.

- C report: `/home/hep7hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_091542_dotnet-dealer-critical-c.txt`
- .NET report: `/home/hep7hep7/project/zlink/bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260812_091556_dotnet-dealer-critical.txt`
