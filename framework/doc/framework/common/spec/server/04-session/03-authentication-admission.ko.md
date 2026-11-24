# 인증 입장 제한과 로그인 대기열

> 대상: STREAM session을 쓰는 서버 Framework와 Stream Connector
> 이슈: #4546
> 공통 fixture: `framework/runtime/conformance/login-admission-v1.json`

로그인이 한꺼번에 몰리면 인증 handler와 그 뒤의 DB 조회·Actor 생성이 같이 몰린다. 이 문서는 그 순간 폭주에서
인증 handler와 그 뒤의 자원을 지키는 입장 제한을 정한다. 1단계는 node별 동시 인증 한도이고, 2단계는 그 위에
전역 순번을 보여 주는 로그인 대기열이다. 두 단계 모두 기본값은 꺼져 있고 앱이 켠다.

## 1. 1단계: node별 동시 인증 한도

### 1.1 설정

| 설정 | 의미 | 기본값 |
|---|---|---|
| `AuthenticationPacketName` | 인증 request로 다룰 packet 이름 하나 | 없음(지정하지 않으면 이 절 전체를 적용하지 않는다) |
| `MaxConcurrentAuthentications` | 한 node가 동시에 처리하는 인증 request 수의 상한. 양의 정수 | node의 유효 CPU 코어 수 × 8 |
| `AuthenticationRetryDelay` | 거절한 request에 알려 줄 다시 시도까지의 시간(ms) | 500 |

설정은 STREAM session 등록 options에 둔다. 언어별 이름은 각 언어의 session 등록 options 관례를 따르며
언어별 exact interface 문서가 정한다. 0이나 음수 한도, 0 이하 지연은 시작할 때 설정 오류다. 한도와 지연은
`1..2147483647` 범위의 정수이며(지연 단위는 ms), 범위 밖도 시작할 때 설정 오류다. 유효 CPU 코어 수는 그 언어
runtime이 보고하는 사용 가능 processor 수(container CPU quota·affinity 반영)이고, 기본 한도 계산이 `2147483647`을
넘으면 `2147483647`을 쓴다. 한 node에 STREAM 등록이 여럿이어도 처리 중 인증 수와 한도는 node에 한 벌이다 —
등록마다 다른 `AuthenticationPacketName`·`MaxConcurrentAuthentications`·`AuthenticationRetryDelay`를 지정하면
시작할 때 설정 오류다.

### 1.2 판정

판정 대상은 `AuthenticationPacketName`과 이름이 같은 **Request** kind packet이다. Send, Response, 다른 이름의
packet은 이 한도와 무관하다.

1. Framework는 기존 수신 순서(ingress permit 확보 → packet 수신 → header 해석, [Application job queue §3](../01-execution/04-application-job-queue-and-backpressure.ko.md))를 그대로 지키고, header를 해석해 인증 request임을 안 직후 application job으로 게시하기 전에 node의 처리 중 인증 수와 한도를 비교한다.
2. 처리 중 수가 한도보다 작으면 수를 1 늘리고 request를 기존 경로로 넘긴다. 이후 permit·queue·handler 실행은
   [Application job queue와 backpressure](../01-execution/04-application-job-queue-and-backpressure.ko.md)를 그대로 따른다.
3. 처리 중 수가 한도와 같으면 handler를 실행하지 않고 그 request를 §1.3의 거절 결과로 바로 끝낸다. 거절한 request는
   이미 확보한 ingress permit을 한 번 반환하고 application job으로 게시하지 않으며, Actor 생성·조회와 Location Store
   읽기·쓰기를 일으키지 않는다.

처리 중 수는 수락한 인증 request가 다음 terminal 중 하나에 이를 때 한 번만 1 줄어든다.

| terminal | 뜻 |
|---|---|
| `replySuccess` | handler가 성공 reply를 냈다 |
| `replyFailure` | handler가 실패 reply를 냈다 |
| `handlerException` | handler가 예외로 끝났다 |
| `cancellation` | 서버 쪽에서 request 작업이 취소됐다(session 종료, runtime 취소) |
| `enqueueRejected` | application job queue가 수락을 거절했다 |
| `callbackWithoutReply` | handler가 reply 없이 끝났다 |
| `shutdown` | runtime 종료로 끝났다 |

반환은 그 request의 terminal을 판정하는 기존 request·callback 소유자 하나가 한다. `replySuccess`·`replyFailure`는
reply를 STREAM 송신에 제출해 수락된 시점이고, 그 뒤 callback이 더 실행되어도 인증 수에 넣지 않는다.
`callbackWithoutReply`는 reply 없이 callback이 끝난 시점이다. Client의 request timeout은 서버 handler의 종료가
아니므로 반환하지 않는다. 같은 request에 terminal 후보가 겹치면 먼저 확정된 하나만 반환한다. 별도 timer로
반환을 보정하지 않는다.

인증 handler 안에서 기다리는 Actor 생성·DB 조회 시간은 모두 처리 중에 포함된다. 처리 중 수와 한도는 node
메모리의 값 하나이며 Location Store에 쓰지 않는다. 새 Spot·전역 상태·토큰을 두지 않는다.

### 1.3 거절 결과

거절은 기존 Error packet으로 보낸다. Service wire는 바뀌지 않는다.

- Error body는 `{"code":"unavailable","message":"Authentication admission is full."}`이다
  ([Framework error model](../00-foundation/07-framework-error-model.ko.md)의 `Unavailable`).
- Error packet header의 metadata에 `$zlink.admission.retryAfterMs`를 넣는다. 값은 다시 시도할 때까지의 상대
  지연(ms)을 부호와 선행 0 없는 10진수 문자열로 쓴 것이며 `AuthenticationRetryDelay`와 같다. 범위는 0 이상
  2^31 − 1 이하다. 서버와 client의 시계가 어긋나도 재전송 시각이 몰리지 않도록 절대 시각을 쓰지 않는다.
- 이 metadata가 있는 `Unavailable`만 입장 거절이다. Handler가 실행되지 않았음을 Framework가 보장한다. 일반
  `Unavailable` 오류에는 이 metadata를 넣지 않는다. 이 제한된 예외는
  [Framework error model §7](../00-foundation/07-framework-error-model.ko.md#7-재시도-판단)이 정하며, 일반 오류의 retry hint 금지는 그대로다.

### 1.4 관측

Framework는 [runtime metrics §3](../06-observability/02-runtime-metrics.ko.md#3-host-core-hwm과-application-job-queue)의
`zlink.host.authentication_admission.in_flight`와 `zlink.host.authentication_admission.rejections`로 처리 중 인증 수와
거절 수를 남긴다. 처리 중 수는 admission이 가진 값을 관측하며 metric을 별도 권위 상태로 두지 않는다.

## 2. Connector의 입장 대기와 자동 재시도

Stream Connector의 동작은 [Stream Connector §5.8](../../stream-connector/32-stream-connector.ko.md#58-입장-대기와-자동-재시도)이
정한다. 앱은 재시도 루프를 만들지 않는다.

## 3. 2단계: 로그인 대기열

### 3.1 목적과 관계

2단계는 1단계 위에 얹는 선택 모듈이다. 1단계가 인증·DB를 보호하고, 2단계는 client에게 전역 순번과 예상 대기
시간을 보여 준다. 2단계를 켜도 1단계의 node별 한도와 판정은 그대로 남는다. 같은 처리 중 수를 두 곳에서 세지
않는다: 대기열은 순번과 입장 ticket만 소유하고, 인증 request의 수락·거절은 1단계 한 곳이 판정한다.

### 3.2 동작

1. 대기열은 mesh의 Instance Spot 하나가 소유한다. 순번은 그 Spot의 메모리에 있고 대기자마다 Location Store key를
   두지 않는다. Spot이 사라지면 대기 중인 client는 처음부터 다시 줄을 선다.
2. Client는 인증 전에 대기열 입장을 요청한다. 대기열 Spot은 순번을 정하고, 입장할 수 있을 때까지 순번과 예상
   대기 시간을 client에 push한다.
3. 입장할 차례가 되면 Spot은 만료 시각이 있는 서명된 입장 ticket을 준다. Client는 그 ticket을 인증 request에 담아
   보낸다.
4. 인증 handler가 실행되기 전에 Framework가 ticket의 서명과 만료를 검사한다. 맞지 않으면 handler를 실행하지
   않고 거절한다. 맞으면 1단계 판정으로 넘어간다.
5. Connector는 대기 상태·순번 이벤트와 ticket 재사용을 처리해 client 앱에는 callback만 알린다.

대기열이 쓰는 node의 처리 중 수는 ticket 발급 속도를 정하는 비구속 참고값이다. Ticket 발급은 node의 자리를
예약하지 않으며 인증 request의 수락 권위는 1단계에 남는다. Ticket을 가진 client가 1단계에서 거절되면 connector가
[§5.8](../../stream-connector/32-stream-connector.ko.md#58-입장-대기와-자동-재시도)대로 다시 보내고, ticket은 만료까지 유효하다.

### 3.3 정한 값

다음 값은 PM 판정(11-20)으로 정했다. 바꿀 때는 이 절과 공통 fixture를 함께 고친다.

| 항목 | 기본안 |
|---|---|
| ticket 서명 | HMAC-SHA256, 서버 설정의 비밀 키. ticket에 session RoutingId·만료 시각·대기열 epoch를 담는다 |
| ticket 전달 | 인증 request header metadata `$zlink.admission.ticket` |
| 순번 push | 대기열 Spot이 client session에 보내는 Send packet(이름 `$zlink.admission.position`), 값은 순번·예상 대기 ms |
| 입장 허가 속도 | 대기열이 1단계 한도에서 남은 자리만큼 ticket을 낸다. 남은 자리는 각 node가 대기열 Spot에 알리는 처리 중 수로 계산한다 |
| Connector callback | 대기 상태 변화(`OnAdmissionQueued`)와 순번 변화(`OnAdmissionPosition`) |
| ticket 만료 | 발급부터 30초. 대기열 epoch 수명보다 길지 않다 |

Ticket 검증과 비밀 키는 다음을 지킨다.

- 서버는 현재 키와 이전 키 하나로 ticket을 검증한다. 키를 바꾸는 동안 이전 키로 서명한 ticket을 가진 대기자는
  끊기지 않는다. 이전 키로는 서명하지 않는다.
- 서명 비교는 상수 시간으로 한다. 만료 시각이 지났거나 epoch가 현재 대기열과 다르면 서명을 맞게 검증했더라도 거절한다.
- 비밀 키는 설정 값으로만 받는다. Framework는 키를 log, metric, 오류 메시지, trace에 남기지 않는다.

### 3.4 Wire

대기열은 service wire를 바꾸지 않고 STREAM packet과 header metadata만 쓴다. 모든 JSON 정수는 JSON number이고
`ticket`은 RFC 4648 §5 base64url(padding 없음) 문자열이다.

| 방향 | Packet | 내용 |
|---|---|---|
| Server → client, 인증 Request의 Error | `unavailable` + header metadata `$zlink.admission.queueRequired` = `"1"` | 2단계가 켜져 있고 인증 request에 유효한 ticket이 없다. Handler를 실행하지 않고 1단계 처리 중 수에도 넣지 않는다 |
| Client → server | Request `$zlink.admission.enqueue`, payload `{}` | 대기열에 들어간다. Framework가 대기열 Instance Spot으로 보낸다 |
| Server → client, 위 Request의 Response | `{"position": n, "estimatedWaitMs": m}` 또는 `{"ticket": "...", "expiresInMs": t}` | 대기 순번과 예상 대기, 또는 바로 입장할 수 있으면 ticket |
| Server → client | Send `$zlink.admission.position`, `{"position": n, "estimatedWaitMs": m}` | 순번이 바뀔 때 보낸다 |
| Server → client | Send `$zlink.admission.ticket`, `{"ticket": "...", "expiresInMs": t}` | 차례가 되었을 때 한 번 보낸다 |
| Client → server, 인증 Request | header metadata `$zlink.admission.ticket` = ticket 문자열 | 받은 ticket을 담아 인증 request를 다시 보낸다 |

Ticket이 없거나, 서명이 맞지 않거나, 만료됐거나, epoch가 현재 대기열과 다르면 모두 첫 행과 같은 결과로 거절한다.
거절 사유를 client에 구분해 알리지 않는다. 대기열은 session이 끝나거나, 보낸 ticket이 쓰이지 않고 만료되면 그
대기자를 지운다.

Connector는 다음과 같이 처리한다.

- `$zlink.admission.queueRequired`가 있는 거절을 받으면 갖고 있던 ticket을 버리고 `$zlink.admission.enqueue`를 보낸다.
  대기열에 들어갈 때 `OnAdmissionQueued`를, 순번을 받을 때마다 `OnAdmissionPosition`을 부른다.
- Ticket을 받으면 원래 인증 request를 같은 packet 이름·payload·application metadata·correlation과
  `$zlink.admission.ticket`으로 다시 보낸다. 그 request가 1단계에서 거절되면 [Stream Connector §5.8](../../stream-connector/32-stream-connector.ko.md#58-입장-대기와-자동-재시도)대로
  기다렸다가 같은 ticket으로 다시 보낸다.
- 마감은 §5.8과 같다: 원래 request deadline과 `처음 거절을 받은 시각 + MaxAdmissionWait` 중 이른 쪽. 마감이 지나면
  `RequestTimeout`으로 끝내고 더 보내지 않는다.
- `$zlink.admission.queueRequired` 값이 `"1"`이 아니거나 Response·Send payload가 위 형식이 아니면 `FrameDecodeFailed`로
  그 request만 끝내고 연결은 유지한다.

## 4. 시험

- 공통 fixture `login-admission-v1.json`의 서버 시나리오 12개와 connector 시나리오 7개를 네 언어가 실행한다.
- 네 언어 서버 × 네 언어 connector 교차언어 contract test: 한도 초과 시 handler 미실행, 거절 결과의 code와
  `$zlink.admission.retryAfterMs`, connector 자동 재시도 뒤 성공, 거절된 request의 Actor·Location Store·permit 미사용.
- Fixture validator 통과는 runtime 통과가 아니다. 각 언어 runtime 시험은 fixture에 없는 다음도 확인한다: 거절된
  request의 Actor 조회·Location Store 읽기 부재, Response kind 제외, 0·음수 설정의 시작 오류, 대기 중 연결 끊김과
  caller 취소, `retryAfterMs` 경계값(0, 선행 0, 부호, 2^31 − 1, 그보다 큰 값).
