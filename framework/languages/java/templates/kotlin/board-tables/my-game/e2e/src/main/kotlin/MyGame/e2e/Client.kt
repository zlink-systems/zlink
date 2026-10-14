package MyGame.e2e

import MyGame.*
import java.net.URI
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import systems.zlink.framework.kotlin.*
import systems.zlink.stream.connector.*

const val MESSAGE_SECONDS = 5L

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

class Client(endpoint: String, val playerId: String, val tableId: String) {
    val connector = createConnector(endpoint)
    private val queue = LinkedBlockingQueue<Any>()
    private val pending = mutableListOf<Any>()
    val history = java.util.Collections.synchronizedList(mutableListOf<Any>())
    private val registrations = mutableListOf<AutoCloseable>()
    @Volatile var disconnects = 0

    init {
        listOf(
                TableView::class.java,
                HandDealt::class.java,
                HandResult::class.java,
                ActionRejected::class.java,
            )
            .forEach { type ->
                registrations +=
                    connector.on(type) { message ->
                        history.add(message.payload())
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

    suspend fun connect(): TableView {
        connector.kotlin().connect().await()
        send(JoinTableRequest(playerId, tableId))
        return next<TableView> { it.actorId == playerId }
    }

    suspend fun sessionId(): String =
        connector.kotlin().request<SessionInfo>(SessionInfoRequest()).await().sessionId

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
                    ?: error("Timed out waiting for ${type.simpleName}, received=$history")
            if (event is Throwable) throw event
            pending.add(event)
        }
    }

    fun clear() {
        pending.clear()
        while (true) {
            val event = queue.poll() ?: break
            if (event is Throwable) throw event
        }
    }
}
