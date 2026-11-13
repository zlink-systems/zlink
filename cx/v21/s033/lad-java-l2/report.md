# #4352 Java ladder L2 — envelope header

## 결과

Framework envelope header를 client가 encode하고 server가 한 번 parse하도록 복원했다. Permit은 L1과 같은 위치에서 계속 켠다. 마지막 표의 L2/binding 중앙값으로 계산한 단계 손실은 6셀 중 5셀이 7% 이내이며, request-backpressure 1K는 9.28%로 기준을 넘는다. send-saturation 1K는 손실 기준을 통과했지만, L2의 5회 G5 변동이 11.68%로 남았다.

단계 손실은 `1 − (L2 ÷ binding) ÷ (L1 ÷ binding)`이다. L1 비율은 지정된 `lad-java-l1/report.md` 값을 그대로 썼다. 처리량은 msg/s 중앙값이다.

| 셀 | L1÷binding | 반복 | binding | L2 | L2÷binding | 단계 손실 | 7% 판정 | G5 |
|---|---:|---:|---:|---:|---:|---:|---|---|
| send-saturation 1K | 0.998 | 5 | 538,993.2 | 526,614.6 | 0.977 | 2.10% | 통과 | binding 통과, L2 실패 11.68% |
| send-saturation 4K | 0.986 | 3 | 198,528.4 | 193,121.0 | 0.973 | 1.34% | 통과 | 모두 통과 |
| request-serial 1K | 1.021 | 5 | 14,675.6 | 14,391.0 | 0.981 | 3.96% | 통과 | 모두 통과 |
| request-serial 4K | 1.002 | 3 | 13,847.4 | 13,393.6 | 0.967 | 3.47% | 통과 | 모두 통과 |
| request-backpressure 1K | 1.007 | 3 | 139,752.2 | 127,665.4 | 0.914 | 9.28% | **초과** | 모두 통과 |
| request-backpressure 4K | 1.017 | 3 | 111,021.2 | 107,784.2 | 0.971 | 4.54% | 통과 | 모두 통과 |

모든 최종 선택 결과는 `errors=0`, `server_errors=0`, `submitted=completed=target_stats.received`다. send-saturation 1K는 다섯 번 재측정 후에도 L2 G5 기준을 통과하지 못했다. 따라서 2.10% 단계 손실은 관측 중앙값에 대한 판정이며, 변동성은 별도로 남긴다.

## 복원한 경로와 payload 소유권

- Source는 [FrameworkStack.java](../../../../../../worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/FrameworkStack.java:127)에서 L2 전용 operation을 고르고, `:141-151`에서 `ZLinkChannelEnvelope.create`와 `encodeHeader`로 Framework request/command header를 만든다. Request header에는 Framework 계약의 per-request correlation ID가 포함된다.
- Binding과 같은 send saturation driver를 유지하도록 [RawStack.java](../../../../../../worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/RawStack.java:120)에 L2 operation이 `RawSendOperation`을 상속한다. `:185-203`은 prepared send의 header만 바꾸며 기존 prepare/submit 경로를 사용한다. L1 기본 `request()`와 `send()` 경로는 그대로다.
- Target은 [ZLinkFrameworkBenchServer.java](../../../../../../worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/ZLinkFrameworkBenchServer.java:200)에서 첫 part에 대해 `decodeHeader`를 한 번 호출한다(`:201`). 같은 함수에서 body는 마지막 part의 `dataBuffer()`를 기존 `RawWire.decodeBenchPayloadBody`에 전달한다(`:213`). Header parser에는 첫 part만 전달되므로 envelope 때문에 payload를 복사하지 않는다. payload protobuf decode와 reply는 L1 경로와 같다.
- L2 permit은 기존 L1 설정 분기에서 유지한다: `ZLinkFrameworkBenchServer.java:124-141`. Response header/body와 두-part reply는 바꾸지 않았다.

L2는 binding보다 wire header를 추가하고 다르게 encode한다. Request header가 per-message correlation ID를 가지는 것은 `ZLinkChannelEnvelope.create:135-177` 계약 때문이다. 실험 코드는 Framework envelope API 사용과 binding-shaped driver를 비교하며, 제품 runtime 코드는 바꾸지 않았다.

## 초과 셀 비용과 제안

request-backpressure 1K는 단계 손실 9.28%다. paired 1-run JFR은 `raw/profile/reqbp-1K/`에 보존했다. Binding과 L2를 같은 측정 잠금 세션에서 각각 30초 warmup/5초 active로 실행했고, 각 JVM recording은 startup·warmup·active·drain을 포함한다.

- L2 client recording `alloc-1020349.jfr`에서 `ZLinkChannelEnvelope.encodeHeader` stack에 걸린 allocation sample 714개, 추정 sample weight 합계 1,751.31 MB를 확인했다. 그중 byte array 1,298.70 MB, `Message` 302.94 MB다. 호출은 `ZLinkChannelEnvelope.encodeHeader:497-498` → `encodeHeaderBytesUncached:568-579` → `FrameworkStack.envelopeHeader:141-151`이다. Request는 simple command cache를 사용할 수 없다(`ZLinkChannelEnvelope.java:510-521`).
- L2 server recording `alloc-1020263.jfr`에서 `decodeHeader` stack의 allocation sample 331개, 추정 sample weight 합계 742.47 MB를 확인했다. 상위 클래스는 `char[]` 242.14 MB, `byte[]` 173.69 MB, `Header` 136.22 MB, `HeaderReader` 85.95 MB다. 호출은 server `:201` → `ZLinkChannelEnvelope.decodeHeader:635-650`, canonical reader `:853-870,1213-1219`다.
- 이 값은 JFR sampled allocation weight이며 정확한 allocation 횟수나 payload 복사량은 아니다. 네 recording의 기간과 요약은 `raw/profile/reqbp-1K/` 안의 JFR 파일에서 확인할 수 있다.

제안: 결정과 구현은 `ZLinkChannelEnvelope` 소유 모듈 한 곳에 둔다. `writeHeader:438-494`와 `encodeHeaderBytesUncached:568-579`에서 channel/message/content type 이름을 `PreparedNames`로 한 번 준비하고, per-request correlation ID만 한 번의 writer pass로 넣는 경로를 설계한다. 결과는 메시지당 Framework envelope byte array 하나와 `Message` 하나를 소유하도록 해 현재 두 번의 full header write와 Header/JSON 중간 할당을 줄인다. Parser 쪽은 `decodeHeader:638-685,846-870`의 canonical path에서 정적 이름용 `String`/reader 할당을 줄이되, 계약상 header는 메시지당 한 번 계속 parse한다. 이 제안은 구현 전이며, 현재 L2 코드는 변경하지 않았다.

## 빌드·측정·raw

- Branch `exp/4350-java-ladder`; L1 `15c8bab1a6`, L2 commit `5d74ddac1ecc0623f9825ddecc99cff50a1f045c`. Commit은 local only다.
- `framework/bench/grpc/java/build.sh` 최종 소스 빌드 성공(`raw/build-final.log`). 빌드한 artifact hash와 source provenance는 각 최종 측정 전 `progress.md`에 기록했다.
- Binding과 L2는 각 셀 run 안에서 같은 프로세스 세션으로 측정했다. 30초 warmup, 5초 active, binding source CPU 0–7, target CPU 8–15, exclusive `/root/jobs/fx/cx/measure.lock`을 사용했다. Cross test는 하지 않았다.
- 최종 raw: send-saturation 1K와 request-serial 1K는 `raw/final-v2-G5-5run/run{1..5}/`; send-saturation 4K는 `raw/final-v2-send4k/run{1..3}/`; 나머지 네 request cell은 `raw/final-corrected/run{1..3}/`에 있다. `raw/final/`의 첫 탐색 결과는 binding 기본 경로에 supplier 비용이 섞여 있어 제외했다. `raw/final-corrected/`의 send 결과는 L2가 send 전용 driver specialization을 놓쳐 제외했다. 모두 재측정 원인과 함께 보존했다.
- Aggregation: `raw/aggregate-final-G5.{json,log}`, `raw/aggregate-final-send4k.{json,log}`, `raw/aggregate-final-requests.{json,log}`. 검증에 사용한 선택 raw 50개 모두 카운터와 error 조건을 통과했다.
