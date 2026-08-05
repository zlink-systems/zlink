package systems.zlink.samples.gamequest.shared.contracts;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;

import java.util.List;

public final class Messages {
    private Messages() {
    }

    public static final class QuestIds {
        public static final String FirstHunt = "first-hunt";
        public static final String OpenAuction = "open-auction";
        public static final String HerbGathering = "herb-gathering";

        private QuestIds() {
        }
    }

    public static final class QuestStatuses {
        public static final String InProgress = "InProgress";
        public static final String RewardGranted = "RewardGranted";

        private QuestStatuses() {
        }
    }

    public record KillMonsterReq(String playerId, String monsterId, String areaId, String idempotencyKey) {
    }

    public record KillMonsterRes(String eventId) {
    }

    public record CollectItemReq(String playerId, String itemId, int count, String idempotencyKey) {
    }

    public record CollectItemRes(String eventId) {
    }

    public record CompleteMissionReq(String playerId, String missionId, String idempotencyKey) {
    }

    public record CompleteMissionRes(String eventId) {
    }

    public record EnterAreaReq(String playerId, String areaId, String idempotencyKey) {
    }

    public record EnterAreaRes(String eventId) {
    }

    public record UnlockFeatureReq(String playerId, String featureId, String idempotencyKey) {
    }

    public record UnlockFeatureRes(String eventId) {
    }

    public record JoinSessionReq(String playerId) {
    }

    public record JoinSessionRes(List<QuestProgress> activeQuests) {
    }

    public record GetQuestProgressReq(String playerId) {
    }

    public record GetQuestProgressRes(List<QuestProgress> activeQuests) {
    }

    public record DeleteQuestProjectionReq(String playerId, String questId) {
    }

    public record DeleteQuestProjectionRes(boolean deleted) {
    }

    public record RebuildQuestProjectionReq(String playerId, String questId, int count) {
    }

    public record SyncQuestProgressReq(String playerId) {
    }

    public record SyncQuestProgressRes(List<QuestProgress> updatedQuests) {
    }

    public record GetGameplaySnapshotReq(String playerId) {
    }

    public record GetGameplaySnapshotRes(
        String playerId,
        List<KillCountSnapshot> killCounts,
        List<ItemCountSnapshot> itemCounts,
        List<String> completedMissionIds,
        List<String> unlockedFeatureIds,
        List<String> enteredAreaIds,
        long snapshotVersion) {
    }

    public record KillCountSnapshot(String monsterId, String areaId, int count) {
    }

    public record ItemCountSnapshot(String itemId, int count) {
    }

    public record QuestProgressNotify(String playerId, String targetConnectionId, QuestProgress progress) {
    }

    public record QuestCompletedNotify(
        String playerId,
        String targetConnectionId,
        QuestProgress progress,
        boolean rewardGranted) {
    }

    public record QuestProgress(
        String playerId,
        String questId,
        String status,
        int currentCount,
        int requiredCount,
        String lastEventId,
        long updatedAtUnixMs) {
    }

    public record GameplayMsg(
        String eventId,
        String playerId,
        String type,
        byte[] payload,
        long occurredAtUnixMs) {
        public static GameplayMsg create(
            String eventId,
            String playerId,
            String type,
            String idempotencyKey,
            String value,
            int count,
            String sourceApi,
            long occurredAtUnixMs,
            boolean publish) {
            return new GameplayMsg(
                eventId,
                playerId,
                type,
                GameplayPayload.encode(
                    new GameplayPayload(idempotencyKey, value, count, sourceApi, publish)),
                occurredAtUnixMs);
        }

        public GameplayPayload decodePayload() {
            return GameplayPayload.decode(payload);
        }

        public String idempotencyKey() { return decodePayload().idempotencyKey(); }
        public String eventType() { return type; }
        public String value() { return decodePayload().value(); }
        public int count() { return decodePayload().count(); }
        public String sourceApi() { return decodePayload().sourceApi(); }
        public long createdAtUnixMs() { return occurredAtUnixMs; }
        public boolean publish() { return decodePayload().publish(); }
    }

    public record GameplayPayload(
        String idempotencyKey,
        String value,
        int count,
        String sourceApi,
        boolean publish) {
        private static byte[] encode(GameplayPayload payload) {
            try {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                try (DataOutputStream output = new DataOutputStream(bytes)) {
                    output.writeUTF(payload.idempotencyKey());
                    output.writeUTF(payload.value());
                    output.writeInt(payload.count());
                    output.writeUTF(payload.sourceApi());
                    output.writeBoolean(payload.publish());
                }
                return bytes.toByteArray();
            } catch (IOException error) {
                throw new IllegalStateException("failed to encode gameplay payload", error);
            }
        }

        private static GameplayPayload decode(byte[] bytes) {
            try (DataInputStream input = new DataInputStream(new ByteArrayInputStream(bytes))) {
                return new GameplayPayload(
                    input.readUTF(),
                    input.readUTF(),
                    input.readInt(),
                    input.readUTF(),
                    input.readBoolean());
            } catch (IOException error) {
                throw new IllegalArgumentException("invalid gameplay payload", error);
            }
        }
    }

    public record QuestProgressedEvent(
        String eventId,
        String playerId,
        String questId,
        int delta,
        int currentCount,
        int requiredCount,
        String sourceEventId) {
    }

    public record QuestCompletedEvent(
        String eventId,
        String playerId,
        String questId,
        String sourceEventId,
        long completedAtUnixMs) {
    }

    public record QuestRewardGrantedEvent(
        String eventId,
        String playerId,
        String questId,
        String sourceEventId,
        String rewardId,
        long grantedAtUnixMs) {
    }

    public record QuestProgressReconciledEvent(
        String eventId,
        String playerId,
        String questId,
        int currentCount,
        String reason,
        long reconciledAtUnixMs) {
    }

    public record StoredQuestEvent(
        String eventId,
        String sourceEventId,
        String playerId,
        String questId,
        String eventType,
        int delta,
        int currentCount,
        int requiredCount,
        String status,
        long version,
        long createdAtUnixMs) {
    }

    public record QuestProcessingMsg(
        String eventId,
        String playerId,
        List<QuestProgress> projection,
        List<QuestProgressNotify> progressNotifications,
        List<QuestCompletedNotify> completedNotifications,
        boolean duplicate) {
    }

    public record GameQuestServerAssertRes(boolean passed, List<String> evidence) {
    }
}
