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
    private val inboxes: Map<Class<*>, LinkedBlockingQueue<Any>> =
        listOf(TownView::class.java, DungeonView::class.java, ActionRejected::class.java)
            .associateWith { LinkedBlockingQueue<Any>() }
    private val registrations = mutableListOf<AutoCloseable>()
    @Volatile var disconnects = 0

    init {
        for ((type, inbox) in inboxes) {
            registrations +=
                connector.on(type) { message ->
                    inbox.add(message.payload())
                    CompletableFuture.completedFuture(null)
                }
        }
        registrations +=
            connector.onDisconnected { event ->
                disconnects++
                inboxes.values.forEach { it.add(IllegalStateException("Disconnected: $event")) }
                CompletableFuture.completedFuture(null)
            }
        registrations +=
            connector.onErrorReceived { event ->
                inboxes.values.forEach { it.add(IllegalStateException("Connector error: $event")) }
                CompletableFuture.completedFuture(null)
            }
    }

    suspend fun connect() = connector.kotlin().connect().await()

    suspend fun join(channel: Long = 2) = send(JoinTownRequest(playerId, channel))

    suspend fun view(predicate: (DungeonView) -> Boolean = { true }): DungeonView =
        sendFor(DungeonView::class.java, ViewRequest(), predicate)

    suspend fun sessionInfo(): SessionInfo =
        connector.kotlin().request<SessionInfo>(SessionInfoRequest()).await()

    suspend fun send(message: Any) = connector.kotlin().send(message).await()

    suspend fun <T> sendFor(
        type: Class<T>,
        request: Any,
        predicate: (T) -> Boolean,
    ): T {
        if (request is ViewRequest) inboxes.getValue(type).clear()
        send(request)
        return next(type, predicate)
    }

    suspend fun close() {
        connector.kotlin().close().await()
        registrations.forEach { it.close() }
    }

    inline fun <reified T> next(noinline predicate: (T) -> Boolean = { true }): T =
        next(T::class.java, predicate)

    fun <T> next(type: Class<T>, predicate: (T) -> Boolean): T {
        val inbox = inboxes.getValue(type)
        val expires = System.nanoTime() + TimeUnit.SECONDS.toNanos(MESSAGE_SECONDS)
        while (true) {
            val event =
                inbox.poll((expires - System.nanoTime()).coerceAtLeast(0), TimeUnit.NANOSECONDS)
                    ?: error("Timed out waiting for ${type.simpleName}")
            if (event is Throwable) throw event
            if (event is ActionRejected && !type.isInstance(event))
                error("Action rejected: ${event.reason}")
            if (type.isInstance(event)) {
                val message = type.cast(event)
                if (predicate(message)) return message
            }
        }
    }
}
