package systems.zlink.e2e.kotlin.submitadmission

import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.io.IOException
import java.net.InetSocketAddress
import java.net.URI
import java.net.URLDecoder
import java.nio.charset.StandardCharsets
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardOpenOption
import java.time.Duration
import java.time.Instant
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionException
import java.util.concurrent.CompletionStage
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import org.springframework.beans.factory.ObjectProvider
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.context.annotation.Bean
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.channels.ZLinkFanoutClient
import systems.zlink.framework.channels.ZLinkFanoutHandler
import systems.zlink.framework.channels.ZLinkPublishMessageContext
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.channels.ZLinkRouteMessageContext
import systems.zlink.framework.channels.ZLinkRouteSendHandler
import systems.zlink.framework.channels.ZLinkSendHandler
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.monitoring.ZLinkPeerState
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer

private const val MESH = "submit.mesh"
private const val CHANNEL = "submit.channel"
private const val CLIENT_SERVER_CHANNEL = "submit.clientserver"
private const val FANOUT = "submit.fanout"
private lateinit var roleConfig: RoleConfig

fun main(args: Array<String>) {
    roleConfig = RoleConfig.parse(args)
    val builder = SpringApplicationBuilder(SubmitAdmissionApplication::class.java)
        .web(WebApplicationType.NONE)
        .properties("spring.main.banner-mode=off", "logging.level.root=ERROR")
    builder.application().setKeepAlive(true)
    builder.run(*args)
}

@EnableZLinkFramework
@SpringBootApplication(proxyBeanMethods = false)
class SubmitAdmissionApplication {
    @Bean
    fun roleState(): RoleState = RoleState(roleConfig)

    @Bean
    fun routeHandler(state: RoleState): RouteHandler = RouteHandler(state)

    @Bean
    fun channelHandler(state: RoleState): ChannelHandler = ChannelHandler(state)

    @Bean
    fun fanoutHandler(state: RoleState): FanoutHandler = FanoutHandler(state)

    @Bean
    fun configureFramework(state: RoleState): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options ->
            val config = state.config
            config.applicationHwmBytes?.let { bytes ->
                options.configureInboundDispatch().setApplicationHwmBytes(bytes)
            }
            options.setDefaultRequestTimeout(Duration.ofMillis(config.defaultRequestTimeoutMillis))
            if (config.role != "publisher" && config.role != "subscriber") {
                val mesh = options.addRouteMesh(MESH)
                    .listen(config.meshEndpoint)
                    .setRoutingId(RoutingId.from(config.rid))
                    .setDefaultRequestTimeout(Duration.ofSeconds(1))
                mesh.addRouteSendHandler(RouteHandler::class.java, RouteProbeMsg::class.java)
                mesh.channelName(CHANNEL)
                    .server()
                    .setWeight(if (config.role == "target") 100 else 0)
                    .addSendHandler(ChannelHandler::class.java, ChannelProbeMsg::class.java)
                if (config.peerEndpoint.isNotBlank()) {
                    mesh.peerConnections().connect(
                        RoutingId.from(config.peerRid),
                        config.peerEndpoint,
                    )
                }
                config.peerConnections.forEach { peer ->
                    mesh.peerConnections().connect(
                        RoutingId.from(peer.first),
                        peer.second,
                    )
                }
            }
            if (config.clientServerRole == "server") {
                val server = options.addClientServerChannel(CLIENT_SERVER_CHANNEL)
                    .server()
                    .setBindHost("127.0.0.1")
                    .setAdvertiseHost("127.0.0.1")
                    .setWeight(100)
                    .addSendHandler(ChannelHandler::class.java, ChannelProbeMsg::class.java)
                if (config.clientServerPort > 0) {
                    server.listen(config.clientServerPort)
                } else {
                    server.listen()
                }
            } else if (config.clientServerRole == "client") {
                options.addClientServerChannel(CLIENT_SERVER_CHANNEL)
                    .client()
                    .connect(config.clientServerEndpoint)
            }
            if (config.role == "publisher") {
                options.addFanoutChannel(FANOUT).enablePublisher(config.fanoutEndpoint)
            } else if (config.role == "subscriber") {
                options.addHandlersFromPackageOf(FanoutHandler::class.java)
                options.addFanoutChannel(FANOUT)
                    .connect(config.fanoutEndpoint)
                    .addPublishHandler(FanoutHandler::class.java, FanoutProbeEvent::class.java)
            }
        }

    @Bean(destroyMethod = "close")
    fun roleHttpServer(
        state: RoleState,
        routes: ZLinkRouteClient,
        client: ZLinkClient,
        fanout: ZLinkFanoutClient,
        meshRuntime: ZLinkRouteMeshRuntime,
        runtime: ObjectProvider<ZLinkFrameworkRuntime>,
    ): RoleHttpServer = RoleHttpServer(state, routes, client, fanout, meshRuntime, runtime)
}

data class RouteProbeMsg(
    val operationId: String,
    val sequence: Int,
    val payload: String = "",
)

data class ChannelProbeMsg(
    val operationId: String,
    val sequence: Int,
    val payload: String = "",
)

data class FanoutProbeEvent(
    val operationId: String,
    val sequence: Int,
    val payload: String = "",
)

class RouteHandler(private val state: RoleState) : ZLinkRouteSendHandler<RouteProbeMsg> {
    override fun handle(message: RouteProbeMsg, context: ZLinkRouteMessageContext): CompletionStage<Void> =
        state.handle("route", message.operationId, message.sequence)
}

class FanoutHandler(private val state: RoleState) : ZLinkFanoutHandler<FanoutProbeEvent> {
    override fun handle(message: FanoutProbeEvent, context: ZLinkPublishMessageContext): CompletionStage<Void> =
        state.handle("fanout", message.operationId, message.sequence)
}

class ChannelHandler(private val state: RoleState) : ZLinkSendHandler<ChannelProbeMsg> {
    override fun handle(message: ChannelProbeMsg, context: ZLinkMessageContext): CompletionStage<Void> =
        state.handle("channel", message.operationId, message.sequence)
}

class RoleState(val config: RoleConfig) {
    private val handlerStarted = AtomicInteger()
    private val handlerCompleted = AtomicInteger()
    private val handlerStartedByOperation = ConcurrentHashMap<String, AtomicInteger>()
    private val handlerCompletedByOperation = ConcurrentHashMap<String, AtomicInteger>()
    private val operations = ConcurrentHashMap<String, OperationState>()
    private val operationJobs = ConcurrentHashMap<String, Job>()
    private val operationScope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    fun handle(family: String, operationId: String, sequence: Int): CompletionStage<Void> {
        val started = handlerStarted.incrementAndGet()
        handlerStartedByOperation
            .computeIfAbsent(operationId) { AtomicInteger() }
            .incrementAndGet()
        record(
            "handler_started",
            mapOf(
                "family" to family,
                "operationId" to operationId,
                "sequence" to sequence.toString(),
                "handlerStarted" to started.toString(),
            ),
        )
        val completion = CompletableFuture<Void>()
        Thread.ofVirtual().start {
            try {
                if (config.gateFile.isNotBlank() && operationId.startsWith("handler-gate")) {
                    val gate = Path.of(config.gateFile)
                    while (!Files.exists(gate)) {
                        Thread.sleep(10L)
                    }
                }
                val completed = handlerCompleted.incrementAndGet()
                handlerCompletedByOperation
                    .computeIfAbsent(operationId) { AtomicInteger() }
                    .incrementAndGet()
                record(
                    "handler_completed",
                    mapOf(
                        "family" to family,
                        "operationId" to operationId,
                        "sequence" to sequence.toString(),
                        "handlerCompleted" to completed.toString(),
                    ),
                )
                completion.complete(null)
            } catch (failure: Throwable) {
                completion.completeExceptionally(failure)
            }
        }
        return completion
    }

    @Synchronized
    fun record(event: String, fields: Map<String, String>) {
        if (config.evidenceFile.isBlank()) return
        val line = buildString {
            append("{\"timestamp\":\"").append(escape(Instant.now().toString())).append('"')
            append(",\"role\":\"").append(escape(config.role)).append('"')
            append(",\"event\":\"").append(escape(event)).append('"')
            fields.toSortedMap().forEach { (key, value) ->
                append(",\"").append(escape(key)).append("\":\"").append(escape(value)).append('"')
            }
            append("}\n")
        }
        try {
            Files.writeString(
                Path.of(config.evidenceFile),
                line,
                StandardCharsets.UTF_8,
                StandardOpenOption.CREATE,
                StandardOpenOption.APPEND,
            )
        } catch (failure: IOException) {
            throw IllegalStateException("failed to write evidence", failure)
        }
    }

    fun counts(): String = "started=${handlerStarted.get()},completed=${handlerCompleted.get()}"

    fun handlerCounts(operationId: String): String =
        "started=${handlerStartedByOperation[operationId]?.get() ?: 0}," +
            "completed=${handlerCompletedByOperation[operationId]?.get() ?: 0}"

    fun startOperation(
        operationId: String,
        operation: suspend () -> Unit,
    ) {
        check(operations.putIfAbsent(operationId, OperationState()) == null) {
            "operation already exists: $operationId"
        }
        val job = operationScope.launch {
            val state = operations.getValue(operationId)
            state.status = "RUNNING"
            record("operation_started", mapOf("operationId" to operationId))
            try {
                operation()
                state.status = "Submitted"
                record(
                    "operation_terminal",
                    mapOf("operationId" to operationId, "status" to "Submitted"),
                )
            } catch (cancelled: CancellationException) {
                state.status = "CANCELLED"
                record(
                    "operation_terminal",
                    mapOf("operationId" to operationId, "status" to "CANCELLED"),
                )
            } catch (failure: Throwable) {
                state.status = errorKind(failure)
                record(
                    "operation_terminal",
                    mapOf("operationId" to operationId, "status" to state.status),
                )
            }
        }
        operationJobs[operationId] = job
        job.invokeOnCompletion { operationJobs.remove(operationId, job) }
    }

    fun cancelOperation(operationId: String): Boolean {
        val job = operationJobs[operationId] ?: return false
        if (!job.isActive) return false
        job.cancel()
        return true
    }

    fun operationStatus(operationId: String): String =
        operations[operationId]?.status ?: "UNKNOWN"

    fun close() {
        operationScope.cancel()
    }

    private fun escape(value: String): String = value.replace("\\", "\\\\").replace("\"", "\\\"")

    private fun errorKind(failure: Throwable): String {
        var current = failure
        while ((current is CompletionException || current is ExecutionException) && current.cause != null) {
            current = current.cause!!
        }
        return if (current is ZLinkFrameworkException) current.kind().name else current::class.simpleName ?: "FAILURE"
    }

    private class OperationState {
        @Volatile
        var status: String = "PENDING"
    }
}

class RoleHttpServer(
    private val state: RoleState,
    private val routes: ZLinkRouteClient,
    private val client: ZLinkClient,
    private val fanout: ZLinkFanoutClient,
    private val meshRuntime: ZLinkRouteMeshRuntime,
    private val runtime: ObjectProvider<ZLinkFrameworkRuntime>,
) : AutoCloseable {
    private val executor = Executors.newVirtualThreadPerTaskExecutor()
    private val server = HttpServer.create(InetSocketAddress("127.0.0.1", state.config.httpPort), 0)

    init {
        server.executor = executor
        server.createContext("/health") { exchange -> respond(exchange, 200, "ok") }
        server.createContext("/ready", ::ready)
        server.createContext("/clientserver-ready", ::clientServerReady)
        server.createContext("/send-node", ::sendNode)
        server.createContext("/send-channel", ::sendChannel)
        server.createContext("/send-clientserver", ::sendClientServer)
        server.createContext("/publish", ::publish)
        server.createContext("/start-node", ::startNode)
        server.createContext("/start-channel", ::startChannel)
        server.createContext("/start-clientserver", ::startClientServer)
        server.createContext("/start-publish", ::startPublish)
        server.createContext("/operation", ::operation)
        server.createContext("/cancel", ::cancel)
        server.createContext("/handler-counts", ::handlerCounts)
        server.createContext("/runtime-status", ::runtimeStatus)
        server.createContext("/shutdown", ::shutdown)
        server.createContext("/counts") { exchange -> respond(exchange, 200, state.counts()) }
        server.start()
        state.record("role_ready", mapOf("httpPort" to state.config.httpPort.toString()))
    }

    private fun ready(exchange: HttpExchange) {
        val target = query(exchange.requestURI)["targetRid"].orEmpty()
        val ready = state.config.role != "publisher" &&
            state.config.role != "subscriber" &&
            meshRuntime.snapshot(MESH).peers().any { peer ->
                (peer.state() == ZLinkPeerState.READY || peer.state() == ZLinkPeerState.NOT_REQUIRED) &&
                    peer.nodeRid().toString() == target
            }
        respond(exchange, if (ready) 200 else 503, ready.toString())
    }

    private fun clientServerReady(exchange: HttpExchange) {
        val ready = state.config.clientServerRole == "client" &&
            runtime.getIfAvailable()?.clientServerRuntime()?.isReady(CLIENT_SERVER_CHANNEL) == true
        respond(exchange, 200, ready.toString())
    }

    private fun sendNode(exchange: HttpExchange) {
        val values = query(exchange.requestURI)
        val operationId = values["operationId"] ?: "missing-operation"
        val targetRid = values["targetRid"] ?: "missing-target"
        val sequence = values["sequence"]?.toIntOrNull() ?: 0
        submit(exchange, "node", operationId) {
            routes.kotlin()
                .sendToNode(MESH, RoutingId.from(targetRid), RouteProbeMsg(
                    operationId, sequence, values["payload"].orEmpty()))
                .await()
        }
    }

    private fun sendChannel(exchange: HttpExchange) {
        val values = query(exchange.requestURI)
        val operationId = values["operationId"] ?: "missing-operation"
        val sequence = values["sequence"]?.toIntOrNull() ?: 0
        submit(exchange, "channel", operationId) {
            routes.kotlin().sendToChannel(CHANNEL, ChannelProbeMsg(
                operationId, sequence, values["payload"].orEmpty())).await()
        }
    }

    private fun sendClientServer(exchange: HttpExchange) {
        val values = query(exchange.requestURI)
        val operationId = values["operationId"] ?: "missing-operation"
        val sequence = values["sequence"]?.toIntOrNull() ?: 0
        submit(exchange, "clientserver", operationId) {
            client.kotlin()
                .sendToChannel(CLIENT_SERVER_CHANNEL, ChannelProbeMsg(
                    operationId, sequence, values["payload"].orEmpty()))
                .await()
        }
    }

    private fun publish(exchange: HttpExchange) {
        val values = query(exchange.requestURI)
        val operationId = values["operationId"] ?: "missing-operation"
        val sequence = values["sequence"]?.toIntOrNull() ?: 0
        submit(exchange, "fanout", operationId) {
            fanout.kotlin().publish(FANOUT, FanoutProbeEvent(
                operationId, sequence, values["payload"].orEmpty())).await()
        }
    }

    private fun startNode(exchange: HttpExchange) {
        val values = query(exchange.requestURI)
        val operationId = values.required("operationId")
        val targetRid = values.required("targetRid")
        val sequence = values["sequence"]?.toIntOrNull() ?: 0
        val payload = values["payload"].orEmpty()
        state.startOperation(operationId) {
            routes.kotlin()
                .sendToNode(MESH, RoutingId.from(targetRid), RouteProbeMsg(operationId, sequence, payload))
                .await()
        }
        respond(exchange, 202, operationId)
    }

    private fun startChannel(exchange: HttpExchange) {
        val values = query(exchange.requestURI)
        val operationId = values.required("operationId")
        val sequence = values["sequence"]?.toIntOrNull() ?: 0
        val payload = values["payload"].orEmpty()
        state.startOperation(operationId) {
            routes.kotlin().sendToChannel(CHANNEL, ChannelProbeMsg(operationId, sequence, payload)).await()
        }
        respond(exchange, 202, operationId)
    }

    private fun startClientServer(exchange: HttpExchange) {
        val values = query(exchange.requestURI)
        val operationId = values.required("operationId")
        val sequence = values["sequence"]?.toIntOrNull() ?: 0
        val payload = values["payload"].orEmpty()
        state.startOperation(operationId) {
            client.kotlin()
                .sendToChannel(CLIENT_SERVER_CHANNEL, ChannelProbeMsg(operationId, sequence, payload))
                .await()
        }
        respond(exchange, 202, operationId)
    }

    private fun startPublish(exchange: HttpExchange) {
        val values = query(exchange.requestURI)
        val operationId = values.required("operationId")
        val sequence = values["sequence"]?.toIntOrNull() ?: 0
        val payload = values["payload"].orEmpty()
        state.startOperation(operationId) {
            fanout.kotlin().publish(FANOUT, FanoutProbeEvent(operationId, sequence, payload)).await()
        }
        respond(exchange, 202, operationId)
    }

    private fun operation(exchange: HttpExchange) {
        val operationId = query(exchange.requestURI).required("operationId")
        respond(exchange, 200, state.operationStatus(operationId))
    }

    private fun cancel(exchange: HttpExchange) {
        val operationId = query(exchange.requestURI).required("operationId")
        respond(exchange, 200, if (state.cancelOperation(operationId)) "CANCEL_REQUESTED" else "NOT_PENDING")
    }

    private fun handlerCounts(exchange: HttpExchange) {
        val operationId = query(exchange.requestURI).required("operationId")
        respond(exchange, 200, state.handlerCounts(operationId))
    }

    private fun runtimeStatus(exchange: HttpExchange) {
        val status = runtime.getIfAvailable()?.status()
            ?: throw IllegalStateException("framework runtime is not ready")
        val inbound = status.inboundDispatch()
        respond(
            exchange,
            200,
            "state=${status.state()},acceptingWork=${status.acceptingWork()}," +
                "paused=${inbound.applicationReceivePaused()},pendingBytes=${inbound.pendingPayloadBytes()}," +
                "queuedBytes=${inbound.queuedPayloadBytes()},activeBytes=${inbound.activePayloadBytes()}",
        )
    }

    private fun shutdown(exchange: HttpExchange) {
        val current = runtime.getIfAvailable()
            ?: throw IllegalStateException("framework runtime is not ready")
        Thread.ofVirtual().start {
            current.shutdown(Duration.ofSeconds(10)).toCompletableFuture().join()
        }
        respond(exchange, 202, "SHUTDOWN_REQUESTED")
    }

    private fun submit(
        exchange: HttpExchange,
        family: String,
        operationId: String,
        operation: suspend () -> Unit,
    ) {
        try {
            runBlocking { operation() }
            state.record(
                "submit_terminal",
                mapOf("family" to family, "operationId" to operationId, "status" to "Submitted"),
            )
            respond(exchange, 200, "Submitted")
        } catch (failure: Throwable) {
            respond(exchange, 500, errorKind(failure))
        }
    }

    override fun close() {
        server.stop(0)
        executor.close()
        state.close()
    }

    private fun query(uri: URI): Map<String, String> {
        val raw = uri.rawQuery ?: return emptyMap()
        if (raw.isBlank()) return emptyMap()
        return raw.split('&').associate { item ->
            val pair = item.split('=', limit = 2)
            URLDecoder.decode(pair[0], StandardCharsets.UTF_8) to
                URLDecoder.decode(pair.getOrElse(1) { "" }, StandardCharsets.UTF_8)
        }
    }

    private fun Map<String, String>.required(key: String): String =
        this[key]?.takeIf { it.isNotBlank() }
            ?: throw IllegalArgumentException("missing query parameter: $key")

    private fun respond(exchange: HttpExchange, status: Int, body: String) {
        val bytes = body.toByteArray(StandardCharsets.UTF_8)
        exchange.sendResponseHeaders(status, bytes.size.toLong())
        exchange.responseBody.use { output -> output.write(bytes) }
    }

    private fun errorKind(failure: Throwable): String {
        var current = failure
        while ((current is CompletionException || current is ExecutionException) && current.cause != null) {
            current = current.cause!!
        }
        return if (current is ZLinkFrameworkException) current.kind().name else current.toString()
    }
}

data class RoleConfig(
    val role: String,
    val rid: String,
    val httpPort: Int,
    val meshEndpoint: String,
    val peerRid: String,
    val peerEndpoint: String,
    val fanoutEndpoint: String,
    val gateFile: String,
    val evidenceFile: String,
    val peerConnections: List<Pair<String, String>>,
    val applicationHwmBytes: Long?,
    val defaultRequestTimeoutMillis: Long,
    val clientServerRole: String,
    val clientServerEndpoint: String,
    val clientServerPort: Int,
) {
    companion object {
        fun parse(args: Array<String>): RoleConfig {
            val values = args.asSequence()
                .filter { it.startsWith("--") && it.contains('=') }
                .associate { argument ->
                    val pair = argument.substring(2).split('=', limit = 2)
                    pair[0] to pair[1]
                }
            return RoleConfig(
                role = values.required("role"),
                rid = values.required("rid"),
                httpPort = values.required("httpPort").toInt(),
                meshEndpoint = values["meshEndpoint"].orEmpty(),
                peerRid = values["peerRid"].orEmpty(),
                peerEndpoint = values["peerEndpoint"].orEmpty(),
                fanoutEndpoint = values["fanoutEndpoint"].orEmpty(),
                gateFile = values["gateFile"].orEmpty(),
                evidenceFile = values["evidenceFile"].orEmpty(),
                peerConnections = parsePeers(
                    values["peerRids"].orEmpty(),
                    values["peerEndpoints"].orEmpty(),
                ),
                applicationHwmBytes = values["applicationHwmBytes"]?.toLongOrNull(),
                defaultRequestTimeoutMillis =
                    values["defaultRequestTimeoutMillis"]?.toLongOrNull() ?: 1_000L,
                clientServerRole = values["clientServerRole"].orEmpty(),
                clientServerEndpoint = values["clientServerEndpoint"].orEmpty(),
                clientServerPort = values["clientServerPort"]?.toIntOrNull() ?: 0,
            )
        }

        private fun parsePeers(rids: String, endpoints: String): List<Pair<String, String>> {
            if (rids.isBlank() && endpoints.isBlank()) return emptyList()
            val ridValues = rids.split(',').filter(String::isNotBlank)
            val endpointValues = endpoints.split(',').filter(String::isNotBlank)
            require(ridValues.size == endpointValues.size) {
                "peerRids and peerEndpoints must contain the same number of values"
            }
            return ridValues.zip(endpointValues)
        }

        private fun Map<String, String>.required(key: String): String =
            get(key)?.takeIf { it.isNotBlank() }
                ?: throw IllegalArgumentException("--$key is required")
    }
}
