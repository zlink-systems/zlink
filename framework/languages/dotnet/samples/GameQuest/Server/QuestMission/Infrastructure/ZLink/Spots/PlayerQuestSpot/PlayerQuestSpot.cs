using GameQuest.QuestMission.Application;
using GameQuest.QuestMission.Infrastructure.Store;
using GameQuest.Server.Configuration;
using GameQuest.Shared;
using Zlink.Framework.Contracts.Spots;

namespace GameQuest.QuestMission.Infrastructure.ZLink.Spots.PlayerQuestSpot;

internal sealed record ClosePlayerQuestMsg;

internal sealed class PlayerQuestSpot(
    IZLinkInstanceSpotContext context,
    QuestEventProcessor processor,
    QuestStore store,
    ILogger<PlayerQuestSpot> logger) : IZLinkInstanceSpot
{
    public string PlayerId { get; private set; } = string.Empty;
    private int Generation { get; set; }
    private bool ReplayEvidencePending { get; set; }
    public IZLinkInstanceSpotContext Context { get; } = context;

    public void Configure()
    {
    }

    public async ValueTask OnInitializeAsync(
        CancellationToken cancellationToken)
    {
        PlayerId = Context.SpotId;
        Generation = await store.RecordOwnerRehydratedAsync(PlayerId, cancellationToken);
        ReplayEvidencePending = Generation > 1;
        logger.LogInformation(
            "gamequest-owner ready player={PlayerId} generation={Generation} node={NodeId}",
            PlayerId,
            Generation,
            processor.MissionName);
    }

    public ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        _ = context;
        cleanupCancellationToken.ThrowIfCancellationRequested();
        logger.LogInformation(
            "gamequest-owner closed player={PlayerId} generation={Generation} node={NodeId}",
            PlayerId,
            Generation,
            processor.MissionName);
        return ValueTask.CompletedTask;
    }

    public async ValueTask ApplyGameplayEventAsync(
        GameplayMsg message,
        CancellationToken cancellationToken)
    {
        await processor.ProcessAsync(
            QuestContractMapper.ToDomain(message),
            TakeReplayEvidenceGeneration(),
            cancellationToken);
    }

    public async ValueTask<SyncQuestProgressRes> SyncAsync(
        SyncQuestProgressReq request,
        CancellationToken cancellationToken)
    {
        var projection = await processor.SyncAsync(
            request.PlayerId,
            TakeReplayEvidenceGeneration(),
            cancellationToken);
        return new SyncQuestProgressRes(
            projection.Select(QuestContractMapper.ToContract).ToArray());
    }

    private int? TakeReplayEvidenceGeneration()
    {
        if (!ReplayEvidencePending) return null;
        ReplayEvidencePending = false;
        return Generation;
    }
}

internal sealed class ClosePlayerQuestHandler :
    IZLinkSpotPacketHandler<PlayerQuestSpot, ClosePlayerQuestMsg>
{
    public async ValueTask HandleAsync(
        PlayerQuestSpot spot,
        ClosePlayerQuestMsg message,
        CancellationToken cancellationToken)
    {
        _ = message;
        await spot.Context.CloseAsync(cancellationToken);
    }
}

internal sealed class ApplyGameplayEventHandler :
    IZLinkSpotPacketHandler<PlayerQuestSpot, GameplayMsg>
{
    public ValueTask HandleAsync(
        PlayerQuestSpot spot,
        GameplayMsg message,
        CancellationToken cancellationToken)
    {
        return spot.ApplyGameplayEventAsync(message, cancellationToken);
    }
}

internal sealed class SyncQuestProgressHandler :
    IZLinkSpotRequestHandler<PlayerQuestSpot, SyncQuestProgressReq, SyncQuestProgressRes>
{
    public ValueTask<SyncQuestProgressRes> HandleAsync(
        PlayerQuestSpot spot,
        SyncQuestProgressReq request,
        CancellationToken cancellationToken)
    {
        return spot.SyncAsync(request, cancellationToken);
    }
}
