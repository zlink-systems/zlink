using System.Text.Json;
using GameQuest.QuestMission.Domain;
using GameQuest.Shared;

namespace GameQuest.QuestMission.Infrastructure.Store;

internal static class QuestContractMapper
{
    private sealed record GameplayPayload(string Value, int Count, string SourceApi);

    public static GameplayFact ToDomain(GameplayMsg message)
    {
        var payload = message.Payload.Deserialize<GameplayPayload>()
                      ?? throw new InvalidOperationException(
                          $"Gameplay message '{message.EventId}' has an empty payload.");
        return new GameplayFact(
            message.EventId,
            message.PlayerId,
            message.Type,
            payload.Value,
            payload.Count,
            payload.SourceApi,
            message.OccurredAtUnixMs);
    }

    public static QuestProgressState ToDomain(QuestProgress progress)
    {
        return new QuestProgressState(
            progress.PlayerId,
            progress.QuestId,
            progress.Status switch
            {
                "Active" => QuestProgressStatus.Active,
                "Completed" => QuestProgressStatus.Completed,
                "RewardGranted" => QuestProgressStatus.RewardGranted,
                _ => throw new InvalidOperationException(
                    $"Unsupported quest progress status '{progress.Status}'.")
            },
            progress.CurrentCount,
            progress.RequiredCount,
            progress.LastSourceEventId,
            progress.Version,
            progress.UpdatedAtUnixMs);
    }

    public static QuestProgress ToContract(QuestProgressState progress)
    {
        return new QuestProgress(
            progress.PlayerId,
            progress.QuestId,
            progress.Status.ToString(),
            progress.CurrentCount,
            progress.RequiredCount,
            progress.LastSourceEventId,
            progress.Version,
            progress.UpdatedAtUnixMs);
    }

    public static QuestDomainEvent ToDomain(StoredQuestEvent stored)
    {
        var sourceEventId = stored.SourceEventId
                            ?? throw new InvalidOperationException(
                                $"Stored quest event '{stored.EventId}' has no source event id.");
        return stored.Type switch
        {
            nameof(QuestProgressedEvent) => Progressed(),
            nameof(QuestCompletedEvent) => new QuestCompleted(
                stored.EventId,
                sourceEventId,
                stored.PlayerId,
                stored.QuestId,
                stored.Version,
                stored.CreatedAtUnixMs),
            nameof(QuestRewardGrantedEvent) => RewardGranted(),
            nameof(QuestReconciled) => Reconciled(),
            _ => throw new InvalidOperationException(
                $"Unsupported stored quest event type '{stored.Type}'.")
        };

        QuestProgressed Progressed()
        {
            var payload = stored.Payload.Deserialize<QuestProgressedEvent>()
                          ?? throw InvalidPayload(stored);
            return new QuestProgressed(
                stored.EventId,
                sourceEventId,
                stored.PlayerId,
                stored.QuestId,
                payload.Delta,
                payload.CurrentCount,
                payload.RequiredCount,
                stored.Version,
                stored.CreatedAtUnixMs);
        }

        QuestRewardGranted RewardGranted()
        {
            var payload = stored.Payload.Deserialize<QuestRewardGrantedEvent>()
                          ?? throw InvalidPayload(stored);
            return new QuestRewardGranted(
                stored.EventId,
                sourceEventId,
                stored.PlayerId,
                stored.QuestId,
                payload.RewardId,
                stored.Version,
                stored.CreatedAtUnixMs);
        }

        QuestProgressReconciled Reconciled()
        {
            var payload = stored.Payload.Deserialize<QuestReconciled>()
                          ?? throw InvalidPayload(stored);
            return new QuestProgressReconciled(
                stored.EventId,
                sourceEventId,
                stored.PlayerId,
                stored.QuestId,
                payload.CurrentCount,
                payload.Reason,
                stored.Version,
                stored.CreatedAtUnixMs);
        }
    }

    public static StoredQuestEvent ToContract(QuestDomainEvent domainEvent)
    {
        var (eventType, payload) = domainEvent switch
        {
            QuestProgressed progressed => (
                nameof(QuestProgressedEvent),
                JsonSerializer.SerializeToElement(new QuestProgressedEvent(
                    progressed.EventId,
                    progressed.PlayerId,
                    progressed.QuestId,
                    progressed.Delta,
                    progressed.CurrentCount,
                    progressed.RequiredCount,
                    progressed.SourceEventId))),
            QuestCompleted completed => (
                nameof(QuestCompletedEvent),
                JsonSerializer.SerializeToElement(new QuestCompletedEvent(
                    completed.EventId,
                    completed.PlayerId,
                    completed.QuestId,
                    completed.SourceEventId,
                    completed.OccurredAtUnixMs))),
            QuestRewardGranted granted => (
                nameof(QuestRewardGrantedEvent),
                JsonSerializer.SerializeToElement(new QuestRewardGrantedEvent(
                    granted.EventId,
                    granted.PlayerId,
                    granted.QuestId,
                    granted.SourceEventId,
                    granted.RewardId,
                    granted.OccurredAtUnixMs))),
            QuestProgressReconciled reconciled => (
                nameof(QuestReconciled),
                JsonSerializer.SerializeToElement(new QuestReconciled(
                    reconciled.EventId,
                    reconciled.PlayerId,
                    reconciled.QuestId,
                    reconciled.CurrentCount,
                    reconciled.Reason,
                    reconciled.OccurredAtUnixMs))),
            _ => throw new ArgumentOutOfRangeException(nameof(domainEvent))
        };

        return new StoredQuestEvent(
            domainEvent.EventId,
            domainEvent.SourceEventId,
            domainEvent.PlayerId,
            domainEvent.QuestId,
            eventType,
            payload,
            domainEvent.Version,
            domainEvent.OccurredAtUnixMs);
    }

    private static InvalidOperationException InvalidPayload(StoredQuestEvent stored)
    {
        return new InvalidOperationException(
            $"Stored quest event '{stored.EventId}' has an empty payload.");
    }
}
