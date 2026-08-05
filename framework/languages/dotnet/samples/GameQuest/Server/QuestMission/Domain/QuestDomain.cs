namespace GameQuest.QuestMission.Domain;

internal enum QuestProgressStatus
{
    Active,
    Completed,
    RewardGranted
}

internal sealed record GameplayFact(
    string EventId,
    string PlayerId,
    string EventType,
    string Value,
    int Count,
    string SourceApi,
    long OccurredAtUnixMs);

internal sealed record QuestDefinition(
    string QuestId,
    string EventType,
    string Value,
    int Required);

internal sealed record QuestProgressState(
    string PlayerId,
    string QuestId,
    QuestProgressStatus Status,
    int CurrentCount,
    int RequiredCount,
    string? LastSourceEventId,
    long Version,
    long UpdatedAtUnixMs);

internal abstract record QuestDomainEvent(
    string EventId,
    string SourceEventId,
    string PlayerId,
    string QuestId,
    long Version,
    long OccurredAtUnixMs);

internal sealed record QuestProgressed(
    string EventId,
    string SourceEventId,
    string PlayerId,
    string QuestId,
    int Delta,
    int CurrentCount,
    int RequiredCount,
    long Version,
    long OccurredAtUnixMs)
    : QuestDomainEvent(EventId, SourceEventId, PlayerId, QuestId, Version, OccurredAtUnixMs);

internal sealed record QuestCompleted(
    string EventId,
    string SourceEventId,
    string PlayerId,
    string QuestId,
    long Version,
    long OccurredAtUnixMs)
    : QuestDomainEvent(EventId, SourceEventId, PlayerId, QuestId, Version, OccurredAtUnixMs);

internal sealed record QuestRewardGranted(
    string EventId,
    string SourceEventId,
    string PlayerId,
    string QuestId,
    string RewardId,
    long Version,
    long OccurredAtUnixMs)
    : QuestDomainEvent(EventId, SourceEventId, PlayerId, QuestId, Version, OccurredAtUnixMs);

internal sealed record QuestProgressReconciled(
    string EventId,
    string SourceEventId,
    string PlayerId,
    string QuestId,
    int CurrentCount,
    string Reason,
    long Version,
    long OccurredAtUnixMs)
    : QuestDomainEvent(EventId, SourceEventId, PlayerId, QuestId, Version, OccurredAtUnixMs);

internal static class QuestCatalog
{
    public static readonly QuestDefinition[] All =
    [
        new("first-hunt", "MonsterKilled", "*", 3),
        new("herb-gathering", "ItemCollected", "healing-herb", 5),
        new("visit-ruins", "AreaEntered", "ruins", 1)
    ];

    public static QuestDefinition? Match(GameplayFact gameplayFact)
    {
        return All.FirstOrDefault(definition =>
            definition.EventType == gameplayFact.EventType
            && (definition.Value == "*" || definition.Value == gameplayFact.Value));
    }
}

internal sealed record QuestProgressDecision(
    QuestProgressState State,
    IReadOnlyList<QuestDomainEvent> Events);

internal sealed class QuestProgressAggregate
{
    private readonly QuestDefinition _definition;
    private readonly HashSet<string> _appliedSourceEventIds;

    private QuestProgressAggregate(
        QuestDefinition definition,
        QuestProgressState? state,
        HashSet<string> appliedSourceEventIds)
    {
        _definition = definition;
        State = state;
        _appliedSourceEventIds = appliedSourceEventIds;
    }

    public QuestProgressState? State { get; private set; }

    public static QuestProgressAggregate Rehydrate(
        QuestDefinition definition,
        IReadOnlyList<QuestDomainEvent> stream)
    {
        if (stream.Count == 0)
            return new QuestProgressAggregate(
                definition,
                null,
                new HashSet<string>(StringComparer.Ordinal));

        var currentCount = 0;
        var requiredCount = definition.Required;
        var status = QuestProgressStatus.Active;
        string? lastSourceEventId = null;
        long updatedAtUnixMs = 0;
        var appliedSourceEventIds = new HashSet<string>(StringComparer.Ordinal);
        var ordered = stream.OrderBy(static item => item.Version).ToArray();
        foreach (var @event in ordered)
        {
            if (@event.QuestId != definition.QuestId)
                throw new InvalidOperationException(
                    $"Quest stream '{@event.QuestId}' cannot rehydrate '{definition.QuestId}'.");

            switch (@event)
            {
                case QuestProgressed progressed:
                    currentCount = progressed.CurrentCount;
                    requiredCount = progressed.RequiredCount;
                    break;
                case QuestProgressReconciled reconciled:
                    currentCount = reconciled.CurrentCount;
                    break;
                case QuestCompleted:
                    status = QuestProgressStatus.Completed;
                    break;
                case QuestRewardGranted:
                    status = QuestProgressStatus.RewardGranted;
                    break;
            }

            lastSourceEventId = @event.SourceEventId;
            appliedSourceEventIds.Add(@event.SourceEventId);
            updatedAtUnixMs = Math.Max(updatedAtUnixMs, @event.OccurredAtUnixMs);
        }

        var state = new QuestProgressState(
            ordered[0].PlayerId,
            definition.QuestId,
            status,
            currentCount,
            requiredCount,
            lastSourceEventId,
            ordered[^1].Version,
            updatedAtUnixMs);
        return new QuestProgressAggregate(definition, state, appliedSourceEventIds);
    }

    public QuestProgressDecision? Decide(GameplayFact gameplayFact)
    {
        if (_appliedSourceEventIds.Contains(gameplayFact.EventId)) return null;

        var before = State;
        var nextCount = gameplayFact.EventType == "SnapshotKillCount"
            ? Math.Max(before?.CurrentCount ?? 0, gameplayFact.Count)
            : Math.Min(_definition.Required, (before?.CurrentCount ?? 0) + gameplayFact.Count);
        var nextStatus = nextCount >= _definition.Required
            ? QuestProgressStatus.RewardGranted
            : QuestProgressStatus.Active;
        if (before is not null
            && before.CurrentCount == nextCount
            && before.Status == nextStatus) return null;

        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var events = new List<QuestDomainEvent>();
        var nextVersion = before?.Version ?? 0;
        if (gameplayFact.EventType == "SnapshotKillCount")
            events.Add(new QuestProgressReconciled(
                EventId("QuestReconciled"),
                gameplayFact.EventId,
                gameplayFact.PlayerId,
                _definition.QuestId,
                nextCount,
                "GameplaySnapshot",
                ++nextVersion,
                now));
        else
            events.Add(new QuestProgressed(
                EventId("QuestProgressedEvent"),
                gameplayFact.EventId,
                gameplayFact.PlayerId,
                _definition.QuestId,
                Math.Max(0, nextCount - (before?.CurrentCount ?? 0)),
                nextCount,
                _definition.Required,
                ++nextVersion,
                now));

        if (before?.Status != QuestProgressStatus.RewardGranted
            && nextStatus == QuestProgressStatus.RewardGranted)
        {
            events.Add(new QuestCompleted(
                EventId("QuestCompletedEvent"),
                gameplayFact.EventId,
                gameplayFact.PlayerId,
                _definition.QuestId,
                ++nextVersion,
                now));
            events.Add(new QuestRewardGranted(
                EventId("QuestRewardGrantedEvent"),
                gameplayFact.EventId,
                gameplayFact.PlayerId,
                _definition.QuestId,
                $"reward-{_definition.QuestId}",
                ++nextVersion,
                now));
        }

        State = new QuestProgressState(
            gameplayFact.PlayerId,
            _definition.QuestId,
            nextStatus,
            nextCount,
            _definition.Required,
            gameplayFact.EventId,
            nextVersion,
            now);
        _appliedSourceEventIds.Add(gameplayFact.EventId);
        return new QuestProgressDecision(State, events);

        string EventId(string eventType) =>
            $"{gameplayFact.PlayerId}-{_definition.QuestId}-{gameplayFact.EventId}-{eventType}";
    }
}
