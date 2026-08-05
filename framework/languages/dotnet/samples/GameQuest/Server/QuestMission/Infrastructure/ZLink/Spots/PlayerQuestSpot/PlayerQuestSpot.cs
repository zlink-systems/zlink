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
    public IZLinkInstanceSpotContext Context { get; } = context;

    public void Configure()
    {
    }

    public async ValueTask OnInitializeAsync(
        CancellationToken cancellationToken)
    {
        PlayerId = Context.SpotId;
        await store.RecordOwnerRehydratedAsync(PlayerId, cancellationToken);
        logger.LogInformation(
            "gamequest player quest spot ready player={PlayerId} spot={SpotId}",
            PlayerId,
            Context.SpotId);
    }

    public ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        _ = context;
        cleanupCancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }

    public async ValueTask ApplyGameplayEventAsync(
        GameplayMsg message,
        CancellationToken cancellationToken)
    {
        await processor.ProcessAsync(
            QuestContractMapper.ToDomain(message),
            cancellationToken);
    }

    public async ValueTask<SyncQuestProgressRes> SyncAsync(
        SyncQuestProgressReq request,
        CancellationToken cancellationToken)
    {
        var projection = await processor.SyncAsync(request.PlayerId, cancellationToken);
        return new SyncQuestProgressRes(
            projection.Select(QuestContractMapper.ToContract).ToArray());
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
