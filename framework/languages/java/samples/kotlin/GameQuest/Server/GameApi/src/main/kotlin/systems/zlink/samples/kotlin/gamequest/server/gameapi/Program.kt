package systems.zlink.samples.kotlin.gamequest.server.gameapi

import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.fasterxml.jackson.module.kotlin.readValue
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.nio.file.Path
import java.time.Instant
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.future.await
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.ConfigurableApplicationContext
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.ZLinkMessageContext
import systems.zlink.framework.actors.ActorRef
import systems.zlink.framework.actors.ZLinkActor
import systems.zlink.framework.actors.ZLinkActorContext
import systems.zlink.framework.actors.ZLinkActorCreateResult
import systems.zlink.framework.actors.ZLinkActorManager
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.ZLinkSuspendingActorFactory
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpot
import systems.zlink.framework.kotlin.ZLinkSuspendingEntrySpotActorSendHandler
import systems.zlink.framework.kotlin.ZLinkSuspendingSession
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
import systems.zlink.framework.messaging.ZLinkMessage
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.framework.streams.ZLinkSessionContext
import systems.zlink.framework.streams.ZLinkSessionDispatchContext
import systems.zlink.framework.streams.ZLinkSessionActor
import systems.zlink.framework.spots.ZLinkEntrySpotContext
import systems.zlink.samples.kotlin.gamequest.server.configuration.RedisSampleStore
import systems.zlink.samples.kotlin.gamequest.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.gamequest.server.configuration.SampleNames
import systems.zlink.samples.kotlin.gamequest.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.gamequest.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.gamequest.shared.contracts.CollectItemReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.CollectItemRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.CompleteMissionReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.CompleteMissionRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.DeleteQuestProjectionReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.DeleteQuestProjectionRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.EnterAreaReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.EnterAreaRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GameQuestServerAssertRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GameplayMsg
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GetGameplaySnapshotReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GetGameplaySnapshotRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GetQuestProgressReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GetQuestProgressRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.ItemCountSnapshot
import systems.zlink.samples.kotlin.gamequest.shared.contracts.JoinSessionReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.JoinSessionRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.KillCountSnapshot
import systems.zlink.samples.kotlin.gamequest.shared.contracts.KillMonsterReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.KillMonsterRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestCompletedEvent
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestIds
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProcessingRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProgress
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProgressReconciledEvent
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProgressedEvent
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestRewardGrantedEvent
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestStatuses
import systems.zlink.samples.kotlin.gamequest.shared.contracts.RebuildQuestProjectionReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.StoredQuestEvent
import systems.zlink.samples.kotlin.gamequest.shared.contracts.SyncQuestProgressReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.SyncQuestProgressRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.UnlockFeatureReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.UnlockFeatureRes

fun main(args: Array<String>) {
    val app = Program.run(SampleTopology.configPath(args))
    val http = startHttp(Program.store, app.getBean(SampleTopology::class.java))
    Runtime.getRuntime().addShutdownHook(Thread {
        http.stop(0)
        app.close()
    })
    Thread.currentThread().join()
}

@EnableZLinkFramework
@EnableConfigurationProperties(SampleTopology::class)
@SpringBootApplication(
    proxyBeanMethods = false,
    scanBasePackageClasses = [Program::class],
)
class Program {
    @Bean
    fun gameApiFramework(topology: SampleTopology): ZLinkFrameworkConfigurer {
        val api = topology.gameApi()
        return ZLinkFrameworkConfigurer { options ->
            options.configureLocations()
            options.addLocationStore(SampleLocationStore.create(topology))
            val location = topology.location()
            options.addRelocationStore(
                ZLinkRedisRelocationStore(
                    ZLinkRedisRelocationOptions()
                        .setConnectionString(location.redisEndpoint)
                        .setKeyPrefix("${location.redisKeyPrefix}relocation:"),
                ),
            )
            options.useCoroutineHandlers(Dispatchers.Default)
            options.addHandlersFromPackageOf(Program::class.java)
            options.configureDispatch()
                .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
                .traceLogFile("${api.logDirectory}/flow-${api.instanceName}.log")
                .traceLabel(api.instanceName)
            options.addRouteMesh(SampleNames.PlayerQuestMesh)
                .setRoutingIdPrefix("gamequest-api")
                .listen()
                .objects().server()
                .addEntrySpot(GameQuestEntrySpot::class.java)
                .addActorFactory(
                    SampleNames.PlayerSessionActorType,
                    GameQuestPlayerActor::class.java,
                    GameQuestPlayerActorFactory::class.java,
                ) { factory -> factory.recreateOnRelocation() }
            options.addStreamNode(SampleNames.StreamNode)
                .bind(api.streamEndpoint)
                .enableActorDispatch()
                .registerSession(GameQuestSession::class.java)
        }
    }

    @Bean(destroyMethod = "close")
    fun gameQuestStore(topology: SampleTopology): GameQuestStore =
        GameQuestStore(topology).also { store = it }

    @Bean
    fun gameQuestApiServices(store: GameQuestStore, routes: ZLinkRouteClient): GameQuestApiServices {
        Companion.store = store
        Companion.routes = routes
        return GameQuestApiServices()
    }

    class GameQuestApiServices

    companion object {
        lateinit var store: GameQuestStore
        lateinit var routes: ZLinkRouteClient
        fun run(configPath: String): ConfigurableApplicationContext {
            val environment = StandardEnvironment().apply {
                propertySources.remove(StandardEnvironment.SYSTEM_ENVIRONMENT_PROPERTY_SOURCE_NAME)
                propertySources.remove(StandardEnvironment.SYSTEM_PROPERTIES_PROPERTY_SOURCE_NAME)
            }
            val context = SpringApplicationBuilder(Program::class.java)
                .environment(environment)
                .also { it.application().setKeepAlive(true) }
                .web(WebApplicationType.NONE)
                .properties("spring.config.location=${Path.of(configPath).toAbsolutePath().toUri()}")
                .run()
            return context
        }
    }
}

class GameQuestSession(
    private val context: ZLinkSessionContext,
    private val routes: ZLinkRouteClient,
    private val store: GameQuestStore,
    private val topology: SampleTopology,
    private val actors: ZLinkActorManager,
) : ZLinkSuspendingSession() {
    private var playerId: String? = null
    private var playerActor: ZLinkSessionActor? = null
    override fun context(): ZLinkSessionContext = context

    override suspend fun onDisconnectedSuspending() {
        playerActor?.notifyDisconnected()?.await()
        playerId?.let(store::unbind)
    }

    override suspend fun onDispatchSuspending(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage) {
        when (dispatch.packetName()) {
            "JoinSessionReq" -> handleJoin(payload.decode(JoinSessionReq::class.java))
            "GetQuestProgressReq" -> handleGetProgress(payload.decode(GetQuestProgressReq::class.java))
            "SyncQuestProgressReq" -> handleSync(payload.decode(SyncQuestProgressReq::class.java))
            "KillMonsterReq" -> handleKill(payload.decode(KillMonsterReq::class.java))
            "CollectItemReq" -> handleCollect(payload.decode(CollectItemReq::class.java))
            "CompleteMissionReq" -> handleMission(payload.decode(CompleteMissionReq::class.java))
            "EnterAreaReq" -> handleArea(payload.decode(EnterAreaReq::class.java))
            "UnlockFeatureReq" -> handleFeature(payload.decode(UnlockFeatureReq::class.java))
            else -> error("Unknown GameQuest packet: ${dispatch.packetName()}")
        }
    }

    private suspend fun handleJoin(request: JoinSessionReq) {
        playerId = request.playerId
        store.bind(request.playerId, topology.gameApi().instanceName)
        val actorRef = ensurePlayerActor(request)
        playerActor = context.actors().find(actorRef.actorId).orElse(null)
            ?: context.actors().bind(actorRef).await()
        val ownerProjection = routes
            .requestToSpot(request.playerId, GetQuestProgressReq(request.playerId))
            .timeout(SampleTimings.RequestTimeout)
            .instanceSpot(SampleNames.PlayerQuestSpotType)
            .inMesh(SampleNames.PlayerQuestMesh)
            .submit(GetQuestProgressRes::class.java)
            .await()
        store.mergeProjection(request.playerId, ownerProjection.activeQuests)
        context.client().reply(JoinSessionRes(ownerProjection.activeQuests)).submit()
    }

    private suspend fun handleGetProgress(request: GetQuestProgressReq) {
        val ownerProjection = routes
            .requestToSpot(request.playerId, request)
            .timeout(SampleTimings.RequestTimeout)
            .instanceSpot(SampleNames.PlayerQuestSpotType)
            .inMesh(SampleNames.PlayerQuestMesh)
            .submit(GetQuestProgressRes::class.java)
            .await()
        store.mergeProjection(request.playerId, ownerProjection.activeQuests)
        context.client().reply(ownerProjection).submit()
    }

    private suspend fun handleSync(request: SyncQuestProgressReq) {
        val response = routes
            .requestToSpot(request.playerId, request)
            .timeout(SampleTimings.RequestTimeout)
            .instanceSpot(SampleNames.PlayerQuestSpotType)
            .inMesh(SampleNames.PlayerQuestMesh)
            .submit(SyncQuestProgressRes::class.java)
            .await()
        store.mergeProjection(request.playerId, response.updatedQuests)
        context.client().reply(response).submit()
    }

    private suspend fun handleKill(request: KillMonsterReq) {
        val processed = process(event(request.playerId, request.idempotencyKey, "kill", request.monsterId, 1, true))
        context.client().reply(KillMonsterRes(processed.eventId)).submit()
    }

    private suspend fun handleCollect(request: CollectItemReq) {
        val processed = process(event(request.playerId, request.idempotencyKey, "collect", request.itemId, request.count, true))
        context.client().reply(CollectItemRes(processed.eventId)).submit()
    }

    private suspend fun handleMission(request: CompleteMissionReq) {
        val processed = process(event(request.playerId, request.idempotencyKey, "mission", request.missionId, 1, true))
        context.client().reply(CompleteMissionRes(processed.eventId)).submit()
    }

    private suspend fun handleArea(request: EnterAreaReq) {
        val processed = process(event(request.playerId, request.idempotencyKey, "area", request.areaId, 1, true))
        context.client().reply(EnterAreaRes(processed.eventId)).submit()
    }

    private suspend fun handleFeature(request: UnlockFeatureReq) {
        val processed = process(event(request.playerId, request.idempotencyKey, "feature", request.featureId, 1, true))
        context.client().reply(UnlockFeatureRes(processed.eventId)).submit()
    }

    private suspend fun process(event: GameplayMsg): GameplayMsg {
        store.recordGameplay(event)
        routes
            .sendToSpot(event.playerId, event)
            .instanceSpot(SampleNames.PlayerQuestSpotType)
            .inMesh(SampleNames.PlayerQuestMesh)
            .submit()
            .await()
        return event
    }

    private suspend fun ensurePlayerActor(request: JoinSessionReq): ActorRef =
        when (val result = actors.kotlin().getOrCreate(
            request.playerId,
            SampleNames.PlayerSessionActorType,
        ).request(request).await()) {
            is ZLinkActorCreateResult.Existing -> result.actor
            is ZLinkActorCreateResult.Created -> result.actor
            is ZLinkActorCreateResult.Rejected ->
                error("Player session Actor creation was rejected")
        }

    private fun event(playerId: String, idempotencyKey: String, eventType: String, value: String, count: Int, publish: Boolean) =
        GameplayMsg.create(
            "$playerId-$idempotencyKey",
            playerId,
            eventType,
            idempotencyKey,
            value,
            count,
            topology.gameApi().instanceName,
            Instant.now().toEpochMilli(),
            publish,
        )
}

class GameQuestPlayerActor(
    private val id: String,
    private val actorContext: ZLinkActorContext,
) : ZLinkActor {
    override fun context(): ZLinkActorContext = actorContext

    fun push(message: QuestProcessingRes) {
        message.progressNotifications.forEach { actorContext.boundSession().send(it).submit() }
        message.completedNotifications.forEach { actorContext.boundSession().send(it).submit() }
    }
}

class GameQuestPlayerActorFactory : ZLinkSuspendingActorFactory() {
    override suspend fun createActor(context: ZLinkActorContext): ZLinkActor =
        GameQuestPlayerActor(context.actorId(), context)
}

class GameQuestEntrySpot(
    override val context: ZLinkEntrySpotContext,
) : ZLinkSuspendingEntrySpot<GameQuestPlayerActor>() {
    override suspend fun onJoinedActorSuspending(actor: GameQuestPlayerActor) {
    }

    override suspend fun onLeaveActorSuspending(actor: GameQuestPlayerActor) {
    }
}

class QuestProcessingActorHandler(
    private val store: GameQuestStore,
) : ZLinkSuspendingEntrySpotActorSendHandler<
    GameQuestEntrySpot,
    GameQuestPlayerActor,
    QuestProcessingRes,
    > {
    override suspend fun handle(
        entrySpot: GameQuestEntrySpot,
        actor: GameQuestPlayerActor,
        context: ZLinkMessageContext,
        message: QuestProcessingRes,
    ) {
        store.mergeProjection(message.playerId, message.projection)
        actor.push(message)
    }
}

private fun startHttp(store: GameQuestStore, topology: SampleTopology): HttpServer {
    val json = jacksonObjectMapper()
    val uri = URI.create(topology.gameApi().httpEndpoint)
    val server = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
    server.createContext("/health") { exchange -> writeJson(exchange, 200, mapOf("status" to "ok")) }
    server.createContext("/internal/snapshot") { exchange ->
        val request = json.readValue<GetGameplaySnapshotReq>(exchange.requestBody)
        writeJson(exchange, 200, store.snapshot(request.playerId))
    }
    server.createContext("/quest/progress/") { exchange ->
        val playerId = exchange.requestURI.path.removePrefix("/quest/progress/")
        writeJson(exchange, 200, GetQuestProgressRes(store.projection(playerId)))
    }
    server.createContext("/self-check/gameplay/kill-without-publish/") { exchange ->
        val playerId = exchange.requestURI.path.removePrefix("/self-check/gameplay/kill-without-publish/")
        store.addUnpublishedKill(playerId)
        writeJson(exchange, 200, mapOf("accepted" to true))
    }
    server.createContext("/self-check/projection/") { exchange -> handleProjection(exchange, store, topology) }
    server.createContext("/self-check/assert") { exchange -> writeJson(exchange, 200, store.assertState()) }
    server.start()
    return server
}

private fun handleProjection(exchange: HttpExchange, store: GameQuestStore, topology: SampleTopology) {
    val parts = exchange.requestURI.path.split("/")
    val playerId = parts.getOrElse(3) { "" }
    val questId = parts.getOrElse(4) { "" }
    when (parts.getOrElse(5) { "" }) {
        "delete" -> {
            val deleted = kotlinx.coroutines.runBlocking {
                    Program.routes
                    .requestToSpot(
                        playerId,
                        DeleteQuestProjectionReq(playerId, questId),
                    )
                    .timeout(SampleTimings.RequestTimeout)
                    .instanceSpot(SampleNames.PlayerQuestSpotType)
                    .inMesh(SampleNames.PlayerQuestMesh)
                    .submit(DeleteQuestProjectionRes::class.java)
                    .await()
            }
            store.deleteProjection(playerId, questId)
            writeJson(exchange, 200, deleted)
        }
        "rebuild" -> {
            val rebuilt = kotlinx.coroutines.runBlocking {
                    Program.routes
                    .requestToSpot(
                        playerId,
                        RebuildQuestProjectionReq(playerId, questId, 0),
                    )
                    .timeout(SampleTimings.RequestTimeout)
                    .instanceSpot(SampleNames.PlayerQuestSpotType)
                    .inMesh(SampleNames.PlayerQuestMesh)
                    .submit(QuestProgress::class.java)
                    .await()
            }
            store.mergeProjection(playerId, listOf(rebuilt))
            writeJson(exchange, 200, rebuilt)
        }
        else -> writeJson(exchange, 404, mapOf("error" to "unknown projection action"))
    }
}

private fun writeJson(exchange: HttpExchange, status: Int, body: Any) {
    val bytes = jacksonObjectMapper().writeValueAsString(body).toByteArray(StandardCharsets.UTF_8)
    exchange.responseHeaders.add("content-type", "application/json")
    exchange.sendResponseHeaders(status, bytes.size.toLong())
    exchange.responseBody.use { it.write(bytes) }
}

class GameQuestStore(topology: SampleTopology) : AutoCloseable {
    private val shared = RedisSampleStore(topology)
    private val projections = mutableMapOf<String, MutableList<QuestProgress>>()
    private val completedMissions = mutableMapOf<String, MutableSet<String>>()
    private val unlockedFeatures = mutableMapOf<String, MutableSet<String>>()
    private val enteredAreas = mutableMapOf<String, MutableSet<String>>()
    private val kills = mutableMapOf<String, MutableMap<String, Int>>()
    private val items = mutableMapOf<String, MutableMap<String, Int>>()

    @Synchronized
    fun bind(playerId: String, apiName: String) = shared.bind(playerId, apiName)

    @Synchronized
    fun unbind(playerId: String) = shared.unbind(playerId)

    @Synchronized
    fun recordGameplay(event: GameplayMsg) {
        when (event.type) {
            "kill" -> kills.getOrPut(event.playerId) { mutableMapOf() }.merge(event.decodePayload().value, event.decodePayload().count, Int::plus)
            "collect" -> items.getOrPut(event.playerId) { mutableMapOf() }.merge(event.decodePayload().value, event.decodePayload().count, Int::plus)
            "mission" -> completedMissions.getOrPut(event.playerId) { mutableSetOf() } += event.decodePayload().value
            "feature" -> unlockedFeatures.getOrPut(event.playerId) { mutableSetOf() } += event.decodePayload().value
            "area" -> enteredAreas.getOrPut(event.playerId) { mutableSetOf() } += event.decodePayload().value
        }
    }

    @Synchronized
    fun mergeProjection(playerId: String, projection: List<QuestProgress>) {
        projections[playerId] = projection.toMutableList()
        shared.writeProjection(playerId, projection)
    }

    @Synchronized
    fun projection(playerId: String): List<QuestProgress> {
        val sharedProjection = shared.readProjection(playerId)
        if (sharedProjection.isNotEmpty()) {
            projections[playerId] = sharedProjection.toMutableList()
            return sharedProjection
        }
        return projections[playerId]?.toList() ?: emptyList()
    }

    @Synchronized
    fun deleteProjection(playerId: String, questId: String) {
        val projection = projections.getOrPut(playerId) { mutableListOf() }
        projection.removeIf { it.questId == questId }
        shared.writeProjection(playerId, projection)
    }

    @Synchronized
    fun addUnpublishedKill(playerId: String) {
        kills.getOrPut(playerId) { mutableMapOf() }.merge("wolf", 1, Int::plus)
    }

    @Synchronized
    fun snapshot(playerId: String): GetGameplaySnapshotRes {
        val killCounts = kills[playerId].orEmpty().map { KillCountSnapshot(it.key, null, it.value) }
        val itemCounts = items[playerId].orEmpty().map { ItemCountSnapshot(it.key, it.value) }
        return GetGameplaySnapshotRes(
            playerId,
            killCounts,
            itemCounts,
            completedMissions[playerId].orEmpty().toList(),
            unlockedFeatures[playerId].orEmpty().toList(),
            enteredAreas[playerId].orEmpty().toList(),
            (killCounts.size + itemCounts.size).toLong(),
        )
    }

    @Synchronized
    fun assertState(): GameQuestServerAssertRes {
        val alice = projection("player-alice")
        val bob = projection("player-bob")
        val evidence = mutableListOf<String>()
        alice.forEach { evidence += "${it.playerId}:${it.questId}:${it.status}:${it.currentCount}/${it.requiredCount}" }
        bob.forEach { evidence += "${it.playerId}:${it.questId}:${it.status}:${it.currentCount}/${it.requiredCount}" }
        val bindingHistory = shared.bindingHistory()
        val activeBindings = shared.activeBindings()
        val events = shared.readQuestEvents()
        val rehydrates = shared.rehydrates()
        bindingHistory.forEach { evidence += "binding:$it" }
        events.forEach { evidence += "event:${it.playerId}:${it.questId}:${it.eventType}:v${it.version}:source=${it.sourceEventId}" }
        rehydrates.forEach { (playerId, count) -> evidence += "rehydrated:$playerId:$count" }

        val checks = listOf(
            check(evidence, "missing:player-alice:first-hunt:RewardGranted") {
                alice.any { it.questId == QuestIds.FirstHunt && it.status == QuestStatuses.RewardGranted }
            },
            check(evidence, "missing:player-alice:open-auction:RewardGranted") {
                alice.any { it.questId == QuestIds.OpenAuction && it.status == QuestStatuses.RewardGranted }
            },
            check(evidence, "missing:player-bob:herb-gathering:RewardGranted") {
                bob.any { it.questId == QuestIds.HerbGathering && it.status == QuestStatuses.RewardGranted }
            },
            check(evidence, "missing:binding:player-bob:api-b") { bindingHistory.contains("player-bob:api-b") },
            check(evidence, "unexpected:active-binding:player-alice") { !activeBindings.contains("player-alice") },
            check(evidence, "missing:event:player-alice:first-hunt:QuestProgressedEvent:3") {
                count(events, "player-alice", QuestIds.FirstHunt, QuestProgressedEvent::class.java.simpleName) == 3L
            },
            check(evidence, "missing:event:player-alice:first-hunt:QuestCompletedEvent:1") {
                count(events, "player-alice", QuestIds.FirstHunt, QuestCompletedEvent::class.java.simpleName) == 1L
            },
            check(evidence, "missing:event:player-alice:first-hunt:QuestRewardGrantedEvent:1") {
                count(events, "player-alice", QuestIds.FirstHunt, QuestRewardGrantedEvent::class.java.simpleName) == 1L
            },
            check(evidence, "missing:event:player-alice:first-hunt:QuestProgressReconciledEvent:1") {
                count(events, "player-alice", QuestIds.FirstHunt, QuestProgressReconciledEvent::class.java.simpleName) == 1L
            },
            check(evidence, "missing:event:player-alice:open-auction:QuestCompletedEvent:1") {
                count(events, "player-alice", QuestIds.OpenAuction, QuestCompletedEvent::class.java.simpleName) == 1L
            },
            check(evidence, "missing:event:player-alice:open-auction:QuestRewardGrantedEvent:1") {
                count(events, "player-alice", QuestIds.OpenAuction, QuestRewardGrantedEvent::class.java.simpleName) == 1L
            },
            check(evidence, "missing:event:player-bob:herb-gathering:QuestCompletedEvent:1") {
                count(events, "player-bob", QuestIds.HerbGathering, QuestCompletedEvent::class.java.simpleName) == 1L
            },
            check(evidence, "missing:event:player-bob:herb-gathering:QuestRewardGrantedEvent:1") {
                count(events, "player-bob", QuestIds.HerbGathering, QuestRewardGrantedEvent::class.java.simpleName) == 1L
            },
            check(evidence, "missing:rehydrated:player-alice:2") {
                rehydrates.getOrDefault("player-alice", "0").toInt() >= 2
            },
            check(evidence, "missing:rehydrated:player-bob:1") {
                rehydrates.getOrDefault("player-bob", "0").toInt() >= 1
            },
            check(evidence, "duplicate:event-version") { uniqueEventVersions(events) },
        )
        var passed = true
        checks.forEach { passed = it() && passed }
        return GameQuestServerAssertRes(passed, evidence.sorted())
    }

    override fun close() {
        shared.close()
    }

    private fun check(evidence: MutableList<String>, failure: String, condition: () -> Boolean): () -> Boolean =
        {
            condition().also { passed ->
                if (!passed) {
                    evidence += "failure:$failure"
                }
            }
        }

    private fun count(events: List<StoredQuestEvent>, playerId: String, questId: String, eventType: String): Long =
        events.count { it.playerId == playerId && it.questId == questId && it.eventType == eventType }.toLong()

    private fun uniqueEventVersions(events: List<StoredQuestEvent>): Boolean =
        events.map { "${it.playerId}:${it.questId}:${it.version}" }.distinct().size == events.size
}
