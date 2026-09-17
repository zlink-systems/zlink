package systems.zlink.tutorial.streamclient;

import java.net.URI;
import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.stream.connector.ZLinkStreamMessage;
import systems.zlink.tutorial.shared.Contracts;

public final class StreamClientProgram {

    private StreamClientProgram() {
    }

    public static void main(String[] args) {
        // --8<-- [start:stream-client]
        // A game client outside the mesh. It references the connector only, never
        // the Framework, and speaks to the port the stream node opened.
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
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
        Contracts.Pong pong = connector
            .request(new Contracts.Ping(Long.toString(sentAt)))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.Pong.class)
            .toCompletableFuture()
            .join();

        System.out.println(
            "round trip: " + (System.currentTimeMillis() - Long.parseLong(pong.sentAtUnixMs())) + "ms");
        // --8<-- [end:stream-client]

        // --8<-- [start:session-actor-client]
        // Binds this connection to a player. Until then the server has no player
        // to forward packets to.
        Contracts.Authenticated authenticated = connector
            .request(new Contracts.Authenticate("p1"))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.Authenticated.class)
            .toCompletableFuture()
            .join();

        System.out.println("bound player: " + authenticated.playerId());

        // Arrange to receive the push before sending, so a fast server cannot
        // answer before the client is listening.
        CompletionStage<ZLinkStreamMessage<Contracts.NicknameChanged>> changed = connector
            .waitFor(Contracts.NicknameChanged.class)
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.NicknameChanged.class);

        // No session handler matches this packet, so the session relays it to the
        // bound player, whose handler pushes the result back over this same
        // connection.
        connector.send(new Contracts.ChangeNickname("speedy")).submit().toCompletableFuture().join();

        System.out.println("pushed: " + changed.toCompletableFuture().join().payload().nickname());
        // --8<-- [end:session-actor-client]

        connector.close().submit().toCompletableFuture().join();
    }
}
