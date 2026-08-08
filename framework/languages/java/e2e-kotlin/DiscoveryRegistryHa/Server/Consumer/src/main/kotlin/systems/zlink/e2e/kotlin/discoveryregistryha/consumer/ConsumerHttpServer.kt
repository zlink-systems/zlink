package systems.zlink.e2e.kotlin.discoveryregistryha.consumer

import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.net.URLDecoder
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
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.locations.ZLinkLocationObjectEntry
import systems.zlink.framework.locations.ZLinkLocationObjectFilter
import systems.zlink.framework.locations.ZLinkLocationTopologyFilter
import systems.zlink.framework.locations.ZLinkPlacementObjectKind
import systems.zlink.framework.locations.ZLinkPageRequest
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle

class ConsumerHttpServer(
    private val client: ZLinkClient,
    private val routes: ZLinkRouteClient,
    private val lifecycle: ZLinkFrameworkLifecycle,
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
        httpServer.createContext("/object/request") { exchange ->
            try {
                val request = json.readValue(exchange.requestBody, Contracts.ObjectReq::class.java)
                writeResult(exchange, objectRequest(request))
            } catch (error: Exception) {
                writeError(exchange, error)
            }
        }
        httpServer.createContext("/locations/status") { exchange ->
            writeResult(exchange, status())
        }
        httpServer.createContext("/locations/peers") { exchange ->
            writeResult(exchange, peers())
        }
        httpServer.createContext("/locations/objects") { exchange ->
            try {
                val kind = queryParameter(exchange, "kind")
                require(kind == "spot") { "kind=spot is required" }
                val pageSize = queryParameter(exchange, "pageSize").toIntOrNull() ?: 1_000
                require(pageSize > 0) { "pageSize must be positive" }
                writeResult(
                    exchange,
                    objectLocations(pageSize, queryParameter(exchange, "continuationToken").ifBlank { null }),
                )
            } catch (error: Exception) {
                writeError(exchange, error)
            }
        }
        httpServer.createContext("/location/object") { exchange ->
            try {
                writeResult(
                    exchange,
                    exactObjectLocation(queryParameter(exchange, "kind"), queryParameter(exchange, "id")),
                )
            } catch (error: Exception) {
                writeError(exchange, error)
            }
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

    private fun objectRequest(request: Contracts.ObjectReq): CompletionStage<Contracts.ObjectOutcome> =
        try {
            routes.requestToSpot(request.spotId, request)
                .timeout(Duration.ofSeconds(3))
                .instanceSpot(Contracts.OBJECT_TYPE)
                .inMesh(Contracts.CHANNEL)
                .submit(Contracts.ObjectRes::class.java)
                .handle { reply, error ->
                    if (error == null) {
                        Contracts.ObjectOutcome(true, reply, "", "")
                    } else {
                        val cause = unwrap(error)
                        Contracts.ObjectOutcome(
                            false,
                            null,
                            cause.javaClass.simpleName,
                            cause.message.orEmpty(),
                        )
                    }
                }
        } catch (error: RuntimeException) {
            CompletableFuture.completedFuture(
                Contracts.ObjectOutcome(
                    false,
                    null,
                    error.javaClass.simpleName,
                    error.message.orEmpty(),
                ),
            )
        }

    private fun peers(): CompletionStage<List<Map<String, Any>>> =
        lifecycle.monitoringLocationRuntimeQuery().listTopology(
            ZLinkLocationTopologyFilter.all(),
            ZLinkPageRequest(1_000, null),
        ).thenApply { page -> page.items().map { server ->
                mapOf(
                    "nodeRid" to server.nodeRid().toString(),
                    "endpoint" to server.endpoint(),
                    "ownerId" to "",
                    "role" to "ROUTER",
                    "state" to server.state().name,
                    "draining" to server.draining,
                    "meshName" to server.meshName(),
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

    private fun objectLocations(
        pageSize: Int,
        continuationToken: String?,
    ): CompletionStage<Map<String, Any>> =
        lifecycle.monitoringLocationRuntimeQuery().listObjectLocations(
            ZLinkLocationObjectFilter(
                ZLinkPlacementObjectKind.INSTANCE_SPOT,
                Contracts.OBJECT_TYPE,
                null,
            ),
            ZLinkPageRequest(pageSize, continuationToken),
        ).thenApply { page ->
            mapOf(
                "items" to page.items().map(::objectLocation),
                "continuationToken" to (page.continuationToken() ?: ""),
            )
        }

    private fun exactObjectLocation(
        kind: String,
        id: String,
    ): CompletionStage<Map<String, Any>> {
        require(kind == "spot" && id.isNotBlank()) { "kind=spot and a non-empty id are required" }
        return lifecycle.monitoringLocationRuntimeQuery().findSpotLocation(id).thenApply { optional ->
            val entry = optional.orElse(null)
            if (entry == null) {
                mapOf("found" to false)
            } else {
                mapOf(
                    "found" to true,
                    "objectId" to entry.globalId(),
                    "stableType" to entry.stableType(),
                    "meshName" to entry.meshName(),
                    "ownerNodeRid" to entry.nodeRid().toString(),
                    "objectGeneration" to entry.objectGeneration(),
                    "state" to entry.state().name.lowercase(java.util.Locale.ROOT),
                )
            }
        }
    }

    private fun queryParameter(exchange: HttpExchange, name: String): String {
        val query = exchange.requestURI.rawQuery ?: return ""
        if (query.isBlank()) return ""
        for (pair in query.split('&')) {
            val separator = pair.indexOf('=')
            val rawKey = if (separator < 0) pair else pair.substring(0, separator)
            if (name != URLDecoder.decode(rawKey, StandardCharsets.UTF_8)) continue
            val rawValue = if (separator < 0) "" else pair.substring(separator + 1)
            return URLDecoder.decode(rawValue, StandardCharsets.UTF_8)
        }
        return ""
    }

    private fun objectLocation(entry: ZLinkLocationObjectEntry): Map<String, Any> = mapOf(
        "globalId" to entry.globalId(),
        "objectGeneration" to entry.objectGeneration(),
        "meshName" to entry.meshName(),
        "nodeRid" to entry.nodeRid().toString(),
        "state" to entry.state().name,
        "stableType" to entry.stableType(),
    )

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
