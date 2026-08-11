# Message storage와 batch 판단 과정

## 범위

- 작업 branch: `core-0.10.0-bindings-performance`
- Core runtime: git release package `0.10.1`
- 대상: Node receive storage, STREAM body storage, Java/.NET/Node multipart 경계, hidden receive prefetch

## 독립 record batch 판단

Receive prefetch는 public receive 한 번보다 많은 record를 Core queue에서 먼저 꺼내므로 sender의
HWM과 backpressure 발생 시점을 바꾼다. Send batch는 기존 동기식 `submit()` interface로 각
record의 `DONTWAIT`, timeout과 실패 결과를 돌려줄 수 없다. 따라서 세 binding에서 독립 record
send batch와 receive prefetch를 사용하지 않는다.

기존 receive batch ON/OFF 측정은 다음과 같다. 조건은 `tcp`, 64B, clients `100`, duration
`1초`, runs `1`이며 각 실행은 직렬로 수행했다.

| Binding / pattern | OFF | ON | ON 변화 |
|---|---:|---:|---:|
| Node `MULTI_DEALER_DEALER` | 423,798 msg/s | 411,270 msg/s | -2.96% |
| Node `MULTI_DEALER_ROUTER` | 55,837 msg/s | 65,673 msg/s | +17.62% |
| .NET `MULTI_DEALER_DEALER` | 1,423,109 msg/s | 1,497,404 msg/s | +5.22% |
| Java `MULTI_DEALER_DEALER` | 1,392,473 msg/s | 1,135,450 msg/s | -18.46% |

효과가 pattern과 binding마다 달랐고 public 결과 계약을 유지할 수 없으므로 제거했다.

## Multipart 경계 판단

Multipart는 여러 독립 record를 합치는 batch가 아니라 하나의 record다. Java와 .NET은 모든
native frame을 먼저 준비한 뒤 managed code에서 Core `*_part`를 호출한다. 두 binding의 별도
native multipart bridge는 제거했다.

Node는 JavaScript에서 part별 addon 호출을 시작하면 뒤 part의 materialization 실패 뒤 Core에
열린 multipart sequence가 남을 수 있다. Sol review에 따라 Node의 private addon adapter를
유지한다. Adapter는 모든 part validation과 native frame 준비, request callback 상태 생성을 첫
Core 호출 전에 끝낸 뒤 Core `*_part`를 반복한다. STREAM은 `MORE`를 쓰지 않고 여러 builder
part를 하나의 payload로 만든 뒤 `FINAL` 한 번으로 제출한다.

두 part, 합계 64B diagnostic 측정에서 별도 bridge는 Java `-4.04%`, .NET `-1.83%`, Node
`-8.33%`였다. Node의 bridge OFF 구현은 ownership과 STREAM record 계약을 만족하지 못하므로
성능 수치는 진단값으로만 남기고 채택하지 않았다.

## Receive storage와 STREAM option

Java의 FFM `ByteBuffer`와 .NET의 `Span<byte>`는 native payload view를 제공하므로 receive
`msg_t` ownership을 유지한다. Node는 같은 native view가 없고 payload 접근 시 addon 경계를
다시 넘는 비용이 있어 일반 receive에서 JS-owned `Buffer`를 사용한다. 이 규칙은 general,
routed, SUB, request completion과 multipart receive에 동일하게 적용했다.

STREAM callback body는 relay와 JavaScript 처리의 요구가 달라 Node에만
`packetBodyMaterialization` option을 추가했다. 기본 `Native`는 Core frame을 유지하고,
`Managed`는 callback 전에 JS-owned `Buffer`로 복사한다. Java와 .NET에는 이 option을 추가하지
않았다.

최종 `MULTI_STREAM/tcp/64B/100 clients/1초/1회` 직렬 측정은 Native 49,208 msg/s,
2.034ms, Managed 42,840 msg/s, 2.337ms였다. Native의 throughput이 14.86% 높고 mean latency가
12.97% 낮아 기본값으로 유지했다.

## 함께 수정한 lifecycle 오류

STREAM socket close가 threadsafe callback finalizer보다 먼저 slot을 재사용 가능 상태로 바꾸고
있었다. 이전 finalizer가 다음 socket의 handler slot을 초기화해 후속 callback이 사라질 수 있었다.
Close는 callback을 중단하고 socket 연결만 끊으며, slot 재사용은 finalizer만 수행하도록 수정했다.
수정 뒤 STREAM receive, option 잠금, Managed relay와 Native relay 테스트 4개가 모두 통과했다.

TCP peer의 JavaScript receive를 pause해 Node send backpressure를 강제하는 검사는 OS TCP receive
window를 제한하지 못해 결정적이지 않았다. 이 테스트는 제거했다. Core integration test가
`EAGAIN`에서 native frame ownership과 retry를 검증하며, Node public wrapper retention은 draft의
구현 gap으로 남겼다.
