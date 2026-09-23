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
import java.util.concurrent.CompletionStage;

public final class StreamClientProgram {

    private StreamClientProgram() {}

    public static void main(String[] args) {
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
        // Binds this connection to a player. Until then the server has no player
        // to forward packets to.
        Contracts.Authenticated authenticated =
                connector
                        .request(new Contracts.Authenticate("p1"))
                        .timeout(Duration.ofSeconds(5))
                        .submit(Contracts.Authenticated.class)
                        .toCompletableFuture()
                        .join();

        System.out.println("bound player: " + authenticated.playerId());

        // --8<-- [start:actor-handle-send]
        ZLinkStreamActor player = connector.actor(authenticated.playerId()).orElseThrow();
        System.out.println("actor handle: " + player.actorId());
        // --8<-- [end:actor-handle-send]

        // Arrange to receive the push before sending, so a fast server cannot
        // answer before the client is listening.
        CompletionStage<ZLinkStreamMessage<Contracts.NicknameChanged>> changed =
                connector
                        .waitFor(Contracts.NicknameChanged.class)
                        .timeout(Duration.ofSeconds(5))
                        .submit(Contracts.NicknameChanged.class);

        // The handle addresses this player directly. Its handler pushes the
        // result back over the same connection.
        // --8<-- [start:actor-handle-send-call]
        player.send(new Contracts.ChangeNickname("speedy")).submit().toCompletableFuture().join();
        // --8<-- [end:actor-handle-send-call]

        // --8<-- [start:actor-handle-receive]
        ZLinkStreamMessage<Contracts.NicknameChanged> pushed = changed.toCompletableFuture().join();
        System.out.println(
                "pushed: " + pushed.payload().nickname() + ", actor: " + pushed.actorId());
        // --8<-- [end:actor-handle-receive]
        // --8<-- [end:session-actor-client]

        connector.close().submit().toCompletableFuture().join();
    }
}
