# 네 언어 메시징 비용 측정과 최적화 지침

이 문서는 C++·.NET·Java·Kotlin 유지보수자가 기능별 비용을 비교하고, 계약을 유지하는
최적화를 자기 언어에 적용할 때 참고한다. 현재 저장된 단계별 실측은 .NET이며, 다른 언어는
같은 조건의 진단을 준비 중이다. 아래 .NET 결과를 다른 언어의 실측값으로 사용하지 않는다.

- 현재 누적 실측: [남은 기능의 누적 추가](#남은-기능의-누적-추가--진행-중).
- 공통 적용 준비: [네 언어 단계 정렬](#네-언어-단계-정렬).
- Codec·Envelope·Wire·Mailbox 적용 근거: [반복 최적화 2차](#반복-최적화-2차--진행-중).
- 이전 실험과 profiler 자료는 각 비교의 조건·원자료를 확인하는 참고 자료다.
  서로 다른 실험의 손실률을 합치거나 현재 전체 Framework 비용으로 해석하지 않는다.

## 측정 범위

아래 .NET 측정은 2026-09-14의 `bench/319-unified-runner-contract` 작업 트리에서 실행했다.
목적은 Core와 Framework의 차이를 기능별로 분리하는 것이다.
아래 `wire` 단계는 전체 Framework가 아니라 Core와 같은 수신·응답 루프에
Framework의 실제 body codec, envelope header, application wire 처리를 넣은 진단 실행 파일이다.
Core와 binding 코드는 변경하지 않았다. 전체 Framework의 성능 개선이 완료되었다는 결과는 아니다.

조건은 Release, .NET 8.0.31, Linux, 논리 CPU 20개, 각 패턴 1회,
warmup 3초, active 10초다. `request-serial`과 `request-backpressure`는
64 B 요청·4,096 B 응답, `send-saturation`은 4,096 B 단방향 전송이다.
이 크기는 application payload 기준이며 wire header 크기를 포함하지 않는다.
실행 파일은 기존 test friend assembly에 진단 코드를 포함해 만들었다.

Core-like 단계도 매번 typed `BenchPayload`를 protobuf로 직렬화·역직렬화한다.
Codec 단계에서 추가하는 것은 Framework serializer 선택·호출 경계이며 protobuf 자체가 아니다.
다른 언어의 기준을 bare payload 전송으로 바꾸면 비교 대상의 작업이 달라지므로 사용하지 않는다.

사용자 요청에 따라 다음 새 측정부터 warmup 2초·active 5초·runs 1회로 실행한다.
이미 완료된 3초/10초 결과는 보존한다. Runner는 새 실행의 실제 설정을 검증하고,
비교 대상의 측정 시간도 함께 기록한다. 서로 다른 측정 시간의 결과를 동일 조건으로 표기하지 않는다.

유효성 조건은 client/server errors 0, abandoned 0, drain bound 미도달,
최종 서버 수신 수와 제출 수 일치다. Request는 완료 수도 제출 수와 같아야 한다.
실패한 실행은 처리량 비교에서 제외한다. 단일 실행의 수 퍼센트 차이는
측정 변동과 구분할 수 없으며, 단계별 손실률을 더해서 전체 손실률로 해석하지 않는다.

## 지표 정의

- 처리량 손실: `100 × (1 − 기능 추가 후 처리량 / 해당 비교의 기준 처리량)`.
- 서버 CPU 비용: `server_cpu_percent × 20 × 10,000 / 처리량`, 단위 CPU µs/message.
  CPU 비율은 전체 논리 CPU 용량 기준이다. 이 값은 메시지당 서버 프로세스 CPU 사용량이지 응답 지연이 아니다.
- CPU sample 비중: profiler가 채집한 CPU sample 중 특정 stack을 포함한 비율.
  처리량 손실률이나 I/O 대기 시간과 다르다.

## 누적 기능 추가의 기준 상태

이 절은 기능을 제외한 상태부터 하나씩 추가하는 비교다. 아래의 독립 비교와
서로 다른 측정 계열이며, 독립 비교의 손실률을 누적 비용으로 사용하지 않는다.
각 경계는 이전 단계와 새 단계만 실행하고 결과 확인 전에 다음 기능을 실행하지 않는다.

첫 기준은 `MessagingFeatureRamp.cs`의 `RunBenchBaseline`, `RunBaselineRouter`,
`RunBaselineRecord`와 기존 Client의 제출·응답·drain 루프를 사용한다.
이는 production runtime에서 기능을 모두 활성화한 결과가 아니라 기능을 제외한 진단 실행이다.
Body는 `RawWire`, header는 고정 raw header이며 reusable `Received`에서 즉시 응답한다.
Body owner는 `new Message(size)`로 고정하여 이후 실제 codec과 소유권 할당 조건을 맞춘다.
Framework body codec, JSON envelope, application/service wire, mailbox,
worker 인계, permit, handler DI와 peer/lifecycle 처리는 추가하지 않았다.

| 패턴 | 기존 Core 벤치 | 기능 제외 기준 | 기준 / Core | 서버 CPU 비용: Core → 기준 |
| --- | ---: | ---: | ---: | ---: |
| request-serial | 7,318.0 | 7,360.5 | 100.6% | 49.74 → 46.33 |
| request-backpressure | 210,995.3 | 216,569.0 | 102.6% | 9.15 → 8.98 |
| send-saturation | 423,908.0 | 403,314.8 | 95.1% | 4.07 → 4.05 |

단위는 message/s와 CPU µs/message다. 모두 오류 0과 drain 조건을 통과했다.
이번 단일 실행에서 기준은 Core와 5% 이내였으며, 100%를 넘은 값은 음수 오버헤드의 근거가 아니다.
### 첫 기능: Framework body codec

같은 진단 빌드에서 기준을 새로 측정한 뒤 실제 Framework body codec만 추가했다.
벤치에 등록된 Protobuf codec의 encode/decode이며 JSON envelope와 wire는 추가하지 않았다.
아래 손실률은 이 비교의 기준에 대한 값이지 위 기준 실행과의 비율이 아니다.

| 패턴 | 기능 제외 기준 | Codec 추가 | 처리량 손실 | 서버 CPU 비용: 기준 → 추가 |
| --- | ---: | ---: | ---: | ---: |
| request-serial | 7,544.3 | 6,781.0 | 10.1% | 45.46 → 52.50 |
| request-backpressure | 217,237.2 | 204,960.2 | 5.7% | 9.03 → 9.53 |
| send-saturation | 406,589.2 | 387,953.9 | 4.6% | 4.06 → 4.05 |

각 1회 실행했으며 모두 오류 0과 drain 조건을 통과했다.
Codec을 추가한 상태의 처리량 손실은 이번 실행에서 4.6~10.1%다.
이는 production Framework 전체 성능 격차를 설명하는 결과가 아니다.
이 첫 비교에서는 JSON envelope, wire와 이후 기능을 추가하지 않았다.

### Codec 경계의 원인 분리

Codec 기능 구성은 유지하고 encode와 decode의 추가 비용만 분리했다.
진단 빌드의 Framework, Protobuf codec, binding, Google.Protobuf와 Shared DLL 해시는
첫 기능 비교에 사용한 빌드와 일치했다. Production runtime과 Core/binding 코드는 변경하지 않았다.

순수 CPU 비교는 I/O 없이 operation별 100,000회 warmup 후 1,000,000회 실행했다.
고정 순서의 영향을 줄이기 위해 10개 block의 실행 순서를 회전했다.
기준과 codec 모두 `new Message(size)`를 사용했다.

| Operation | 기준 | Framework codec | 차이 |
| --- | ---: | ---: | ---: |
| 64 B encode | 58.81 ns | 63.03 ns | +4.22 ns |
| 64 B decode | 46.45 ns | 66.27 ns | +19.82 ns |
| 4,096 B encode | 83.90 ns | 95.88 ns | +11.98 ns |
| 4,096 B decode | 263.69 ns | 267.41 ns | +3.71 ns |

이 비교의 64 B 요청과 4,096 B 응답에 필요한 encode/decode를 합하면,
CPU 비용은 기준 452.86 ns, codec 492.60 ns로 차이는 39.73 ns(약 0.040 µs)다.
이는 I/O 없이 warmup 후 측정한 직접 처리 비용이지 실제 통신의 응답 지연 자체가 아니다.
Managed allocation은 양쪽에서 같았으며 encode는 각 88 B,
64 B payload의 decode는 152 B, 4,096 B payload의 decode는 4,184 B였다.
Native 메모리 할당량은 이 GC 카운터의 측정 대상이 아니다.

추가 처리는 `ZLinkEnvelopeCodec.cs:226`의 serializer 선택·타입 검사·간접 호출과,
`ZLinkProtobufCodec.cs:60`의 생성 함수 캐시 참조다.
타입별 serializer 선택은 cache hit이면 lock을 사용하지 않는다
(`ZLinkSerializerSelectionRegistry.cs:65`). Content type에 따른 조회도 dictionary 참조이며,
이 fixture에서 메시지별 reflection이나 새로운 4 KB 중간 배열을 추가하지 않는다.
Serializer 조회만 하는 operation은 이번 측정에서 4.87~9.18 ns, managed allocation 0 B였다.

다음은 각 상태·각 패턴을 1회씩 실행한 통신 비교다. Encode-only와 decode-only는
codec 내부 원인을 찾기 위한 독립 비교이며 다음 Framework 기능을 추가한 결과가 아니다.

| 상태 | request-serial | request-backpressure | send-saturation |
| --- | ---: | ---: | ---: |
| 기능 제외 기준 | 6,997.5 | 211,280.6 | 395,075.3 |
| Encode-only | 7,483.2 | 203,974.7 | 418,654.6 |
| Decode-only | 7,332.7 | 217,437.7 | 438,499.0 |
| Encode/decode 모두 codec | 6,680.7 | 214,298.4 | 396,253.3 |

단위는 message/s다. 모든 12개 실행이 오류 0과 drain 조건을 통과했다.
양쪽을 codec으로 바꾼 손실률은 serial 4.5%, backpressure −1.4%, send −0.3%였다.
Encode-only/decode-only 수치를 더해서 합성 비용으로 해석하지 않는다.

Serial의 별도 native profile 실행은 기준 6,763.6, codec 6,728.4 message/s(손실 0.5%)였다.
Active 중 약 2초의 scheduler 기록에는 양쪽 모두 JIT 실행이 있었으며,
source의 tiered compilation thread 실행 시간 합계는 169.677/146.299 ms였다.
같은 약 2초의 `perf stat`에서 source CPU는 1,332.09/1,332.17 ms,
target CPU는 692.27/678.23 ms로 codec 쪽에 큰 CPU 증가가 나타나지 않았다.
Context switch 수는 source 45,975/52,793, target 32,117/35,762였다.
수가 많다는 이유만으로 불필요한 I/O 대기나 지연의 원인이라고 판단하지 않는다.
CPU sample 수와 미해석 kernel stack의 제약으로 함수 비율에 따른 전체 비용 배분은 하지 않는다.

Tiered compilation을 끈 별도 serial 비교는 기준 6,698.5, codec 6,775.0 message/s로,
손실은 −1.1%였다. 이는 JIT를 켠 동일 조건의 반복이 아니라 진단을 위한 조건 변경이다.
이 결과만으로 JIT가 최초 10.1% 차이의 원인이라고 단정하지 않는다.

최초 10.1%는 codec의 고정 비용으로 재현되지 않았다.
확인한 직접 추가 비용은 약 0.040 µs/왕복으로, 최초 처리량의 역수로 계산한 약 14.92 µs의
요청 1회 처리당 경과 시간 차이를 설명하지 못한다. 남은 측정 변동을 JIT나 I/O·스레드 대기 중 어디에
인과적으로 배분할지는 증거가 부족하다. 확인하지 않은 고정 10%를 전제로 runtime을 변경하지 않는다.

### 환경변수 선택 분기를 제거한 비교

두 실행 파일은 동일한 진단 소스와 Release 설정에서 컴파일했다.
`MessagingFixedCodec=core`는 Protobuf 직접 호출만,
`MessagingFixedCodec=codec`은 실제 Framework body codec 호출만
`EncodeBenchPayload`와 `DecodeBenchPayload`에 포함한다.
이 두 함수에서 실행 중 codec을 선택하는 분기는 없다.
공통 수신·전송 루프의 stage와 나머지 기능 값도 같은 compile-time 상수로 고정했다.
Codec 등록·초기화는 양쪽 모두 유지한다.

`run_fixed_codec.sh`의 `BENCH_DIAGNOSTIC_SOURCE_DLL`·`TARGET_DLL`은
실행 파일을 지정할 뿐 기능을 선택하지 않는다.
잘못된 stage·permit 환경변수와 mailbox ON·다른 body owner 설정을 전달해도
고정 실행 파일의 구성이 바뀌지 않는 것을 확인했다.
생성된 encode/decode assembly에서도 각각 `RawWire`와 `ZLinkEnvelopeCodec` 호출을 확인했다.

양쪽의 binding, Framework, codec, Google.Protobuf, Shared DLL과 native library는
바이트 단위로 같았다. 요청·응답·send fixture의 전체 header/body 바이트와 decode 결과도 같았다.
고정 raw header는 201 B, application payload 64 B/4,096 B의 encoded body는 66 B/4,099 B였다.
다음은 기존과 같은 warmup 3초·active 10초·패턴별 1회 측정이다.

| 패턴 | Protobuf 직접 호출 | Framework codec 호출 | 처리량 손실 | 서버 CPU 비용: 직접 → codec |
| --- | ---: | ---: | ---: | ---: |
| request-serial | 7,334.1 | 7,285.9 | 0.7% | 49.09 → 50.65 |
| request-backpressure | 212,730.0 | 206,754.7 | 2.8% | 9.12 → 9.39 |
| send-saturation | 404,174.2 | 419,684.3 | −3.8% | 4.07 → 3.94 |

단위는 message/s와 CPU µs/message다. 모든 6개 실행은 오류 0과 drain 조건을 통과했다.
이번 고정 비교의 request 손실은 0.7%/2.8%로 최초 serial 10.1%가 나타나지 않았다.
Send의 증가를 codec의 음수 비용으로 해석하지 않는다.
이전 환경변수 비교와 실행 시점이 다른 단일 측정이므로
환경변수 분기가 이전 큰 차이의 유일한 원인이었다고 단정하지 않는다.
이 codec 비교에는 header·wire·mailbox·worker 인계·permit·DI와 이후 기능을 추가하지 않았다.

### 다음 기능: Codec 유지 + JSON envelope header

Compile-time 고정 `codec`과 `envelope` 실행 파일을 비교했다.
양쪽 모두 실제 Framework Protobuf body codec과 같은 수신·응답 루프를 유지한다.
`envelope`에만 다음 처리를 추가했다.

- Client: 실제 `ZLinkClientCallCodec.CreateEnvelope`로 요청 correlation·deadline 또는 send header를 생성하고,
  `ZLinkEnvelopeCodec.EncodeHeader`로 JSON을 직렬화한다.
- Server: `RunBaselineRecord`에서 실제 `DecodeHeader`로 JSON을 해석하고,
  요청의 correlation을 유지한 응답 header를 생성·직렬화한다.
- Client 응답: `ZLinkEnvelopeReplyDecoder.Decode`로 header·응답 kind·body 존재를 검증하고,
  header의 content type으로 같은 body codec을 선택한다.

Application/service wire packing, mailbox, worker 인계, permit, handler DI,
peer/lifecycle 기능은 추가하지 않았다. 환경변수는 기능을 선택하지 않는다.
잘못된 stage·permit 값과 mailbox ON을 전달해도 고정 구성이 유지됐다.
두 빌드의 공통 managed DLL과 native library는 바이트 단위로 같았고,
세 fixture의 body 바이트·decode 결과도 일치했다.
Envelope fixture에서는 요청의 correlation·deadline, 응답의 correlation 유지,
send의 correlation·deadline 없음도 확인했다.

기준은 header가 없는 상태가 아니라 201 B 고정 raw header를 전송하는 상태다.
실제 JSON envelope header는 이번 fixture에서 요청 199 B, 응답 156 B, send 169 B였다.
따라서 아래는 동일 header 바이트에 대한 순수 JSON CPU 비용이 아니라,
고정 header를 실제 생성·해석 경로로 바꾼 기능 경계의 통신 결과다.
Encoded body는 양쪽 모두 요청 66 B, 응답·send 4,099 B였다.

| 패턴 | Codec 기준 | Codec + envelope | 추가 처리량 손실 | 서버 CPU 비용: codec → envelope |
| --- | ---: | ---: | ---: | ---: |
| request-serial | 6,438.4 | 6,691.4 | −3.9% | 57.62 → 54.25 |
| request-backpressure | 209,398.8 | 190,309.9 | 9.1% | 9.29 → 10.48 |
| send-saturation | 390,547.6 | 398,385.8 | −2.0% | 4.34 → 4.39 |

단위는 message/s와 CPU µs/message다. 각 단계·각 패턴을 1회씩,
warmup 3초·active 10초로 실행했고 모든 6개 실행은 오류 0과 drain 조건을 통과했다.
이번 backpressure의 추가 처리량 손실은 9.1%, 메시지당 서버 CPU 증가는 12.8%였다.
Serial·send의 증가를 JSON의 음수 비용으로 해석하지 않는다.
기준은 이 비교에서 새로 측정한 codec이며 앞 절의 codec 절대값과 다르다.
이전 Core 비율과 곱하거나 단계별 손실을 더해서 전체 Framework 손실로 해석하지 않는다.
이 결과 이후 다음 기능은 추가하지 않았다. Production runtime과 Core/binding은 변경하지 않았다.

### 다음 기능: Envelope 유지 + application/service wire

Compile-time 고정 `envelope`과 `wire` 실행 파일을 비교했다.
`wire`는 envelope와 Protobuf body codec을 그대로 유지하고 다음 실제 Framework wire 처리만 추가한다.

- `ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage`로 envelope header와 body를 하나의 application payload로 pack한다.
- `ZLinkServiceWireCodec.EncodeApplicationMessage` 또는 `EncodeReplyMessage`로 service header를 추가한다.
- Server에서 service header를 해석하고 multipart view를 복원한 뒤, client에서 reply service header와 multipart view를 해석한다.

Mailbox, worker 인계, permit, handler DI와 peer/lifecycle는 추가하지 않았다.
Compile-time assembly에는 위 multipart pack·service header encode/decode·reply decoder 호출이 포함됨을 확인했다.
잘못된 feature 환경변수도 fixed 실행 파일의 구성을 바꾸지 못했다.
Wire fixture는 request/reply/send의 service command·correlation·terminal result와 envelope 의미를 검증한다.
내부 Protobuf body는 envelope 단계와 동일하게 요청 66 B, 응답·send 4,099 B이며 decode 결과도 일치했다.

| 패턴 | Envelope 기준 | Envelope + wire | 추가 처리량 손실 | 서버 CPU 비용: envelope → wire |
| --- | ---: | ---: | ---: | ---: |
| request-serial | 6,802.9 | 6,553.7 | 3.7% | 53.36 → 56.15 |
| request-backpressure | 190,300.3 | 175,627.2 | 7.7% | 10.36 → 11.26 |
| send-saturation | 408,787.7 | 416,331.9 | −1.8% | 4.33 → 3.99 |

단위는 message/s와 CPU µs/message다. 각 단계·각 패턴은 1회, warmup 3초·active 10초로 실행했고
오류 0과 drain 조건을 통과했다. Backpressure의 wire 추가 손실은 7.7%, 메시지당 서버 CPU 증가는 8.7%다.
Serial·send의 단일 실행 증가를 wire의 음수 비용으로 해석하지 않는다.
이 표는 stripped diagnostic 경로의 기능 경계이며 production Framework 전체 처리량이 아니다.

### 기능 추가 단계의 저장된 측정값

각 셀은 `실측 처리량 (표의 직전 열 대비 감소율)`이다.
감소율은 `100 × (1 − 현재 열 처리량 / 바로 왼쪽 열 처리량)`으로 계산한다.
이 표에서는 이전 비교의 paired 손실률을 옮겨 쓰거나, 손실률을 합산·곱해 현재 처리량을 만들지 않는다.

| 패턴 | 기능 추가 전 | + Body codec | + JSON envelope | + Application/service wire | + Mailbox queue/claim | 초기 Mailbox 캡처 할당 최적화 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| request-serial | 7,334.1 | 7,285.9 (0.7%) | 6,691.4 (8.2%) | 6,553.7 (2.1%) | 5,967.8 (8.9%) | 6,302.4 (−5.6%) |
| request-backpressure | 212,730.0 | 206,754.7 (2.8%) | 190,309.9 (8.0%) | 175,627.2 (7.7%) | 157,805.0 (10.1%) | 159,674.2 (−1.2%) |
| send-saturation | 404,174.2 | 419,684.3 (−3.8%) | 398,385.8 (5.1%) | 416,331.9 (−4.5%) | 375,387.0 (9.8%) | 378,574.5 (−0.8%) |

단위는 message/s다. 각 열은 해당 기능까지 적용한 뒤의 1회 측정값이며,
마지막 열은 codec·envelope·wire·mailbox를 유지한 초기 최적화 단계다. 최신 최적화 결과는 아래 별도 표에 기록한다.
열마다 실행 시점이 다르므로 괄호는 저장된 측정값 사이의 변화율이지 반복 검증한 고정 기능 비용이 아니다.
음수 감소율은 해당 측정값이 직전 열보다 컸다는 뜻이다.
다음 기능은 새 단계만 1회 측정하고 기존 측정값을 재사용해 이 표에 추가한다.

### 다음 기능: Wire 유지 + mailbox queue/claim

`MessagingFixedCodec=mailbox`는 fixed wire와 같은 body codec·JSON envelope·application/service wire를 유지한다.
수신 스레드에서 실제 `ZLinkMeshNodeOwnedMailbox`에 enqueue한 뒤 claim → drain → release한다.
`ZLinkMeshQueuedRecord`·`MeshReceiveBatch`와 수신 payload 소유권도 포함한다.
Worker 인계·permit·DI·peer/lifecycle 처리는 이 단계에 포함하지 않는다.

이번 실행은 `.artifacts/fixed-wire-production-comparison`의 fixed wire 실행 파일과 측정값을 재사용했고,
새 mailbox 단계의 세 패턴만 각각 1회 측정했다. Warmup 3초·active 10초와 payload 조건은 같다.
두 실행 파일의 공통 managed DLL·native library와 세 fixture의 Protobuf body가 일치했다.
새 세 실행은 client/server errors 0, abandoned 0, drain bound 미도달,
서버 최종 수신 수와 제출 수 일치를 통과했으며 request 완료 수도 일치했다.
관찰된 직전 대비 감소율은 serial 8.9%, backpressure 10.1%, send 9.8%다.

### Mailbox 유지 + 무경합 lane 캡처 할당 최적화

`ZLinkStateLane.RunAsync<T>`는 경합 시 사용하는 callback의 캡처를 static local function 안에서 생성한다.
무경합 반환 경로는 그 function을 호출하지 않는다. FIFO·재진입 검사·claim·payload 소유권과
worker/permit 설정은 유지한다. Mailbox claim은 수신 배치를 꺼내는 권한이며,
비동기 handler 완료까지 유지하는 Actor 실행 권한과는 다르다.

`.artifacts/mailbox-capture-optimized/summary.json`은 저장된 mailbox 처리량과 새 수정 버전을 비교한다.
세 패턴은 각각 warmup 3초·active 10초로 1회 측정했고 기존 단계는 재측정하지 않았다.
세 실행 모두 client/server errors 0, abandoned 0, drain bound 미도달,
제출 수와 서버 최종 수신 수 일치, request 제출 수와 완료 수 일치를 통과했다.
Binding·Google.Protobuf·Shared·Contracts DLL과 native library는 이전 실행 파일과 byte 단위로 일치했다.
Framework DLL 및 재빌드된 Framework Protobuf codec DLL의 hash는 `framework.sha256`에 기록한다.
이 작업에서 Protobuf codec 소스는 변경하지 않았다. Fidelity 검사는 요청의 가변 deadline을 포함한
전체 body hash 대신 framing 크기와 logical Protobuf body의 크기·hash를 비교했고 일치했다.

무경합 lane 단독 비교는 같은 새 진단 실행 파일·cached static delegate를 사용하고
Framework DLL만 이전/수정 버전으로 바꿨다. 각 버전은 10,000회 준비 후 1,000,000회 실행했다.

| 항목 | 이전 | 수정 |
| --- | ---: | ---: |
| 관리 메모리 할당 / lane 호출 | 104 B | 72 B |
| 시간 / lane 호출 | 64.13 ns | 59.40 ns |

할당은 호출당 32 B 감소했다. 이 비교의 진단 mailbox의 enqueue·claim·drain·release 네 호출에서는
메시지당 128 B 감소하며, queued record와 호출부의 람다 할당은 유지된다.
단독 비교의 ns 값은 1회 관찰값이며, 이를 전체 처리량 개선의 원인별 비중으로 환산하지 않는다.

| 패턴 | 서버 CPU 시간 / 메시지: 이전 → 수정 |
| --- | ---: |
| request-serial | 65.02 → 58.87 µs |
| request-backpressure | 12.91 → 12.42 µs |
| send-saturation | 5.13 → 5.27 µs |

전체 처리량 증가를 모두 캡처 할당 제거의 효과로 단정하지 않는다. 할당 감소는 단독 비교로 확인됐지만,
send의 CPU 비용은 증가했고 backpressure·send의 처리량 변화도 작다.
이 초기 최적화 단계의 저장된 wire 처리량 대비 mailbox 감소율은 serial 3.8%, backpressure 9.1%, send 9.1%이며,
mailbox 전체 비용을 해결한 결과는 아니다. 다음 기능은 추가하지 않았다.

변경 분류는 B(불필요한 캡처 할당 개선), 소유 계층은 Framework state lane이다.
계약은 `06-state-ownership-and-lanes` §4 C2를 유지하며 Java의 synchronized mailbox에는
이 RunAsync callback 경로가 없다. 직렬화 규칙 수는 1 → 1(같은 lane), 추가 예외 규칙은 0이다.
StateLane·Mailbox focused test 31개와 .NET 전체 단위 테스트 2,270개가 통과했다.
전체 단위 테스트 결과는 `.artifacts/mailbox-capture-optimized/unit-tests.log`에 기록한다.

### Mailbox 이후 누적 최적화 측정값

Codec + envelope + wire + mailbox를 모두 유지하고, 새 단계의 세 패턴만 각각 1회 측정했다.
기존 단계는 재측정하지 않았다. 각 셀은 `message/s (바로 위 단계 대비 감소율)`이다.
음수 감소율은 처리량 증가이며, 현재 처리량은 마지막 행의 실측값이다.

| 누적 단계 | request-serial | request-backpressure | send-saturation |
| --- | ---: | ---: | ---: |
| 초기 lane 캡처 최적화 | 6,302.4 | 159,674.2 | 378,574.5 |
| + Mailbox static callback | 6,297.0 (0.1%) | 153,098.5 (4.1%) | 387,389.2 (−2.3%) |
| + Lane 소유권 조회 재사용 | 6,085.2 (3.4%) | 148,451.8 (3.0%) | 411,770.5 (−6.3%) |
| + 기존 배치 바이트 집계 수정 | 6,136.6 (−0.8%) | 156,834.1 (−5.6%) | 375,103.5 (8.9%) |
| + Wire 길이·span 재사용 | 6,404.4 (−4.4%) | 161,382.9 (−2.9%) | 397,432.6 (−6.0%) |
| + Codec·envelope 정책 통합 (이 비교의 마지막 단계) | 5,905.2 (7.8%) | 154,911.9 (4.0%) | 366,582.9 (7.8%) |

이는 compile-time 고정 diagnostic 경로다. Worker 인계·permit·DI·peer/lifecycle은 추가하지 않았다.
Production Framework 전체와 Core의 성능 비율로 읽지 않는다. Warmup 3초·active 10초,
request 64 B → 4,096 B, send 4,096 B 조건은 유지했다.
새 모든 실행은 client/server errors 0, abandoned 0, drain bound 미도달,
제출 수와 서버 최종 수신 수 일치, request 제출 수와 완료 수 일치를 통과했다.
공통 binding·Google.Protobuf·Shared·Contracts DLL 및 native library는 byte 단위로 일치했다.
Fixture의 logical Protobuf body 크기·hash와 header 크기도 일치했다. 가변 request deadline 때문에
전체 packed body의 길이·hash는 비교하지 않으며, deadline 등 header 필드 의미는 fidelity 검사에서 검증한다.

#### 변경 내용과 채택 근거

1. **Static callback:** `ZLinkStateLane`의 기존 callback overload와 mailbox 호출부가
   하나의 typed-state 실행 경로를 사용한다. 무경합 mailbox enqueue → claim → drain → release
   단독 비교의 관리 할당은 944 → 576 B/record로 368 B 감소했다. 초기 캡처 최적화의 128 B까지
   포함하면 원래 구조 대비 496 B 감소다. 이 값은 같은 스레드의 mailbox fixture 범위이며
   Framework 전체 메시지 할당량이 아니다. 처리량 개선이 모든 패턴에서 확인된 것은 아니다.
2. **소유권 조회 재사용:** 같은 turn의 `CurrentLane.Value`를 재진입 검사와 복원에 재사용한다.
   보호·FIFO·완료·drain 규칙은 동일하다. `AsyncLocal` 설정 비용은 남으며 lane 호출당 72 B도 유지된다.
   Serial·backpressure는 측정상 감소했다. 동일 상태를 한 번 읽는 구조 개선으로 채택했다.
3. **기존 배치 집계 수정:** `CanAdd`와 `Add`가 record의 기존 payload 크기를 사용한다.
   Zero-copy record의 materialized parts가 비어 있을 때 바이트가 0으로 집계되던 B 결함을 수정했다.
   수신을 기다리거나 첫 메시지 처리를 미루는 수집 단계는 없다. 기존 최대 64건 drain도 바꾸지 않았다.
   한도에서 남은 record는 원래 claim 아래 다음 drain으로 넘긴다. 새 바이트 한도 회귀 테스트는
   수정 전 실패했고 수정 후 통과했다. 처리량 증가 여부와 무관하게 정합성 수정으로 채택했다.
4. **Wire 재사용:** writer에 전달된 최종 frame 길이에서 payload 길이를 구하고,
   native part의 span을 한 번 얻어 길이와 복사에 사용한다. Format·필수 payload 복사는 유지한다.
   Wire 단독 4,096 B encode 시간은 237.39 → 705.02 ns로 오히려 증가했다.
   따라서 표의 처리량 증가 전부를 이 변경의 효과로 단정하지 않는다. 중복 순회·조회 제거로 채택했다.
5. **Codec·envelope 정책 통합:** Protobuf의 native/managed 출력 경로에서 반복된 payload 검사를
   codec 내부 한 함수로 모으고, envelope의 Message/view 경로가 같은 수신 serializer 선택 함수를 사용한다.
   기본 typed JSON·등록된 Protobuf·오류 의미·소유권은 유지한다. 두 변경은 한 단계로 측정했으므로
   각각의 독립 비용은 이 표로 분리할 수 없다. 세 패턴 모두 감소했으며 성능 개선으로 채택한 것이 아니다.
   중복 정책 제거의 구조 개선으로 채택했다. 10~15% 이내 오버헤드 목표 달성을 주장하지 않는다.

기존 큐 backlog 단독 측정은 live receive 없이 dummy record만 사용했다. 1건 turn은 576 B/record,
이미 64건이 있는 turn은 363.375 B/record였다. 이는 기존 drain의 호출 비용 분산을 보여 주며,
새 수신 batching의 효과나 실제 벤치 처리량 개선으로 환산하지 않는다.

소유 계층은 Framework state lane·mailbox·payload wire·codec이다. 유지한 계약은
`06-state-ownership-and-lanes` §4 C2·§5, `05-payload-ownership-and-codec` §4·§7,
`06-wire-protocol` §2의 Framework multipart profile이다. 변경 분류는 모두 B다.
Java mailbox는 synchronized 경로로 .NET callback 캡처 비용이 없으며, 같은 record 크기로 검사·집계한다.
Java wire writer도 계산한 multipart 길이를 재사용한다. Java의 수신 serializer 선택은 한 함수가 소유한다.
Protobuf 출력 API는 언어별 구조가 달라 두 출력 경로의 중복 검사는 .NET 내부에서만 통합했다.
수정 전/후 규칙 수: lane 직렬화 1 → 1, 소유권 값 조회 2 → 1,
바이트 크기 산정 기준 2 → 1, Protobuf payload 검사 2 → 1, 수신 serializer 선택 2 → 1.
새 수신 대기·상태·cache·pool·feature option은 추가하지 않았다. Core와 binding도 변경하지 않았다.

원자료는 `.artifacts/mailbox-staged-optimization/` 아래
`static-callback`·`ownership-lookup`·`batch-accounting`·`wire-length`·`codec-envelope-policy`의
`summary.json`, 패턴별 `results.json`, fidelity·DLL hash 및 단독 비용 JSON이다.
최종 관련 테스트 113개가 통과했다. `.artifacts`의 진단 DLL로 전체 테스트를 실행한 시도는
상위 경로에 `Zlink.Framework.sln`이 없어 golden fixture 탐색에 실패해 중단했다.
정상 프로젝트 경로에서 다시 실행한 .NET 전체 단위 테스트 2,276개는 실패·skip 없이 통과했다(3분 13초).
결과는 `codec-envelope-policy/unit-tests-normal-layout.log`에 보관한다. 문서 독립 2축 리뷰에서 발견된
과거 production 결과의 현재성 표현은 원자료 확인 후 당시 결과로 수정했다.

새 최적화 단계의 재현은 미리 빌드한 두 driver와 저장된 이전 결과를 사용한다.
다음 실행의 `OUTPUT`은 기존 완료 결과와 다른 새 경로여야 한다.

```bash
bash scripts/perf/perf-ticket.sh submit -p 2 -o codex -d 'cumulative optimization once' -- \
  bash framework/bench/grpc/dotnet/Diagnostics/run_optimization.sh \
  BEFORE_DRIVER BEFORE_RESULTS AFTER_DRIVER OUTPUT LABEL
```

### 반복 최적화 2차 — 진행 중

이 비교도 Codec + envelope + wire + mailbox를 유지한다. 각 셀은
`message/s (직전 행 대비 감소율)`이다. 새 단계·패턴만 1회 측정했고 이전 수치는 저장값을 재사용했다.

| 누적 단계 | request-serial | request-backpressure | send-saturation |
| --- | ---: | ---: | ---: |
| 1차 정책 통합 기준 | 5,905.2 | 154,911.9 | 366,582.9 |
| + Deadline 단일 포맷 | 6,247.0 (−5.8%) | 155,466.1 (−0.4%) | 401,952.5 (−9.6%) |
| + Queue payload 크기 단일 저장 | 6,383.6 (−2.2%) | 147,324.9 (5.2%) | 376,664.1 (6.3%) |
| + Protobuf factory 생성 시 타입 검증 | 6,372.9 (0.2%) | 151,746.3 (−3.0%) | 373,098.4 (0.9%) |
| + Multipart 검증 통합 | 6,234.6 (2.2%) | 150,525.9 (0.8%) | 354,729.1 (4.9%) |
| + Span escaping·Codec null 오류 보완 | 6,602.3 (−5.9%) | 156,807.0 (−4.2%) | 403,548.4 (−13.8%) |

원자료는 `.artifacts/mailbox-staged-optimization/`의 `envelope-deadline`·`mailbox-size-owner`·
`codec-factory-validation`·`wire-validation`이다. 모든 세 패턴은 errors·abandoned 0,
drain bound 미도달 및 제출·수신·request 완료 수 일치를 통과했다.

**단계별 판단과 계약 검토**

- Deadline: `EncodePlannedHeader`가 같은 35-byte stack buffer의 포맷 결과를 길이 계산·쓰기에
  재사용한다. JSON 날짜 형식·fraction trimming·offset·null은 유지한다. 헤더 단독 request encode는
  318.465 → 318.820 ns, 88.004 → 88.004 B로 개선을 확인하지 못했다. 이 변경의 효과로 send 처리량
  증가 전부를 설명하지 않는다. 중복 포맷 제거로 채택했다. 관련 테스트 103개 통과.
- Mailbox: `_payloadBytes` 복사본을 제거하고 생성 시 정규화한 읽기 전용 owned record의
  `ApplicationPayloadBytes`를 사용한다. Queued record 할당을 포함한 mailbox 단독 비교는
  576 → 568 B/record였다. Explicit 크기 우선순위·HWM 계상·claim·FIFO·소유권은 유지했다.
  Source record 또는 반환된 copy를 바꿔도 queued size가 바뀌지 않는 회귀 테스트를 추가했다.
  관련 테스트 44개 통과. 큰 `MeshReceiveRecord`의 종류별 전면 분리는 이번 단계에서 하지 않았다.
- Codec: 새 cache를 추가하지 않고, 기존 factory cache에 저장하기 전에 IMessage 타입·생성자를
  검증한다. Cache hit의 반복 assignability 검사를 줄인다. 관련 테스트 82개 통과.
  이후 계약 회귀 검토에서 null 타입의 오류가 dictionary 오류로 달라질 수 있음을 발견했다.
  다음 단계에서 캐시 접근 전 기존 `InvalidOperationException`과 message를 유지하고, managed/span
  두 경로의 null 회귀 테스트를 추가한다. 이 보완 전의 측정값을 보완 후 측정값으로 바꾸어 쓰지 않는다.
- Wire: native materialization과 borrowed view가 같은 multipart count·경계·trailing-byte 검증 함수를
  사용한다. 기존 각 경로의 최대 count와 검증 후 materialization 순서를 유지한다.
  Managed/native/view 세 경로에 zero count·oversized count·oversized part·trailing-byte 회귀 테스트를
  추가했다. 관련 테스트 86개 통과. 처리량은 세 패턴에서 감소했으며, 검증 규칙 중복 제거로 채택했다.

소유 계층은 Framework codec·envelope·mailbox·application wire다. 유지한 계약은
`05-payload-ownership-and-codec` §4·§7, `06-state-ownership-and-lanes` §4·§5,
`06-wire-protocol` §2다. 변경 분류는 B다. 기본 typed JSON 경로·Protobuf 등록 계약은 유지한다.
새 runtime option·offset cache·pool·수신 지연·Core/binding 보상 경로는 추가하지 않았다.
수정 전/후 규칙 수: deadline 포맷 2 → 1, queued payload 크기 저장 2 → 1,
multipart 검증 루프 2 → 1. 타입 검증은 기존 factory cache의 생성 경계로 이동했다.

**다른 언어에 적용할 기준**

1. 같은 결과가 메시지마다 반복 계산되는지 먼저 확인하고, 기존 소유 모듈 안에서 한 번 계산한다.
   Java는 헤더 writer 한 회전에서 문자열·deadline을 쓰므로 .NET의 2-pass deadline 개선을 그대로
   이식하지 않는다. 적용 가능성은 cache·serializer 이름이 아니라 실제 반복 비용으로 판단한다.
2. Queue가 크기 값을 복사해 갖는다면 queue-owned immutable record를 정규화해 한 곳으로 모은다.
   Java mailbox의 `Record.retainedBytes()`는 별도 wrapper payload copy가 없는 다른 구조다.
   Queue pressure byte와 Core physical HWM byte는 서로 다른 단위이므로 둘을 합치지 않는다.
3. Factory/parser cache는 검증된 타입·생성자만 담아야 한다. Java Protobuf는 parser를 매번 조회하며
   타입을 검사하므로 .NET의 cache-hit 검사 제거를 그대로 적용할 수 없다. Null·미지원 타입·생성자
   없음·잘못된 payload의 기존 오류 의미와 성공 경로를 함께 검증한다.
4. Materialized/native/view 경로의 같은 wire 검증은 wire codec 한 곳이 소유한다.
   Java는 이미 `decodeMultipartView`를 공유한다. Byte order·count 한도·부분 길이·trailing bytes와
   malformed 입력의 owner 정리는 golden 및 부정 입력으로 검증한다.
5. 성능 증가뿐 아니라 할당 감소·중복 규칙 제거도 채택 근거로 기록하되, 처리량 감소를 숨기지 않는다.
   새로운 상태·cache·retry·timeout·Core 보상 경로가 필요한 후보는 채택 전에 계약 소유자를 확인한다.

Span escaping·Codec null 오류 보완은 측정과 관련 테스트 136개를 통과했다.
문자열은 기존 `JavaScriptEncoder.Default.Encode` span API가 256-char stack scratch에 쓰고,
codec이 UTF-8으로 최종 native header에 쓴다. 별도 ASCII escaping 규칙·unsafe 코드·pool은 없다.
경계 255/256/257·511/512/513, 4,096-char prefix, 한글·emoji·잘못된 surrogate를 기존 JSON bytes와
비교했다. Request header 단독 encode는 302.045 → 305.965 ns, response는 296.445 → 284.815 ns,
할당은 모두 88.004 B로 유지됐다. 전체 처리량 증가 전부를 이 변경으로 설명하지 않는다.
Protobuf factory 전후 비교는 같은 새 실행 파일에 이전 Framework·Protobuf codec DLL을 넣은
shadow driver를 사용했다. 1,000,000회·10개 순환 block에서 codec decode는
64 B: 61.686 → 60.156 ns, 4,096 B: 290.447 → 270.833 ns였다.
할당은 각각 152 B와 4,184 B로 동일했다. 이 할당에는 Protobuf payload 역직렬화가 포함되며
Framework wrapper만의 할당량으로 해석하지 않는다. 결과는 `envelope-span-encoder/`의
`codec-before.json`·`codec-after.json`·`envelope-before.json`·`envelope-after.json`에 보관한다.
Null 타입 보완 후 두 serializer 입력 경로의 기존 오류가 유지됐으며 새로운 spec 계약은 만들지 않았다.

이후 필요한 수신/reply 소유권, host permit, worker 인계, handler dispatch·DI,
cancellation/deadline 및 실제 route/lifecycle 경계를 순서대로 검토한다.
Worker primitive 진단을 production 전체 dispatcher의 성능으로 표기하지 않는다.
각 기능을 추가한 뒤 해당 경계의 성능·POSDDD·spec 회귀를 검토하고 다음 기능으로 넘어간다.

### 남은 기능의 누적 추가 — 진행 중

기존 `MessagingFixedCodec=mailbox` 빌드에 test-only `MessagingFixedRuntimeLevel`을 적용한다.
Level 0은 이전 mailbox 단계, 1은 독립 Received/reply context, 2는 host permit,
3은 router별 지속 worker 하나와 수신 작업 사이의 mailbox 전달이다.
추가 기능은 compile-time으로 결정하며 환경변수로 runtime 기능을 켜거나 끄지 않는다.
실행 파일의 `--fixed-codec-info`에서 freshReceived·permitBatchSize·workers를 확인한다.

| 누적 단계 | request-serial | request-backpressure | send-saturation |
| --- | ---: | ---: | ---: |
| Span escaping 이후 기준 | 6,602.3 | 156,807.0 | 403,548.4 |
| + 독립 수신/reply 소유권 | 6,330.4 (4.1%) | 166,869.9 (−6.4%) | 388,130.8 (3.8%) |
| + Host permit | 6,892.3 (−8.9%) | 154,395.3 (7.5%) | 386,510.0 (0.4%) |
| Permit static callback 최적화 | 6,752.2 (2.0%) | 164,184.7 (−6.3%) | 417,300.3 (−8.0%) |
| + Worker 전달 — 원인 조사 중 | 5,898.4 (12.6%) | 16,299.4 (90.1%) | 314,568.9 (24.6%) |
| 전용 수신 작업 분리 — 진단, 채택 보류 | 5,717.6 (3.1%) | 17,444.2 (−7.0%) | 266,625.7 (15.2%) |

수신/reply 소유권 단계는 수신마다 `Received.Create()`를 사용한다. Request reply callback은
기존 `MeshReceiveRecord`에 보관하고, reply parts는 그 callback을 통해 기존 binding operation으로
한 번 제출한다. 새로운 retry·connection 선택·reply 주소 추적은 없다. Payload는 원래 Received가
소유하고 queue → batch → 처리 완료의 기존 경로에서 정리된다. Worker 이전에 이를 측정했으므로
이후 worker 손실에 이 준비 단계의 비용을 섞지 않는다. 관련 테스트 69개와 세 live 패턴의
errors·abandoned 0, drain·count 조건을 통과했다. 원자료는 `receive-reply-owner/`다.
수신 클래스 재사용은 inline diagnostic의 기준이며, 서로 다른 대기 중 메시지에 같은 mutable
수신 객체를 공유하는 최적화는 적용하지 않는다.

Host permit 단계는 기존 host-shared `ZLinkApplicationJobQueue`에서
receive 전에 예약하고, `ZLinkApplicationJobQueueRecordOwner`가 payload와 admission을 큐로 이전한다.
`TryClaim`은 admission 포함 여부를 확인한다. 기존 invocation scope에서 body 역직렬화 후,
실제 metrics/handler callback 시작 직전에 permit을 반환한다. Malformed·NoData·terminal cleanup도
기존 lease의 idempotent 반환 경로를 쓴다. 별도 byte pressure·quota·retry는 만들지 않는다.
계약 기준은 `04-application-job-queue-and-backpressure` §1·§3 및
`08-messaging-hot-path` §4이며, production 전체 dispatcher 검증과 이 primitive 진단을 구분한다.

Permit 최적화는 기존 typed-state lane에 static callback을 전달하여 호출부의 closure를 줄인다.
동일 실행 파일을 사용한 이전/현재 runtime 비교에서 permit 예약 → queued 전환 → handler 시작 반환의
동기 처리 비용은 744→456 B/turn, 424.9→366.6 ns/turn이다. FIFO·취소·batch 반환을 포함한
관련 테스트 81개가 통과했다. 원자료는 `host-permit/`, `host-permit-static/`에 있다.
이 할당 감소를 세 패턴의 처리량 변화 전체에 대한 인과관계로 해석하지 않는다.

Worker 단계는 이미 준비된 record만 기존 mailbox에서 꺼내며 수신 수집을 위한 지연을 추가하지 않는다.
관련 테스트 60개와 세 패턴의 오류·누락·drain 검사를 통과했지만, backpressure 처리량이 크게
하락하여 다음 기능 추가를 보류한다. 원자료는 `worker-handoff/summary.json`과 패턴별
`fixed-worker-handoff/*/results.json`이다. 이 결과는 진단용 worker primitive의 비용이며
실제 Framework 전체 dispatch 비용이나 네 언어의 공통 worker 비용으로 일반화하지 않는다.
이 진단 코드를 production worker 구현으로 이식하지 않는다. 종료 시 producer·worker를 모두
정리한 뒤 mailbox와 host queue를 해제해야 하며, permit은 body 해석 후 handler 시작 전에 반환한다.

전용 수신 작업 분리는 worker 기능·native/binding/Framework/codec DLL을 그대로 유지하고,
producer 실행과 permit 완료 대기만 compile-time으로 변경한 원인 분리다. Pending permit을
비동기로 기다린 뒤 ThreadPool로 이동하지 않도록 전용 작업 안에서 완료까지 기다린다.
관련 unit 테스트 81개와 실제 socket의 idle 취소·malformed wire·worker decode 오류 검증 3개가
통과했다. 오류 검증은 정확한 예외 종류를 확인하고 모든 worker/producer 종료와 permit·pending byte
반환을 검사한다. 세 live 패턴도 오류·누락·drain 검사를 통과했다.

이 마지막 비교는 설정 변경 요청 당시 이미 실행 중이어서 warmup 3초·active 10초로 완료했다.
Backpressure 평균 지연은 약 834 ms, p99는 약 1,064 ms다. 전용 producer만으로 큰 손실이
회복되지 않았으므로 이 변경을 성능 해결책으로 채택하지 않는다. Worker의 동기 lane
대기와 scheduler 의존성을 다음 조사 대상으로 남긴다. 원자료는
`worker-dedicated-ingress/summary.json`, `fixed-worker-dedicated-ingress/*/results.json` 및
`build.log`, `shared-build.log`, `tests.log`, `lifecycle.log`다. 실제 production ingress의 I0 계약을
모두 구현한 변경으로 해석하지 않는다.

| 마지막 진단 패턴 | Bandwidth MB/s | Source CPU % | Target CPU % | Source memory MB | Target memory MB |
| --- | ---: | ---: | ---: | ---: | ---: |
| request-serial | 23.42 | 3.28 | 4.06 | 122.96 | 474.38 |
| request-backpressure | 71.45 | 5.31 | 2.02 | 190.29 | 542.58 |
| send-saturation | 1,092.10 | 20.89 | 14.63 | 132.36 | 599.32 |

값은 해당 `results.json`의 CPU·memory·bandwidth 필드다. Bandwidth는 처리량 × 4,096 / 1,000,000인
application payload 기준이며, request에서는 응답 4 KB, send에서는 전송 4 KB만 센다.
64 B 요청과 wire header·transport overhead를 포함한 실제 NIC 전송량이 아니다.
CPU %는 전체 논리 CPU 용량 기준이며 memory는 report의 프로세스 측정값이지 메시지당 할당량이 아니다.

### 네 언어 단계 정렬

.NET의 검증된 단계별 원자료는 재사용한다. C++·Java·Kotlin은 기존 benchmark의 요청/응답 크기,
codec 및 처리 단계를 먼저 대조하고 같은 조건의 최소 경로부터 측정한다. 기존 실행 조건이
64 B 요청/4 KB 응답과 다르면 그 결과를 정렬된 기준으로 사용하지 않는다.
언어별 구현과 관련 테스트는 독립 범위에서 진행하되 성능 측정은 빌드·테스트와 분리한다.
공통 적용 대상은 소유권·계약·처리 순서이며, 언어별 callback·GC·binding 비용은 별도로 검증한다.

| 언어 | 정렬 상태 | 기존 결과 사용 범위 |
| --- | --- | --- |
| .NET | Codec·Envelope·Wire·Mailbox·permit 실측 보존, worker 대기 원인 분리 중 | 위 각 누적 단계의 조건에 한정 |
| C++ | Core-like·Codec부터 test-only 단계 진단 준비 | 기존 unified benchmark는 4 KB 요청/4 KB 응답이므로 64 B 요청 기준으로 재사용 불가 |
| Java | Core-like·Codec 진단 범위 확정 중 | 기존 echo는 요청과 응답 크기가 같아 정렬 기준으로 재사용 불가 |
| Kotlin | 별도 Kotlin 진단 source와 공유 JVM codec target 준비 범위 확정 중 | 기존 보조 셀은 request-window 전용이며 source의 1024/runner의 4096 조건이 불일치하여 세 패턴 기준으로 재사용 불가 |

C++의 크기 차이는 `cpp/client/bench_cpp_client.cpp`의 `framework_driver_t` reply 크기 검증과
`cpp/framework/bench_framework_cpp_server.cpp`의 echo handler에서 확인한다. Native buffer에
protobuf를 바로 쓰는 C++ codec은 이미 중간 managed byte 배열을 만들지 않는다. .NET의
callback closure 제거를 C++에 그대로 적용하는 대신 각 언어의 실제 소유 buffer와 실행 방식을 확인한다.

Kotlin public facade는 Java RouteClient와 codec runtime을 사용한다. 따라서 codec 선택·직렬화
최적화의 소유자는 공유 Java module이다. Kotlin 진단 source의 실측과 Java source 실측은 따로
남기되, 이를 Kotlin 전용 codec runtime의 비용으로 설명하지 않는다. 기존 Kotlin 보조 실행기는
Java target을 사용하며 1024/4096 설정도 혼재하므로 새 단계별 실측과 구분한다.

### 이전 production Framework / ZLink Core 측정

이 표는 위 누적 최적화 이전에 production `zlink-dotnet`(Core raw benchmark)과
`zlink-framework-dotnet`을 build해 같은 패턴·payload·warmup 3초·active 10초로
각각 1회 실행한 저장된 결과다. 최신 최적화가 반영된 production 실행 결과가 아니다.
각 수치는 단일 실행이며, wire 단계의 누적 비용과 곱하거나 합치지 않는다.

| 패턴 | ZLink Core | 당시 production Framework | Framework / Core | 판정 |
| --- | ---: | ---: | ---: | --- |
| request-serial | 7,422.3 | 4,338.5 | 58.5% | 유효 |
| request-backpressure | 211,141.3 | — | — | 무효 |
| send-saturation | 397,862.0 | 174,072.2 | 43.8% | 유효 |

Backpressure Framework 실행은 319,667 client error가 발생해 처리량을 비교하지 않았다.
오류는 `target route is not connected` / `NotConnected` / `Route channel 'bench' is not connected`이며,
제출 415,012건 중 completed 95,345건, target 수신 95,360건이었다.
Core와 serial·send Framework 실행은 오류 0, abandoned 0, drain 조건 통과였다.
따라서 이 실행은 당시 production Framework / Core 비율을 serial 58.5%, send 43.8%로 보여 주지만,
backpressure는 성능 수치가 아니라 route 연결 실패를 먼저 분리해야 한다.
이 오류와 diagnostic wire 추가 비용의 인과관계는 이 결과만으로 주장하지 않는다.

## Wire 기준 처리량 — 별도 진단 비교

| 단계 | request-serial | request-backpressure | send-saturation |
| --- | ---: | ---: | ---: |
| Core-like direct | 7,751.0 | 211,465.1 | 383,802.3 |
| Wire/header | 6,964.3 | 178,452.8 | 408,630.1 |
| Wire / Core-like | 89.9% | 84.4% | 106.5% |

단위는 message/s다. Send의 106.5%는 단일 실행 결과이며,
wire 처리가 음수 비용이거나 전체 Framework가 Core보다 빠르다는 근거가 아니다.

## Wire 이후 기능별 독립 비교

각 기능은 wire/header 기준에 **하나만** 추가했다. 서로 누적한 결과가 아니다.
Mailbox과 permit 비교는 같은 빌드로 기준부터 새로 측정했으므로 위 표의 wire 값과 다르다.

| 추가 기능 | 패턴 | Wire 기준 | 기능 추가 | 처리량 손실 | 서버 CPU 비용: 기준 → 추가 |
| --- | --- | ---: | ---: | ---: | ---: |
| 메시지마다 `Task.Yield` | request-serial | 6,964.3 | 6,811.5 | 2.2% | 50.97 → 94.69 |
| 메시지마다 `Task.Yield` | request-backpressure | 178,452.8 | 174,796.5 | 2.0% | 10.98 → 16.81 |
| 메시지마다 `Task.Yield` | send-saturation | 408,630.1 | 419,635.4 | −2.7% | 4.15 → 6.73 |
| Mailbox queue/claim | request-serial | 6,913.1 | 6,504.0 | 5.9% | 51.79 → 57.96 |
| Mailbox queue/claim | request-backpressure | 183,352.0 | 167,403.4 | 8.7% | 10.81 → 11.79 |
| Mailbox queue/claim | send-saturation | 415,173.7 | 418,134.5 | −0.7% | 4.24 → 4.67 |
| Application job permit | request-serial | 6,263.6 | 6,311.2 | −0.8% | 57.63 → 55.77 |
| Application job permit | request-backpressure | 172,766.4 | 163,431.6 | 5.4% | 11.78 → 12.94 |
| Application job permit | send-saturation | 377,031.9 | 383,976.0 | −1.8% | 5.10 → 4.98 |

Mailbox은 실제 `ZLinkMeshNodeOwnedMailbox`, `ZLinkMeshQueuedRecord`,
`MeshReceiveBatch`를 사용한다. 수신 envelope의 소유권도 batch로 넘긴다.
동일 수신 스레드에서 enqueue → claim → drain → release를 실행하며,
peer identity/generation은 진단용 고정값이다. Worker 인계, peer 검증,
permit, handler DI는 포함하지 않는다. 따라서 이 수치를 전체 mailbox dispatch 비용으로 부르면 안 된다.
`FixedRecordBytes = 256`은 메모리 제한의 회계 상수이지 추가 256 B payload 할당이 아니다.

Permit은 실제 host-shared `ZLinkApplicationJobQueue`의 메시지별 예약,
queued 표시, handler 시작 시 반납을 같은 수신 루프에서 실행한다.
Mailbox, worker 인계, handler DI는 제외했으며 capacity 소진 후 대기 비용은 측정하지 않았다.
동일 스레드에서 추가한 기능 중 가장 큰 양의 처리량 손실은 mailbox의 backpressure 8.7%였다.
이는 메시지당 CPU 증가가 가장 큰 재스케줄링과 다른 순위다.

## 재스케줄링의 CPU 비용과 최적화 비교

매 메시지의 `Task.Yield`는 메시지당 서버 CPU 비용을 늘린다.
이를 제거하고 동일 수신 루프를 유지한 비교의 CPU 비용 감소는
serial 46.2%, backpressure 34.7%, send 38.3%다.
반면 request 처리량 차이는 약 2%였다. **불필요한 재스케줄링은 CPU 낭비를 설명하지만,
전체 Framework의 큰 처리량 격차를 단독으로 설명하지 못한다.**

전체 Framework backpressure의 별도 native CPU profile은 다음 sample 비율을 보여 준다.
유효한 실행에서 Core 216,061.2 message/s, Framework 80,547.9 message/s였으며,
`LowLevelSpinWaiter.Wait`를 포함한 stack은 Framework 서버 sample의 11.3%,
클라이언트 sample의 14.7%였다. 일부 native/JIT stack은 미해석이므로
모든 비용의 순위를 완전히 확인한 profile은 아니다.
이 profile만으로 해당 stack과 `Task.Yield`의 인과관계나
yield 제거의 production 처리량 효과를 입증할 수는 없다.

재스케줄링을 64개마다 하도록 변경한 진단 실행은 request-serial만 통과했다.
Backpressure는 `ZlinkConfigException`, error code 704 (`InternalError`)로 실패했고
send는 실행하지 않았다. 이 대안은 채택하지 않았다. 재시도나 timeout 증가로 실패를 숨기지 않았다.

## Worker 인계와 대기 구간

메시지 소유권 할당과 스레드 인계를 분리하기 위해 동일 스레드 기준에서도
메시지별 `Received`를 생성했다. 같은 빌드에서 재사용 기준 대비 처리량 차이는
serial +3.3%, backpressure −0.5%, send −0.5%였다.
이 단일 실행에서는 수신 객체 할당만으로 큰 격차를 설명할 수 없다.

인계 버전은 실제 mailbox의 empty→ready 통지로 `SemaphoreSlim`을 깨우고,
지속 worker가 claim 뒤 최대 64개를 drain해서 동일 decode·encode·reply 함수를 실행한다.
수신 객체는 worker에서 해제한다. Worker의 수는 측정 중인 socket당 20개다.
Production 전체 `ZLinkMeshDispatchPump`가 아니라 **인계 primitive의 진단 경로**이며,
mesh-ready owner 선택, peer 검증, application permit, DI는 포함하지 않는다.

다음 비교는 양쪽 모두 public poller에서 readiness를 기다리고
`Recv(DontWait)`를 실행한다. 따라서 blocking receive를 그대로 유지한 비교와 구분한다.

| 패턴 | 동일 스레드·메시지별 소유권 | Worker 인계 | 처리량 손실 | 서버 CPU 비용: 기준 → 인계 |
| --- | ---: | ---: | ---: | ---: |
| request-serial | 6,810.5 | 5,768.6 | 15.3% | 52.42 → 137.99 |
| request-backpressure | 156,554.1 | 16,564.1 | 89.4% | 13.14 → 20.41 |
| send-saturation | 389,103.3 | 397,361.0 | −2.1% | 5.07 → 10.25 |

Backpressure에서는 인계 버전의 서버 CPU 비율이 1.69%로 낮았고,
peak in-flight는 16,385였다. 별도의 blocking receive 인계 비교에서도
worker를 20개에서 1개로 줄였을 때 처리량은 16,127.8 → 16,030.0으로 회복되지 않았다.
Worker 수를 줄이는 방안은 이 backpressure 저하의 해결책으로 채택하지 않는다.

### 진단용 구간 시간

Readiness 기반 인계 경로에만 시간 측정을 켠 별도 backpressure 실행에서
active 중 약 2초 간격의 snapshot 차이를 구했다.
성능 비교 표에는 시간 측정이 꺼진 실행을 사용했다.

| 완료된 구간 | 호출 수 | 경과 시간 합계 | 호출당 경과 시간 |
| --- | ---: | ---: | ---: |
| Request socket readiness wait | 16,557 | 1,017.260 ms | 61.440 µs |
| Public `Recv(DontWait)` | 16,557 | 31.212 ms | 1.885 µs |
| Mailbox enqueue | 16,557 | 8.274 ms | 0.500 µs |
| Mailbox claim | 262 | 0.156 ms | 0.597 µs |
| Reply 제출 | 16,670 | 46.749 ms | 2.804 µs |

각 구간은 경과 시간이며 CPU 시간이 아니다. 생산자와 소비자는 동시에 실행하므로
합계를 전체 실행 시간으로 더하지 않는다. Snapshot 시점에 끝나지 않은 호출은 포함하지 않아
긴 wait의 시간에는 경계 오차가 있다. Drain·decode·encode·release와 runnable 대기는
이 구간 표에서 따로 측정하지 않았다. 사용하지 않는 command socket의 idle wait 합계
2,002.098 ms는 request socket 수치와 분리했다.

Mailbox의 queued-byte snapshot은 17,388 B → 0 B였다.
이 시점 값만으로 전체 구간의 queue 상태를 판단할 수는 없다.
다만 측정한 enqueue·claim·reply 호출 자체가 항상 수백 ms를 소비한 결과는 아니다.
**큰 처리량 손실을 그대로 메모리 할당이나 스레드 전환의 계산 비용으로 해석하지 않는다.**
특히 이 경로에는 실제 admission과 mesh-ready 제어가 빠져 있으므로,
89.4%를 production Framework의 worker 오버헤드로 채택하지 않는다.
요청이 대기하는 동안 ingress 진행이 낮아지는 원인은 아직 미확정이다.

## Production 경로의 CPU와 scheduler 기록

다음은 진단 인계 경로가 아니라 기존 `zlink-dotnet`과
`zlink-framework-dotnet` 실행 파일로 측정한 결과다. 패턴별 1회,
warmup 3초·active 10초이며 active 중 2초 동안 native scheduler와 switch stack을 기록했다.
서버·클라이언트의 Framework DLL 해시는 진단 빌드의 DLL과 일치했다.
Profiler 파일은 repository 밖의 `/var/tmp/`에 두었다.

| 패턴 | Core | Framework | Framework / Core |
| --- | ---: | ---: | ---: |
| request-serial | 7,374.1 | 4,500.7 | 61.0% |
| request-backpressure | 226,153.0 | 83,831.1 | 37.1% |
| send-saturation | 443,478.8 | 199,104.8 | 44.9% |

모든 실행은 오류 0과 drain 유효성 조건을 통과했다.
이는 profiler가 붙은 실행의 처리량이며 최적화 전후의 개선율 표가 아니다.
전체 Framework는 아직 Core 대비 10~15% 손실 목표를 만족하지 않는다.

다음 표는 서버 `.NET TP Worker` 스레드의 **합계**다.
각 trace는 약 2초이며 시작·끝에 걸친 미완료 구간은 제외한다.
Scheduler record 손실과 음수 구간은 보고되지 않았다.

| 패턴 | CPU 실행 시간: Core → Framework | Runnable 대기: Core → Framework | 실행 구간 수: Core → Framework |
| --- | ---: | ---: | ---: |
| request-serial | 322.202 → 1,248.244 ms | 14.854 → 36.363 ms | 13,472 → 28,182 |
| request-backpressure | 1,859.411 → 4,147.488 ms | 6.355 → 181.098 ms | 4,393 → 43,591 |
| send-saturation | 1,299.042 → 5,695.322 ms | 41.953 → 125.715 ms | 33,878 → 25,312 |

CPU 실행 시간은 스레드가 CPU에서 실행된 구간의 합이다. Runnable 대기는
실행 준비가 된 뒤 실제 CPU를 배정받기까지의 합이며 I/O 수신 대기와 다르다.
여러 스레드의 시간은 겹치므로 합계가 2초를 넘을 수 있다.
실행 구간 수는 메시지 수와 일대일 대응하지 않는다.

Backpressure에는 runnable 지연과 실행 구간 증가가 실제로 있지만,
이 기록만으로 context switching이 전체 처리량 격차의 주원인이라고 단정하지 않는다.
Framework의 CPU 실행 시간 증가도 별도로 확인되므로 실행 작업량과 대기를 함께 분리해야 한다.
Idle worker의 sleep 합계를 메시지별 불필요한 대기 시간으로 해석하지 않는다.

Switch stack 기록에는 backpressure Core 28.02%, Framework 32.46%,
Framework send 8.47%의 sample 손실이 보고됐다.
따라서 해당 stack의 함수 비중은 비용 분류 표에 채택하지 않았다.
손실이 보고되지 않은 별도 scheduler record와 구분한다.

### 연결된 idle socket의 readiness 검증

Public API로 DEALER→ROUTER 연결에서 DATA 한 건을 소비한 뒤 idle 상태를 검증했다.
Mask별 64회, wait timeout 5 ms이며 결과는 다음과 같다.

| Mask | Timeout 수 | 경과 시간 |
| --- | ---: | ---: |
| PollIn·PollErr | 64 | 326.344 ms |
| PollIn·PollErr·PollCompletion | 64 | 325.616 ms |
| PollIn·PollErr·PollCompletion·PollOut | 64 | 325.682 ms |

이 fixture에서 `PollOut` 등록만으로 idle busy loop가 생긴다는 가설은 지지되지 않았다.
이는 DEALER peer를 사용한 결과이며 전체 Framework의 ROUTER peer·lifecycle 동작을
동일하게 검증한 결과는 아니다.

## Runtime 변경 전 진단

- 원인 후보: `ZLinkManagedMeshNode.cs:362`의 `LongRunning` 시작 작업과
  `:4980`의 async 수신 루프, `:5021`의 매 수신 turn `Task.Yield` 조합.
  최초 yield 뒤에는 continuation이 ThreadPool에서 실행되어 전용 blocking 수신 스레드가 유지되지 않는다.
- 소유 계층과 계약: Framework mesh ingress 실행 모델.
  [Messaging hot path §4.1·§6](../../../../doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md)은
  thread 기반 runtime의 blocking readiness wait와 .NET 전용 수신 작업을 규정한다.
- 교차언어 대조: Java의 `ZLinkJavaRawMeshNode.java:4400`은
  단일 platform thread에서 blocking readiness와 ingress drain을 반복하며 매 turn 재스케줄링하지 않는다.
- 변경 분류: **B 후보 — 기존 실행 모델 결함**. 전용 blocking 수신 루프 유지가 더 단순한 대안이다.
  규칙은 현재의 전용 시작 작업 + ThreadPool continuation 두 실행 방식에서 전용 수신 방식 하나로 줄어든다.
  이 문서는 1단계 진단이며 production runtime 적용은 승인 후 별도 검증 대상이다.

## 미분리 기능

| 기능 | 상태 |
| --- | --- |
| Production mesh-ready → claim → dispatch | 전체 경로 미분리; 인계 primitive 결과와 구분 |
| Peer 분류·lifecycle 상태 검증 | 미측정 |
| Handler dispatch·DI scope | 미측정 |
| Request completion·timeout 관리 | 미측정 |
| Infrastructure·socket monitor 처리 | 미측정 |
| Flow tracing·metrics | 이 표에서 독립 비용 미측정 |

미측정 기능의 비용을 추정해서 표를 채우지 않는다. 전체 Framework와 wire 단계의
차이를 특정 기능의 오버헤드로 단정하지 않는다.

## 재현과 원자료

진단 빌드:

```bash
dotnet build framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj \
  -c Release -f net8.0 -m:1 -nr:false -p:UseSharedCompilation=false \
  -p:MessagingFeatureRamp=true \
  -p:OutputPath=/home/hep7/project/zlink/.artifacts/permit-feature-check/driver/
```

아래 환경변수 명령은 이전 독립 진단의 재현용이며, 현재 compile-time 누적 단계 측정과 구분한다.
저장소 root에서 실행한다. `INGRESS_YIELD_INTERVAL`을 0/1로 바꾸면
재스케줄링 비교, 그 값을 0으로 두고 `INLINE_MAILBOX`를 0/1로 바꾸면 mailbox 비교다.
두 옵션을 모두 0으로 두고 `PERMIT_BATCH_SIZE`를 0/1로 바꾸면 permit 비교다.
Worker 비교는 mailbox를 켜고 `BENCH_DIAGNOSTIC_FRESH_RECEIVED=1`,
`BENCH_DIAGNOSTIC_READINESS_WAIT=1`을 양쪽에 설정한 뒤
`BENCH_DIAGNOSTIC_MAILBOX_WORKERS`만 0/20으로 바꾼다.
시간 측정은 `BENCH_DIAGNOSTIC_WORKER_TIMING=1`로 켜며,
target의 `/bench/diagnostic-worker`에서 tick/count snapshot을 읽는다.
Idle readiness 검증은 같은 진단 DLL에 `--idle-readiness`를 전달한다.
다른 진단 옵션은 설정하지 않은 환경에서 실행하며 perf ticket으로 측정을 직렬화한다.

```bash
scripts/perf/perf-ticket.sh submit -p 2 -o codex -d 'isolated feature check' -- \
  env BENCH_DIAGNOSTIC_STAGE=wire BENCH_DIAGNOSTIC_DIRECT_BODY_OWNER=1 \
  BENCH_DIAGNOSTIC_INGRESS_YIELD_INTERVAL=0 BENCH_DIAGNOSTIC_INLINE_MAILBOX=1 \
  BENCH_DIAGNOSTIC_PERMIT_BATCH_SIZE=0 \
  BENCH_DIAGNOSTIC_SOURCE_DLL=/home/hep7/project/zlink/.artifacts/permit-feature-check/driver/Zlink.Framework.UnitTests.dll \
  BENCH_DIAGNOSTIC_TARGET_DLL=/home/hep7/project/zlink/.artifacts/permit-feature-check/driver/Zlink.Framework.UnitTests.dll \
  bash framework/bench/grpc/dotnet/run_local.sh --skip-build --scenario all \
  --implementation zlink-dotnet --duration-seconds 10 --warmup-seconds 3 \
  --output /home/hep7/project/zlink/.artifacts/isolated-feature-recheck
```

독립 측정 원자료는 `.artifacts/permit-feature-check/` 아래
`core-direct`, `wire`, `wire-yield-1`, `wire-yield-64`,
`wire-mailbox-base`, `wire-mailbox`, `wire-permit-base`, `wire-permit-1`의
패턴별 `results.json`과 실행 log다.
소유권·인계 비교는 같은 root의 `wire-owner-base`, `wire-owner`,
`wire-worker-20`, `wire-worker-1`, `wire-ready-owner`, `wire-ready-worker-20`이다.
최종 구간 시간은 `handoff-request-wait-timing.start.json`·`.end.json`과
`handoff-request-wait-timing/`의 실행 결과다.
Native profile은 `.artifacts/worker-cpu-check/` 아래 `.perf.data`, `.map`, `.resolved.txt`다.
Production scheduler 원자료는 `/var/tmp/zlink-production-waits.sElSkU/`의
패턴별 `results.json`, `.threads.tsv`, `.pid-map.tsv`, `.events.txt`,
`.sched-summary.txt`, `.stacks.data`, `.script.log`다.
Idle readiness 결과는 `.artifacts/permit-feature-check/idle-readiness.jsonl`이다.
누적 기능 추가의 기준 원자료는 `.artifacts/cumulative-restart-core/summary.json`,
`build.sha256`과 `binding-core`·`core` 아래 패턴별 `results.json` 및 실행 log다.
재현은 `Diagnostics/run_cumulative.sh --stage core --driver <진단 DLL> --output <새 경로>`다.
첫 기능 비교는 `--stage codec`이며 이전 `core`와 새 `codec`만 실행한다.
첫 기능 원자료는 `.artifacts/cumulative-restart-codec/summary.json`,
`build.sha256`과 `core`·`codec` 아래 패턴별 `results.json` 및 실행 log다.
Codec 원인 분리 원자료는 `.artifacts/codec-attribution/codec-cost.json`, `summary.json`,
`core`·`codec-encode`·`codec-decode`·`codec` 및 `tiering-disabled`의 실행 결과다.
Serial native profile 원자료는 `/var/tmp/zlink-codec-serial.jy07tt/`의
패턴별 결과, `.stat.csv`, `.sched-summary.txt`, `.native.log`, `.mapping.log`, `.script.log`다.
Compile-time 고정 비교는 `Diagnostics/run_fixed_codec.sh`로 재현한다.
원자료는 `.artifacts/fixed-codec-comparison/summary.json`, `build.sha256`,
`core.info.log`·`codec.info.log`, `.fidelity.json`, `core-disassembly.log`·`codec-disassembly.log`,
`fixed-core`·`fixed-codec`의 패턴별 실행 결과다.
고정 실행 파일의 build 조건은 unit test project의 `MessagingFeatureRamp=true`,
`MessagingFixedCodec=core|codec|envelope|wire|mailbox`이며 기존 Framework runtime 코드는 변경하지 않는다.
Codec + envelope 비교는 같은 runner에 `--feature envelope`를 지정한다.
원자료는 `.artifacts/fixed-envelope-comparison/summary.json`, `build.sha256`,
`codec.info.log`·`envelope.info.log`, `.fidelity.json`, `.body-fidelity.json`,
`codec-disassembly.log`·`envelope-disassembly.log`, `fixed-codec`·`fixed-envelope`의 패턴별 결과다.

```bash
bash scripts/perf/perf-ticket.sh submit -p 2 -o codex -d 'codec plus envelope only' -- \
  bash framework/bench/grpc/dotnet/Diagnostics/run_fixed_codec.sh --feature envelope
```

Wire 비교는 `--feature wire`로 실행한다. Production Core/Framework 비교가 필요할 때만
`--production-comparison`을 추가하며, 이는 fixed feature ladder와 별도 결과로 남긴다.
원자료는 `.artifacts/fixed-wire-production-comparison/summary.json`,
`production-core-framework-summary.json`, fixed build·fidelity·disassembly log와
`fixed-envelope`·`fixed-wire`·`production-zlink-dotnet`·`production-zlink-framework-dotnet`의 패턴별 결과다.

Mailbox는 `--feature mailbox --previous-output <저장된 wire 결과 경로>`로 실행한다.
`--previous-output`은 직전 실행 파일과 결과를 읽기만 하며 새 단계만 build·측정한다.
원자료는 `.artifacts/fixed-mailbox-comparison/summary.json`, `build.sha256`,
`wire.info.log`·`mailbox.info.log`, `.fidelity.json`·`.body-fidelity.json`,
`mailbox-build.log`와 `fixed-mailbox`의 패턴별 결과다.

```bash
bash scripts/perf/perf-ticket.sh submit -p 2 -o codex -d 'wire plus mailbox only' -- \
  bash framework/bench/grpc/dotnet/Diagnostics/run_fixed_codec.sh --feature mailbox \
  --previous-output /home/hep7/project/zlink/.artifacts/fixed-wire-production-comparison
```

이 artifact 경로는 로컬 결과이며 Git에 포함되지 않는다.
