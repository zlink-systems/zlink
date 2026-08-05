package systems.zlink.e2e.kotlin.spotservice.play.endpoints

import systems.zlink.e2e.kotlin.spotservice.ScenarioState
import systems.zlink.e2e.kotlin.spotservice.Contracts
import systems.zlink.e2e.kotlin.spotservice.play.spots.TimerScenarioSpot
import systems.zlink.e2e.kotlin.spotservice.play.spots.MismatchedSpot
import systems.zlink.e2e.kotlin.spotservice.play.spots.UserSpot
import com.fasterxml.jackson.databind.ObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.net.URLDecoder
import java.nio.charset.StandardCharsets
import java.time.Duration
import java.util.concurrent.Executors
import java.util.concurrent.ExecutionException
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import org.springframework.context.SmartLifecycle
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spots.SpotRef
import systems.zlink.framework.spots.ZLinkSpotCreateResult
import systems.zlink.framework.spots.ZLinkSpotManager

class EvidenceHttpServer(
    private val state: ScenarioState,
    private val json: ObjectMapper,
    private val endpoint: String,
    private val spots: ZLinkSpotManager,
    private val routes: ZLinkRouteClient,
    private val meshOptions: ZLinkRouteMeshRuntimeOptions,
) : SmartLifecycle {
    private var server: HttpServer? = null
    private var executor: java.util.concurrent.ExecutorService? = null
    private var running = false

    override fun start() {
        if (endpoint.isBlank()) {
            return
        }
        try {
            val uri = URI.create(endpoint)
            val nextServer = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
            nextServer.createContext("/health") { exchange ->
                write(exchange, 200, "ok\n")
            }
            nextServer.createContext("/placement-weight") { exchange ->
                handle(exchange) {
                    requirePost(exchange)
                    val request = readJson(exchange, Contracts.PlacementWeightReq::class.java)
                    val placement = meshOptions.mesh(Contracts.SPOT_MESH)
                    placement.setPlacementWeight(request.weight)
                    writeJson(
                        exchange,
                        200,
                        Contracts.PlacementWeightRes(placement.placementWeight()),
                    )
                }
            }
            nextServer.createContext("/evidence") { exchange ->
                val body = json.writeValueAsBytes(state.snapshot())
                exchange.responseHeaders.add("Content-Type", "application/json")
                exchange.sendResponseHeaders(200, body.size.toLong())
                exchange.responseBody.write(body)
                exchange.close()
            }
            nextServer.createContext("/evidence/wait") { exchange ->
                requirePost(exchange)
                val request = exchange.requestBody.use { body ->
                    json.readValue(body, Contracts.EvidenceWaitReq::class.java)
                }
                val snapshot = state.waitUntilContainsAll(
                    request.containsAll,
                    Duration.ofMillis(request.timeoutMilliseconds.coerceIn(1, 30_000).toLong())
                )
                val body = json.writeValueAsBytes(snapshot)
                exchange.responseHeaders.add("Content-Type", "application/json")
                exchange.sendResponseHeaders(200, body.size.toLong())
                exchange.responseBody.write(body)
                exchange.close()
            }
            nextServer.createContext("/spot/create") { exchange ->
                handle(exchange) {
                    requirePost(exchange)
                    val request = readJson(exchange, Contracts.CreateSpotReq::class.java)
                    val result = createSpot(request.spotRid, localSpotType("user"))
                    writeJson(
                        exchange,
                        200,
                        Contracts.CreateSpotRes(
                            result.spot().spotId(),
                            result.spot().nodeRid().toString(),
                        )
                    )
                }
            }
            nextServer.createContext("/spot/state/request") { exchange ->
                handle(exchange) {
                    requirePost(exchange)
                    val request = readJson(exchange, Contracts.SpotStateRouteReq::class.java)
                    val message: Any = if (request.packetName == "StateReq") {
                        Contracts.StateReq(request.op)
                    } else {
                        Contracts.MissingSpotReq(request.op)
                    }
                    val call = routes.requestToSpot(request.spotRid, message)
                        .timeout(Duration.ofMillis(request.timeoutMilliseconds.coerceIn(1, 30_000).toLong()))
                    writeJson(exchange, 200, call.submit(Contracts.StateRes::class.java).toCompletableFuture().get(5, TimeUnit.SECONDS))
                }
            }
            nextServer.createContext("/spot/state/command") { exchange ->
                handle(exchange) {
                    requirePost(exchange)
                    val request = readJson(exchange, Contracts.SpotStateCommandReq::class.java)
                    val message: Any = if (request.packetName == "StateMsg") {
                        Contracts.StateMsg(request.value)
                    } else {
                        Contracts.MissingSpotMsg(request.value)
                    }
                    routes.sendToSpot(request.spotRid, message)
                        .submit()
                    writeJson(exchange, 200, Contracts.AckRes(true))
                }
            }
            nextServer.createContext("/spot/stage/request") { exchange ->
                handle(exchange) {
                    requirePost(exchange)
                    val request = readJson(exchange, Contracts.SpotStageProbeReq::class.java)
                    val reply = routes.requestToSpot(
                        request.spotRid,
                        Contracts.StageProbeReq(request.marker, request.op)
                    )
                        .timeout(Duration.ofSeconds(5))
                        .submit(Contracts.StateRes::class.java).toCompletableFuture().get(5, TimeUnit.SECONDS)
                    writeJson(exchange, 200, reply)
                }
            }
            nextServer.createContext("/spot/stage/timer") { exchange ->
                handle(exchange) {
                    requirePost(exchange)
                    val request = readJson(exchange, Contracts.SpotStageTimerReq::class.java)
                    routes.sendToSpot(
                        request.spotRid,
                        Contracts.StageTimerStartMsg(request.name, request.periodMilliseconds)
                    )
                        .submit()
                    state.waitUntilContainsAll(
                        listOf("StageTimer", request.spotRid, request.name),
                        Duration.ofSeconds(10)
                    )
                    writeJson(exchange, 200, Contracts.SpotStageTimerRes(request.spotRid, request.name, true))
                }
            }
            nextServer.createContext("/spot/slow/request") { exchange ->
                handle(exchange) {
                    requirePost(exchange)
                    val request = readJson(exchange, Contracts.SpotSlowRouteReq::class.java)
                    val call = routes.requestToSpot(
                        request.spotRid,
                        Contracts.SlowReq(request.value)
                    )
                        .timeout(Duration.ofMillis(request.timeoutMilliseconds.coerceIn(1, 30_000).toLong()))
                    writeJson(exchange, 200, call.submit(Contracts.StateRes::class.java).toCompletableFuture().get(5, TimeUnit.SECONDS))
                }
            }
            nextServer.createContext("/spot/outbound/request") { exchange ->
                handle(exchange) {
                    requirePost(exchange)
                    val request = readJson(exchange, Contracts.SpotOutboundRouteReq::class.java)
                    val reply = routes.requestToSpot(
                        request.spotRid,
                        Contracts.OutboundReq(request.value)
                    )
                        .timeout(Duration.ofSeconds(5))
                        .submit(Contracts.OutboundRes::class.java).toCompletableFuture().get(5, TimeUnit.SECONDS)
                    writeJson(exchange, 200, reply)
                }
            }
            nextServer.createContext("/spot/outbound/command") { exchange ->
                handle(exchange) {
                    requirePost(exchange)
                    val request = readJson(exchange, Contracts.SpotOutboundCommandReq::class.java)
                    routes.sendToSpot(
                        "room-a",
                        Contracts.SpotToSpotCommandReq(request.spotRid, request.value)
                    )
                        .submit()
                    state.waitUntilContainsAll(
                        listOf("SpotToSpotSend", request.value),
                        Duration.ofSeconds(10)
                    )
                    writeJson(exchange, 200, Contracts.AckRes(true))
                }
            }
            nextServer.createContext("/route/ping") { exchange ->
                handle(exchange) {
                    requirePost(exchange)
                    val request = readJson(exchange, Contracts.RoutePingHttpReq::class.java)
                    val reply = routes.requestToNode(
                        Contracts.SPOT_MESH,
                        RoutingId.from(request.targetRid),
                        Contracts.RoutePingReq(request.value)
                    )
                        .timeout(Duration.ofSeconds(5))
                        .submit(Contracts.RoutePingRes::class.java).toCompletableFuture().get(5, TimeUnit.SECONDS)
                    writeJson(exchange, 200, reply)
                }
            }
            nextServer.createContext("/admin/close") { exchange ->
                val rid = queryValue(exchange.requestURI, "rid")
                if (rid.isNullOrBlank()) {
                    write(exchange, 400, "missing rid\n")
                    return@createContext
                }
                val closed = try {
                    closeSpot(rid)
                } catch (error: InterruptedException) {
                    Thread.currentThread().interrupt()
                    throw IllegalStateException("spot close interrupted", error)
                } catch (error: ExecutionException) {
                    throw IllegalStateException("spot close failed", error)
                } catch (error: TimeoutException) {
                    throw IllegalStateException("spot close failed", error)
                }
                write(exchange, 200, "{\"closed\":$closed}\n")
            }
            nextServer.createContext("/admin/create-timer") { exchange ->
                val rid = queryValue(exchange.requestURI, "rid")
                if (rid.isNullOrBlank()) {
                    write(exchange, 400, "missing rid\n")
                    return@createContext
                }
                try {
                    createSpot(rid, localSpotType("timer"), ZLinkMessage.of("e2e"))
                } catch (error: InterruptedException) {
                    Thread.currentThread().interrupt()
                    throw IllegalStateException("timer spot create interrupted", error)
                } catch (error: ExecutionException) {
                    throw IllegalStateException("timer spot create failed", error)
                } catch (error: TimeoutException) {
                    throw IllegalStateException("timer spot create failed", error)
                }
                write(exchange, 200, "{\"created\":true}\n")
            }
            nextServer.createContext("/admin/create-user") { exchange ->
                val rid = queryValue(exchange.requestURI, "rid")
                if (rid.isNullOrBlank()) {
                    write(exchange, 400, "missing rid\n")
                    return@createContext
                }
                try {
                    createSpot(rid, localSpotType("user"), ZLinkMessage.of("admin"))
                } catch (error: InterruptedException) {
                    Thread.currentThread().interrupt()
                    throw IllegalStateException("user spot create interrupted", error)
                } catch (error: ExecutionException) {
                    throw IllegalStateException("user spot create failed", error)
                } catch (error: TimeoutException) {
                    throw IllegalStateException("user spot create failed", error)
                }
                write(exchange, 200, "{\"created\":true}\n")
            }
            nextServer.createContext("/admin/create-alternate") { exchange ->
                val rid = queryValue(exchange.requestURI, "rid")
                if (rid.isNullOrBlank()) {
                    write(exchange, 400, "missing rid\n")
                    return@createContext
                }
                try {
                    createSpot(rid, localSpotType("mismatched"), ZLinkMessage.of("admin"))
                } catch (error: InterruptedException) {
                    Thread.currentThread().interrupt()
                    throw IllegalStateException("alternate spot create interrupted", error)
                } catch (error: ExecutionException) {
                    throw IllegalStateException("alternate spot create failed", error)
                } catch (error: TimeoutException) {
                    throw IllegalStateException("alternate spot create failed", error)
                }
                write(exchange, 200, "{\"created\":true}\n")
            }
            nextServer.createContext("/admin/type-mismatch") { exchange ->
                val rid = queryValue(exchange.requestURI, "rid")
                if (rid.isNullOrBlank()) {
                    write(exchange, 400, "missing rid\n")
                    return@createContext
                }
                try {
                    createSpot(rid, localSpotType("mismatched"))
                    write(exchange, 500, "{\"mismatch\":false}\n")
                    return@createContext
                } catch (error: InterruptedException) {
                    Thread.currentThread().interrupt()
                    throw IllegalStateException("spot type mismatch interrupted", error)
                } catch (error: ExecutionException) {
                    state.record("SpotTypeMismatch", rid, error.cause?.message)
                } catch (error: TimeoutException) {
                    throw IllegalStateException("spot type mismatch timed out", error)
                } catch (error: RuntimeException) {
                    state.record("SpotTypeMismatch", rid, error.message)
                }
                try {
                    createSpot(rid, localSpotType("user"))
                    state.record("SpotTypeMismatchStateOk", rid, "existing")
                } catch (error: InterruptedException) {
                    Thread.currentThread().interrupt()
                    throw IllegalStateException("spot type mismatch follow-up interrupted", error)
                } catch (error: ExecutionException) {
                    throw IllegalStateException("spot type mismatch follow-up failed", error)
                } catch (error: TimeoutException) {
                    throw IllegalStateException("spot type mismatch follow-up failed", error)
                }
                write(exchange, 200, "{\"mismatch\":true}\n")
            }
            val nextExecutor = Executors.newCachedThreadPool()
            nextServer.executor = nextExecutor
            nextServer.start()
            server = nextServer
            executor = nextExecutor
            running = true
        } catch (error: Exception) {
            throw IllegalStateException("failed to start evidence endpoint $endpoint", error)
        }
    }

    override fun stop() {
        server?.stop(0)
        server = null
        executor?.shutdownNow()
        executor = null
        running = false
    }

    override fun isRunning(): Boolean =
        running

    private fun createSpot(
        spotId: String,
        stableType: String,
        request: Any? = null,
    ): ZLinkSpotCreateResult {
        var call = spots.getOrCreate(spotId, stableType)
        if (request != null) {
            call = call.request(request)
        }
        return call.submit()
            .toCompletableFuture()
            .get(5, TimeUnit.SECONDS)
    }

    private fun localSpotType(family: String): String = family

    private fun closeSpot(spotId: String): Boolean {
        val spot: SpotRef = spots.find(spotId)
            .toCompletableFuture()
            .get(5, TimeUnit.SECONDS)
            .orElseThrow { IllegalStateException("spot was not found: $spotId") }
        return spots.close(spot)
            .toCompletableFuture()
            .get(5, TimeUnit.SECONDS)
    }

    private fun queryValue(uri: URI, name: String): String? {
        val query = uri.rawQuery
        if (query.isNullOrBlank()) {
            return null
        }
        for (part in query.split("&")) {
            val pair = part.split("=", limit = 2)
            if (pair.size == 2 && name == pair[0]) {
                return URLDecoder.decode(pair[1], StandardCharsets.UTF_8)
            }
        }
        return null
    }

    private fun write(exchange: HttpExchange, status: Int, value: String) {
        val body = value.toByteArray(StandardCharsets.UTF_8)
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.write(body)
        exchange.close()
    }

    private fun <T> readJson(exchange: HttpExchange, type: Class<T>): T =
        exchange.requestBody.use { body -> json.readValue(body, type) }

    private fun writeJson(exchange: HttpExchange, status: Int, value: Any) {
        val body = json.writeValueAsBytes(value)
        exchange.responseHeaders.add("Content-Type", "application/json")
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.write(body)
        exchange.close()
    }

    private fun handle(exchange: HttpExchange, action: () -> Unit) {
        try {
            action()
        } catch (error: Exception) {
            if (exchange.responseBody != null) {
                write(exchange, 500, "${error::class.java.name}: ${error.message}\n")
            }
        }
    }

    private fun requirePost(exchange: HttpExchange) {
        if (exchange.requestMethod == "POST") {
            return
        }
        write(exchange, 405, "method not allowed\n")
        throw IllegalStateException("HTTP ${exchange.requestMethod} is not allowed for ${exchange.requestURI.path}")
    }
}
