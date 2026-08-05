package systems.zlink.e2e.kotlin.discoveryregistryha.consumer

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.time.Duration
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionException
import java.util.concurrent.CompletionStage
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import org.springframework.context.SmartLifecycle
import systems.zlink.e2e.kotlin.discoveryregistryha.Contracts
import systems.zlink.e2e.kotlin.discoveryregistryha.consumer.Configuration.ConsumerOptions
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.locations.ZLinkLocationStore
import systems.zlink.framework.locations.ZLinkPageRequest
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle

class ConsumerHttpServer(
    private val client: ZLinkClient,
    private val lifecycle: ZLinkFrameworkLifecycle,
    private val locations: ZLinkLocationStore,
    private val json: ObjectMapper,
    private val options: ConsumerOptions,
    private val delayState: LocationStoreDelayState,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var executor: ExecutorService? = null
    private var running = false

    override fun start() {
        val uri = URI.create(options.httpEndpoint)
        val httpServer = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
        val requestExecutor = Executors.newFixedThreadPool(8)
        httpServer.executor = requestExecutor
        httpServer.createContext("/health") { exchange ->
            write(exchange, """{"status":"ready","rid":"${options.rid}"}""")
        }
        httpServer.createContext("/profile/request") { exchange ->
            handleRequest(exchange, waitForRoute = false)
        }
        httpServer.createContext("/profile/request/wait") { exchange ->
            handleRequest(exchange, waitForRoute = true)
        }
        httpServer.createContext("/locations/status") { exchange ->
            writeResult(exchange, status())
        }
        httpServer.createContext("/locations/peers") { exchange ->
            writeResult(exchange, peers())
        }
        httpServer.createContext("/admin/store-delay") { exchange ->
            try {
                val request = json.readValue(exchange.requestBody, Contracts.StoreDelayReq::class.java)
                delayState.setDelay(Duration.ofMillis(request.delayMilliseconds.toLong()))
                write(exchange, json.writeValueAsString(mapOf("delayMilliseconds" to delayState.delayMilliseconds())))
            } catch (error: Exception) {
                writeError(exchange, error)
            }
        }
        httpServer.createContext("/shutdown") { exchange ->
            write(exchange, """{"status":"stopping"}""")
            Thread { stop() }.start()
        }
        httpServer.start()
        server = httpServer
        executor = requestExecutor
        running = true
    }

    override fun stop() {
        server?.stop(0)
        server = null
        executor?.shutdownNow()
        executor = null
        running = false
    }

    override fun isRunning(): Boolean = running

    private fun handleRequest(exchange: HttpExchange, waitForRoute: Boolean) {
        try {
            val request = json.readValue(exchange.requestBody, Contracts.WorkReq::class.java)
            val reply = if (waitForRoute) requestWithRetry(request) else requestOnce(request)
            writeResult(exchange, reply)
        } catch (error: Exception) {
            writeError(exchange, error)
        }
    }

    private fun requestWithRetry(request: Contracts.WorkReq): CompletionStage<Contracts.WorkRes> =
        requestWithRetry(request, System.nanoTime() + TimeUnit.SECONDS.toNanos(10), null)

    private fun requestWithRetry(
        request: Contracts.WorkReq,
        deadline: Long,
        previousFailure: Throwable?,
    ): CompletionStage<Contracts.WorkRes> = requestOnce(request).handle { reply, error ->
        if (error == null) {
            CompletableFuture.completedFuture(reply)
        } else if (System.nanoTime() >= deadline) {
            CompletableFuture.failedFuture(
                IllegalStateException("Timed out waiting for profile request routing.", unwrap(error)),
            )
        } else {
            CompletableFuture.runAsync({}, CompletableFuture.delayedExecutor(100, TimeUnit.MILLISECONDS))
                .thenCompose { requestWithRetry(request, deadline, unwrap(error ?: previousFailure!!)) }
        }
    }.thenCompose { it }

    private fun requestOnce(request: Contracts.WorkReq): CompletionStage<Contracts.WorkRes> =
        client.requestToChannel(Contracts.CHANNEL, request)
            .timeout(Duration.ofSeconds(3))
            .submit(Contracts.WorkRes::class.java)

    private fun peers(): CompletionStage<List<Map<String, Any>>> =
        locations.listClientServers(
            Contracts.CHANNEL,
            ZLinkPageRequest(1_000, null),
        ).thenApply { page -> page.items().map { server ->
                mapOf(
                    "nodeRid" to server.serverRid().toString(),
                    "endpoint" to server.endpoint(),
                    "ownerId" to server.ownerId(),
                    "role" to "ROUTER",
                    "meshName" to server.channelName(),
                )
            } }

    private fun status(): CompletionStage<Map<String, Any>> =
        lifecycle.monitoringLocationRuntimeQuery().getStatus().thenApply { status -> mapOf(
            "storeHealthy" to status.storeHealthy(),
            "watchEnabled" to status.watchEnabled(),
            "pollingIntervalMillis" to status.pollingInterval().toMillis(),
            "lastRefreshAt" to (status.lastRefreshAt()?.toString() ?: ""),
            "lastError" to (status.lastError() ?: ""),
            "ownerLeaseHealthy" to status.ownerLeaseHealthy(),
            "ownerLeaseRenewedAt" to (status.ownerLeaseRenewedAt()?.toString() ?: ""),
        ) }

    private fun writeResult(exchange: HttpExchange, result: CompletionStage<*>) {
        result.whenComplete { value, error ->
            if (error == null) write(exchange, json.writeValueAsString(value))
            else writeError(exchange, unwrap(error))
        }
    }

    private fun writeError(exchange: HttpExchange, error: Throwable) {
        val message = error.message?.replace("\"", "'") ?: error.javaClass.simpleName
        write(exchange, """{"error":"$message"}""", status = 500)
    }

    private fun unwrap(error: Throwable): Throwable =
        if (error is CompletionException && error.cause != null) error.cause!! else error

    private fun write(exchange: HttpExchange, value: String, status: Int = 200) {
        val body = value.toByteArray(StandardCharsets.UTF_8)
        exchange.responseHeaders.add("Content-Type", "application/json")
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.use { stream -> stream.write(body) }
        exchange.close()
    }
}
