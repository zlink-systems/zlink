# Java DEALER_ROUTER routing ID cache 측정 결과

## 대상과 조건

C의 `MULTI_DEALER_ROUTER_SENDSEND`과 Java의 같은 의미 pattern
`MULTI_DEALER_ROUTER`를 순서대로 단독 실행했다. Release Core `0.10.1`, clients
`100`, duration `1초`, runs `1`, balanced auto-HWM, message size
`64, 256, 1024, 4096, 65536, 131072B`를 사용했다.

## 결과

| 구현 | C 대비 throughput 비율 (size 순서) | 산술평균 | 판정 |
|------|-----------------------------------|---------:|------|
| caller-provided `Received` single-part builder 단순화 + bounded routing ID cache | 61.63 / 60.54 / 58.34 / 65.84 / 89.44 / 90.88% | 71.11% | 통과 |

ROUTER 수신에서 16 bytes 이하 routing ID는 기존 thread-local bounded `RoutingId`
cache를 조회해 hit 시 새 `byte[]` 복사 없이 immutable 내부 bytes를 사용한다. single-part
`Received.send()`은 multipart가 되기 전까지 목록을 만들지 않는다. 둘 다 public interface,
multipart 동작, message ownership을 바꾸지 않는다.

## 결과 파일

- C: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260812_124229_java-router-recv-cache-c.txt`
- Java: `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260812_124527_java-router-recv-cache.txt`
