package systems.zlink.tutorial.streamclient

import java.net.URI
import java.time.Duration
import kotlinx.coroutines.async
import kotlinx.coroutines.runBlocking
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode
import systems.zlink.tutorial.shared.Authenticate
import systems.zlink.tutorial.shared.Authenticated
import systems.zlink.tutorial.shared.ChangeNickname
import systems.zlink.tutorial.shared.NicknameChanged
import systems.zlink.tutorial.shared.Ping
import systems.zlink.tutorial.shared.Pong

fun main() = runBlocking {
    // --8<-- [start:stream-client]
    // A game client outside the mesh. It references the connector only, never the
    // Framework, and speaks to the port the stream node opened.
    val connector =
        ZLinkStreamConnectorFactory.create(
                ZLinkStreamConnectorOptions(
                    URI.create("tcp://127.0.0.1:7621"),
                    ZLinkStreamDispatchMode.IMMEDIATE,
                    Duration.ofSeconds(5),
                    1,
                )
            )
            .kotlin()

    connector.connect().await()
    println("connected: ${connector.isConnected}")

    // A request waits for its reply. Use send for one-way traffic; the server
    // then answers with client().send rather than reply.
    val sentAt = System.currentTimeMillis()
    val pong =
        connector.request(Ping(sentAt.toString())).timeout(Duration.ofSeconds(5)).awaitReply<Pong>()

    println("round trip: ${System.currentTimeMillis() - pong.sentAtUnixMs.toLong()}ms")
    // --8<-- [end:stream-client]

    // --8<-- [start:session-actor-client]
    // Binds this connection to a player. Until then the server has no player to
    // forward packets to.
    val authenticated =
        connector
            .request(Authenticate("p1"))
            .timeout(Duration.ofSeconds(5))
            .awaitReply<Authenticated>()

    println("bound player: ${authenticated.playerId}")

    // Arrange to receive the push before sending, so a fast server cannot answer
    // before the client is listening.
    val changed = async {
        connector.waitFor<NicknameChanged>().timeout(Duration.ofSeconds(5)).await()
    }

    // No session handler matches this packet, so the session relays it to the
    // bound player, whose handler pushes the result back over this same
    // connection.
    connector.send(ChangeNickname("speedy")).await()

    println("pushed: ${changed.await().payload().nickname}")
    // --8<-- [end:session-actor-client]

    connector.close().await()
}
