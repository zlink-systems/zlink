# Raw Sample Policy

## 목적

이 문서는 `core/samples/`와 `bindings/*/samples/`에서 raw socket 공개 API를 설명하는
샘플의 공통 계약을 정의한다. 샘플은 사용자가 그대로 따라 할 수 있는 예제이자 반복
실행할 수 있는 smoke 검증이어야 한다.

이 문서의 범위는 Core가 제공하는 context, message, raw socket, monitor, poller와
generic timer로 한정한다. Application framework 샘플은 해당 framework 문서가 따로 정의한다.

## 1. 적용 범위

- `core/samples/`
- `bindings/<language>/samples/`
- 런타임을 공유하는 언어의 raw socket 샘플
- 공식 샘플 실행 스크립트와 샘플 전용 helper

공개 배포 패키지에 포함되는 샘플과 CI sample smoke는 이 문서를 필수로 따른다.
현재 샘플과 이 계약이 다르면 샘플을 수정한다.

## 2. 기본 원칙

- 샘플은 공개된 표준 API만 사용한다.
- Deprecated 표면, private symbol, generated 내부 type과 raw flag 우회를 사용하지 않는다.
- 실제 endpoint를 설정하고 message를 전송한 뒤 수신 결과를 검증한다.
- 역할, topology, 전송 값, 수신 값과 종료 결과가 출력에서 드러나야 한다.
- 같은 샘플은 모든 언어에서 같은 scenario, topic, payload, 순서와 출력 형식을 사용한다.
- 언어별 차이는 ownership, exception, awaitable과 naming과 같은 표현 방식에만 두는다.

## 3. 공식 샘플 목록

각 raw socket 표면은 지원하는 socket type에 맞춰 다음 샘플을 제공한다.

- `pair_recv_sample`
- `pubsub_recv_sample`
- `dealer_router_recv_sample`
- `stream_recv_sample`
- `stream_packet_callback_sample`
- `monitor_recv_sample`

Binding이 raw DEALER/ROUTER request wrapper를 공개하면
`request_reply_async_sample`도 제공한다. Core C API에 request wrapper가 없다면 `core/samples/`에
해당 샘플을 만들지 않는다.

공식 runner와 README는 이 목록에 있는 샘플만 onboarding 경로로 노출한다. Migration
또는 compatibility 목적의 임시 파일은 공식 샘플 수에 포함하지 않는다.

## 4. 파일과 수신 모델

- Direct receive 샘플 파일명은 `*_recv_sample`로 끝난다.
- STREAM packet callback 샘플은 `stream_packet_callback_sample`로 따로 두며 direct receive와
  한 파일에서 섞지 않는다.
- Async request 샘플은 `*_async_sample`로 끝나며 reply를 awaitable, future, task 또는
  coroutine으로 확인한다.
- Monitor 샘플은 data receive API를 monitor event 확인 수단으로 사용하지 않는다.
- 하나의 샘플은 하나의 수신 모델만 설명한다.

Packet callback 완료는 latch, future, event, condition variable, channel, promise 또는 semaphore와
같은 결정적 신호로 검증한다. 고정 `sleep`, busy-wait와 timeout이 없는 대기를 사용하지
않는다. 대기는 hard timeout을 가져야 하며 초과하면 non-zero exit code 또는 exception으로
실패한다.

## 5. 코드 구조

샘플 본문에서 다음 흐름을 직접 확인할 수 있어야 한다.

1. Context와 socket을 만든다.
2. Endpoint를 bind하거나 connect한다.
3. 필요하면 subscription과 callback을 설정한다.
4. 연결 준비 상태를 bounded wait로 확인한다.
5. Message를 전송하고 수신한다.
6. Topic, payload 또는 monitor event를 검증한다.
7. Socket과 Context를 종료한다.

Endpoint 확보, OS ephemeral port 조회, bounded readiness wait와 fixture 정리는 얇은 helper로
분리할 수 있다. Sender/receiver 역할, message 순서, 핵심 API 호출과 결과 검증은 helper
뒤에 숨기지 않는다.

## 6. Transport와 topology

- TCP 샘플은 `tcp://127.0.0.1:<port>`와 OS ephemeral port를 기본으로 사용한다.
- 연결 동작을 설명하는 샘플에서 `inproc://`로 network readiness를 대체하지 않는다.
- 기본 topology는 하나의 process와 두 thread다. 한 thread는 sender, publisher 또는 client
  역할을 맡고 다른 thread는 receiver, subscriber 또는 server 역할을 맡는다.
- Monitor처럼 단일 흐름으로 의미가 충분한 샘플은 하나의 thread를 사용할 수 있다.
- Multi-process 구성은 공식 초보 샘플이 아니라 integration 검증에 둔다.

Readiness는 이유 없는 고정 `sleep`으로 대체하지 않는다. Raw socket connect는 monitor event
또는 binding이 제공하는 동등한 readiness 표면으로 확인한다. 모든 대기와 retry에는 명시적
timeout이 있어야 한다.

## 7. 고정 scenario

### 7.1 PAIR

- Topology: `pair A -> pair B`
- Payload: `"hello-pair"`
- 출력: `[pair/recv] send: "hello-pair" -> recv: "hello-pair"`

### 7.2 PUB/SUB

- Topology: `publisher -> subscriber`
- Topic: `"prices"`
- Payload: `"101.25"`
- Subscriber는 send 전에 topic subscription을 설정한다.
- 출력: `[pubsub/recv] publish: "prices/101.25" -> subscribe: "prices/101.25"`

### 7.3 DEALER/ROUTER

- Topology: `dealer -> router -> dealer`
- Request: `"ping"`
- Reply: `"pong"`
- Router가 수신한 routing information을 reply에 정확히 사용했는지 검증한다.
- 출력: `[dealer-router/recv] send: "ping" -> recv: "pong"`

### 7.4 Async request wrapper

- Topology: `dealer -> router`
- Request: `"ping"`
- Reply: `"pong"`
- Requester는 binding의 공개 async request 표면을 사용한다.
- 출력: `[dealer-router/request-reply/async] send: "ping" -> recv: "pong"`

### 7.5 STREAM direct receive

- Topology: `tcp client -> zlink stream server`
- Payload: `"hello-stream"`
- STREAM endpoint는 direct receive API로 payload를 확인한다.
- 출력: `[stream/recv] send: "hello-stream" -> recv: "hello-stream"`

### 7.6 STREAM packet callback

- Topology: `tcp client -> zlink stream server`
- Payload: `"hello-stream"`
- Callback이 실제로 호출되었고 전달된 payload가 고정 값과 같은지 검증한다.
- 출력: `[stream/packet-callback] send: "hello-stream" -> recv: "hello-stream"`

### 7.7 Monitor

- Monitor consumer가 connection readiness event를 direct monitor receive API로 읽는다.
- Binding이 non-blocking monitor receive를 공개하면 event queue가 빈 경로도 검증할 수 있다.
- 출력: `[monitor/recv] recv: "connection-ready"`

## 8. STREAM 표면

STREAM을 지원하는 표면은 direct receive와 packet callback 샘플을 모두 제공한다. STREAM
payload는 ZLink message 계약을 따른다. `len32be` 또는 다른 length-prefixed framing을 ZLink 공개
계약으로 가정하거나 샘플에 숨은 전제로 넣지 않는다.

## 9. Runner

공식 샘플을 제공하는 표면은 해당 샘플 directory 안에 반복 실행 가능한 runner를
둔다. Runner는 지원하는 공식 샘플을 명시적으로 실행하고 성공·실패를 요약한다.
일부 샘플만 수동으로 실행하는 절차를 완료 검증으로 사용하지 않는다.

권장 위치는 다음과 같다.

- `core/samples/run_samples.sh`
- `bindings/<language>/samples/run_samples.sh`
- Windows를 지원하면 `bindings/<language>/samples/run_samples.ps1`

## 10. 런타임 공유 언어

Kotlin은 Java raw binding을, JavaScript는 Node.js raw binding을 사용할 수 있다. 런타임을 공유하는
언어 샘플은 별도 native binding을 만들지 않고 기존 package의 공개 API만 사용한다.
같은 scenario의 고정 topic, payload, 순서와 출력은 native binding 샘플과 같아야 한다.

## 11. 문서와 코드의 일치

샘플은 raw socket guide 코드의 기준이다. Guide가 샘플을 인용하면 핵심 API, 역할,
message 순서와 고정 값을 같게 유지한다. Test synchronization과 assertion은 guide에서 줄일 수
있지만 송수신 의미를 바꾸지 않는다.

## 12. 완료 검증

샘플 변경은 다음 항목을 모두 통과해야 한다.

- 공식 샘플 목록에서 지원하는 항목이 누락되지 않았다.
- Direct receive, async request, packet callback과 monitor 수신 모델을 한 파일에서 섞지 않았다.
- Private API, reflection, native symbol 직접 호출과 generated 내부 type을 사용하지 않았다.
- 고정 `sleep`, timeout이 없는 busy-wait과 lock polling을 사용하지 않았다.
- 실제 message를 전송하고 topic, payload, routing information 또는 monitor event를 검증했다.
- 같은 샘플이 모든 언어에서 같은 고정 값과 출력 형식을 사용한다.
- 개별 샘플과 전체 runner가 모두 정상 종료한다.
