using System.Text.Json;

namespace GameQuest.Shared;

public sealed record KillMonsterReq(string PlayerId, string MonsterId, string AreaId, string IdempotencyKey);

public sealed record KillMonsterRes(string EventId);

public sealed record CollectItemReq(string PlayerId, string ItemId, int Count, string IdempotencyKey);

public sealed record EnterAreaReq(string PlayerId, string AreaId, string IdempotencyKey);

public sealed record JoinSessionReq(string PlayerId);

public sealed record JoinSessionRes(string PlayerId, QuestProgress[] ActiveQuests);

public sealed record GetQuestProgressReq(string PlayerId);

public sealed record GetQuestProgressRes(QuestProgress[] ActiveQuests);

public sealed record SyncQuestProgressReq(string PlayerId);

public sealed record SyncQuestProgressRes(QuestProgress[] UpdatedQuests);

public sealed record QuestProgressNotify(string PlayerId, QuestProgress Progress);

public sealed record QuestCompletedNotify(
    string PlayerId,
    QuestProgress Progress,
    bool RewardGranted);

public sealed record QuestProgress(
    string PlayerId,
    string QuestId,
    string Status,
    int CurrentCount,
    int RequiredCount,
    string? LastSourceEventId,
    long Version,
    long UpdatedAtUnixMs);

public sealed record GameplayMsg(
    string EventId,
    string PlayerId,
    string Type,
    JsonElement Payload,
    long OccurredAtUnixMs);

public sealed record QuestProgressedEvent(
    string EventId,
    string PlayerId,
    string QuestId,
    int Delta,
    int CurrentCount,
    int RequiredCount,
    string SourceEventId);

public sealed record QuestCompletedEvent(
    string EventId,
    string PlayerId,
    string QuestId,
    string SourceEventId,
    long CompletedAtUnixMs);

public sealed record QuestRewardGrantedEvent(
    string EventId,
    string PlayerId,
    string QuestId,
    string SourceEventId,
    string RewardId,
    long GrantedAtUnixMs);

public sealed record QuestReconciled(
    string EventId,
    string PlayerId,
    string QuestId,
    int CurrentCount,
    string Reason,
    long ReconciledAtUnixMs);

public sealed record StoredQuestEvent(
    string EventId,
    string? SourceEventId,
    string PlayerId,
    string QuestId,
    string Type,
    JsonElement Payload,
    long Version,
    long CreatedAtUnixMs);
