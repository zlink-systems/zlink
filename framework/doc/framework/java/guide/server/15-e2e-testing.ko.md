---
title: "15. E2E 테스트 — client로 시스템 전체를 검증하기 · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/15-e2e-testing.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: 14. 샘플 고르기 — 내 문제에 가까운 예제부터](14-samples.ko.md) | [다음: 16. Options — 설정 목록과 기본값](16-options.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C#/.NET](../../../dotnet/guide/server/15-e2e-testing.ko.md) · [C++](../../../cpp/guide/server/15-e2e-testing.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/server/15-e2e-testing.ko.md) · [Node/TypeScript](../../../node/guide/server/15-e2e-testing.ko.md)
<!-- language-switch:end -->

# 15. E2E 테스트 — client로 시스템 전체를 검증하기

> **이 장에는 계약을 소유하는 스펙 문서가 없다.** 자기 시스템에 테스트를 만드는 방법을
> 다루기 때문이다. 각 샘플이 무엇을 검증하는지는
> [공통 sample 문서](../../../common/sample/README.ko.md)가 정의한다. connector의 정식 API 표면은
> [언어별 Stream Connector 공개 계약](../../../common/spec/stream-connector/README.ko.md)이
> 소유한다. 이 챕터는 **자기 시스템에 E2E 테스트를 만드는 방법**을 다룬다.

## 0. E2E 테스트가 필요한 지점

handler 단위 테스트를 아무리 촘촘히 작성해도 확인되지 않는 항목이 남는다. 등록이 실제로
적용되었는지, 두 node 사이 라우팅이 맞는지, 방에 있는 다른 참가자에게 push가 전달되는지다.
이 항목들은 **process를 기동하고 실제 연결로 확인해야** 판정할 수 있다.

이 지점에서 대개 테스트 전용 client를 따로 구현한다. 소켓을 열고, 프레임을 조립하고, 응답을
기다리는 코드를 시나리오마다 다시 작성한다. ZLink에서는 그 작업이 필요하지 않다. **실제
사용자가 사용할 client 라이브러리가 그대로 검증 도구**이기 때문이다. E2E 테스트는 다음
코드만으로 끝난다.

```java
client.connect().submit().toCompletableFuture().join();               // 실제 연결
AuthenticateRes auth = client.request(new AuthenticateReq(actorId))    // 실제 request
    .submit(AuthenticateRes.class).toCompletableFuture().join();
var push = other.waitFor(PlayerJoinedNotify.class)                    // 실제 push 도착 확인
    .submit(PlayerJoinedNotify.class).toCompletableFuture().join();
ZLinkStreamAssert.ensure(
    push.payload().actorId().equals(auth.player().actorId()), "join push actor mismatch.");
```

`waitFor`처럼 **검증에 필요한 대기 함수를 connector가 직접 제공하므로** 별도 테스트
하네스를 구현하지 않는다. 저장소의 샘플이 모두 이 방식으로 검증된다.

**E2E가 검증하는 범위를 구분한다.** E2E는 등록·라우팅·push·lifecycle처럼 **여러 process가
함께 동작해야 드러나는 항목**을 확인한다. handler 안의 분기나 계산은 단위 테스트가 훨씬
빠르고 정확하므로 E2E에 포함하지 않는다.

## 1. 검증에 사용되는 라이브러리

검증에 쓰는 라이브러리는 역할이 겹치지 않는다.

| | `Zlink.HttpClient` | `Systems.Zlink.Stream.Connector` |
| --- | --- | --- |
| 검증 대상 | 관리·관문 HTTP API | STREAM server node |
| 사용하는 경우 | 방 생성, 조회, 관리 명령처럼 **요청 한 번에 결과가 끝나는** 것 | 연결을 유지한 채 **server가 먼저 보내는 push**까지 확인해야 하는 것 |
| 대표 호출 | `Post(...).Body(...).Fetch<T>()` | `connect` · `request` · `waitFor` · `expectNone` |

대부분의 시나리오는 둘을 이어서 사용한다. HTTP로 대상을 만들고, 그 응답에 담겨 온
endpoint로 STREAM에 접속하는 순서다.

```java
// 1단계 — 관문 API로 방을 만든다.
ZLinkHttpClient api = ZLinkHttpClient.create(options.apiUrl())
    .timeout(options.httpTimeout())
    .build();
// fetch는 역직렬화된 본문을 그대로 돌려준다.
CreateGameHttpRes room = api.post("/games")
    .body(new CreateGameHttpReq(options.gameName()))
    .fetch(CreateGameHttpRes.class);

// 2단계 — 응답이 알려 준 endpoint로 실시간 연결을 연다.
ZLinkStreamConnector client = ZLinkStreamConnectorFactory.create(
    new ZLinkStreamConnectorOptions(
        URI.create(room.playEndpoints().get(0)),
        ZLinkStreamDispatchMode.IMMEDIATE, // console 시나리오는 자동 펌프를 사용한다.
        options.streamTimeout()));
```

`dispatchMode`가 `Immediate`이면 connector가 수신을 자체적으로 처리하므로 시나리오
코드에서 별도로 펌프를 실행하지 않는다. 게임 엔진처럼 프레임 루프에 맞춰 직접 펌프해야 하는 환경은
Stream Connector 가이드가 다룬다.

두 라이브러리의 전체 사용법은 각자의 가이드가 소유한다.

- HTTP Client 가이드 — 요청 구성, 본문, 인증·TLS, 재시도,
  오류 처리까지 13장
- Stream Connector 가이드 — 실행 환경별 통합(Unity,
  Godot). 서버 쪽 STREAM 등록은 [09-stream](09-stream.ko.md)이 다룬다.

## 2. 검증 함수와 사용법

connector가 제공하는 검증 함수로 대부분의 시나리오를 표현한다.

| 검증 항목 | 사용하는 함수 |
| --- | --- |
| 요청을 보내고 응답을 확인한다 | `Request(req)` — 응답 타입은 terminal에 지정한다 |
| server가 먼저 보내는 push가 오는지 확인한다 | `WaitFor<TNotify>()` |
| push가 **오지 않아야** 함을 확인한다 | `ExpectNone<TNotify>().Within(window)` |
| push가 **정해진 순서로** 오는지 확인한다 | `WaitForSequence<TNotify>().Expect(...).Expect(...)` |
| 요청이 **실패해야** 함을 확인한다 | `expectFailure(...)` |

**terminal 표기는 언어를 따른다** — `.NET`은 `Async`, Java · Node · C++은 `submit`,
Kotlin은 `await`다([비동기 실행 정책](../../../common/spec/05-async-execution-policy.ko.md)).

값 비교는 `Ensure(조건, 메시지)`를 사용한다. 메시지는 필수이며, 실패하면 그
메시지를 담은 예외로 시나리오가 끝난다.

### push 도착 확인

`where(...)`로 조건을 지정하면 **조건에 맞는 첫 message까지 기다린다.** 관심 대상이 아닌
push가 섞여 들어와도 시나리오가 영향을 받지 않는다.

```java
var joined = client1.waitFor(PlayerJoinedNotify.class)
    .where(PlayerJoinedNotify.class,
        message -> message.payload().actorId().equals(options.oActorId()))
    .submit(PlayerJoinedNotify.class)
    .toCompletableFuture().join();
ZLinkStreamAssert.ensure(joined.payload().mark() == TicTacToeMarks.O, "joined mark mismatch.");
```

### push 미도착 확인

도착하지 않는다는 사실은 관찰 구간 없이 확정할 수 없으므로 `within(...)`으로 구간을 반드시
지정한다. 지정하지 않으면 오류다.

```java
// 방금 들어온 본인에게는 자기 입장 알림이 가지 않아야 한다.
client2.expectNone(PlayerJoinedNotify.class)
    .within(Duration.ofMillis(250))
    .submit()
    .toCompletableFuture().join();
```

### push 순서 확인

상태가 단계적으로 바뀌는 흐름에서는 도착 여부가 아니라 **순서**가 계약이다.

```java
var statusSequence = customer.waitForSequence(DeliveryStatusNotify.class)
    .expect(DeliveryStatusNotify.class,
        message -> matchesStatus(message, deliveryId, DeliveryStatus.Assigned))
    .expect(DeliveryStatusNotify.class,
        message -> matchesStatus(message, deliveryId, DeliveryStatus.Accepted))
    .expect(DeliveryStatusNotify.class,
        message -> matchesStatus(message, deliveryId, DeliveryStatus.PickedUp))
    .expect(DeliveryStatusNotify.class,
        message -> matchesStatus(message, deliveryId, DeliveryStatus.Delivered))
    .timeout(customer.options().waitTimeout())
    .submit(DeliveryStatusNotify.class)
    .toCompletableFuture().join();
```

### 요청 실패 확인

권한이 없거나 순서가 맞지 않는 요청이 **거절되는지**도 계약이다. 성공 경로만 검증하면 이
경로가 검증되지 않은 채 남는다.

```java
// 인증 전에는 대화를 열 수 없어야 한다.
ZLinkStreamAssert.expectFailure(
    () -> agent.request(new OpenConversationReq("unauthenticated"))
        .submit(OpenConversationRes.class),
    ZLinkStreamErrorCode.RemoteError);
```

## 3. 메시지 대기 처리 방법

E2E는 대부분 같은 원인으로 간헐 실패한다. **행동을 먼저 하고 그다음에 대기를
시작하여**, 그 사이에 도착한 push를 받지 못하는 것이다.

순서를 반대로 둔다. 대기를 먼저 등록하고, 그다음에 그 push를 유발하는 행동을 실행한다.

```java
// 대기를 먼저 등록한다 — 아직 join하지 않는다.
var statusSequenceStage = customer.waitForSequence(DeliveryStatusNotify.class)
    .expect(DeliveryStatusNotify.class,
        message -> matchesStatus(message, deliveryId, DeliveryStatus.Assigned))
    .timeout(customer.options().waitTimeout())
    .submit(DeliveryStatusNotify.class);

// 그다음에 push를 유발하는 행동을 실행한다.
CreateDeliveryRes created = http.post("/deliveries")
    .body(new CreateDeliveryReq(deliveryId, "customer-1", "Kitchen 12", "Customer Lobby"))
    .fetch(CreateDeliveryRes.class);

// 마지막에 결과를 받는다.
var statusSequence = statusSequenceStage.toCompletableFuture().join();
```

여러 client가 같은 사건을 확인해야 한다면 각각 등록해 두고 `Task.WhenAll`로 함께 받는다.

```java
// Bingo — 두 player가 모두 입장하면 방이 시작되고, 두 client가 같은 push를 받는다.
var client1Started = client1.waitFor(BingoGameStartedNotify.class)
    .submit(BingoGameStartedNotify.class);
var client2Started = client2.waitFor(BingoGameStartedNotify.class)
    .submit(BingoGameStartedNotify.class);

CompletableFuture.allOf(
    client1Started.toCompletableFuture(), client2Started.toCompletableFuture()).join();
```

`Sleep`으로 시점을 맞추지 않는다. 대기는 전부 `waitFor`·`expectNone`·`waitForSequence`의
timeout으로 표현한다. `Sleep`은 느린 장비에서 실패하고 빠른 장비에서는 시간을 낭비한다.

## 4. 전체 시나리오 예제

`TicTacToe` 샘플이 가장 짧다. HTTP로 방을 만들고 → 두 player가 접속·인증하고 → 입장 push를
확인하고 → 수를 두고 → 상대가 그 수를 관찰하는지 확인하는 순서다.

```java
public void run(TicTacToeClientOptions options) {
    // 1. 관문 API로 방을 만들고 접속할 endpoint를 받는다.
    ZLinkHttpClient api = ZLinkHttpClient.create(options.apiUrl())
        .timeout(options.httpTimeout()).build();
    CreateGameHttpRes room = api.post("/games")
        .body(new CreateGameHttpReq(options.gameName()))
        .fetch(CreateGameHttpRes.class);
    ZLinkStreamAssert.ensure(room.playEndpoints().size() >= 2, "play endpoints are missing.");

    // 2. player 둘을 서로 다른 Play node에 연결한다 — node 사이 라우팅이 여기서 검증된다.
    ZLinkStreamConnector client1 = createStreamClient(room.playEndpoints().get(0), options);
    ZLinkStreamConnector client2 = createStreamClient(room.playEndpoints().get(1), options);

    // 3. 먼저 접속한 쪽이 인증하고 빈 방에 들어간다.
    client1.connect().submit().toCompletableFuture().join();
    client1.request(new AuthenticateReq(options.xActorId()))
        .submit(AuthenticateRes.class).toCompletableFuture().join();
    JoinGameRes join1 = joinGame(client1, room.roomId()); // 대기 등록 → send → 수신(§3)
    ZLinkStreamAssert.ensure(
        join1.state().status() == TicTacToeGameStatuses.WaitingForPlayers,
        "room should wait for the second player.");

    // 혼자 들어왔을 때 자기 입장 알림이 자기에게 오면 안 된다.
    client1.expectNone(PlayerJoinedNotify.class)
        .within(Duration.ofMillis(250)).submit().toCompletableFuture().join();

    // 4. 두 번째 player가 입장하면 방이 시작된다.
    client2.connect().submit().toCompletableFuture().join();
    client2.request(new AuthenticateReq(options.oActorId()))
        .submit(AuthenticateRes.class).toCompletableFuture().join();
    JoinGameRes join2 = joinGame(client2, room.roomId());
    ZLinkStreamAssert.ensure(
        join2.state().status() == TicTacToeGameStatuses.InProgress,
        "room should start with two players.");

    // 5. 수를 두면 응답과 상대에게 전달된 push가 같은 상태를 가리켜야 한다.
    PlaceMarkRes move = client1.request(new PlaceMarkReq(0))
        .submit(PlaceMarkRes.class).toCompletableFuture().join();
    var sawMove = client2.waitFor(GameStateNotify.class)
        .where(GameStateNotify.class, message -> message.payload().state().lastMoveCell() == 0)
        .submit(GameStateNotify.class).toCompletableFuture().join();
    ZLinkStreamAssert.ensure(
        sawMove.payload().state().board().equals(move.state().board()), "board state mismatch.");
}
```

**검증 지점은 다음 기준으로 고른다.** 요청의 응답만 확인하지 않고 *다른 client가 같은
사실을 관찰하는지*까지 확인한다. 서버 내부 상태가 아니라 **사용자에게 실제로 도달하는
결과**를 계약으로 삼는 것이 E2E의 목적이다.

## 5. 다중 client 검증

한 시나리오 안에서 client를 여럿 생성할 수 있다. 역할을 나누면 client 하나로는 확인할 수
없는 계약까지 확인한다.

- **player 둘** — 한쪽의 행동이 다른 쪽에 전달되는지, 그리고 **자기에게는 오지 않는지**
- **관전자** — 참가자가 아닌 연결에도 알림이 전달되는지, 반대로 참가자 전용 알림이
  전달되지 않는지
- **서로 다른 node에 연결한 둘** — node 사이 라우팅과 위치 해석이 실제로 동작하는지

```java
// join 응답은 request의 reply가 아니라 push로 온다 — 대기를 먼저 등록하고 send한다.
private static JoinGameRes joinGame(ZLinkStreamConnector connector, String roomId) {
    var completion = connector.waitFor(JoinGameRes.class).submit(JoinGameRes.class);
    connector.send(new JoinGameReq(roomId)).submit().toCompletableFuture().join();
    return completion.toCompletableFuture().join().payload();
}
```

`Bingo` 샘플이 이 구성을 그대로 사용한다 — player 둘과 관전자 하나를 함께 두고, 승리
알림이 관전자에게만 전달되는 것까지 확인한다.

## 6. 실행 스크립트와 성공 판정

실행 script는 **server 기동, client 실행, 실행 뒤 정리**를 담당한다.

```bash
# gradle이 실행 가능한 배포본을 만들고, script는 그 실행 파일을 띄운다.
gradle_run :Server:installDist :Client:installDist

start_server play-a "$(app_bin Server Server)" --config "${CONFIG_DIR}/play-a.json"
start_server play-b "$(app_bin Server Server)" --config "${CONFIG_DIR}/play-b.json"
start_server api-a  "$(app_bin Server Server)" --config "${CONFIG_DIR}/api-a.json"

wait_port "${PLAY_A_ROUTE_ENDPOINT}"        # 포트가 열릴 때까지 기다린다. sleep을 쓰지 않는다.

"$(app_bin Client Client)" --api-url "http://127.0.0.1:${api_a_http_port}" \
  >"${log_dir}/client.log" 2>&1

RUN_SUCCEEDED=1
```

script는 다음 규칙을 따른다.

- **client의 exit code가 판정 기준이다.** `set -e` 아래에서 client가 예외로 종료하면
  script도 그 지점에서 실패한다. 별도 판정 로직을 구현하지 않는다.
- **`sleep`이 아니라 조건으로 기다린다.** server 기동은 포트가 열렸는지로, 비동기 후처리는
  로그에 특정 줄이 나왔는지로 확인한다.
- **`trap`으로 정리한다.** 시나리오가 중간에 실패해도 기동한 process와 임시 디렉터리,
  컨테이너가 남지 않아야 다음 실행에 영향을 주지 않는다.

client가 통과했더라도 **서버 로그에 오류가 없는지** 함께 확인한다. client 입장에서는
정상으로 관측되지만 서버가 dispatch 오류를 기록하는 경우가 있다.

```bash
if grep -R -q "dispatch-error" "${LOG_DIR}"; then
  echo "Unexpected dispatch-error in sample logs." >&2
  exit 1
fi
```

## 7. 자주 발생하는 문제

- **push를 받지 못해 간헐적으로 실패한다** → 행동보다 대기를 먼저 등록했는지 확인한다
  ([§3](#3-메시지-대기-처리-방법)). 대기를 나중에 시작하면 그 사이에
  도착한 push를 받지 못한다.
- **`expectNone`이 오류로 끝난다** → `within(...)`을 지정하지 않은 경우다. 관찰 구간 없이
  도착하지 않는다는 사실을 확정할 수 없으므로 구간을 명시적으로 요구한다.
- **`waitFor`가 다른 message를 반환한다** → 조건 없이 타입만으로 대기한 경우다.
  `where(...)`로 이 시나리오가 기다리는 사건인지 좁힌다.
- **로컬에서는 통과하고 CI에서만 실패한다** → `sleep`으로 맞춘 시점이 남아 있는지 확인한다.
  대기는 전부 timeout이 지정된 대기 함수로 표현한다.
- **client는 통과했는데 서버 로그에 오류가 남는다** → 서버 로그 오류 검사를 script에
  추가하지 않은 경우다([§6](#6-실행-스크립트와-성공-판정)).
- **연결은 되지만 push가 도착하지 않는다** → 엔진 통합처럼 수동 펌프가 필요한 환경에서
  `dispatch`를 실행하지 않은 경우다(Stream Connector 가이드).

## 8. 관련 문서

- 어떤 샘플을 먼저 볼지: [14-samples](14-samples.ko.md)
- 서버 쪽 STREAM 등록과 session: [09-stream](09-stream.ko.md)
- HTTP client 전체 사용법: HTTP Client 가이드
- 엔진 통합과 수동 펌프: Stream Connector 가이드
- connector 정식 계약:
  [언어별 Stream Connector 공개 계약](../../../common/spec/stream-connector/README.ko.md)
- 각 샘플이 검증하는 시나리오: [공통 sample 문서](../../../common/sample/README.ko.md)
