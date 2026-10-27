package MyGame.e2e

import MyGame.*
import java.net.URI
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.delay
import systems.zlink.framework.kotlin.*
import systems.zlink.stream.connector.*

const val MESSAGE_SECONDS = 10L

private fun createConnector(endpoint: String): ZLinkStreamConnector {
    val defaults = ZLinkStreamConnectorOptions.createDefault(URI(endpoint))
    return ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            URI(endpoint),
            ZLinkStreamDispatchMode.IMMEDIATE,
            Duration.ofSeconds(MESSAGE_SECONDS),
            defaults.maxReconnectAttempts(),
            defaults.connectTimeout(),
            defaults.maxSendPayloadSize(),
            defaults.heartbeatEnabled(),
            defaults.heartbeatInterval(),
            defaults.heartbeatTimeout(),
            false,
            defaults.reconnectInitialDelay(),
            defaults.reconnectMaxDelay(),
            defaults.reconnectBackoffFactor(),
        )
    )
}

class Client(endpoint: String, val playerId: String) {
    val connector = createConnector(endpoint)
    private val queue = LinkedBlockingQueue<Any>()
    private val pending = mutableListOf<Any>()
    private val registrations = mutableListOf<AutoCloseable>()
    @Volatile var disconnects = 0

    init {
        listOf(
                WorldState::class.java,
                PlayerAppeared::class.java,
                PlayerMoved::class.java,
                PlayerLeft::class.java,
                ActionRejected::class.java,
                DungeonOpened::class.java,
            )
            .forEach { type ->
                registrations +=
                    connector.on(type) { message ->
                        queue.add(message.payload())
                        CompletableFuture.completedFuture(null)
                    }
            }
        registrations +=
            connector.onDisconnected { event ->
                disconnects++
                queue.add(IllegalStateException("Disconnected: $event"))
                CompletableFuture.completedFuture(null)
            }
        registrations +=
            connector.onErrorReceived { event ->
                queue.add(IllegalStateException("Connector error: $event"))
                CompletableFuture.completedFuture(null)
            }
    }

    suspend fun connect() = connector.kotlin().connect().await()

    suspend fun join(x: Double): WorldState {
        send(JoinWorldRequest(playerId, x))
        return next<WorldState>()
    }

    suspend fun moveResult(delta: Double, sequence: Long): Any {
        send(MoveRequest(delta, sequence))
        return nextMovement(sequence)
    }

    suspend fun move(delta: Double, sequence: Long): WorldState {
        while (true) {
            when (val result = moveResult(delta, sequence)) {
                is WorldState -> {
                    check(result.sequence == sequence) { "Movement sequence changed" }
                    return result
                }
                is ActionRejected -> {
                    check(result.reason == "move-budget" && result.retryAfterMs > 0) {
                        "Movement rejected: ${result.reason}"
                    }
                    delay(result.retryAfterMs.toLong())
                }
                else -> error("Unexpected movement result: $result")
            }
        }
    }

    suspend fun moveRejected(delta: Double, sequence: Long): ActionRejected {
        val result = moveResult(delta, sequence)
        check(result is ActionRejected) { "Movement unexpectedly succeeded: $result" }
        return result
    }

    data class ImmediateMoveResponses(
        val accepted: WorldState,
        val rejected: ActionRejected,
    )

    suspend fun moveWithImmediateBudgetRejection(
        acceptedDelta: Double,
        acceptedSequence: Long,
        rejectedDelta: Double,
        rejectedSequence: Long,
    ): ImmediateMoveResponses {
        val accepted = CompletableFuture<WorldState>()
        val rejected = CompletableFuture<ActionRejected>()
        val acceptedRegistration =
            connector.on(WorldState::class.java) { message ->
                if (message.payload().sequence == acceptedSequence) accepted.complete(message.payload())
                CompletableFuture.completedFuture(null)
            }
        val rejectedRegistration =
            connector.on(ActionRejected::class.java) { message ->
                if (message.payload().reason == "move-budget") rejected.complete(message.payload())
                CompletableFuture.completedFuture(null)
            }
        try {
            send(MoveRequest(acceptedDelta, acceptedSequence))
            send(MoveRequest(rejectedDelta, rejectedSequence))
            accepted.get(MESSAGE_SECONDS, TimeUnit.SECONDS)
            rejected.get(MESSAGE_SECONDS, TimeUnit.SECONDS)
            return ImmediateMoveResponses(
                next<WorldState> { it.sequence == acceptedSequence },
                next<ActionRejected> { it.reason == "move-budget" },
            )
        } finally {
            acceptedRegistration.close()
            rejectedRegistration.close()
        }
    }

    suspend fun state(): WorldState {
        send(JoinWorldRequest(playerId, 0.0))
        return next<WorldState>()
    }

    suspend fun packetRejected(packet: Any): ActionRejected {
        send(packet)
        return next<ActionRejected> { it.reason == "packet-not-allowed" }
    }

    suspend fun dungeon(id: String): DungeonOpened {
        send(OpenDungeonRequest(id))
        return next<DungeonOpened> { it.dungeonId == id }
    }

    suspend fun sessionInfo(): SessionInfo =
        connector.kotlin().request<SessionInfo>(SessionInfoRequest()).await()

    suspend fun send(message: Any) = connector.kotlin().send(message).await()

    suspend fun close() {
        connector.kotlin().close().await()
        registrations.forEach { it.close() }
    }

    inline fun <reified T> next(noinline predicate: (T) -> Boolean = { true }): T =
        next(T::class.java, predicate)

    fun <T> next(type: Class<T>, predicate: (T) -> Boolean): T {
        val expires = System.nanoTime() + TimeUnit.SECONDS.toNanos(MESSAGE_SECONDS)
        while (true) {
            val index = pending.indexOfFirst { type.isInstance(it) && predicate(type.cast(it)) }
            if (index >= 0) return type.cast(pending.removeAt(index))
            val event =
                queue.poll((expires - System.nanoTime()).coerceAtLeast(0), TimeUnit.NANOSECONDS)
                    ?: error("Timed out waiting for ${type.simpleName}, pending=$pending")
            if (event is Throwable) throw event
            pending.add(event)
        }
    }

    private fun nextMovement(sequence: Long): Any {
        val expires = System.nanoTime() + TimeUnit.SECONDS.toNanos(MESSAGE_SECONDS)
        while (true) {
            val index = pending.indexOfFirst {
                (it is WorldState && it.sequence == sequence) || it is ActionRejected
            }
            if (index >= 0) return pending.removeAt(index)
            val event =
                queue.poll((expires - System.nanoTime()).coerceAtLeast(0), TimeUnit.NANOSECONDS)
                    ?: error("Timed out waiting for movement result, pending=$pending")
            if (event is Throwable) throw event
            pending.add(event)
        }
    }
}
