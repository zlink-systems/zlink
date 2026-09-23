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

    // A second Actor on the same connection calls for explicit handles.
    val authenticatedP2 =
        connector.request<Authenticated>(Authenticate("p2")).timeout(Duration.ofSeconds(5)).await()
    println("bound player: ${authenticatedP2.playerId}")

    // --8<-- [start:actor-handle-send]
    val playerP1 = requireNotNull(connector.actor(authenticatedP1.playerId))
    val playerP2 = requireNotNull(connector.actor(authenticatedP2.playerId))
    println("actor handle: ${playerP1.actorId}")
    println("actor handle: ${playerP2.actorId}")
    // --8<-- [end:actor-handle-send]

    // Each callback receives only the push for its handle's Actor.
    // --8<-- [start:actor-handle-per-handle-receive]
    val changedP1 = CompletableFuture<ZLinkStreamMessage<NicknameChanged>>()
    val changedP2 = CompletableFuture<ZLinkStreamMessage<NicknameChanged>>()
    val receiveP1 =
        playerP1.on<NicknameChanged> { message ->
            changedP1.complete(message)
            CompletableFuture.completedFuture(null)
        }
    val receiveP2 =
        playerP2.on<NicknameChanged> { message ->
            changedP2.complete(message)
            CompletableFuture.completedFuture(null)
        }
    // --8<-- [end:actor-handle-per-handle-receive]

    // --8<-- [start:actor-id-receive]
    // Connector-level callbacks can distinguish the same pushes by ActorId.
    val receiveActorIds =
        connector.on<NicknameChanged> { message ->
            println("received actor id: ${message.actorId()}")
            CompletableFuture.completedFuture(null)
        }
    // --8<-- [end:actor-id-receive]

    // Each handle sends to its own player over the same connection.
    // --8<-- [start:actor-handle-send-call]
    playerP1.send(ChangeNickname("speedy-p1")).await()
    playerP2.send(ChangeNickname("speedy-p2")).await()
    // --8<-- [end:actor-handle-send-call]

    // --8<-- [start:actor-handle-receive]
    val pushedP1 = changedP1.await()
    val pushedP2 = changedP2.await()
    println("pushed: ${pushedP1.payload().nickname}, actor: ${pushedP1.actorId()}")
    println("pushed: ${pushedP2.payload().nickname}, actor: ${pushedP2.actorId()}")
    // --8<-- [end:actor-handle-receive]
    // --8<-- [end:session-actor-client]

    receiveP1.close()
    receiveP2.close()
    receiveActorIds.close()
    boundNotice.cancelAndJoin()
    unboundNotice.cancelAndJoin()
    connector.close().await()
}
