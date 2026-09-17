package systems.zlink.tutorial.streamclient

import java.net.URI
import java.time.Duration
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode
import systems.zlink.tutorial.shared.Authenticate
import systems.zlink.tutorial.shared.Authenticated
import systems.zlink.tutorial.shared.ChangeNickname
import systems.zlink.tutorial.shared.NicknameChanged
import systems.zlink.tutorial.shared.Ping
import systems.zlink.tutorial.shared.Pong

fun main() {
    // --8<-- [start:stream-client]
    // A game client outside the mesh. It references the connector only, never the
    // Framework, and speaks to the port the stream node opened. The connector has
    // no Kotlin wrapper and this process runs no coroutines, so its calls end in
    // join() on the Java CompletionStage.
    val connector = ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            URI.create("tcp://127.0.0.1:7621"),
            ZLinkStreamDispatchMode.IMMEDIATE,
            Duration.ofSeconds(5),
            1,
        ),
    )

    connector.connect().submit().toCompletableFuture().join()
    println("connected: ${connector.isConnected}")

    // A request waits for its reply. Use send for one-way traffic; the server
    // then answers with client().send rather than reply.
    val sentAt = System.currentTimeMillis()
    val pong = connector
        .request(Ping(sentAt.toString()))
        .timeout(Duration.ofSeconds(5))
        .submit(Pong::class.java)
        .toCompletableFuture()
        .join()

    println("round trip: ${System.currentTimeMillis() - pong.sentAtUnixMs.toLong()}ms")
    // --8<-- [end:stream-client]

    // --8<-- [start:session-actor-client]
    // Binds this connection to a player. Until then the server has no player to
    // forward packets to.
    val authenticated = connector
        .request(Authenticate("p1"))
        .timeout(Duration.ofSeconds(5))
        .submit(Authenticated::class.java)
        .toCompletableFuture()
        .join()

    println("bound player: ${authenticated.playerId}")

    // Arrange to receive the push before sending, so a fast server cannot answer
    // before the client is listening.
    val changed = connector
        .waitFor(NicknameChanged::class.java)
        .timeout(Duration.ofSeconds(5))
        .submit(NicknameChanged::class.java)
        .toCompletableFuture()

    // No session handler matches this packet, so the session relays it to the
    // bound player, whose handler pushes the result back over this same
    // connection.
    connector.send(ChangeNickname("speedy")).submit().toCompletableFuture().join()

    println("pushed: ${changed.join().payload().nickname}")
    // --8<-- [end:session-actor-client]

    connector.close().submit().toCompletableFuture().join()
}
