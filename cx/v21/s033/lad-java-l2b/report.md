# #4352 Java ladder L2 correction

## 변경

- `FrameworkStack.java:34-38,136-160`, `RawStack.java:93-140,210-230`: 채널·메시지·content-type `PreparedNames`를 한 번 준비하고, L2 요청/전송마다 Framework의 `ZLinkFrameworkMultipartView`용 envelope encode overload를 호출한다. 변경은 `e32f4ff12720` 및 overload parity를 맞춘 최종 커밋 `bf8327525794`에 로컬 커밋했다.
- production과 같은 호출: `ZLinkChannelCallRuntime.java:478-504`가 `encode(create(...), payload, channelNames, names, names)`를 호출한다. L2도 같은 인자와 multipart-view overload `ZLinkChannelEnvelope.java:600-608`를 사용하며, header bytes는 `encodeHeaderBytesInternal`을 거쳐 `encodeHeaderBytesUncached`의 size/write 두 단계(`:558-579`)로 만들어진다. 단일 `encodeHeaderBytes(header, names)` overload(`:552-555`)는 호출하지 않는다.
- 서버 production receive는 `ZLinkChannelRouteDispatcher.java:70,121` → `decodeDispatchPacket`(`ZLinkChannelEnvelope.java:726-729`) → `decodeDispatchHeader`(`:710-712`) → `decodeHeader` 경로다. 더 싼 별도 parser는 없어 bench의 `ZLinkFrameworkBenchServer.java:201` `decodeHeader(..., false)`를 유지했다.
- 제품 runtime 코드는 바꾸지 않았다.

## 측정

Throughput 중앙값(msg/s). L1 비율은 L1 보고서의 같은 셀 값을 사용했다. 단계 손실은 `1 − (L2÷binding)/(L1÷binding)`이다.

| 셀 | 회수 | binding | L2 | L2÷binding | L1÷binding | 단계 손실 | G5 spread (binding / L2) | raw 근거 |
|---|---:|---:|---:|---:|---:|---:|---|---|
| request-backpressure 1K | 5 | 142,181.6 | 136,130.6 | 0.957 | 1.007 | 4.92% 통과 | 1.23% 통과 / 17.19% 실패 | [L2 raw](raw/v2/request-backpressure-1K/run{1..5}/{zlink-java,zlink-framework-java}-request-backpressure-1024/results.json), [aggregate](raw/v2/aggregate-final.json); [L1 raw](../lad-java-l1/raw/final/request-backpressure-1K/run{1,2,3}/{zlink-java,zlink-framework-java}-request-backpressure-1024/results.json) |
| send-saturation 1K | 5 | 543,230.4 | 535,779.0 | 0.986 | 0.998 | 1.17% 통과 | 2.70% 통과 / 3.36% 통과 | [L2 raw](raw/v2/send-saturation-1K/run{1..5}/{zlink-java,zlink-framework-java}-send-saturation-1024/results.json), [aggregate](raw/v2/aggregate-final.json); [L1 raw](../lad-java-l1/raw/final/send-saturation-1K/run{1,2,3}/{zlink-java,zlink-framework-java}-send-saturation-1024/results.json) |
| request-backpressure 4K | 3 | 114,016.6 | 109,052.2 | 0.956 | 1.017 | 5.95% 통과 | 1.99% 통과 / 6.15% 통과 | [L2 raw](raw/v2/request-backpressure-4K/run{1..3}/{zlink-java,zlink-framework-java}-request-backpressure-4096/results.json), [aggregate](raw/v2/aggregate-final.json); [L1 raw](../lad-java-l1/raw/final/request-backpressure-4K/run{1,2,3}/{zlink-java,zlink-framework-java}-request-backpressure-4096/results.json) |

집계에서 세 셀 모두 errors와 target_errors는 0이고 카운터 불일치·불완전 run은 없었다([aggregate](raw/v2/aggregate-final.json), [run log](raw/L2bV2-corrected-group-driver.log)). request-backpressure 1K 단계 손실은 7% 이내여서 조건부 JFR 재측정은 하지 않았다. 해당 L2 셀은 G5 spread 17.19%로 실패했다.

## 빌드 및 남은 비용

`framework/bench/grpc/java/build.sh` 빌드 성공([build log](raw/build-final-v2.log)); client/server JAR이 해당 모듈의 최신 Java/proto source보다 최신임을 확인했다([freshness](raw/v2/source-jar-freshness.txt)). 추가 profile 및 남은 비용 proposal은 조건에 해당하지 않아 없다. 추가 테스트는 실행하지 않았다.
