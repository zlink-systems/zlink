package systems.zlink.e2e.kotlin.resiliencelifecycle

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.time.Duration
import java.net.Socket
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.springframework.context.SmartLifecycle
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle

class ConsumerHttpServer(
    private val client: ZLinkClient,
    private val lifecycle: ZLinkFrameworkLifecycle,
    private val clientServerRuntime: ZLinkClientServerRuntime,
    private val json: ObjectMapper,
    private val endpoint: String,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var executor: java.util.concurrent.ExecutorService? = null
    private var running = false

    override fun start() {
        val uri = URI.create(endpoint)
        val httpServer = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
        val requestExecutor = Executors.newCachedThreadPool { runnable ->
            Thread(runnable, "kotlin-rl-consumer-http").apply { isDaemon = true }
        }
        httpServer.createContext("/health") { exchange -> write(exchange, 200, "ok\n") }
        httpServer.createContext("/profile/request") { exchange ->
            try {
                val request = exchange.readJson(Contracts.WorkReq::class.java)
                val timeoutMillis = exchange.query("timeoutMillis")?.toLongOrNull() ?: 3000L
                val reply = client.requestToChannel(Contracts.CHANNEL, request)
                    .timeout(Duration.ofMillis(timeoutMillis))
                    .submit(Contracts.WorkRes::class.java).toCompletableFuture().get(5, TimeUnit.SECONDS)
                exchange.writeJson(reply)
            } catch (error: Exception) {
                exchange.writeError(error)
            }
        }
        httpServer.createContext("/profile/send") { exchange ->
            val request = exchange.readJson(Contracts.WorkMsg::class.java)
            client.sendToChannel(Contracts.CHANNEL, request).submit()
            exchange.writeJson(mapOf("status" to "sent"))
        }
        httpServer.createContext("/profile/unhandled") { exchange ->
            try {
                val request = exchange.readJson(Contracts.UnhandledReq::class.java)
                val reply = client.requestToChannel(Contracts.CHANNEL, request)
                    .timeout(Duration.ofSeconds(3))
                    .submit(Contracts.WorkRes::class.java).toCompletableFuture().get(5, TimeUnit.SECONDS)
                exchange.writeJson(reply)
            } catch (error: Exception) {
                exchange.writeError(error)
            }
        }
        httpServer.createContext("/topology/wait") { exchange ->
            val request = exchange.readJson(Contracts.TopologyWaitReq::class.java)
            val matched = waitForTopology(request)
            exchange.writeJson(Contracts.TopologyWaitRes(matched))
        }
        httpServer.createContext("/topology/read") { exchange ->
            exchange.writeJson(readTopology())
        }
        httpServer.executor = requestExecutor
        httpServer.start()
        executor = requestExecutor
        server = httpServer
        running = true
    }

    private fun waitForTopology(request: Contracts.TopologyWaitReq): Int {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(20)
        val expectedEndpoint = request.endpoint
        while (System.nanoTime() < deadline) {
            val matches = try {
                val status = clientServerRuntime.snapshot(Contracts.CHANNEL)
                status.targets()
                    .asSequence()
                    .filter { request.routingId == null || it.nodeRid().toString() == request.routingId }
                    .count { it.state().name == "READY" }
            } catch (_: Exception) {
                0
            }
            if (matches > 0 && expectedEndpoint != null && !endpointAcceptsConnections(expectedEndpoint)) {
                Thread.sleep(200)
                continue
            }
            if (
                (request.expectedRouters == 0 && matches == 0) ||
                (request.expectedRouters > 0 && matches >= request.expectedRouters)
            ) {
                return matches
            }
            Thread.sleep(200)
        }
        throw IllegalStateException("location peer topology did not match $request")
    }

    private fun readTopology(): Contracts.TopologyReadRes {
        return try {
            val matches = clientServerRuntime.snapshot(Contracts.CHANNEL)
                .readyTargetCount()
            Contracts.TopologyReadRes("ok", matches, null)
        } catch (error: Exception) {
            Contracts.TopologyReadRes("error", 0, error.javaClass.simpleName)
        }
    }

    override fun stop() {
        server?.stop(0)
        server = null
        executor?.shutdownNow()
        executor = null
        running = false
    }

    override fun isRunning(): Boolean = running

    private fun endpointAcceptsConnections(endpoint: String): Boolean {
        return try {
            val uri = URI.create(endpoint)
            Socket().use { socket ->
                socket.connect(InetSocketAddress(uri.host, uri.port), 200)
            }
            true
        } catch (_: Exception) {
            false
        }
    }

    private fun <T> HttpExchange.readJson(type: Class<T>): T =
        requestBody.use { json.readValue(it, type) }

    private fun HttpExchange.writeJson(value: Any) {
        val body = json.writeValueAsBytes(value)
        responseHeaders.add("Content-Type", "application/json")
        sendResponseHeaders(200, body.size.toLong())
        responseBody.use { it.write(body) }
    }

    private fun HttpExchange.writeError(error: Exception) {
        val body = json.writeValueAsBytes(mapOf("error" to (error.message ?: error.javaClass.name)))
        responseHeaders.add("Content-Type", "application/json")
        sendResponseHeaders(500, body.size.toLong())
        responseBody.use { it.write(body) }
    }

    private fun HttpExchange.query(name: String): String? =
        requestURI.rawQuery
            ?.split("&")
            ?.mapNotNull {
                val index = it.indexOf('=')
                if (index < 0) null else it.substring(0, index) to it.substring(index + 1)
            }
            ?.firstOrNull { it.first == name }
            ?.second

    private fun write(exchange: HttpExchange, status: Int, value: String) {
        val body = value.toByteArray(StandardCharsets.UTF_8)
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.use { it.write(body) }
    }
}
