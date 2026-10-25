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
        ),
    )
}

class Client(endpoint: String, val playerId: String) {
    val connector = createConnector(endpoint)
    private val incoming = LinkedBlockingQueue<Any>()
    private val pending = mutableListOf<Any>()
    private val registrations = mutableListOf<AutoCloseable>()
    @Volatile var disconnects = 0
        private set
    private var closed = false

    init {
        listOf(
                RoomView::class.java,
                MatchFound::class.java,
                MatchEnded::class.java,
                MoveResult::class.java,
                ShotResult::class.java,
                ShotRejected::class.java,
                ActionRejected::class.java,
            )
            .forEach { type ->
                registrations += connector.on(type) { message ->
                    incoming.add(message.payload())
                    CompletableFuture.completedFuture(null)
                }
            }
        registrations += connector.onDisconnected { event ->
            disconnects++
            incoming.add(IllegalStateException("Disconnected: $event"))
            CompletableFuture.completedFuture(null)
        }
        registrations += connector.onErrorReceived { event ->
            incoming.add(IllegalStateException("Connector error: $event"))
            CompletableFuture.completedFuture(null)
        }
    }

    suspend fun connect() = connector.kotlin().connect().await()

    suspend fun queue(mode: String = Names.DEFAULT_MODE) = send(QueueRequest(playerId, mode))

    suspend fun send(message: Any) = connector.kotlin().send(message).await()

    suspend fun action(message: Any): Any {
        send(message)
        return nextAny {
            when (message) {
                is MoveRequest -> it is ActionRejected || it is MoveResult && it.sequence == message.sequence
                is ShootRequest -> it is ShotRejected || it is ShotResult && it.sequence == message.sequence
                else -> it is ActionRejected
            }
        }
    }

    suspend fun view(predicate: (RoomView) -> Boolean = { true }): RoomView {
        pending.removeAll { it is RoomView }
        while (true) {
            val event = incoming.poll() ?: break
            if (event is Throwable) throw event
            if (event !is RoomView) pending.add(event)
        }
        send(RoomViewRequest())
        return next(predicate)
    }

    suspend fun sessionInfo(): SessionInfo =
        connector.kotlin().request<SessionInfo>(SessionInfoRequest()).await()

    suspend fun close() {
        if (closed) return
        closed = true
        connector.kotlin().close().await()
        registrations.forEach { it.close() }
    }

    inline fun <reified T> next(noinline predicate: (T) -> Boolean = { true }): T =
        next(T::class.java, predicate)

    fun <T> next(type: Class<T>, predicate: (T) -> Boolean): T =
        type.cast(nextAny { type.isInstance(it) && predicate(type.cast(it)) })

    private fun nextAny(predicate: (Any) -> Boolean): Any {
        val expires = System.nanoTime() + TimeUnit.SECONDS.toNanos(MESSAGE_SECONDS)
        while (true) {
            val index = pending.indexOfFirst(predicate)
            if (index >= 0) return pending.removeAt(index)
            val event = incoming.poll((expires - System.nanoTime()).coerceAtLeast(0), TimeUnit.NANOSECONDS)
                ?: error("Timed out waiting for a matching message, pending=$pending")
            if (event is Throwable) throw event
            if (predicate(event)) return event
            pending.add(event)
        }
    }
}
