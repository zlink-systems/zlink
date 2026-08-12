# Java DEALER_DEALER caller-provided receive 측정 결과

## 대상과 조건

`MULTI_DEALER_DEALER / tcp`만 C를 먼저, Java를 다음에 단독 실행했다. Release Core
`0.10.1`, clients `100`, duration `1초`, runs `1`, balanced auto-HWM,
message size `64, 256, 1024, 4096, 65536, 131072B`를 사용했다.

## 결과

| 구현 | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 |
|------|-----------------------------------|---------:|------|
| server `Received` caller-provided storage 재사용 | 73.61 / 73.47 / 76.77 / 64.50 / 57.27 / 87.62% | 72.21% | 통과 |

Java multi DEALER server는 매 수신마다 `Received`를 새로 생성하던 유일한 경로였다. server
수명 동안 caller-provided `Received` 하나를 재사용해 다른 binding의 표준 receive 사용과
같은 의미로 정렬했다. public interface, message ownership, active measurement window는
변경하지 않았다.

## 결과 파일

- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_132243_java-dealer-received-reuse-c.txt`
- Java: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_132311_java-dealer-received-reuse.txt`
