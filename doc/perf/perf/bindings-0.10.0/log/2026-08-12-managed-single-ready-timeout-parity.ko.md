# Managed single ready timeout parity 확인 결과

## 기준

C single perf는 `PERF_CONNECT_READY_TIMEOUT_MS`가 설정되지 않았을 때 `1000ms`를 사용한다.
C multi perf의 기본값 `10000ms`와 같지 않다.

## 변경

Java, .NET, Node의 single perf fallback을 `1000ms`로 맞췄다. multi fallback은
`10000ms`를 유지했다. 이 값은 연결 준비 실패를 판단하는 제어 시간이며 throughput
측정 window에는 포함하지 않는다.

## 검증

| 대상 | 검증 | 결과 |
|---|---|---|
| Java | `./gradlew --no-daemon test :perf-single:test :perf-multi:test` (JDK 22, Core 0.10.1) | 통과 |
| .NET | Release `perf/single` 및 `perf/multi` build (Core 0.10.1) | 통과 |
| Node | TypeScript 및 perf tool 산출물 재생성 | 통과 |
