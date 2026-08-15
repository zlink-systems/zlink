# Java DEALER/DEALER poll parity 측정 결과

## 대상과 조건

`MULTI_DEALER_DEALER / tcp`만 C를 먼저, Java를 다음에 단독 실행했다.
Release Core `0.10.1`, clients `100`, duration `1초`, runs `1`, balanced
auto-HWM, message size `64, 256, 1024, 4096, 65536, 131072B`를 사용했다.

Java server active receive loop의 poll timeout을 남은 측정 시간에서 `-1`로 바꿨다.
ready socket을 받은 뒤 `DONT_WAIT`로 비울 때까지 수신하는 방식은 유지했다. 이는 C의
signal-driven poll과 같은 종료·drain 순서다.

## 결과

| Size | C Kmsg/s | Java Kmsg/s | Java/C |
|------|---------:|------------:|-------:|
| 64B | 2336.280 | 1037.700 | 44.42% |
| 256B | 1296.712 | 700.486 | 54.02% |
| 1024B | 456.261 | 542.965 | 119.00% |
| 4096B | 254.373 | 112.199 | 44.11% |
| 65536B | 90.700 | 65.675 | 72.41% |
| 131072B | 39.604 | 35.446 | 89.50% |
| 산술 평균 | - | - | **70.58%** |

Java process는 모든 size에서 정상 종료했고, C와 같은 poll 방식으로 변경한 뒤에도
결과 수집이 완료됐다.

## 결과 파일

- C: `/tmp/zlink-java-dd-poller-c/multi/report/perf_c_multi_linux_20260812_214701.txt`
- Java: `/tmp/zlink-java-dd-poller-java/multi/report/perf_java_multi_linux_20260812_214726.txt`
