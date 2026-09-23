package systems.zlink.tutorial.streamclient;

import systems.zlink.stream.connector.ZLinkStreamActor;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.stream.connector.ZLinkStreamMessage;
import systems.zlink.tutorial.shared.Contracts;

import java.net.URI;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;

public final class StreamClientProgram {

    private StreamClientProgram() {}

    public static void main(String[] args) throws Exception {
        // --8<-- [start:stream-client]
        // A game client outside the mesh. It references the connector only, never
        // the Framework, and speaks to the port the stream node opened.
        ZLinkStreamConnector connector =
                ZLinkStreamConnectorFactory.create(
                        new ZLinkStreamConnectorOptions(
                                URI.create("tcp://127.0.0.1:7521"),
                                ZLinkStreamDispatchMode.IMMEDIATE,
                                Duration.ofSeconds(5),
                                1));

        connector.connect().submit().toCompletableFuture().join();
        System.out.println("connected: " + connector.isConnected());

        // A request waits for its reply. Use send for one-way traffic; the server
        // then answers with client().send rather than reply.
        long sentAt = System.currentTimeMillis();
        Contracts.Pong pong =
                connector
                        .request(new Contracts.Ping(Long.toString(sentAt)))
                        .timeout(Duration.ofSeconds(5))
                        .submit(Contracts.Pong.class)
                        .toCompletableFuture()
                        .join();

        System.out.println(
                "round trip: "
                        + (System.currentTimeMillis() - Long.parseLong(pong.sentAtUnixMs()))
                        + "ms");
        // --8<-- [end:stream-client]

        // --8<-- [start:session-actor-client]
        // --8<-- [start:actor-handle-events]
        AutoCloseable boundNotice =
                connector.onActorBound(
                        actor -> {
                            System.out.println("actor bound: " + actor.actorId());
                            return java.util.concurrent.CompletableFuture.completedFuture(null);
                        });
        AutoCloseable unboundNotice =
                connector.onActorUnbound(
                        actor -> {
                            System.out.println("actor unbound: " + actor.actorId());
                            return java.util.concurrent.CompletableFuture.completedFuture(null);
                        });
        // --8<-- [end:actor-handle-events]
        // With one Actor bound, the connector can send without an Actor handle.
        Contracts.Authenticated authenticatedP1 =
                connector
                        .request(new Contracts.Authenticate("p1"))
                        .timeout(Duration.ofSeconds(5))
                        .submit(Contracts.Authenticated.class)
                        .toCompletableFuture()
                        .join();

        System.out.println("bound player: " + authenticatedP1.playerId());

        // --8<-- [start:single-actor-send]
        CompletableFuture<ZLinkStreamMessage<Contracts.NicknameChanged>> singleChanged =
                new CompletableFuture<>();
        AutoCloseable singleReceive =
                connector.on(
                        Contracts.NicknameChanged.class,
                        message -> {
                            singleChanged.complete(message);
                            return CompletableFuture.completedFuture(null);
                        });
        connector
                .send(new Contracts.ChangeNickname("speedy"))
                .submit()
                .toCompletableFuture()
                .join();
        ZLinkStreamMessage<Contracts.NicknameChanged> singlePush = singleChanged.join();
        System.out.println(
                "pushed: " + singlePush.payload().nickname() + ", actor: " + singlePush.actorId());
        singleReceive.close();
        // --8<-- [end:single-actor-send]

        // A second Actor on the same connection calls for explicit handles.
        Contracts.Authenticated authenticatedP2 =
                connector
                        .request(new Contracts.Authenticate("p2"))
                        .timeout(Duration.ofSeconds(5))
                        .submit(Contracts.Authenticated.class)
                        .toCompletableFuture()
                        .join();
        System.out.println("bound player: " + authenticatedP2.playerId());

        // --8<-- [start:actor-handle-send]
        ZLinkStreamActor playerP1 = connector.actor(authenticatedP1.playerId()).orElseThrow();
        ZLinkStreamActor playerP2 = connector.actor(authenticatedP2.playerId()).orElseThrow();
        System.out.println("actor handle: " + playerP1.actorId());
        System.out.println("actor handle: " + playerP2.actorId());
        // --8<-- [end:actor-handle-send]

        // Register on each handle before sending. Each callback sees only that
        // Actor's pushes, although both use the same connection and packet type.
        // --8<-- [start:actor-handle-per-handle-receive]
        CompletableFuture<ZLinkStreamMessage<Contracts.NicknameChanged>> changedP1 =
                new CompletableFuture<>();
        CompletableFuture<ZLinkStreamMessage<Contracts.NicknameChanged>> changedP2 =
                new CompletableFuture<>();
        AutoCloseable receiveP1 =
                playerP1.on(
                        Contracts.NicknameChanged.class,
                        message -> {
                            changedP1.complete(message);
                            return CompletableFuture.completedFuture(null);
                        });
        AutoCloseable receiveP2 =
                playerP2.on(
                        Contracts.NicknameChanged.class,
                        message -> {
                            changedP2.complete(message);
                            return CompletableFuture.completedFuture(null);
                        });
        // --8<-- [end:actor-handle-per-handle-receive]

        // --8<-- [start:actor-id-receive]
        // Connector-level callbacks can distinguish the same pushes by ActorId.
        AutoCloseable receiveActorIds =
                connector.on(
                        Contracts.NicknameChanged.class,
                        message -> {
                            System.out.println("received actor id: " + message.actorId());
                            return CompletableFuture.completedFuture(null);
                        });
        // --8<-- [end:actor-id-receive]

        // The handle addresses this player directly. Its handler pushes the
        // result back over the same connection.
        // --8<-- [start:actor-handle-send-call]
        playerP1.send(new Contracts.ChangeNickname("speedy-p1"))
                .submit()
                .toCompletableFuture()
                .join();
        playerP2.send(new Contracts.ChangeNickname("speedy-p2"))
                .submit()
                .toCompletableFuture()
                .join();
        // --8<-- [end:actor-handle-send-call]

        // --8<-- [start:actor-handle-receive]
        ZLinkStreamMessage<Contracts.NicknameChanged> pushedP1 = changedP1.join();
        System.out.println(
                "pushed: " + pushedP1.payload().nickname() + ", actor: " + pushedP1.actorId());
        ZLinkStreamMessage<Contracts.NicknameChanged> pushedP2 = changedP2.join();
        System.out.println(
                "pushed: " + pushedP2.payload().nickname() + ", actor: " + pushedP2.actorId());
        // --8<-- [end:actor-handle-receive]
        // --8<-- [end:session-actor-client]

        receiveP1.close();
        receiveP2.close();
        receiveActorIds.close();
        boundNotice.close();
        unboundNotice.close();
        connector.close().submit().toCompletableFuture().join();
    }
}
