package systems.zlink.tutorial.streamclient

import java.net.URI
import java.time.Duration
import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.request
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode
import systems.zlink.stream.connector.ZLinkStreamMessage
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
    // With one Actor bound, the connector can send without an Actor handle.
    val authenticatedP1 =
        connector.request<Authenticated>(Authenticate("p1")).timeout(Duration.ofSeconds(5)).await()

    println("bound player: ${authenticatedP1.playerId}")

    // --8<-- [start:single-actor-send]
    val singleChanged = CompletableFuture<ZLinkStreamMessage<NicknameChanged>>()
    val singleReceive =
        connector.on<NicknameChanged> { message ->
            singleChanged.complete(message)
            CompletableFuture.completedFuture(null)
        }
    connector.send(ChangeNickname("speedy")).await()
    val pushed = singleChanged.await()
    println("pushed: ${pushed.payload().nickname}, actor: ${pushed.actorId()}")
    singleReceive.close()
    // --8<-- [end:single-actor-send]
    // --8<-- [end:session-actor-client]

    boundNotice.cancelAndJoin()
    unboundNotice.cancelAndJoin()
    connector.close().await()
}
