package systems.zlink.samples.kotlin.gamequest.server.questmission

import com.fasterxml.jackson.module.kotlin.jacksonObjectMapper
import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.net.InetSocketAddress
import java.net.URI
import java.nio.charset.StandardCharsets
import java.nio.file.Path
import java.time.Instant
import java.util.concurrent.CompletableFuture
import java.util.concurrent.CompletionStage
import kotlinx.coroutines.Dispatchers
import org.springframework.boot.WebApplicationType
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.builder.SpringApplicationBuilder
import org.springframework.boot.context.properties.EnableConfigurationProperties
import org.springframework.context.ConfigurableApplicationContext
import org.springframework.context.annotation.Bean
import org.springframework.core.env.StandardEnvironment
import systems.zlink.framework.channels.ZLinkRouteClient
import systems.zlink.framework.actors.ZLinkActorClient
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
import systems.zlink.framework.spots.ZLinkInstanceSpot
import systems.zlink.framework.spots.ZLinkInstanceSpotContext
import systems.zlink.framework.spots.ZLinkSpotPacketHandler
import systems.zlink.framework.spots.ZLinkSpotRequestHandler
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.samples.kotlin.gamequest.server.configuration.RedisSampleStore
import systems.zlink.samples.kotlin.gamequest.server.configuration.SampleLocationStore
import systems.zlink.samples.kotlin.gamequest.server.configuration.SampleNames
import systems.zlink.samples.kotlin.gamequest.server.configuration.SampleTopology
import systems.zlink.samples.kotlin.gamequest.shared.contracts.DeleteQuestProjectionReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.DeleteQuestProjectionRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GameplayMsg
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GetQuestProgressReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GetQuestProgressRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestCompletedEvent
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestCompletedNotify
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestIds
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProcessingRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProgress
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProgressNotify
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProgressReconciledEvent
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProgressedEvent
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestRewardGrantedEvent
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestStatuses
import systems.zlink.samples.kotlin.gamequest.shared.contracts.RebuildQuestProjectionReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.StoredQuestEvent
import systems.zlink.samples.kotlin.gamequest.shared.contracts.SyncQuestProgressReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.SyncQuestProgressRes

fun main(args: Array<String>) {
    val app = Program.run(SampleTopology.configPath(args))
    val store = Program.store
    val http = startHttp(
        store,
        app.getBean(ZLinkRouteClient::class.java),
        app.getBean(SampleTopology::class.java),
    )
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
    fun questMissionFramework(topology: SampleTopology): ZLinkFrameworkConfigurer {
        val mission = topology.questMission()
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
                .traceLogFile("${mission.logDirectory}/flow-${mission.instanceName}.log")
                .traceLabel(mission.instanceName)
            options.addRouteMesh(SampleNames.PlayerQuestMesh)
                .setRoutingIdPrefix("gamequest-mission-owner")
                .listen(mission.channelEndpoint)
                .objects().server()
                .addInstanceSpotFactory(
                    SampleNames.PlayerQuestSpotType,
                    PlayerQuestSpot::class.java,
                ) { factory -> factory.recreateOnRelocation() }
        }
    }

    @Bean(destroyMethod = "close")
    fun questStore(topology: SampleTopology): QuestStore =
        QuestStore(topology).also { store = it }

    companion object {
        lateinit var store: QuestStore
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

private fun startHttp(
    store: QuestStore,
    routes: ZLinkRouteClient,
    topology: SampleTopology,
): HttpServer {
    val json = jacksonObjectMapper()
    val uri = URI.create(topology.questMission().httpEndpoint)
    val server = HttpServer.create(InetSocketAddress(uri.host, uri.port), 0)
    server.createContext("/health") { exchange -> writeJson(exchange, 200, mapOf("status" to "ok")) }
    server.createContext("/self-check/owner/") { exchange ->
        val parts = exchange.requestURI.path.split("/")
        val playerId = parts.getOrElse(3) { "" }
        routes.sendToSpot(playerId, ClosePlayerQuestMsg()).submit().toCompletableFuture().join()
        writeJson(exchange, 202, mapOf("closed" to true, "owner" to true))
    }
    server.createContext("/self-check/events") { exchange -> writeJson(exchange, 200, store.events()) }
    server.start()
    return server
}

private fun writeJson(exchange: HttpExchange, status: Int, body: Any) {
    val bytes = jacksonObjectMapper().writeValueAsString(body).toByteArray(StandardCharsets.UTF_8)
    exchange.responseHeaders.add("content-type", "application/json")
    exchange.sendResponseHeaders(status, bytes.size.toLong())
    exchange.responseBody.use { it.write(bytes) }
}

data class ClosePlayerQuestMsg(val close: Boolean = true)

class PlayerQuestSpot(
    private val instanceContext: ZLinkInstanceSpotContext,
    private val store: QuestStore,
) : ZLinkInstanceSpot {
    override fun context(): ZLinkInstanceSpotContext = instanceContext

    override fun onInitialize(): CompletionStage<Void> {
        store.markRehydrated(instanceContext.spotId())
        return CompletableFuture.completedFuture(null)
    }

    fun requirePlayer(playerId: String) {
        require(playerId == instanceContext.spotId()) {
            "request player does not match owner Spot: $playerId"
        }
    }

    fun apply(request: GameplayMsg): QuestProcessingRes {
        requirePlayer(request.playerId)
        return store.apply(request)
    }
}

class GameplayMsgRouteHandler(
    private val actors: ZLinkActorClient,
) : ZLinkSpotPacketHandler<PlayerQuestSpot, GameplayMsg> {
    override fun handle(
        spot: PlayerQuestSpot,
        request: GameplayMsg,
    ): CompletionStage<Void> {
        actors.sendToActor(request.playerId, spot.apply(request)).submit()
        return CompletableFuture.completedFuture(null)
    }
}

class GetQuestProgressHandler(
    private val store: QuestStore,
) : ZLinkSpotRequestHandler<PlayerQuestSpot, GetQuestProgressReq, GetQuestProgressRes> {
    override fun handle(
        spot: PlayerQuestSpot,
        request: GetQuestProgressReq,
    ): CompletionStage<GetQuestProgressRes> {
        spot.requirePlayer(request.playerId)
        return CompletableFuture.completedFuture(GetQuestProgressRes(store.projection(request.playerId)))
    }
}

class DeleteQuestProjectionHandler(
    private val store: QuestStore,
) : ZLinkSpotRequestHandler<PlayerQuestSpot, DeleteQuestProjectionReq, DeleteQuestProjectionRes> {
    override fun handle(
        spot: PlayerQuestSpot,
        request: DeleteQuestProjectionReq,
    ): CompletionStage<DeleteQuestProjectionRes> {
        spot.requirePlayer(request.playerId)
        return CompletableFuture.completedFuture(
            store.deleteProjection(request.playerId, request.questId),
        )
    }
}

class RebuildQuestProjectionHandler(
    private val store: QuestStore,
) : ZLinkSpotRequestHandler<PlayerQuestSpot, RebuildQuestProjectionReq, QuestProgress> {
    override fun handle(
        spot: PlayerQuestSpot,
        request: RebuildQuestProjectionReq,
    ): CompletionStage<QuestProgress> {
        spot.requirePlayer(request.playerId)
        return CompletableFuture.completedFuture(
            store.rebuildProjection(request.playerId, request.questId),
        )
    }
}

class SyncQuestProgressHandler(
    private val store: QuestStore,
) : ZLinkSpotRequestHandler<PlayerQuestSpot, SyncQuestProgressReq, SyncQuestProgressRes> {
    override fun handle(
        spot: PlayerQuestSpot,
        request: SyncQuestProgressReq,
    ): CompletionStage<SyncQuestProgressRes> {
        spot.requirePlayer(request.playerId)
        return CompletableFuture.completedFuture(store.sync(request.playerId, 4))
    }
}

class ClosePlayerQuestSpotHandler : ZLinkSpotPacketHandler<PlayerQuestSpot, ClosePlayerQuestMsg> {
    override fun handle(
        spot: PlayerQuestSpot,
        request: ClosePlayerQuestMsg,
    ): CompletionStage<Void> = spot.context().close().thenApply { null }
}

class QuestStore(topology: SampleTopology) : AutoCloseable {
    private val domain = QuestDomain()
    private val shared = RedisSampleStore(topology)
    private val projections = mutableMapOf<String, MutableList<QuestProgress>>()
    private val eventIdsByIdempotency = mutableMapOf<String, String>()
    private val events = mutableListOf<StoredQuestEvent>()
    private val rehydrates = mutableMapOf<String, Int>()
    private val loadedPlayers = mutableSetOf<String>()

    @Synchronized
    fun apply(event: GameplayMsg): QuestProcessingRes {
        restorePlayer(event.playerId)
        val key = "${event.playerId}:${event.decodePayload().idempotencyKey}"
        eventIdsByIdempotency[key]?.let {
            return QuestProcessingRes(
                it,
                event.playerId,
                copyProjection(event.playerId),
                emptyList(),
                emptyList(),
                true,
            )
        }
        eventIdsByIdempotency[key] = event.eventId
        val decision = domain.apply(event, copyProjection(event.playerId), nextVersion(event.playerId))
        projections[event.playerId] = decision.projection.toMutableList()
        events += decision.storedEvents
        shared.writeProjection(event.playerId, decision.projection)
        shared.appendQuestEvents(decision.storedEvents)
        return QuestProcessingRes(
            event.eventId,
            event.playerId,
            copyProjection(event.playerId),
            decision.progressNotifications,
            decision.completedNotifications,
            false,
        )
    }

    @Synchronized
    fun sync(playerId: String, firstHuntCount: Int): SyncQuestProgressRes {
        restorePlayer(playerId)
        val projection = copyProjection(playerId).toMutableList()
        val firstHunt = projection.firstOrNull { it.questId == QuestIds.FirstHunt }
        if (firstHunt == null || firstHunt.currentCount < firstHuntCount) {
            val now = Instant.now().toEpochMilli()
            val reconciled = QuestProgress(
                playerId,
                QuestIds.FirstHunt,
                if (firstHuntCount >= 3) QuestStatuses.RewardGranted else QuestStatuses.InProgress,
                firstHuntCount,
                3,
                "sync-$now",
                now,
            )
            projection.removeIf { it.questId == QuestIds.FirstHunt }
            projection += reconciled
            projections[playerId] = projection
            val reconciledEvent = StoredQuestEvent(
                "sync-$now",
                null,
                playerId,
                QuestIds.FirstHunt,
                QuestProgressReconciledEvent::class.java.simpleName,
                reconciled.currentCount,
                reconciled.requiredCount,
                reconciled.status,
                nextVersion(playerId),
                now,
            )
            events += reconciledEvent
            shared.writeProjection(playerId, projection)
            shared.appendQuestEvents(listOf(reconciledEvent))
        }
        return SyncQuestProgressRes(copyProjection(playerId))
    }

    @Synchronized
    fun deleteProjection(playerId: String, questId: String): DeleteQuestProjectionRes {
        restorePlayer(playerId)
        val projection = projections.getOrPut(playerId) { mutableListOf() }
        projection.removeIf { it.questId == questId }
        shared.writeProjection(playerId, projection)
        return DeleteQuestProjectionRes(true)
    }

    @Synchronized
    fun rebuildProjection(playerId: String, questId: String): QuestProgress {
        restorePlayer(playerId)
        val stream = events.filter { it.playerId == playerId && it.questId == questId }.sortedBy { it.version }
        require(stream.isNotEmpty()) { "Quest stream was not found for $playerId/$questId" }
        val last = stream.last()
        val rebuilt = QuestProgress(
            playerId,
            questId,
            last.status,
            last.currentCount,
            last.requiredCount,
            last.sourceEventId,
            stream.maxOf { it.createdAtUnixMs },
        )
        val projection = projections.getOrPut(playerId) { mutableListOf() }
        projection.removeIf { it.questId == questId }
        projection += rebuilt
        shared.writeProjection(playerId, projection)
        return rebuilt
    }

    @Synchronized
    fun markRehydrated(playerId: String) {
        restorePlayer(playerId)
        rehydrates.merge(playerId, 1, Int::plus)
        shared.recordRehydrated(playerId)
    }

    @Synchronized
    fun projection(playerId: String): List<QuestProgress> {
        restorePlayer(playerId)
        return copyProjection(playerId)
    }

    @Synchronized
    fun events(): List<StoredQuestEvent> = events.toList()

    override fun close() {
        shared.close()
    }

    private fun nextVersion(playerId: String): Long =
        events.count { it.playerId == playerId }.toLong() + 1

    private fun copyProjection(playerId: String): List<QuestProgress> =
        projections[playerId]?.toList() ?: emptyList()

    private fun restorePlayer(playerId: String) {
        if (!loadedPlayers.add(playerId)) {
            return
        }
        val stored = shared.readQuestEvents().filter { it.playerId == playerId }
        events += stored
        stored.forEach { event ->
            val sourceEventId = event.sourceEventId ?: return@forEach
            val prefix = "$playerId-"
            if (sourceEventId.startsWith(prefix)) {
                val idempotencyKey = sourceEventId.removePrefix(prefix)
                eventIdsByIdempotency["$playerId:$idempotencyKey"] = sourceEventId
            }
        }
        projections[playerId] = stored
            .groupBy { it.questId }
            .map { (questId, stream) -> fold(playerId, questId, stream) }
            .toMutableList()
    }

    private fun fold(
        playerId: String,
        questId: String,
        stream: List<StoredQuestEvent>,
    ): QuestProgress {
        var current = 0
        var required = 1
        var status = QuestStatuses.InProgress
        var lastEventId: String? = null
        var updatedAt = 0L
        stream.sortedBy { it.version }.forEach { event ->
            required = event.requiredCount
            when (event.eventType) {
                QuestProgressedEvent::class.java.simpleName -> current = event.currentCount
                QuestProgressReconciledEvent::class.java.simpleName -> {
                    current = event.currentCount
                    status = event.status
                }
                QuestCompletedEvent::class.java.simpleName -> current = maxOf(current, required)
                QuestRewardGrantedEvent::class.java.simpleName -> status = QuestStatuses.RewardGranted
            }
            lastEventId = event.sourceEventId
            updatedAt = maxOf(updatedAt, event.createdAtUnixMs)
        }
        return QuestProgress(
            playerId,
            questId,
            status,
            current,
            required,
            lastEventId,
            updatedAt,
        )
    }
}

private class QuestDomain {
    fun apply(event: GameplayMsg, current: List<QuestProgress>, nextVersion: Long): QuestDecision {
        val now = Instant.now().toEpochMilli()
        val stored = mutableListOf<StoredQuestEvent>()
        val progress = mutableListOf<QuestProgressNotify>()
        val completed = mutableListOf<QuestCompletedNotify>()
        val updated = current.toMutableList()
        when {
            event.type == "kill" && event.decodePayload().value == "wolf" ->
                applyCounter(event, updated, stored, progress, completed, QuestIds.FirstHunt, 3, event.decodePayload().count, now, nextVersion)
            event.type == "collect" && event.decodePayload().value == "healing-herb" ->
                applyCounter(event, updated, stored, progress, completed, QuestIds.HerbGathering, 5, event.decodePayload().count, now, nextVersion)
            event.type == "feature" && event.decodePayload().value == "auction" ->
                completeOnce(event, updated, stored, completed, QuestIds.OpenAuction, 1, now, nextVersion)
        }
        return QuestDecision(updated, stored, progress, completed)
    }

    private fun applyCounter(
        event: GameplayMsg,
        projection: MutableList<QuestProgress>,
        stored: MutableList<StoredQuestEvent>,
        progressNotifications: MutableList<QuestProgressNotify>,
        completedNotifications: MutableList<QuestCompletedNotify>,
        questId: String,
        required: Int,
        delta: Int,
        now: Long,
        nextVersion: Long,
    ) {
        val previous = projection.firstOrNull { it.questId == questId }
        val current = minOf(required, (previous?.currentCount ?: 0) + delta)
        val status = if (current >= required) QuestStatuses.RewardGranted else QuestStatuses.InProgress
        val next = QuestProgress(event.playerId, questId, status, current, required, event.eventId, now)
        projection.removeIf { it.questId == questId }
        projection += next
        if (previous == null || previous.currentCount != current) {
            stored += StoredQuestEvent(
                "${event.eventId}:progress",
                event.eventId,
                event.playerId,
                questId,
                QuestProgressedEvent::class.java.simpleName,
                next.currentCount,
                next.requiredCount,
                next.status,
                nextVersion,
                now,
            )
            if (event.decodePayload().publish) {
                progressNotifications += QuestProgressNotify(event.playerId, null, next)
            }
        }
        if ((previous == null || previous.status != QuestStatuses.RewardGranted) && status == QuestStatuses.RewardGranted) {
            stored += StoredQuestEvent(
                "${event.eventId}:completed",
                event.eventId,
                event.playerId,
                questId,
                QuestCompletedEvent::class.java.simpleName,
                next.currentCount,
                next.requiredCount,
                next.status,
                nextVersion + 1,
                now,
            )
            stored += StoredQuestEvent(
                "${event.eventId}:reward",
                event.eventId,
                event.playerId,
                questId,
                QuestRewardGrantedEvent::class.java.simpleName,
                next.currentCount,
                next.requiredCount,
                next.status,
                nextVersion + 2,
                now,
            )
            if (event.decodePayload().publish) {
                completedNotifications += QuestCompletedNotify(event.playerId, null, next, true)
            }
        }
    }

    private fun completeOnce(
        event: GameplayMsg,
        projection: MutableList<QuestProgress>,
        stored: MutableList<StoredQuestEvent>,
        completedNotifications: MutableList<QuestCompletedNotify>,
        questId: String,
        required: Int,
        now: Long,
        nextVersion: Long,
    ) {
        val previous = projection.firstOrNull { it.questId == questId }
        if (previous?.status == QuestStatuses.RewardGranted) {
            return
        }
        val next = QuestProgress(event.playerId, questId, QuestStatuses.RewardGranted, required, required, event.eventId, now)
        projection.removeIf { it.questId == questId }
        projection += next
        stored += StoredQuestEvent(
            "${event.eventId}:completed",
            event.eventId,
            event.playerId,
            questId,
            QuestCompletedEvent::class.java.simpleName,
            next.currentCount,
            next.requiredCount,
            next.status,
            nextVersion,
            now,
        )
        stored += StoredQuestEvent(
            "${event.eventId}:reward",
            event.eventId,
            event.playerId,
            questId,
            QuestRewardGrantedEvent::class.java.simpleName,
            next.currentCount,
            next.requiredCount,
            next.status,
            nextVersion + 1,
            now,
        )
        if (event.decodePayload().publish) {
            completedNotifications += QuestCompletedNotify(event.playerId, null, next, true)
        }
    }
}

private data class QuestDecision(
    val projection: List<QuestProgress>,
    val storedEvents: List<StoredQuestEvent>,
    val progressNotifications: List<QuestProgressNotify>,
    val completedNotifications: List<QuestCompletedNotify>,
)
