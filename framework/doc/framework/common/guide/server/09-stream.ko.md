# 9. STREAM

> **이 장의 계약 소유 문서** — [STREAM 서버 session](../../../common/spec/19-stream-session.ko.md)이
> 동작을, [언어별 STREAM session 공개 계약](../../../common/spec/server/languages/README.ko.md)이
> 서버의 정확한 시그니처를 소유한다. Client package는 Stream Connector 가이드와
> [언어별 공개 계약](../../../common/spec/stream-connector/README.ko.md)을 따른다.

STREAM은 외부 client와 Framework server 사이의 연결 지향 양방향 메시지 채널이다.
Server는 session lifecycle과 packet dispatch를 구현한다. Client는 독립 package인
`Systems.Zlink.Stream.Connector`를 사용한다.

## 1. Server node 등록

Stream node에는 session type 하나를 등록한다. Actor dispatch를 사용하면 명시적으로 활성화한다.

=== "C#/.NET"

    ```csharp
    options.AddStreamNode("client-stream")
        .Bind("tcp://0.0.0.0:9100")
        .EnableActorDispatch()
        .AddSession<PlaySession>(); // 연결마다 만들 session type을 등록한다.
    ```

=== "C++"

    ```cpp
    options.add_stream_node ("client-stream")
      .bind ("tcp://0.0.0.0:9100")
      .enable_actor_dispatch ()
      .register_session<play_session_t> (); // 연결마다 만들 session type을 등록한다.
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/zlink-framework-doc-examples/src/main/java/systems/zlink/framework/docexamples/stream/StreamNodeRegistration.java:register-stream-node"
    ```

=== "Kotlin"

    ```kotlin
    options.addStreamNode("client-stream")
        .bind("tcp://0.0.0.0:9100")
        .enableActorDispatch("play")            // Kotlin도 Java 표면을 그대로 쓴다.
        .registerSession(PlaySession::class.java) // 연결마다 만들 session type을 등록한다.
    ```

=== "Node/TypeScript"

    ```typescript
    builder.addStreamNode('client-stream')
      .bind('tcp://0.0.0.0:9100')
      .enableActorDispatch()
      .registerSession(PlaySessionFactory); // 연결마다 만들 session factory를 등록한다.
    ```


Session handler와 Actor/Spot handler는 Framework의 기본 typed JSON serialization을 사용한다.
Application이 message type마다 codec을 등록하거나 raw frame을 해석하지 않는다.

**등록은 명시적이다.** attribute·annotation·decorator로 stream node를 암시적으로
등록하는 표면은 없다. 축은 셋뿐이다 — node 이름, bind endpoint, session type. 그중 **bind
endpoint는 반드시 지정한다.**

다음 여덟은 첫 연결까지 미루지 않고 **host 시작 전에** 설정 오류로 막는다.

| 조건 |
| --- |
| node 이름이 비어 있다 |
| 같은 node 이름을 두 번 등록했다 |
| bind endpoint가 없다 |
| 같은 session type을 중복 등록했다 |
| 한 node에 session을 둘 이상 등록했다 |
| TLS를 켰는데 인증서 경로가 비어 있다 |
| TLS를 켰는데 key 경로가 비어 있다 |
| TLS server를 설정하지 않고 client 인증서를 요구했다 |

TLS를 켜면 인증서와 key 경로를 함께 지정한다. client 인증서 요구는 기본이 꺼짐이고,
켜면 검증에 실패한 연결은 **session을 만들기 전에** 거부한다.

## 2. Session lifecycle

Session은 연결, packet dispatch, 오류와 disconnect callback을 구현한다. 같은 session의 callback은
직렬로 실행된다.

> **샘플에서 보기 — [TicTacToe](../../../common/sample/tictactoe/README.ko.md).** client
> 연결 하나를 대표하는 session이다. 인증 packet을 먼저 거르고 나머지는 Actor로 relay한다.
> 저장소의 실제 코드다.

=== "C#/.NET"

    ```csharp
    --8<-- "framework/languages/dotnet/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/PlaySession.cs:doc-session"
    ```

=== "C++"

    ```cpp
    --8<-- "framework/languages/cpp/samples/TicTacToe/Server/Play/Infrastructure/ZLink/Sessions/play_session.hpp:doc-session"
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/samples/java/TicTacToe/Server/src/main/java/systems/zlink/samples/tictactoe/server/play/infrastructure/zlink/sessions/PlaySession.java:doc-session"
    ```

=== "Kotlin"

    ```kotlin
    --8<-- "framework/languages/java/samples/kotlin/TicTacToe/Server/src/main/kotlin/systems/zlink/samples/kotlin/tictactoe/server/play/infrastructure/zlink/sessions/PlaySession.kt:doc-session"
    ```

=== "Node/TypeScript"

    ```typescript
    --8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Sessions/play-session.ts:doc-session"
    ```

최소 형태로 보면 이렇다.

=== "C#/.NET"

    ```csharp
    public sealed class PlaySession(
        IZLinkSessionContext context,
        ILogger<PlaySession> logger) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddHandler<PingHandler>(); // typed session packet handler를 등록한다.
        }

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            logger.LogInformation("connected: {SessionId}", Context.SessionId);
            return ValueTask.CompletedTask;
        }

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken)
        {
            if (!await Context.Handlers.TryHandleAsync(
                    dispatch,
                    payload,
                    cancellationToken))
            {
                await Context.CloseAsync(); // application protocol에 없는 packet을 받으면 연결을 닫는다.
            }
        }

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken)
        {
            logger.LogWarning(
                "session error: {Error} {Message}",
                error.Error,
                error.Message);
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
        {
            logger.LogInformation("disconnected: {SessionId}", Context.SessionId);
            return ValueTask.CompletedTask;
        }
    }
    ```

=== "C++"

    ```cpp
    // C++ session은 packet_stream_session_t를 상속하고 callback을 override한다.
    class play_session_t : public packet_stream_session_t
    {
      public:
        task_t<void> on_connected (stream_t &stream) override
        {
            _logger.info ("connected");
            co_return;
        }

        task_t<void> on_packet (stream_t &stream,
                                const stream_dispatch_context_t &dispatch,
                                const zlink::message_t &payload) override
        {
            if (!_ping.can_handle (dispatch)) {
                // application protocol에 없는 packet을 받으면 연결을 닫는다.
                co_await stream.close ();
                co_return;
            }
            co_await _ping.handle (stream, payload);
        }

        task_t<void> on_error (stream_t &, const stream_error_t &error) override
        {
            _logger.warn (std::string ("session error: ") + error.message);
            co_return;
        }

        task_t<void> on_disconnected (stream_t &) override
        {
            _logger.info ("disconnected");
            co_return;
        }
    };
    ```

=== "Java"

    ```java
    public final class PlaySession implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final Logger logger;

        @Override
        public void configure() {
            context.handlers().addHandler(PingHandler.class); // typed session packet handler를 등록한다.
        }

        @Override
        public CompletionStage<Void> onConnected() {
            logger.info("connected: {}", context.sessionId());
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
            ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            return context.handlers().tryHandle(context, dispatch, payload).thenCompose(handled -> handled
                ? CompletableFuture.<Void>completedFuture(null)
                // application protocol에 없는 packet을 받으면 연결을 닫는다.
                : context.close().toCompletableFuture());
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            logger.info("disconnected: {}", context.sessionId());
            return CompletableFuture.completedFuture(null);
        }
    }
    ```

=== "Kotlin"

    ```kotlin
    class PlaySession(
        private val context: ZLinkSessionContext,
        private val logger: Logger,
    ) : ZLinkSession {

        override fun configure() {
            context.handlers().addHandler(PingHandler::class.java) // typed session packet handler를 등록한다.
        }

        override suspend fun onConnected() {
            logger.info("connected: {}", context.sessionId())
        }

        override suspend fun onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
            if (context.handlers().tryHandle(context, dispatch, payload).await()) return
            // application protocol에 없는 packet을 받으면 연결을 닫는다.
            context.close().await()
        }

        override suspend fun onDisconnected() {
            logger.info("disconnected: {}", context.sessionId())
        }
    }
    ```

=== "Node/TypeScript"

    ```typescript
    export class PlaySession implements ZLinkSession {
      constructor(
        private readonly context: ZLinkSessionContext,
        private readonly logger: Logger
      ) {}

      configure(): void {
        this.context.handlers.addHandler(PingHandler); // typed session packet handler를 등록한다.
      }

      async onConnected(): Promise<void> {
        this.logger.log(`connected: ${this.context.sessionId}`);
      }

      async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage): Promise<void> {
        if (await this.context.handlers.tryHandle(dispatch, payload)) return;
        // application protocol에 없는 packet을 받으면 연결을 닫는다.
        await this.context.close();
      }

      async onDisconnected(): Promise<void> {
        this.logger.log(`disconnected: ${this.context.sessionId}`);
      }
    }
    ```


오류가 어디로 가는지는 넷으로 갈린다. **session 오류 callback은 그 session에 귀속되는
transport 오류만 받는다.**

| 오류 | 어디로 가나 |
| --- | --- |
| 그 session의 transport 오류 | session 오류 callback |
| handshake 실패 | runtime monitoring. session이 만들어지기 전이라 부를 대상이 없다 |
| socket · node 단위 오류 | runtime monitoring. session 하나의 오류로 확정할 수 없다 |
| application handler 예외 | handler 예외 처리 경로. **session 오류 callback이 아니다** |

**handler filter는 session dispatch에 적용되지 않는다.** 다른 dispatch에 걸어 둔 filter가
있어도 session callback 앞에서는 돌지 않는다. 인증처럼 session 경로에서 걸러야 하는 일은
session의 handler 등록으로 처리한다.

**recv loop를 직접 도는 표면은 없다.** Framework가 packet을 queue에 넣은 뒤 session
callback을 실행하며, 그 경계에서 dispatch · DI · logging을 일관되게 적용한다. loop ·
취소 · backpressure를 application이 떠안지 않게 하려는 설계다.

## 3. Typed packet handler

Handler registry가 수신 message를 typed message로 decode한다. Request에 reply할 때는 현재 dispatch의
one-shot reply token을 사용한다.

=== "C#/.NET"

    ```csharp
    public sealed class PingHandler
        : IZLinkSessionPacketHandler<IZLinkSessionContext, Ping>
    {
        public async ValueTask HandleAsync(
            IZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            Ping message,
            CancellationToken cancellationToken)
        {
            if (!dispatch.CanReply)
            {
                throw new InvalidOperationException("Ping must be a request.");
            }

            await context.Client
                .Reply(new Pong(message.Sequence))
                .Async(cancellationToken); // 같은 request correlation으로 한 번만 reply한다.
        }
    }
    ```

=== "C++"

    ```cpp
    // C++ typed handler는 stream과 decode된 payload 둘만 받는다. dispatch context는
    // raw on_packet 경로에만 전달되므로 여기서 can_reply를 확인하지 않는다.
    task_t<void> handle (stream_t &stream, const ping_t &message)
    {
        // 같은 request correlation으로 한 번만 reply한다. request가 아니면 실패로 끝난다.
        co_await stream.reply_packet (zlink::message_t::from_json (pong_t{message.sequence}))
          .submit ();
    }
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/zlink-framework-doc-examples/src/main/java/systems/zlink/framework/docexamples/stream/PingHandler.java:typed-packet-handler"
    ```

=== "Kotlin"

    ```kotlin
    suspend fun handle(
        context: ZLinkSessionContext, dispatch: ZLinkSessionDispatchContext, message: Ping) {
        check(dispatch.canReply()) { "Ping must be a request." }

        // 같은 request correlation으로 한 번만 reply한다.
        context.client().reply(Pong(message.sequence)).submit().await()
    }
    ```

=== "Node/TypeScript"

    ```typescript
    async handle(
      context: ZLinkSessionContext, dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
      if (!dispatch.canReply) throw new Error('Ping must be a request.');

      const message = payload.decode<Ping>(Object as never);
      // 같은 request correlation으로 한 번만 reply한다.
      await context.client.reply(pong(message.sequence)).submit();
    }
    ```


`Reply`는 현재 request에서만 유효하며 한 번 제출할 수 있다. Timeout이나 cancellation으로 전송이
실패해도 같은 reply token을 다시 사용할 수 없다.

**응답에는 packet 이름이 실리지 않는다.** client는 request sequence만으로 대기 중인 요청을
찾고, 응답을 어떤 타입으로 읽을지는 **호출할 때 지정한 타입**이 정한다. 이름으로 고르지
않으므로 응답 쪽에 packet 이름을 붙이는 표면도 없다. 오류 응답도 같은 sequence로 돌아온다.

Server가 먼저 push할 때는 `Send`를 사용한다.

=== "C#/.NET"

    ```csharp
    await Context.Client
        .Send(new ServerNotice("maintenance"))
        .Metadata("severity", "info")
        .Compress()
        .Async(cancellationToken); // local transport queue admission까지 기다린다.
    ```

=== "C++"

    ```cpp
    // local transport queue admission까지 기다린다.
    co_await stream.send (server_notice_t{"maintenance"})
      .metadata ("severity", "info")
      .compress ()
      .submit ();
    ```

=== "Java"

    ```java
    --8<-- "framework/languages/java/zlink-framework-doc-examples/src/main/java/systems/zlink/framework/docexamples/stream/StreamNodeRegistration.java:server-push"
    ```

=== "Kotlin"

    ```kotlin
    // local transport queue admission까지 기다린다.
    context.client()
        .send(ServerNotice("maintenance"))
        .metadata("severity", "info")
        .compress()
        .submit()
        .await()
    ```

=== "Node/TypeScript"

    ```typescript
    // local transport queue admission까지 기다린다.
    await context.client
      .send(serverNotice('maintenance'))
      .metadata('severity', 'info')
      .compress()
      .submit();
    ```


## 4. Actor dispatch

인증 뒤 Actor를 session에 bind하고, session 전용 handler가 처리하지 않은 message를
session actor의 relay 호출로 넘길 수 있다. 상세 흐름은
[Session과 Actor binding](08-actor-session.ko.md)을 따른다.

Application은 session route를 Location Store에서 직접 조회하지 않는다. Actor relocation이 완료되면
Framework가 binding route를 갱신한다.

## 5. Client 연결

Client는 server Framework package가 아니라 Stream Connector package를 사용한다.

=== "C#/.NET"

    ```csharp
    await using var connector = ZlinkStreamConnectorFactory.Create(
        new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://game.example.com:9100"),
            DispatchMode = ZlinkStreamDispatchMode.Manual
        });

    connector.On<GameStateNotify>("GameStateNotify", (message, cancellationToken) =>
    {
        Render(message.Payload);
        return ValueTask.CompletedTask;
    });

    await connector.Connect.Async(cancellationToken); // 연결과 receive loop 준비를 완료한다.

    while (running)
    {
        await connector.Dispatch.Async(cancellationToken); // Manual 모드는 이 caller에서 callback을 실행한다.
    }
    ```

=== "C++"

    ```cpp
    zlink::stream_connector::connector_options_t connector_options;
    connector_options.endpoint = "tcp://game.example.com:9100";
    connector_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    auto connector = zlink::stream_connector::connector_factory_t::create (connector_options);

    connector.on<game_state_notify_t> ("GameStateNotify",
                                       [] (const auto &message) { render (message.payload ()); });

    co_await connector.connect ().submit (); // 연결과 receive loop 준비를 완료한다.

    while (running) {
        co_await connector.dispatch ().submit (); // manual 모드는 이 caller에서 callback을 실행한다.
    }
    ```

=== "Java"

    ```java
    ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
        new ZLinkStreamConnectorOptions(
            URI.create("tcp://game.example.com:9100"),
            ZLinkStreamDispatchMode.MANUAL));

    connector.on(GameStateNotify.class, message -> {
        render(message.payload());
        return CompletableFuture.completedFuture(null);
    });

    connector.connect().submit().toCompletableFuture().join(); // 연결과 receive loop 준비를 완료한다.

    while (running) {
        // MANUAL 모드는 이 caller에서 callback을 실행한다.
        connector.dispatch().submit().toCompletableFuture().join();
    }
    ```

=== "Kotlin"

    ```kotlin
    val connector = ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            URI.create("tcp://game.example.com:9100"),
            ZLinkStreamDispatchMode.MANUAL))

    connector.on(GameStateNotify::class.java) { message ->
        render(message.payload())
        CompletableFuture.completedFuture(null)
    }

    connector.connect().submit().await() // 연결과 receive loop 준비를 완료한다.

    while (running) {
        // MANUAL 모드는 이 caller에서 callback을 실행한다.
        connector.dispatch().submit().await()
    }
    ```

=== "Node/TypeScript"

    ```typescript
    const client = zlinkStreamConnectorFactory.create({
      endpoint: 'tcp://game.example.com:9100',
      dispatchMode: ZlinkStreamDispatchMode.Manual
    });

    client.on(GameStateNotify, (message) => {
      render(message.payload);
    });

    await client.connect(); // 연결과 receive loop 준비를 완료한다.

    while (running) {
      await client.dispatch(); // Manual 모드는 이 caller에서 callback을 실행한다.
    }
    ```


게임 loop나 UI thread에서 callback을 실행해야 하면 `Manual`을 사용한다. `Immediate`는 connector의
worker에서 callback을 실행하므로 thread affinity가 필요한 client에는 적합하지 않다.

## 6. Client send와 request

=== "C#/.NET"

    ```csharp
    await connector
        .Send(new PlayerInput(direction))
        .Async(cancellationToken); // bounded outbound queue admission까지 기다린다.

    Profile profile = await connector
        .Request(new GetProfile(playerId))
        .Async<Profile>(cancellationToken); // request sequence로 response를 찾는다.
    ```

=== "C++"

    ```cpp
    // bounded outbound queue admission까지 기다린다.
    co_await connector.send (player_input_t{direction}).submit ();

    // request sequence로 response를 찾는다.
    auto profile = co_await connector.request (get_profile_t{player_id}).submit<profile_t> ();
    ```

=== "Java"

    ```java
    // bounded outbound queue admission까지 기다린다.
    connector.send(new PlayerInput(direction)).submit().toCompletableFuture().join();

    // request sequence로 response를 찾는다.
    Profile profile = connector
        .request(new GetProfile(playerId))
        .submit(Profile.class)
        .toCompletableFuture().join();
    ```

=== "Kotlin"

    ```kotlin
    // bounded outbound queue admission까지 기다린다.
    connector.send(PlayerInput(direction)).submit().await()

    // request sequence로 response를 찾는다.
    val profile = connector.request(GetProfile(playerId)).submit(Profile::class.java).await()
    ```

=== "Node/TypeScript"

    ```typescript
    // bounded outbound queue admission까지 기다린다.
    await client.send(playerInput(direction)).submit();

    // request sequence로 response를 찾는다.
    const profile = await client.request(getProfile(playerId)).submit<Profile>();
    ```


Connector의 기본 typed codec은 JSON이다. Packet name override, push 대기, reconnect, heartbeat와
bounded queue 설정은 Stream Connector 가이드에서 설명한다.

## 7. 관련 문서

- 이 챕터 계약의 실행 검증 예문: `13. Interface 카탈로그` 장 §5 — 검증 클래스 `StreamContracts`
- Session과 Actor binding: [Session Actor Dispatch](08-actor-session.ko.md)
- Client connector 전체 사용법: Stream Connector 가이드
- Location Store와 자동 연결: [Location](10-location.ko.md)
