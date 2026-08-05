package systems.zlink.samples.kotlin.gamequest.shared.contracts

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream

object QuestIds {
    const val FirstHunt = "first-hunt"
    const val OpenAuction = "open-auction"
    const val HerbGathering = "herb-gathering"
}

object QuestStatuses {
    const val InProgress = "InProgress"
    const val RewardGranted = "RewardGranted"
}

data class KillMonsterReq(val playerId: String, val monsterId: String, val areaId: String?, val idempotencyKey: String)
data class KillMonsterRes(val eventId: String)
data class CollectItemReq(val playerId: String, val itemId: String, val count: Int, val idempotencyKey: String)
data class CollectItemRes(val eventId: String)
data class CompleteMissionReq(val playerId: String, val missionId: String, val idempotencyKey: String)
data class CompleteMissionRes(val eventId: String)
data class EnterAreaReq(val playerId: String, val areaId: String, val idempotencyKey: String)
data class EnterAreaRes(val eventId: String)
data class UnlockFeatureReq(val playerId: String, val featureId: String, val idempotencyKey: String)
data class UnlockFeatureRes(val eventId: String)
data class JoinSessionReq(val playerId: String)
data class JoinSessionRes(val activeQuests: List<QuestProgress>)
data class GetQuestProgressReq(val playerId: String)
data class GetQuestProgressRes(val activeQuests: List<QuestProgress>)
data class DeleteQuestProjectionReq(val playerId: String, val questId: String)
data class DeleteQuestProjectionRes(val deleted: Boolean)
data class RebuildQuestProjectionReq(val playerId: String, val questId: String, val count: Int)
data class SyncQuestProgressReq(val playerId: String)
data class SyncQuestProgressRes(val updatedQuests: List<QuestProgress>)
data class GetGameplaySnapshotReq(val playerId: String)
data class GetGameplaySnapshotRes(
    val playerId: String,
    val killCounts: List<KillCountSnapshot>,
    val itemCounts: List<ItemCountSnapshot>,
    val completedMissionIds: List<String>,
    val unlockedFeatureIds: List<String>,
    val enteredAreaIds: List<String>,
    val snapshotVersion: Long,
)

data class KillCountSnapshot(val monsterId: String, val areaId: String?, val count: Int)
data class ItemCountSnapshot(val itemId: String, val count: Int)
data class QuestProgressNotify(val playerId: String, val targetConnectionId: String?, val progress: QuestProgress)
data class QuestCompletedNotify(
    val playerId: String,
    val targetConnectionId: String?,
    val progress: QuestProgress,
    val rewardGranted: Boolean,
)

data class QuestProgress(
    val playerId: String,
    val questId: String,
    val status: String,
    val currentCount: Int,
    val requiredCount: Int,
    val lastEventId: String?,
    val updatedAtUnixMs: Long,
)

data class GameplayMsg(
    val eventId: String,
    val playerId: String,
    val type: String,
    val payload: ByteArray,
    val occurredAtUnixMs: Long,
) {
    fun decodePayload(): GameplayPayload = GameplayPayload.decode(payload)

    companion object {
        fun create(
            eventId: String,
            playerId: String,
            type: String,
            idempotencyKey: String,
            value: String,
            count: Int,
            sourceApi: String,
            occurredAtUnixMs: Long,
            publish: Boolean,
        ): GameplayMsg = GameplayMsg(
            eventId,
            playerId,
            type,
            GameplayPayload.encode(GameplayPayload(idempotencyKey, value, count, sourceApi, publish)),
            occurredAtUnixMs,
        )
    }
}

data class GameplayPayload(
    val idempotencyKey: String,
    val value: String,
    val count: Int,
    val sourceApi: String,
    val publish: Boolean,
) {
    companion object {
        fun encode(payload: GameplayPayload): ByteArray {
            val bytes = ByteArrayOutputStream()
            DataOutputStream(bytes).use { output ->
                output.writeUTF(payload.idempotencyKey)
                output.writeUTF(payload.value)
                output.writeInt(payload.count)
                output.writeUTF(payload.sourceApi)
                output.writeBoolean(payload.publish)
            }
            return bytes.toByteArray()
        }

        fun decode(bytes: ByteArray): GameplayPayload =
            DataInputStream(ByteArrayInputStream(bytes)).use { input ->
                GameplayPayload(
                    input.readUTF(),
                    input.readUTF(),
                    input.readInt(),
                    input.readUTF(),
                    input.readBoolean(),
                )
            }
    }
}

class QuestProgressedEvent
class QuestCompletedEvent
class QuestRewardGrantedEvent
class QuestProgressReconciledEvent

data class StoredQuestEvent(
    val eventId: String,
    val sourceEventId: String?,
    val playerId: String,
    val questId: String,
    val eventType: String,
    val currentCount: Int,
    val requiredCount: Int,
    val status: String,
    val version: Long,
    val createdAtUnixMs: Long,
)

data class QuestProcessingRes(
    val eventId: String,
    val playerId: String,
    val projection: List<QuestProgress>,
    val progressNotifications: List<QuestProgressNotify>,
    val completedNotifications: List<QuestCompletedNotify>,
    val duplicate: Boolean,
)

data class GameQuestServerAssertRes(val passed: Boolean, val evidence: List<String>)
