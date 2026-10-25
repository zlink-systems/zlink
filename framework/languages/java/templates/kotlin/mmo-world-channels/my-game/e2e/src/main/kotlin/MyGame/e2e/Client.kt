package MyGame.e2e

import MyGame.*
import java.net.URI
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import systems.zlink.framework.kotlin.*
import systems.zlink.stream.connector.*

private const val MESSAGE_SECONDS = 5L

class Client(endpoint: String, val playerId: String) {
    private val connector: ZLinkStreamConnector
    private val queue = LinkedBlockingQueue<Any>()
    private val pending = mutableListOf<Any>()
    private val channelViews = CopyOnWriteArrayList<ChannelView>()
    private val registrations = mutableListOf<AutoCloseable>()
    private var closed = false
    val disconnects = AtomicInteger()

    init {
        val uri = URI(endpoint)
        val d = ZLinkStreamConnectorOptions.createDefault(uri)
        connector =
            ZLinkStreamConnectorFactory.create(
                ZLinkStreamConnectorOptions(
                    uri,
                    ZLinkStreamDispatchMode.IMMEDIATE,
                    Duration.ofSeconds(MESSAGE_SECONDS),
                    d.maxReconnectAttempts(),
                    d.connectTimeout(),
                    d.maxSendPayloadSize(),
                    d.heartbeatEnabled(),
                    d.heartbeatInterval(),
                    d.heartbeatTimeout(),
                    false,
                    d.reconnectInitialDelay(),
                    d.reconnectMaxDelay(),
                    d.reconnectBackoffFactor(),
                )
            )
        registrations +=
            connector.on(PlayerView::class.java) { message ->
                queue.add(message.payload())
                CompletableFuture.completedFuture(null)
            }
        registrations +=
            connector.on(ChannelView::class.java) { message ->
                val payload = message.payload()
                channelViews.add(payload)
                queue.add(payload)
                CompletableFuture.completedFuture(null)
            }
        registrations +=
            connector.on(ActionRejected::class.java) { message ->
                queue.add(message.payload())
                CompletableFuture.completedFuture(null)
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

    suspend fun connect(): PlayerView {
        connector.kotlin().connect().await()
        send(EnterWorldRequest(playerId))
        return next()
    }

    suspend fun sessionInfo(): SessionInfo =
        connector.kotlin().request<SessionInfo>(SessionInfoRequest()).await()

    suspend fun send(message: Any) = connector.kotlin().send(message).await()

    suspend fun channelView(): ChannelView {
        val from = markChannelViews()
        send(ChannelViewRequest())
        return channelViewAfter(from) { true }
    }

    fun markChannelViews(): Int = channelViews.size

    fun channelViewAfter(from: Int, predicate: (ChannelView) -> Boolean): ChannelView =
        next(ChannelView::class.java) { view ->
            channelViewPosition(view) >= from && predicate(view)
        }

    private fun channelViewPosition(expected: ChannelView): Int =
        channelViews.indexOfFirst { it === expected }

    fun rejection(): ActionRejected = next()

    fun waitRetryAfter(retryAfterMs: Int) {
        require(retryAfterMs > 0) { "RetryAfterMs must be positive" }
        Thread.sleep(retryAfterMs.toLong())
    }

    inline fun <reified T> next(): T = next(T::class.java)

    fun <T> next(type: Class<T>, predicate: (T) -> Boolean = { true }): T {
        val expires = System.nanoTime() + TimeUnit.SECONDS.toNanos(MESSAGE_SECONDS)
        while (true) {
            var index = 0
            while (index < pending.size) {
                val queued = pending[index]
                if (queued is Throwable) throw queued
                if (type.isInstance(queued)) {
                    val value = type.cast(queued)
                    if (predicate(value)) {
                        pending.removeAt(index)
                        return value
                    }
                }
                index++
            }
            val event =
                queue.poll((expires - System.nanoTime()).coerceAtLeast(0), TimeUnit.NANOSECONDS)
                    ?: error("Timed out waiting for ${type.simpleName}")
            if (event is Throwable) throw event
            if (type.isInstance(event)) {
                val value = type.cast(event)
                if (predicate(value)) return value
            }
            pending.add(event)
        }
    }

    suspend fun close() {
        if (closed) return
        closed = true
        var failure: Exception? = null
        try {
            connector.kotlin().close().await()
        } catch (error: Exception) {
            failure = error
        }
        registrations.forEach { registration ->
            try {
                registration.close()
            } catch (error: Exception) {
                if (failure == null) failure = error else failure!!.addSuppressed(error)
            }
        }
        failure?.let { throw it }
    }
}

suspend fun <T> withClient(endpoint: String, playerId: String, action: suspend (Client) -> T): T {
    val client = Client(endpoint, playerId)
    var primary: Throwable? = null
    try {
        return action(client)
    } catch (error: Throwable) {
        primary = error
        throw error
    } finally {
        try {
            client.close()
        } catch (cleanup: Exception) {
            if (primary == null) throw cleanup else primary.addSuppressed(cleanup)
        }
    }
}
