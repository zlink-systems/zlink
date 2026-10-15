package MyGame.e2e

import MyGame.*
import java.net.URI
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
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
                BorderVisible::class.java,
                DungeonView::class.java,
                FieldView::class.java,
                ActionRejected::class.java,
            )
            .forEach { type ->
                registrations +=
                    connector.on(type) { message ->
                        val payload = message.payload()
                        queue.add(
                            if (payload is ActionRejected)
                                IllegalStateException("Action rejected: ${payload.reason}")
                            else payload
                        )
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

    suspend fun join(x: Int): WorldState {
        send(JoinWorldRequest(playerId, x))
        return next<WorldState>()
    }

    suspend fun move(x: Int, sequence: Int): WorldState {
        send(MoveRequest(x, sequence))
        val state = next<WorldState> { it.sequence == sequence }
        check(state.x == x && state.sequence == sequence) { "Movement lost or changed" }
        return state
    }

    suspend fun dungeon(): DungeonView {
        send(ViewRequest())
        return next<DungeonView>()
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
}
