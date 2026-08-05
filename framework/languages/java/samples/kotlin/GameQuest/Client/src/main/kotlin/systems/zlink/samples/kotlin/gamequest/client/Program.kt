package systems.zlink.samples.kotlin.gamequest.client

import java.net.URI
import java.nio.file.Files
import java.nio.file.Path
import java.time.Duration
import java.time.Instant
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import systems.zlink.httpclient.ZLinkHttpClient
import systems.zlink.httpclient.kotlin.awaitRaw
import systems.zlink.httpclient.kotlin.fetch
import systems.zlink.framework.kotlin.ZLinkKotlinStreamConnector
import systems.zlink.framework.kotlin.await
import systems.zlink.framework.kotlin.awaitReply
import systems.zlink.framework.kotlin.kotlin
import systems.zlink.samples.kotlin.gamequest.server.configuration.SampleNames
import systems.zlink.samples.kotlin.gamequest.server.configuration.SampleTimings
import systems.zlink.samples.kotlin.gamequest.shared.contracts.CollectItemReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.CollectItemRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.CompleteMissionReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.CompleteMissionRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.EnterAreaReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.EnterAreaRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GameQuestServerAssertRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GetGameplaySnapshotReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GetGameplaySnapshotRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GetQuestProgressReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.GetQuestProgressRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.JoinSessionReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.JoinSessionRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.KillMonsterReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.KillMonsterRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestCompletedNotify
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestIds
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProgress
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestProgressNotify
import systems.zlink.samples.kotlin.gamequest.shared.contracts.QuestStatuses
import systems.zlink.samples.kotlin.gamequest.shared.contracts.SyncQuestProgressReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.SyncQuestProgressRes
import systems.zlink.samples.kotlin.gamequest.shared.contracts.UnlockFeatureReq
import systems.zlink.samples.kotlin.gamequest.shared.contracts.UnlockFeatureRes
import systems.zlink.stream.connector.ZLinkStreamCompression
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions
import systems.zlink.stream.connector.ZLinkStreamDispatchMode

suspend fun main(args: Array<String>) {
    val options = GameQuestClientOptions.load(args)
    val apiA = createClient(options.apiAStreamEndpoint)
    val apiB = createClient(options.apiBStreamEndpoint)
    try {
        GameQuestClientScenario(options).run(apiA, apiB)
    } finally {
        apiA.close().await()
        apiB.close().await()
    }
    println(SampleNames.CompletedMarker)
}

private fun createClient(endpoint: String): ZLinkKotlinStreamConnector =
    ZLinkStreamConnectorFactory.create(
        ZLinkStreamConnectorOptions(
            URI.create(endpoint),
            ZLinkStreamDispatchMode.IMMEDIATE,
            SampleTimings.RequestTimeout,
            SampleTimings.RequestTimeout,
            2,
            SampleTimings.ConnectTimeout,
            64 * 1024,
            64 * 1024,
            Int.MAX_VALUE,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            false,
            ZLinkStreamCompression.LZ4,
            null,
            null,
            null,
        ),
    ).kotlin()

class GameQuestClientScenario(private val options: GameQuestClientOptions) {
    suspend fun run(apiAStream: ZLinkKotlinStreamConnector, apiBStream: ZLinkKotlinStreamConnector) = coroutineScope {
        apiAStream.connect().await()
        val joined = apiAStream.request(JoinSessionReq("player-alice")).awaitReply<JoinSessionRes>()
        ensure(joined.activeQuests.isEmpty())

        val firstProgress = async { apiAStream.waitFor<QuestProgressNotify>().await() }
        val firstKill = apiAStream.request(KillMonsterReq("player-alice", "wolf", "forest", "kill-1")).awaitReply<KillMonsterRes>()
        ensure(firstKill.eventId == "player-alice-kill-1")
        val firstPush = firstProgress.await().payload()
        ensure(firstPush.playerId == "player-alice")
        ensure(firstPush.progress.questId == QuestIds.FirstHunt)
        ensure(firstPush.progress.currentCount == 1)

        val firstHuntCompleted = apiAStream.waitFor<QuestCompletedNotify>()
            .where { it.payload().progress.questId == QuestIds.FirstHunt }
            .let { wait -> async { wait.await() } }
        apiAStream.request(KillMonsterReq("player-alice", "wolf", "forest", "kill-2")).awaitReply<KillMonsterRes>()
        val thirdKill = apiAStream.request(KillMonsterReq("player-alice", "wolf", "forest", "kill-3")).awaitReply<KillMonsterRes>()
        ensure(thirdKill.eventId == "player-alice-kill-3")
        val firstHuntPush = firstHuntCompleted.await().payload()
        ensure(firstHuntPush.rewardGranted)
        ensure(firstHuntPush.progress.status == QuestStatuses.RewardGranted)

        val duplicate = apiAStream.request(KillMonsterReq("player-alice", "wolf", "forest", "kill-3")).awaitReply<KillMonsterRes>()
        ensure(duplicate.eventId == thirdKill.eventId)

        val auctionCompleted = apiAStream.waitFor<QuestCompletedNotify>()
            .where { it.payload().progress.questId == QuestIds.OpenAuction }
            .let { wait -> async { wait.await() } }
        val auction = apiAStream.request(UnlockFeatureReq("player-alice", "auction", "unlock-auction")).awaitReply<UnlockFeatureRes>()
        ensure(auction.eventId == "player-alice-unlock-auction")
        ensure(auctionCompleted.await().payload().rewardGranted)
        val snapshot = post<GetGameplaySnapshotRes>(
            options.apiAHttpEndpoint,
            "/internal/snapshot",
            GetGameplaySnapshotReq("player-alice"),
        )
        ensure(snapshot.unlockedFeatureIds.contains("auction"))

        ensure(postRaw(options.missionAHttpEndpoint, "/self-check/owner/player-alice/close"))

        val tutorial = apiAStream.request(CompleteMissionReq("player-alice", "tutorial", "mission-tutorial"))
            .awaitReply<CompleteMissionRes>()
        ensure(tutorial.eventId == "player-alice-mission-tutorial")
        val ruins = apiAStream.request(EnterAreaReq("player-alice", "ruins", "enter-ruins")).awaitReply<EnterAreaRes>()
        ensure(ruins.eventId == "player-alice-enter-ruins")

        val offlineItem = apiAStream.request(CollectItemReq("player-bob", "healing-herb", 1, "herb-1"))
            .awaitReply<CollectItemRes>()
        ensure(offlineItem.eventId == "player-bob-herb-1")

        apiBStream.connect().await()
        val bobJoined = apiBStream.request(JoinSessionReq("player-bob")).awaitReply<JoinSessionRes>()
        ensure(hasProgress(bobJoined.activeQuests, QuestIds.HerbGathering, 1))

        val herbCompleted = apiBStream.waitFor<QuestCompletedNotify>()
            .where { it.payload().progress.questId == QuestIds.HerbGathering }
            .let { wait -> async { wait.await() } }
        val onlineItem = apiBStream.request(CollectItemReq("player-bob", "healing-herb", 4, "herb-2"))
            .awaitReply<CollectItemRes>()
        ensure(onlineItem.eventId == "player-bob-herb-2")
        val herbPush = herbCompleted.await().payload()
        ensure(herbPush.playerId == "player-bob")
        ensure(herbPush.rewardGranted)
        ensure(herbPush.progress.status == QuestStatuses.RewardGranted)

        ensure(postRaw(options.apiAHttpEndpoint, "/self-check/projection/player-bob/${QuestIds.HerbGathering}/delete"))
        val missingProjection = apiBStream.request(GetQuestProgressReq("player-bob")).awaitReply<GetQuestProgressRes>()
        ensure(missingProjection.activeQuests.none { it.questId == QuestIds.HerbGathering })
        val rebuilt = post<QuestProgress>(
            options.apiAHttpEndpoint,
            "/self-check/projection/player-bob/${QuestIds.HerbGathering}/rebuild",
            "",
        )
        ensure(rebuilt.questId == QuestIds.HerbGathering)
        ensure(rebuilt.status == QuestStatuses.RewardGranted)
        val rebuiltProjection = apiBStream.request(GetQuestProgressReq("player-bob")).awaitReply<GetQuestProgressRes>()
        ensure(rebuiltProjection.activeQuests.any {
            it.questId == QuestIds.HerbGathering && it.status == QuestStatuses.RewardGranted
        })

        ensure(postRaw(options.apiBHttpEndpoint, "/self-check/gameplay/kill-without-publish/player-alice"))
        val sync = apiAStream.request(SyncQuestProgressReq("player-alice")).awaitReply<SyncQuestProgressRes>()
        ensure(sync.updatedQuests.any { it.questId == QuestIds.FirstHunt && it.currentCount >= 4 })
        val reconciled = apiBStream.request(GetQuestProgressReq("player-alice")).awaitReply<GetQuestProgressRes>()
        ensure(reconciled.activeQuests.any { it.questId == QuestIds.FirstHunt && it.currentCount >= 4 })

        apiAStream.close().await()
        val assertion = waitForServerAssertion()
        check(assertion.passed) {
            "GameQuest server assertion failed: ${assertion.evidence.filter { it.startsWith("failure:") }}"
        }
        println(SampleNames.ServerEvidenceMarker)
    }

    private suspend fun waitForServerAssertion(): GameQuestServerAssertRes {
        val deadline = Instant.now().plus(Duration.ofSeconds(10))
        var last: GameQuestServerAssertRes? = null
        while (Instant.now().isBefore(deadline)) {
            val current: GameQuestServerAssertRes = post(options.apiAHttpEndpoint, "/self-check/assert", "")
            last = current
            if (current.passed) {
                return current
            }
        }
        return last ?: error("Server assertion did not return a response")
    }

    private fun hasProgress(progress: List<QuestProgress>, questId: String, currentCount: Int): Boolean =
        progress.any { it.questId == questId && it.currentCount == currentCount }

    private suspend fun postRaw(base: String, path: String): Boolean {
        val response = ZLinkHttpClient.create(base)
            .post(path)
            .awaitRaw()
        return response.status() in 200..299
    }

    private suspend inline fun <reified T> post(base: String, path: String, body: Any): T {
        val request = ZLinkHttpClient.create(base).post(path)
        if (body !is String || body.isNotEmpty()) {
            request.body(body)
        }
        return request.fetch()
    }
}

data class GameQuestClientOptions(
    val apiAStreamEndpoint: String,
    val apiBStreamEndpoint: String,
    val apiAHttpEndpoint: String,
    val apiBHttpEndpoint: String,
    val missionAHttpEndpoint: String,
    val missionBHttpEndpoint: String,
) {
    companion object {
        fun load(args: Array<String>): GameQuestClientOptions {
            require(args.size == 2 && args[0] == "--config" && args[1].isNotBlank()) {
                "Usage: Client --config <path>"
            }
            val properties = java.util.Properties().apply {
                Files.newBufferedReader(Path.of(args[1])).use(::load)
            }
            fun required(name: String): String =
                requireNotNull(properties.getProperty(name)?.takeIf(String::isNotBlank)) { "$name is required" }
            return GameQuestClientOptions(
                required("sample.apiAStreamEndpoint"),
                required("sample.apiBStreamEndpoint"),
                required("sample.apiAHttpEndpoint"),
                required("sample.apiBHttpEndpoint"),
                required("sample.missionAHttpEndpoint"),
                required("sample.missionBHttpEndpoint"),
            )
        }
    }
}

private fun ensure(condition: Boolean) {
    if (!condition) {
        throw IllegalStateException("Ensure failed")
    }
}
