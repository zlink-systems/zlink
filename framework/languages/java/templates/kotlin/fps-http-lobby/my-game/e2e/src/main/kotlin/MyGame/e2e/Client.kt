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

class Client(endpoint: String, val playerId: String, private val roomId: String) {
    val connector = createConnector(endpoint)
    private val queue = LinkedBlockingQueue<Any>()
    private val pending = mutableListOf<Any>()
    private val registrations = mutableListOf<AutoCloseable>()
    val disconnects = java.util.concurrent.atomic.AtomicInteger()

    init {
        listOf(
                MatchFound::class.java,
                RoomView::class.java,
                MatchEnded::class.java,
                MoveResult::class.java,
                ShotResult::class.java,
                ShotRejected::class.java,
                ActionRejected::class.java,
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
                disconnects.incrementAndGet()
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

    suspend fun join() = send(JoinRoomRequest(playerId, roomId))

    fun clear() {
        pending.clear()
        queue.removeIf { it !is Throwable }
    }

    suspend fun sessionInfo(): SessionInfo =
        connector.kotlin().request<SessionInfo>(SessionInfoRequest()).await()

    suspend fun send(message: Any) = connector.kotlin().send(message).await()

    suspend fun action(message: Any): Any {
        send(message)
        return next(Any::class.java) {
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
            val event = queue.poll() ?: break
            if (event is Throwable) throw event
            if (event !is RoomView) pending.add(event)
        }
        send(RoomViewRequest())
        return next(RoomView::class.java, predicate)
    }

    private var closed = false

    suspend fun close() {
        if (closed) return
        closed = true
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
