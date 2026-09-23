package systems.zlink.tutorial.streamclient

import java.net.URI
import java.time.Duration
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.request
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
        connector.request<Pong>(Ping(sentAt.toString())).timeout(Duration.ofSeconds(5)).await()

    println("round trip: ${System.currentTimeMillis() - pong.sentAtUnixMs.toLong()}ms")
    // --8<-- [end:stream-client]

    // --8<-- [start:session-actor-client]
    // --8<-- [start:actor-handle-events]
    val boundNotice =
        launch(start = CoroutineStart.UNDISPATCHED) {
            connector.actorBound().collect { actor -> println("actor bound: ${actor.actorId}") }
        }
    val unboundNotice =
        launch(start = CoroutineStart.UNDISPATCHED) {
            connector.actorUnbound().collect { actor -> println("actor unbound: ${actor.actorId}") }
        }
    // --8<-- [end:actor-handle-events]
    // Binds this connection to a player. Until then the server has no player to
    // forward packets to.
    val authenticated =
        connector.request<Authenticated>(Authenticate("p1")).timeout(Duration.ofSeconds(5)).await()

    println("bound player: ${authenticated.playerId}")

    // --8<-- [start:actor-handle-send]
    val player = requireNotNull(connector.actor(authenticated.playerId))
    println("actor handle: ${player.actorId}")
    // --8<-- [end:actor-handle-send]

    // Arrange to receive the push before sending, so a fast server cannot answer
    // before the client is listening.
    val changed = async {
        connector.waitFor<NicknameChanged>().timeout(Duration.ofSeconds(5)).await()
    }

    // The handle addresses this player directly. Its handler pushes the
    // result back over the same connection.
    // --8<-- [start:actor-handle-send-call]
    player.send(ChangeNickname("speedy")).await()
    // --8<-- [end:actor-handle-send-call]

    // --8<-- [start:actor-handle-receive]
    val pushed = changed.await()
    println("pushed: ${pushed.payload().nickname}, actor: ${pushed.actorId()}")
    // --8<-- [end:actor-handle-receive]
    // --8<-- [end:session-actor-client]

    connector.close().await()
    boundNotice.cancelAndJoin()
    unboundNotice.cancelAndJoin()
}
