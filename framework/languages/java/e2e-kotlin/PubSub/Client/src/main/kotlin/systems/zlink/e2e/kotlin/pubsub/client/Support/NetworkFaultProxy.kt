package systems.zlink.e2e.kotlin.pubsub.client.Support

import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class NetworkFaultProxy private constructor(
    private val upstreamHost: String,
    private val port: Int,
) : AutoCloseable {
    private val blocked = AtomicBoolean(false)
    private val running = AtomicBoolean(true)
    private val sockets = ConcurrentHashMap.newKeySet<Socket>()
    private val executor = Executors.newVirtualThreadPerTaskExecutor()
    private val listener = ServerSocket().apply {
        reuseAddress = true
        bind(InetSocketAddress(InetAddress.getByName("127.0.0.1"), port))
    }

    init {
        executor.submit {
            while (running.get()) {
                try {
                    accept(listener.accept())
                } catch (error: Exception) {
                    if (running.get()) throw error
                }
            }
        }
    }

    fun block() {
        blocked.set(true)
        sockets.toList().forEach { closeSocket(it) }
    }

    fun unblock() {
        blocked.set(false)
    }

    private fun accept(client: Socket) {
        if (blocked.get()) {
            client.close()
            return
        }
        val upstream = Socket(upstreamHost, port)
        sockets += client
        sockets += upstream
        executor.submit { copy(client, upstream) }
        executor.submit { copy(upstream, client) }
    }

    private fun copy(source: Socket, target: Socket) {
        try {
            source.getInputStream().transferTo(target.getOutputStream())
        } catch (_: Exception) {
        } finally {
            closeSocket(source)
            closeSocket(target)
        }
    }

    private fun closeSocket(socket: Socket) {
        sockets.remove(socket)
        try {
            socket.close()
        } catch (_: Exception) {
        }
    }

    override fun close() {
        running.set(false)
        block()
        listener.close()
        executor.close()
    }

    companion object {
        fun start(upstreamHost: String, port: Int): NetworkFaultProxy =
            NetworkFaultProxy(upstreamHost, port)
    }
}
