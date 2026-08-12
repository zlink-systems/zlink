package systems.zlink.e2e.kotlin.channelegress.role


import java.util.concurrent.ExecutionException
import systems.zlink.framework.actors.ZLinkActorJoinCompletion
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.file.Path
import java.time.Duration
import java.util.concurrent.CompletionException
import java.util.concurrent.CompletionStage
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.logging.Handler
import java.util.logging.LogRecord
import java.util.logging.Logger
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import org.springframework.beans.factory.ObjectProvider
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.ConfigurationProperties
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.SmartLifecycle
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import systems.zlink.contracts.core.RoutingId
import systems.zlink.e2e.kotlin.channelegress.shared.ChannelProbeRequestHandler
import systems.zlink.e2e.kotlin.channelegress.shared.Contracts
import systems.zlink.e2e.kotlin.channelegress.shared.EvidenceState
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions
import systems.zlink.framework.channels.ZLinkClient
import systems.zlink.framework.channels.ZLinkFanoutClient
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.configuration.ZLinkFrameworkOptions
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder
import systems.zlink.framework.errors.ZLinkFrameworkException
import systems.zlink.framework.kotlin.ZLinkSuspendingActor
import systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpot
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpot
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSpotTimerHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingRequestHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.kotlin.addHandler
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.requestToActor
import systems.zlink.framework.kotlin.requestToChannel
import systems.zlink.framework.kotlin.requestToSpot
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.locations.ZLinkLocationTopologyFilter
import systems.zlink.framework.locations.ZLinkPageRequest
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.monitoring.ZLinkClientServerRuntime
import systems.zlink.framework.monitoring.ZLinkFanoutRuntime
import systems.zlink.framework.monitoring.ZLinkListenerKind
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime
import systems.zlink.framework.handlers.ZLinkHandlerGroup
import systems.zlink.framework.spots.ZLinkActorCreateResponse
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult
import systems.zlink.framework.spots.ZLinkSpotClosingContext
import systems.zlink.framework.spots.ZLinkSpotContext
import systems.zlink.framework.spots.ZLinkSpotCreateResponse
import systems.zlink.framework.spots.ZLinkSpotManager
import systems.zlink.framework.spots.ZLinkTimer
import systems.zlink.framework.spots.ZLinkTimerTick
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkStreamError

@EnableZLinkFramework
@EnableConfigurationProperties(RoleOptions::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackages = ["systems.zlink.e2e.kotlin.channelegress"],
)
class RoleApplication {
    @Bean
    fun evidenceState(options: RoleOptions): EvidenceState = EvidenceState(options.role, options.rid)

    @Bean
    fun objectMapper(): ObjectMapper = jacksonObjectMapper()

    @Bean
    fun httpServer(
        options: RoleOptions,
        evidence: EvidenceState,
        json: ObjectMapper,
        client: ZLinkClient,
        routes: ZLinkRouteClient,
        fanout: ZLinkFanoutClient,
        runtimeOptions: ZLinkChannelRuntimeOptions,
        spots: ObjectProvider<ZLinkSpotManager>,
        actors: ObjectProvider<ZLinkActorManager>,
        routeRuntime: ZLinkRouteMeshRuntime,
        clientServerRuntime: ZLinkClientServerRuntime,
        fanoutRuntime: ZLinkFanoutRuntime,
        runtime: ObjectProvider<ZLinkFrameworkRuntime>,
    ): ChannelEgressHttpServer = ChannelEgressHttpServer(
        options,
        evidence,
        json,
        client,
        routes,
        fanout,
        runtimeOptions,
        spots.ifAvailable,
        actors.ifAvailable,
        routeRuntime,
        clientServerRuntime,
        fanoutRuntime,
        runtime,
    )

    @Bean
    fun framework(role: RoleOptions, evidence: EvidenceState): ZLinkFrameworkConfigurer =
        ZLinkFrameworkConfigurer { options -> configureFramework(options, role, evidence) }

    @Bean
    fun locationStore(options: RoleOptions): ZLinkRedisLocationStore =
        ZLinkRedisLocationStore(
            ZLinkRedisLocationOptions()
                .setConnectionString(options.redisLocationEndpoint)
                .setKeyPrefix(options.locationKeyPrefix),
        )

    private fun configureFramework(
        options: ZLinkFrameworkOptions,
        role: RoleOptions,
        evidence: EvidenceState,
    ) {
        installDispatchEvidence(evidence)
        options.useCoroutineHandlers(Dispatchers.Default)
        options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(500))
        options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(2))
        options.configureDispatch().messageFlow(
            ZLinkMessageFlowLogMode.NORMAL,
        )
        options.addHandlersFromPackageOf(ChannelProbeRequestHandler::class.java)
        options.addHandlersFromPackageOf(SpotWorkflowHandler::class.java)

        if (
            role.gameServers.isNotEmpty() || role.gameClients.isNotEmpty() ||
            role.instanceSpot || role.objectClient
        ) {
            val game = options.addRouteMesh(Contracts.GAME_MESH)
                .setBindHost(role.gameBindHost)
                .setAdvertiseHost(role.gameAdvertiseHost)
                .listen(URI.create(role.gameEndpoint).port)
                .setRoutingId(RoutingId.from(role.rid))
            connectPeers(game, role.gamePeerRids, role.gamePeerEndpoints)
            registerRouteChannels(game, role.gameServers, role.gameClients)
            if (role.instanceSpot || role.objectClient) {
                val objects = game.objects()
                if (role.instanceSpot) {
                    val server = objects.server()
                    server.addEntrySpot(Config12EntrySpot::class.java)
                    server.addSpotFactory(
                        Contracts.INSTANCE_SPOT_TYPE,
                        Config12Spot::class.java,
                    ) { it.disableRelocation() }
                    server.addActorFactory(
                        Contracts.ACTOR_TYPE,
                        Config12Actor::class.java,
                        Config12ActorFactory::class.java,
                    ) { it.disableRelocation() }
                }
                if (role.objectClient) objects.client()
            }
        }

        if (role.auditServers.isNotEmpty() || role.auditClients.isNotEmpty()) {
            val audit = options.addRouteMesh(Contracts.AUDIT_MESH)
                .listen(role.auditEndpoint)
                .setRoutingId(RoutingId.from("${role.rid}-audit"))
            connectPeers(audit, role.auditPeerRids, role.auditPeerEndpoints)
            registerRouteChannels(audit, role.auditServers, role.auditClients)
        }

        if (role.workflowClient || role.workflowServer) {
            val workflow = options.addClientServerChannel(Contracts.WORKFLOW_CHANNEL)
            if (role.workflowClient) workflow.client()
            if (role.workflowServer) {
                val server = workflow.server()
                    .setBindHost(role.workflowBindHost)
                    .setAdvertiseHost(role.workflowAdvertiseHost)
                    .listen(role.workflowPort)
                    .setWeight(role.workflowWeight)
                    .addHandlerGroup(Contracts.HANDLER_GROUP)
                if (role.objectClient) {
                    server.addHandlerGroup(Contracts.STATE_HANDLER_GROUP)
                }
            }
        }

        if (role.fanoutPublisher || role.fanoutSubscriber) {
            val fanout = options.addFanoutChannel(Contracts.FANOUT_CHANNEL)
            if (role.fanoutPublisher) {
                fanout.enablePublisher(role.fanoutPort)
                    .setBindHost(role.fanoutBindHost)
                    .setAdvertiseHost(role.fanoutAdvertiseHost)
                    .setRoutingId(RoutingId.from("${role.rid}-fanout"))
            }
            if (role.fanoutSubscriber) {
                fanout.enableSubscriber().addHandlerGroup(Contracts.FANOUT_HANDLER_GROUP)
            }
        }

        if (role.streamServer) {
            options.addStreamNode(Contracts.STREAM_NODE)
                .setBindHost(role.streamBindHost)
                .setAdvertiseHost(role.streamAdvertiseHost)
                .bind(role.streamPort)
                .registerSession(Config12Session::class.java)
        }
    }

    private fun installDispatchEvidence(evidence: EvidenceState) {
        Logger.getLogger("systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer")
            .addHandler(object : Handler() {
                override fun publish(record: LogRecord) {
                    val fields = diagnosticsFields(record.message) ?: return
                    if (fields["outcome"] == "ERROR") {
                        evidence.add(
                            "dispatch-error",
                            "${fields["reason"]}/${fields["action"]}/${fields["packet"]}",
                        )
                    }
                }

                override fun flush() = Unit
                override fun close() = Unit
            })
    }

    private fun diagnosticsFields(message: String?): Map<String, String>? {
        if (message?.startsWith("message flow ") != true) return null
        return message.removePrefix("message flow ")
            .split(' ')
            .mapNotNull { field ->
                field.split('=', limit = 2).takeIf { it.size == 2 }?.let { it[0] to it[1] }
            }
            .toMap()
    }

    private fun registerRouteChannels(
        node: ZLinkMeshNodeBuilder,
        servers: List<String>,
        clients: List<String>,
    ) {
        servers.forEach { node.channelName(it).server().addHandlerGroup(Contracts.HANDLER_GROUP) }
        clients.forEach { node.channelName(it).client() }
    }

    private fun connectPeers(
        node: ZLinkMeshNodeBuilder,
        peerRids: List<String>,
        peerEndpoints: List<String>,
    ) {
        require(peerRids.size == peerEndpoints.size) { "peer RID and endpoint counts must match" }
        peerRids.zip(peerEndpoints).forEach { (rid, endpoint) ->
            if (rid.isNotBlank() && endpoint.isNotBlank()) {
                node.peerConnections().connect(RoutingId.from(rid.trim()), endpoint.trim())
            }
        }
    }
}

fun main(args: Array<String>) {
    require(args.size == 2 && args[0] == "--config" && args[1].isNotBlank()) {
        "Usage: channel-egress-kotlin-role --config <path>"
    }
    val environment = StandardEnvironment().apply {
        propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
        propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
    }
    SpringApplicationBuilder(RoleApplication::class.java)
        .environment(environment)
        .properties("spring.config.location=${Path.of(args[1]).toAbsolutePath().toUri()}")
        .web(WebApplicationType.NONE)
        .apply { application().setKeepAlive(true) }
        .run()
}

@ConfigurationProperties("e2e")
data class RoleOptions(
    val role: String,
    val rid: String,
    val instanceMarker: String,
    val httpEndpoint: String,
    val redisLocationEndpoint: String,
    val locationKeyPrefix: String,
    val logDirectory: String,
    val gameEndpoint: String,
    val gameBindHost: String = "127.0.0.1",
    val gameAdvertiseHost: String = "127.0.0.1",
    val gamePeerRids: List<String> = emptyList(),
    val gamePeerEndpoints: List<String> = emptyList(),
    val gameServers: List<String> = emptyList(),
    val gameClients: List<String> = emptyList(),
    val auditEndpoint: String,
    val auditPeerRids: List<String> = emptyList(),
    val auditPeerEndpoints: List<String> = emptyList(),
    val auditServers: List<String> = emptyList(),
    val auditClients: List<String> = emptyList(),
    val workflowBindHost: String = "127.0.0.1",
    val workflowAdvertiseHost: String = "127.0.0.1",
    val workflowPort: Int,
    val workflowWeight: Int,
    val workflowClient: Boolean,
    val workflowServer: Boolean,
    val instanceSpot: Boolean = false,
    val objectClient: Boolean = false,
    val fanoutPublisher: Boolean = false,
    val fanoutSubscriber: Boolean = false,
    val fanoutBindHost: String = "127.0.0.1",
    val fanoutAdvertiseHost: String = "127.0.0.1",
    val fanoutPort: Int = 0,
    val streamServer: Boolean = false,
    val streamBindHost: String = "127.0.0.1",
    val streamAdvertiseHost: String = "127.0.0.1",
    val streamPort: Int = 0,
)

class Config12Actor(
    override val context: ZLinkActorContext,
) : ZLinkSuspendingActor() {
    val actorId: String get() = context.actorId()
    override suspend fun onJoinCompletedSuspending(
        completion: ZLinkActorJoinCompletion,
    ) = Unit
}

class Config12ActorFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor = Config12Actor(context)
}

class Config12EntrySpot(
    override val context: ZLinkEntrySpotContext,
    private val evidence: EvidenceState,
) : ZLinkSuspendingEntrySpot<Config12Actor>() {
    override fun configure() {
        context.handlers().addHandler<ActorProbeHandler>()
    }

    override suspend fun onCreateActorSuspending(
        actor: Config12Actor,
        createRequest: ZLinkMessage,
    ): ZLinkActorCreateResponse {
        evidence.add("actor-create", "actor=${actor.actorId}")
        return ZLinkActorCreateResponse.accept()
    }

    override suspend fun onJoinedActorSuspending(actor: Config12Actor) = Unit
    override suspend fun onLeaveActorSuspending(actor: Config12Actor) = Unit
}

class ActorProbeHandler(
    private val evidence: EvidenceState,
) : ZLinkSuspendingEntrySpotActorRequestHandler<
    Config12EntrySpot,
    Config12Actor,
    Contracts.ObjectProbeReq,
    Contracts.ObjectProbeRes
> {
    override suspend fun handle(
        entrySpot: Config12EntrySpot,
        actor: Config12Actor,
        context: ZLinkMessageContext,
        request: Contracts.ObjectProbeReq,
    ): Contracts.ObjectProbeRes {
        evidence.add("actor-request", "actor=${actor.actorId}|id=${request.id}")
        return Contracts.ObjectProbeRes(request.id, "actor", actor.actorId, evidence.role)
    }
}

class Config12Spot(
    override val context: ZLinkSpotContext,
    private val evidence: EvidenceState,
) : ZLinkSuspendingSpot<ZLinkActor>() {
    private var timer: ZLinkTimer? = null

    override fun configure() {
        context.handlers().addHandler<SpotWorkflowHandler>()
        context.handlers().addHandler<SpotObjectProbeHandler>()
    }

    override suspend fun onCreateSuspending(request: ZLinkMessage): ZLinkSpotCreateResponse {
        evidence.add("spot-initialize", "spot=${context.spotId()}")
        return ZLinkSpotCreateResponse.accept()
    }

    override suspend fun onActorJoinSuspending(
        actorId: String,
        request: ZLinkMessage,
    ): ZLinkSpotActorJoinResult = ZLinkSpotActorJoinResult.reject()

    override suspend fun onJoinedActorSuspending(actor: ZLinkActor) = Unit
    override suspend fun onLeaveActorSuspending(actor: ZLinkActor) = Unit

    override suspend fun onClosingSuspending(context: ZLinkSpotClosingContext) {
        evidence.add("spot-closing", "spot=${this.context.spotId()}")
    }

    suspend fun startTimer(name: String) {
        timer = context.addTimer(
            name,
            Duration.ofMillis(1),
            SpotWorkflowTimerHandler::class.java,
            null,
        ).await()
    }

    fun closeTimer() {
        timer?.close()
        timer = null
    }
}

class SpotWorkflowHandler(
    private val evidence: EvidenceState,
) : ZLinkSuspendingSpotRequestHandler<
    Config12Spot,
    Contracts.SpotWorkflowReq,
    Contracts.SpotWorkflowRes
> {
    override suspend fun handle(
        spot: Config12Spot,
        request: Contracts.SpotWorkflowReq,
    ): Contracts.SpotWorkflowRes {
        val sequence = mutableListOf("handler-start")
        evidence.add("spot-handler-start", "spot=${spot.context.spotId()}|id=${request.id}")
        spot.context.outbound()
            .requestToChannel(
                Contracts.WORKFLOW_CHANNEL,
                Contracts.ChannelProbeReq("${request.id}-workflow"),
            )
            .timeout(Duration.ofSeconds(5))
            .awaitReply<Contracts.ChannelProbeRes>()
        sequence += "workflow-reply"
        sequence += "handler-end"
        evidence.add("spot-workflow-reply", "spot=${spot.context.spotId()}|id=${request.id}")
        evidence.add("spot-handler-end", "spot=${spot.context.spotId()}|id=${request.id}")
        spot.startTimer(request.timerName)
        sequence += "timer-start"
        evidence.add(
            "spot-timer-start",
            "spot=${spot.context.spotId()}|id=${request.id}|sequence=${sequence.joinToString()}",
        )
        return Contracts.SpotWorkflowRes(request.id, sequence)
    }
}

class SpotWorkflowTimerHandler(
    private val evidence: EvidenceState,
) : ZLinkSuspendingSpotTimerHandler<Config12Spot> {
    override suspend fun handle(spot: Config12Spot, tick: ZLinkTimerTick) {
        val spotId = spot.context.spotId()
        spot.context.outbound()
            .requestToChannel(
                Contracts.WORKFLOW_CHANNEL,
                Contracts.ChannelProbeReq("$spotId-timer-workflow"),
            )
            .timeout(Duration.ofSeconds(5))
            .awaitReply<Contracts.ChannelProbeRes>()
        evidence.add("spot-timer-workflow-reply", "spot=$spotId|timer=${tick.name()}")
        evidence.add(
            "spot-timer-end",
            "spot=$spotId|timer=${tick.name()}|sequence=" +
                "handler-start,workflow-reply,handler-end,timer-start,workflow-reply,timer-end",
        )
        spot.closeTimer()
    }
}

class SpotObjectProbeHandler(
    private val evidence: EvidenceState,
) : ZLinkSuspendingSpotRequestHandler<
    Config12Spot,
    Contracts.ObjectProbeReq,
    Contracts.ObjectProbeRes
> {
    override suspend fun handle(
        spot: Config12Spot,
        request: Contracts.ObjectProbeReq,
    ): Contracts.ObjectProbeRes {
        val spotId = spot.context.spotId()
        evidence.add("spot-request", "spot=$spotId|id=${request.id}")
        return Contracts.ObjectProbeRes(request.id, "spot", spotId, evidence.role)
    }
}

@ZLinkHandlerGroup(Contracts.STATE_HANDLER_GROUP)
class StateAddressRequestHandler(
    routes: ZLinkRouteClient,
    actors: ZLinkActorClient,
    private val evidence: EvidenceState,
) : ZLinkSuspendingRequestHandler<Contracts.StateAddressReq, Contracts.StateAddressRes> {
    private val routes = routes.kotlin()
    private val actors = actors.kotlin()

    override suspend fun handle(
        request: Contracts.StateAddressReq,
        context: ZLinkMessageContext,
    ): Contracts.StateAddressRes {
        evidence.add("state-address-start", "id=${request.id}")
        val spot = routes.requestToSpot<Contracts.ObjectProbeRes>(
            request.spotId,
            Contracts.ObjectProbeReq(request.id),
        ).timeout(Duration.ofSeconds(5)).await()
        val actor = actors.requestToActor<Contracts.ObjectProbeRes>(
            request.actorId,
            Contracts.ObjectProbeReq(request.id),
        ).timeout(Duration.ofSeconds(5)).await()
        evidence.add("state-address-end", "id=${request.id}")
        return Contracts.StateAddressRes(
            request.id,
            listOf(
                "spot:${spot.objectId}:${spot.role}",
                "actor:${actor.objectId}:${actor.role}",
            ),
        )
    }
}

class Config12Session(
    private val sessionContext: ZLinkSessionContext,
    private val evidence: EvidenceState,
) : ZLinkSuspendingSession() {
    override fun context(): ZLinkSessionContext = sessionContext

    override suspend fun onConnectedSuspending() {
        evidence.add("stream-connected", "session=${sessionContext.sessionId()}")
    }

    override suspend fun onDisconnectedSuspending() {
        evidence.add("stream-disconnected", "session=${sessionContext.sessionId()}")
    }

    override suspend fun onErrorSuspending(error: ZLinkStreamError) {
        evidence.add("stream-error", error.toString())
    }

    override suspend fun onDispatchSuspending(
        dispatch: ZLinkSessionDispatchContext,
        payload: ZLinkMessage,
    ) {
        evidence.add("stream", "packet=${dispatch.packetName()}")
    }
}

class ChannelEgressHttpServer(
    private val options: RoleOptions,
    private val evidence: EvidenceState,
    private val json: ObjectMapper,
    client: ZLinkClient,
    routes: ZLinkRouteClient,
    fanout: ZLinkFanoutClient,
    private val runtimeOptions: ZLinkChannelRuntimeOptions,
    spots: ZLinkSpotManager?,
    actors: ZLinkActorManager?,
    private val routeRuntime: ZLinkRouteMeshRuntime,
    private val clientServerRuntime: ZLinkClientServerRuntime,
    private val fanoutRuntime: ZLinkFanoutRuntime,
    private val runtime: ObjectProvider<ZLinkFrameworkRuntime>,
) : SmartLifecycle {
    private val client = client.kotlin()
    private val routes = routes.kotlin()
    private val fanout = fanout.kotlin()
    private val spots = spots?.kotlin()
    private val actors = actors?.kotlin()
    private var server: HttpServer? = null
    private var executor: ExecutorService? = null
    @Volatile private var running = false
    @Volatile private var drain: CompletionStage<*>? = null

    override fun start() {
        val endpoint = URI.create(options.httpEndpoint)
        val http = HttpServer.create(InetSocketAddress(endpoint.host, endpoint.port), 0)
        val pool = Executors.newVirtualThreadPerTaskExecutor()
        http.executor = pool
        http.createContext("/health") { exchange ->
            write(exchange, mapOf("status" to "ready", "role" to options.role, "rid" to options.rid))
        }
        http.createContext("/evidence") { exchange -> write(exchange, evidence.snapshot()) }
        http.createContext("/request") { exchange -> guarded(exchange) { request(exchange) } }
        http.createContext("/send") { exchange -> guarded(exchange) { send(exchange) } }
        http.createContext("/fanout/publish") { exchange -> guarded(exchange) { publish(exchange) } }
        http.createContext("/status/route") { exchange -> guarded(exchange) { write(exchange, routeStatus(Contracts.GAME_MESH)) } }
        http.createContext("/status/audit") { exchange -> guarded(exchange) { write(exchange, routeStatus(Contracts.AUDIT_MESH)) } }
        http.createContext("/status/workflow") { exchange -> guarded(exchange) { write(exchange, workflowStatus()) } }
        http.createContext("/status/fanout") { exchange -> guarded(exchange) { write(exchange, fanoutStatus()) } }
        http.createContext("/status/locations") { exchange -> guarded(exchange) { write(exchange, locationTopology()) } }
        http.createContext("/status/listeners") { exchange -> guarded(exchange) { write(exchange, listenerStatus()) } }
        http.createContext("/objects/spots") { exchange -> guarded(exchange) { spots(exchange) } }
        http.createContext("/objects/actors") { exchange -> guarded(exchange) { actors(exchange) } }
        http.createContext("/objects/state-address") { exchange -> guarded(exchange) { stateAddress(exchange) } }
        http.createContext("/control/weight") { exchange -> guarded(exchange) { updateWeight(exchange) } }
        http.createContext("/control/hold") { exchange -> evidence.hold(); write(exchange, mapOf("status" to "held")) }
        http.createContext("/control/release") { exchange -> evidence.release(); write(exchange, mapOf("status" to "released")) }
        http.createContext("/control/drain") { exchange ->
            evidence.add("drain-start", "role=${options.role}")
            drain = runtime.`object`.shutdown(Duration.ofSeconds(30))
            write(exchange, mapOf("status" to "draining"))
        }
        http.start()
        server = http
        executor = pool
        running = true
    }

    private fun request(exchange: HttpExchange) {
        val request = read<Contracts.InvokeReq>(exchange)
        val started = System.nanoTime()
        try {
            val reply = runBlocking {
                if (request.channel == Contracts.WORKFLOW_CHANNEL) {
                    client.requestToChannel<Contracts.ChannelProbeRes>(
                        request.channel,
                        Contracts.ChannelProbeReq(request.id, request.mode),
                    ).timeout(if (request.mode == "hold") Duration.ofSeconds(30) else Duration.ofSeconds(5)).await()
                } else {
                    routes.requestToChannel<Contracts.ChannelProbeRes>(
                        request.channel,
                        Contracts.ChannelProbeReq(request.id, request.mode),
                    ).timeout(Duration.ofSeconds(5)).await()
                }
            }
            write(exchange, Contracts.InvokeRes(true, reply = reply, elapsedMilliseconds = elapsed(started)))
        } catch (failure: Throwable) {
            evidence.add("request-error", describe(failure))
            write(exchange, Contracts.InvokeRes(false, publicError(failure), elapsedMilliseconds = elapsed(started)))
        }
    }

    private fun send(exchange: HttpExchange) {
        val request = read<Contracts.InvokeReq>(exchange)
        val started = System.nanoTime()
        try {
            runBlocking {
                if (request.channel == Contracts.WORKFLOW_CHANNEL) {
                    client.sendToChannel(request.channel, Contracts.ChannelProbeMsg(request.id)).await()
                } else {
                    routes.sendToChannel(request.channel, Contracts.ChannelProbeMsg(request.id)).await()
                }
            }
            write(exchange, Contracts.SendRes(true, elapsedMilliseconds = elapsed(started)))
        } catch (failure: Throwable) {
            evidence.add("send-error", describe(failure))
            write(exchange, Contracts.SendRes(false, publicError(failure), elapsed(started)))
        }
    }

    private fun publish(exchange: HttpExchange) {
        val request = read<Contracts.FanoutProbeEvent>(exchange)
        runBlocking { fanout.publish(Contracts.FANOUT_CHANNEL, "probe", request).await() }
        write(exchange, mapOf("published" to true, "id" to request.id))
    }

    private fun spots(exchange: HttpExchange) {
        val manager = requireNotNull(spots) { "this role has no Object Server or Client role" }
        val path = exchange.requestURI.path.split('/')
        if (path.size == 3) {
            val request = read<Contracts.SpotCreateReq>(exchange)
            val result = runBlocking {
                manager.getOrCreate(request.spotId, Contracts.INSTANCE_SPOT_TYPE)
                    .inMesh(Contracts.GAME_MESH)
                    .request(request)
                    .timeout(Duration.ofSeconds(5))
                    .await()
            }
            write(exchange, Contracts.SpotCreateRes(result.spot().spotId(), result.spot().nodeRid().toString()))
            return
        }
        if (path.size == 5 && path[4] == "workflow") {
            val request = read<Contracts.SpotWorkflowReq>(exchange)
            val reply = runBlocking {
                routes.requestToSpot<Contracts.SpotWorkflowRes>(path[3], request)
                    .timeout(Duration.ofSeconds(8))
                    .await()
            }
            write(exchange, reply)
            return
        }
        error("spot path must create or invoke workflow")
    }

    private fun actors(exchange: HttpExchange) {
        val manager = requireNotNull(actors) { "this role has no Object Server role" }
        require(exchange.requestURI.path == "/objects/actors") { "actor path must be /objects/actors" }
        val request = read<Contracts.ActorCreateReq>(exchange)
        val result = runBlocking {
            manager.create(request.actorId, Contracts.ACTOR_TYPE)
                .inMesh(Contracts.GAME_MESH)
                .request(request)
                .timeout(Duration.ofSeconds(5))
                .await()
        }
        val actor: ActorRef = when (result) {
            is ZLinkActorCreateResult.Created -> result.actor()
            is ZLinkActorCreateResult.Existing -> result.actor()
            is ZLinkActorCreateResult.Rejected -> error("actor creation was rejected")
        }
        write(exchange, Contracts.ActorCreateRes(actor.actorId(), actor.nodeRid().toString()))
    }

    private fun stateAddress(exchange: HttpExchange) {
        val request = read<Contracts.StateAddressReq>(exchange)
        val reply = runBlocking {
            client.requestToChannel<Contracts.StateAddressRes>(Contracts.WORKFLOW_CHANNEL, request)
                .timeout(Duration.ofSeconds(5))
                .await()
        }
        write(exchange, reply)
    }

    private fun updateWeight(exchange: HttpExchange) {
        val path = exchange.requestURI.path.split('/')
        require(path.size == 4) { "weight path must be /control/weight/{value}" }
        val weight = path[3].toInt()
        runtimeOptions.clientServerChannel(Contracts.WORKFLOW_CHANNEL).configureServerSocket().weight(weight)
        evidence.add("weight", "value=$weight")
        write(exchange, mapOf("weight" to weight))
    }

    private fun routeStatus(meshName: String): Map<String, Any> {
        val snapshot = routeRuntime.snapshot(meshName)
        return mapOf(
            "state" to snapshot.state().name,
            "isReady" to snapshot.isReady,
            "readyPeerCount" to snapshot.readyPeerCount(),
            "peers" to snapshot.peers().map { mapOf("rid" to it.nodeRid().toString(), "state" to it.state().name) },
            "channels" to snapshot.channels().map {
                mapOf("channelName" to it.channelName(), "isReady" to it.isReady, "readyTargetCount" to it.readyTargetCount())
            },
        )
    }

    private fun workflowStatus(): Contracts.WorkflowStatus {
        val snapshot = clientServerRuntime.snapshot(Contracts.WORKFLOW_CHANNEL)
        return Contracts.WorkflowStatus(
            snapshot.state().name,
            snapshot.isReady,
            snapshot.readyTargetCount(),
            snapshot.localRole().name,
            snapshot.targets().map { Contracts.WorkflowTarget(it.nodeRid().toString(), it.weight(), it.state().name) },
        )
    }

    private fun fanoutStatus(): Map<String, Any> {
        val snapshot = fanoutRuntime.snapshot(Contracts.FANOUT_CHANNEL)
        return mapOf(
            "state" to snapshot.state().name,
            "isReady" to snapshot.isReady,
            "readyPublisherCount" to snapshot.readyPublisherCount(),
            "publishers" to snapshot.publishers().map { mapOf("rid" to it.nodeRid().toString(), "state" to it.state().name) },
        )
    }

    private fun locationTopology(): List<Contracts.LocationEntry> =
        runtime.`object`.monitoringLocationRuntimeQuery()
            .listTopology(ZLinkLocationTopologyFilter.all(), ZLinkPageRequest(1_000, null))
            .toCompletableFuture().join().items().map {
                Contracts.LocationEntry(it.meshName(), it.nodeRid().toString(), it.endpoint(), it.state().name)
            }

    private fun listenerStatus(): List<Contracts.ListenerStatus> {
        fun status(kind: ZLinkListenerKind, name: String, label: String) =
            runtime.getObject().listenerStatus(kind, name).let {
                Contracts.ListenerStatus(label, name, true, it.endpoint(),
                    "public ZLinkFrameworkRuntime.listenerStatus")
            }
        val statuses = mutableListOf<Contracts.ListenerStatus>()
        if (options.gameServers.isNotEmpty()) {
            statuses += status(ZLinkListenerKind.ROUTE_MESH, Contracts.GAME_MESH, "RouteMesh")
        }
        if (options.workflowServer) {
            statuses += status(ZLinkListenerKind.CLIENT_SERVER, Contracts.WORKFLOW_CHANNEL, "ClientServer")
        }
        if (options.fanoutPublisher) {
            statuses += status(ZLinkListenerKind.FANOUT, Contracts.FANOUT_CHANNEL, "Fanout")
        }
        if (options.streamServer) {
            statuses += status(ZLinkListenerKind.STREAM, Contracts.STREAM_NODE, "STREAM")
        }
        return statuses
    }

    private inline fun <reified T> read(exchange: HttpExchange): T =
        exchange.requestBody.use { json.readValue(it, T::class.java) }

    private fun write(exchange: HttpExchange, value: Any, status: Int = 200) {
        val body = json.writeValueAsBytes(value)
        exchange.responseHeaders.add("Content-Type", "application/json")
        exchange.sendResponseHeaders(status, body.size.toLong())
        exchange.responseBody.use { it.write(body) }
    }

    private fun guarded(exchange: HttpExchange, action: () -> Unit) {
        try {
            action()
        } catch (failure: Throwable) {
            write(exchange, mapOf("error" to describe(failure)), 500)
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

    companion object {
        private fun elapsed(started: Long): Long = ((System.nanoTime() - started) / 1_000_000L).coerceAtLeast(0)

        private fun unwrap(failure: Throwable): Throwable {
            var current = failure
            while ((current is CompletionException || current is ExecutionException) && current.cause != null) {
                current = current.cause!!
            }
            return current
        }

        private fun publicError(failure: Throwable): String {
            val current = unwrap(failure)
            return if (current is ZLinkFrameworkException) current.kind().name else current::class.java.simpleName
        }

        private fun describe(failure: Throwable): String {
            val current = unwrap(failure)
            return current::class.java.simpleName + (current.message?.takeIf { it.isNotBlank() }?.let { ":$it" } ?: "")
        }
    }
}
