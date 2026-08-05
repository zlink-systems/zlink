<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: Pub/Sub](config-3-pubsub.ko.md) | [다음: Resilience](config-5-resilience-lifecycle.ko.md)
<!-- framework-adapter-nav:end -->

# Config 4 — Handler 등록과 typed codec

Application handler는 언어가 제공하는 public 등록 방식으로 찾을 수 있어야 하고, 같은 dispatch key가
중복되면 host가 message를 받기 전에 실패해야 한다. Typed payload는 별도 설정이 없으면 JSON으로
처리하며, 다른 wire format이 필요할 때만 root에 codec extension을 한 번 등록한다. 이 config는
registration과 codec 선택이 실제 process 사이에서도 이 계약을 유지하는지 검증한다.

E2E client는 server의 application endpoint를 호출한다. Server endpoint가 public Channel API로
request와 send를 시작하고, target handler와 codec extension이 application evidence를 남긴다. Client가
Framework registry, encoded payload나 private dispatch table을 직접 읽지 않는다.

## 1. 확인 범위

- Runtime reflection을 제공하는 언어의 handler scan과 모든 언어의 explicit registration
- Dispatch마다 생성되는 handler scope와 dependency 수명
- Filter의 before·after 순서와 short-circuit 결과
- 중복 dispatch key의 startup validation
- 별도 등록이 필요 없는 기본 typed JSON
- Root에 한 번 등록하는 Protobuf·MessagePack extension과 codec 불일치 결과
- 다섯 언어 사이에서 유지해야 하는 `framework-json-v1`의 application 값

Runtime reflection을 제공하지 않는 C++는 compile-time type과 explicit builder registration을 사용한다.
RC-A1의 scan을 C++에서 흉내 내기 위해 reflection helper를 추가하지 않는다. 이는 기능 누락이 아니라
[Framework API §8](../spec/06-framework-api.ko.md)이 정한 언어 표현 차이다.

## 2. 배포 구성

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| registration server | 1 | `registration-mesh`의 `echo` Channel Server다. Scan과 explicit registration으로 등록한 handler, filter와 dependency evidence endpoint를 제공한다. |
| codec server | scenario별 1 | JSON 또는 지정한 codec extension을 root에 등록한다. Codec별 encode·decode 횟수와 handler가 복원한 값을 application evidence로 제공한다. |
| caller server | 1 | Client의 HTTP 요청을 받아 public Channel request 또는 send를 시작한다. Manual endpoint를 사용하여 registration·codec 외의 discovery 조건을 제거한다. |
| E2E client | 1 | 언어별 public HTTP client로 caller와 역할 server의 application endpoint를 호출한다. |

Registration handler는 서로 다른 packet name을 사용하지만 같은 의미의 결과를 만든다.
`EchoReq.Value`를 받으면 `EchoRes.Value`에 `echo:<value>`를 반환하고, `EchoMsg`는 marker와 값을 evidence에
한 번 기록한다. Codec scenario는 packet name을 codec 선택 수단으로 사용하지 않고 payload type에 맞는
extension을 선택한다.

## 3. 공통 실행과 판정 방법

Runner는 scenario별 process, port, evidence marker와 log 디렉토리를 새로 만든다. 역할 server의 health와
public RouteMesh status가 ready가 된 뒤 client를 시작한다. Startup 실패 scenario는 listener가 ready가
되지 않았다는 사실과 process의 public configuration error를 함께 확인한다.

Handler와 codec의 호출 횟수는 application이 public registration·extension callback에서 기록하여 public
evidence endpoint로 제공한다. Encoded bytes, private registry entry와 reflection metadata는 판정에
사용하지 않는다. File log는 실패 진단에만 사용한다.

## 4. Scenario

### Track A — Handler 등록과 dispatch

#### RC-A1 지원 언어에서 handler를 scan한다

우선순위: `P0`

Runtime reflection을 제공하는 언어는 application이 지정한 assembly, module 또는 package에서 handler를
찾을 수 있다. Scan 범위를 잘못 해석하면 host는 시작되지만 실제 request에서 handler를 찾지 못한다.

**검증 질문:** Scan을 지원하는 언어에서 지정한 범위의 request·send handler가 explicit registration
없이 message를 처리하는가.

- 시작 조건: Registration server는 `EchoScanReq`와 `EchoScanMsg` handler가 포함된 public scan 범위만
  등록한다. 같은 handler를 explicit builder에 추가하지 않는다.
- 절차: Client가 scan variant의 request와 send를 caller endpoint를 통해 각각 한 번 보낸다.
- 검증: Request는 정확한 `EchoRes`를 받고 send marker는 handler evidence에 한 번 기록된다. Scan 범위
  밖의 대조 handler는 실행되지 않는다. C++ runner는 이 scenario를 `not-applicable`로 기록하고 RC-A3을
  필수로 실행한다.
- 세부 동작: [Framework API §8](../spec/06-framework-api.ko.md)의 runtime
  reflection과 C++ explicit registration 경계를 검증한다.

#### RC-A2 언어별 annotation·attribute handler를 scan한다

우선순위: `P1`

Annotation이나 attribute를 handler scan의 표식으로 사용하는 언어는 그 metadata가 dispatch key로
정확히 변환되는지 확인해야 한다. 해당 표면이 없는 언어에 같은 문법을 요구하지 않는다.

**검증 질문:** 언어별 public interface가 annotation·attribute scan을 제공하는 언어에서 표시한 handler가 지정한
packet name의 request와 send를 처리하는가.

- 시작 조건: 해당 언어의 public annotation·attribute를 사용한 handler와 scan 범위를 등록한다.
- 절차: Client가 annotation variant의 request와 send를 각각 한 번 보낸다.
- 검증: Request reply와 send evidence의 marker·payload가 입력과 일치한다. 언어별 public interface에 이 등록
  방식이 없는 언어는 `not-applicable`이며 대체 annotation helper를 추가하지 않는다.
- 세부 동작: [Public contract governance](../spec/00-public-contract-governance.ko.md)의 언어별 표현과
  공통 dispatch 의미를 검증한다.

#### RC-A3 Handler를 명시적으로 등록한다

우선순위: `P0`

Explicit registration은 reflection 유무와 관계없이 handler type과 dispatch key를 startup 구성에 직접
고정한다. C++에서는 이 방식이 기본 등록 경로다.

**검증 질문:** Public builder로 명시한 request·send handler가 모든 언어에서 같은 결과를 만드는가.

- 시작 조건: Registration server가 explicit variant handler만 public builder에 등록한다.
- 절차: Client가 explicit variant의 request와 send를 각각 한 번 보낸다.
- 검증: Request는 입력 marker와 일치하는 reply를 받고 send handler는 한 번 실행된다. Scan이나 private
  registry mutation 없이 결과가 나온다.
- 세부 동작: [Framework API §8](../spec/06-framework-api.ko.md)의 explicit
  registration을 검증한다.

#### RC-A4 Dispatch마다 dependency scope를 분리한다

우선순위: `P1`

Channel handler와 filter는 dispatch마다 새 scope에서 실행된다. 같은 scoped dependency가 서로 다른
request 사이에 공유되면 한 request의 mutable state가 다른 request에 노출될 수 있다.

**검증 질문:** 연속 request가 서로 다른 dispatch scope를 사용하면서 application singleton은 공유하는가.

- 시작 조건: Handler는 scoped dependency ID와 application singleton ID를 reply와 evidence에 기록한다.
- 절차: Client가 서로 다른 marker의 request 20개를 순차 실행한다.
- 검증: Scoped dependency ID는 20개가 모두 다르고 singleton ID는 모두 같다. 해당 언어의 public DI가
  scope disposal 관측을 제공하면 정상·handler 실패·cancellation 반복에서 scope가 각각 한 번 정리된다.
  Public disposal 관측이 없는 언어는 instance 분리까지만 검증한다.
- 세부 동작: [Framework API §8.2](../spec/06-framework-api.ko.md)의
  Channel dispatch scope를 검증한다.

#### RC-A5 Filter 순서와 short-circuit 결과

우선순위: `P1`

여러 filter는 등록 순서대로 handler 앞을 지나고, handler가 끝나면 반대 순서로 돌아와야 한다. Filter가
`next`를 호출하지 않은 request는 정상 업무 reply처럼 보이면 안 된다.

**검증 질문:** 세 filter가 정해진 순서로 실행되고 short-circuit request가 `Rejected`로 끝나는가.

- 시작 조건: `F1`, `F2`, `F3` filter와 handler를 이 순서로 등록한다. 별도 packet은 `F2`가 `next`를
  호출하지 않도록 marker로 제어한다.
- 절차: 정상 request와 short-circuit request를 각각 한 번 보낸다.
- 검증: 정상 evidence 순서는 `F1-before, F2-before, F3-before, handler, F3-after, F2-after,
  F1-after`이며 handler는 한 번 실행된다. Short-circuit request는 `Rejected`로 끝나고 `F3`와 handler는
  실행되지 않는다.
- 세부 동작: [Framework API §8.1](../spec/06-framework-api.ko.md)의 filter 순서와
  request short-circuit 계약을 검증한다.

#### RC-A6 중복 dispatch key를 startup에서 거부한다

우선순위: `P0`

같은 owner에 같은 message kind와 packet name을 두 번 등록하면 어느 handler를 실행할지 결정할 수 없다.
Framework는 첫 message가 올 때까지 미루지 않고 startup에서 구성을 거부해야 한다.

**검증 질문:** 중복 handler key가 있는 host가 listener를 ready로 공개하기 전에 configuration error로
종료되는가.

- 시작 조건: Negative host에 같은 ChannelName, request kind와 packet name을 가진 handler 두 개를
  public registration 방식으로 추가한다.
- 절차: Runner가 negative host를 시작하고 process terminal과 health endpoint 상태를 수집한다. 이어서
  packet name만 다른 정상 대조 host를 시작한다.
- 검증: Negative host는 listener와 ready status를 공개하지 않고 configuration error로 종료된다. 정상
  대조 host는 ready가 되고 두 packet을 각각 한 번 처리한다.
- 세부 동작: [Framework API §8](../spec/06-framework-api.ko.md)과
  [§14](../spec/06-framework-api.ko.md)의 중복 registration 검증을 확인한다.

### Track B — Typed payload codec 선택

#### RC-B1 별도 등록 없이 기본 JSON을 사용한다

우선순위: `P0`

JSON은 typed message의 기본 codec이다. Application이 message type마다 codec을 등록해야 한다면 기본
serializer 책임이 호출자에게 노출된다.

**검증 질문:** Codec extension을 하나도 등록하지 않아도 typed JSON request와 send가 정확히 처리되는가.

- 시작 조건: Caller와 codec server는 codec extension을 등록하지 않고 JSON DTO handler만 등록한다.
- 절차: Nullable field, string enum, signed 64-bit 값과 bytes를 포함한 request와 send를 각각 보낸다.
- 검증: Handler가 모든 application 값을 복원하고 request reply와 send evidence가 입력과 일치한다.
  Message type별 codec registration은 `0`건이다.
- 세부 동작: [Framework API §9](../spec/06-framework-api.ko.md)과
  [message model §1](../spec/04-message-model.ko.md)의 기본 typed JSON 계약을 검증한다.

#### RC-B2 Root에 등록한 Protobuf extension을 사용한다

우선순위: `P0`

Protobuf를 사용하는 application은 message마다 encoder를 넘기지 않고 extension을 root에 한 번
등록한다. Payload type과 extension이 일치하면 Framework가 해당 codec을 선택한다.

**검증 질문:** Protobuf extension을 root에 한 번 등록하면 Protobuf DTO request와 send가 그 extension을
통해 처리되는가.

- 시작 조건: Caller와 codec server가 공식 Protobuf extension을 root에 각각 한 번 등록한다.
- 절차: Protobuf DTO request와 send를 각각 한 번 보낸다.
- 검증: Reply와 send evidence의 application 값이 입력과 일치한다. Caller encode와 server decode
  extension callback이 message마다 한 번 실행된다. Handler 호출부에 codec option을 전달하지 않는다.
- 세부 동작: [Framework API §9](../spec/06-framework-api.ko.md)의 root codec extension을 검증한다.

#### RC-B3 Root에 등록한 MessagePack extension을 사용한다

우선순위: `P1`

MessagePack도 Protobuf와 같은 root extension 규칙을 사용하며 packet name이나 Channel 설정으로 codec을
선택하지 않는다.

**검증 질문:** MessagePack extension을 root에 한 번 등록하면 MessagePack DTO request와 send가 그
extension을 통해 처리되는가.

- 시작 조건: Caller와 codec server가 공식 MessagePack extension을 root에 각각 한 번 등록한다.
- 절차: MessagePack DTO request와 send를 각각 한 번 보낸다.
- 검증: Reply와 send evidence가 입력과 일치하고 extension의 encode·decode callback이 message마다 한 번
  실행된다. Packet name을 codec 선택 값으로 사용하지 않는다.
- 세부 동작: [Framework API §9](../spec/06-framework-api.ko.md)의 extension 선택을 검증한다.

#### RC-B4 한 root에서 여러 codec을 함께 사용한다

우선순위: `P0`

한 server가 JSON, Protobuf와 MessagePack 업무를 함께 제공해도 한 codec의 등록이 다른 payload type의
선택을 바꾸면 안 된다.

**검증 질문:** 세 payload type을 섞어 보내도 각 type이 기본 JSON 또는 일치하는 extension으로 한 번씩
처리되는가.

- 시작 조건: Caller와 server root에 Protobuf와 MessagePack extension을 등록하고 JSON handler도 둔다.
- 절차: JSON, Protobuf와 MessagePack request를 순환하여 각각 20개씩 보내고 같은 type의 send도 한 번씩
  보낸다.
- 검증: 60개 request와 3개 send의 application 값이 모두 보존된다. Extension callback count는 해당
  payload type의 message 수와 일치하며 JSON message에서는 두 extension이 실행되지 않는다.
- 세부 동작: [Framework API §9](../spec/06-framework-api.ko.md)의 type 기반 codec 선택과 JSON
  fallback을 검증한다.

#### RC-B5 수신 codec이 없으면 `ProtocolError`로 끝난다

우선순위: `P1`

Envelope가 non-JSON content type을 명시했는데 receiver에 일치하는 extension이 없으면 JSON으로 다시
해석해서는 안 된다. 잘못된 fallback은 payload를 다른 값으로 처리하거나 handler 예외를 만든다.

**검증 질문:** Sender만 등록한 Protobuf extension으로 request를 보내면 receiver handler 실행 없이
`ProtocolError`가 반환되는가.

- 시작 조건: Caller에는 Protobuf extension을 등록하고 server에는 기본 JSON만 둔다. 정상 JSON handler도
  함께 등록한다.
- 절차: Protobuf request를 한 번 보낸 뒤 JSON request를 한 번 보낸다.
- 검증: Protobuf request는 `ProtocolError`로 한 번만 끝나고 Protobuf handler는 실행되지 않는다. JSON
  request는 정상 reply를 받는다. Receiver가 non-JSON payload를 JSON으로 fallback하면 실패다.
- 세부 동작: [Framework API §9](../spec/06-framework-api.ko.md)과
  [오류 모델 §5](../spec/32-framework-error-model.ko.md)를 검증한다.

#### RC-B6 다섯 언어가 JSON application 값을 같게 복원한다

우선순위: `P0`

언어별 JSON library가 달라도 public DTO의 application 값은 같아야 한다. JSON object member 순서나
공백처럼 의미가 없는 byte 차이는 검증하지 않는다.

**검증 질문:** 방향이 있는 모든 언어 조합에서 `framework-json-v1` fixture가 같은 typed 값으로
왕복하는가.

- 시작 조건: 각 언어 server와 caller가 별도 codec extension 없이 같은 DTO 의미와 packet name을
  등록한다.
- 절차: Property name 대소문자, string enum, signed 64-bit decimal string, padded Base64 bytes, 32-bit
  JSON number, 유한 floating-point 값과 nullable field를 포함한 golden request를 각 언어 방향으로
  보낸다. Unknown field, duplicate field와 required field 누락 fixture도 각각 보낸다.
- 검증: Golden request는 같은 typed application 값으로 복원되고 reply도 같은 의미를 가진다. Unknown
  field는 무시한다. Duplicate field와 required field 누락은 handler 전에 `ProtocolError`로 끝난다.
  Re-encode한 JSON bytes, whitespace와 member order의 일치는 요구하지 않는다.
- 세부 동작: [Message model §2](../spec/04-message-model.ko.md)의
  `framework-json-v1` 언어 간 의미를 검증한다.

## 5. 완료 조건

- `P0`인 RC-A1, RC-A3, RC-A6, RC-B1, RC-B2, RC-B4와 RC-B6이 지원되는 모든 언어에서 통과한다.
  RC-A1을 지원하지 않는 C++는 `not-applicable`과 정식 spec 근거를 feature map에 기록한다.
- Client는 역할 server의 public 업무·evidence endpoint만 호출한다. Framework registration, codec
  registry나 encoded payload를 직접 읽지 않는다.
- Handler와 extension callback count는 application이 public callback에서 기록한 evidence만 사용한다.
- Readiness와 handler 완료는 public status 또는 bounded application evidence wait로 확인한다.
- 실패 시 client result, handler·codec application evidence와 역할 server log를 보존한다. Log는 진단
  자료이며 통과 조건이 아니다.
