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
    ILogger<PlayerQuestSpot> logger
) : IZLinkInstanceSpot
{
    public string PlayerId { get; private set; } = string.Empty;
    private int RehydrationCount { get; set; }
    private bool ReplayEvidencePending { get; set; }
    public IZLinkInstanceSpotContext Context { get; } = context;

    public void Configure() { }

    // --8<-- [start:doc-gq-spot-init]
    public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        PlayerId = Context.SpotId;
        RehydrationCount = await store.RecordOwnerRehydratedAsync(PlayerId, cancellationToken);
        ReplayEvidencePending = RehydrationCount > 1;
        logger.LogInformation(
            "gamequest-owner ready player={PlayerId} rehydrationCount={RehydrationCount} node={NodeId} objectGeneration={ObjectGeneration}",
            PlayerId,
            RehydrationCount,
            processor.MissionName,
            Context.ObjectGeneration
        );
    }

    // --8<-- [end:doc-gq-spot-init]

    public ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken
    )
    {
        _ = context;
        cleanupCancellationToken.ThrowIfCancellationRequested();
        logger.LogInformation(
            "gamequest-owner closing-entered player={PlayerId} rehydrationCount={RehydrationCount} node={NodeId} objectGeneration={ObjectGeneration}",
            PlayerId,
            RehydrationCount,
            processor.MissionName,
            Context.ObjectGeneration
        );
        return ValueTask.CompletedTask;
    }

    public async ValueTask ApplyGameplayEventAsync(
        GameplayMsg message,
        CancellationToken cancellationToken
    )
    {
        await processor.ProcessAsync(
            QuestContractMapper.ToDomain(message),
            TakeReplayEvidenceRehydrationCount(),
            Context.ObjectGeneration,
            cancellationToken
        );
    }

    public async ValueTask<SyncQuestProgressRes> SyncAsync(
        SyncQuestProgressReq request,
        CancellationToken cancellationToken
    )
    {
        var projection = await processor.SyncAsync(
            request.PlayerId,
            TakeReplayEvidenceRehydrationCount(),
            Context.ObjectGeneration,
            cancellationToken
        );
        return new SyncQuestProgressRes(
            projection.Select(QuestContractMapper.ToContract).ToArray()
        );
    }

    private int? TakeReplayEvidenceRehydrationCount()
    {
        if (!ReplayEvidencePending)
            return null;
        ReplayEvidencePending = false;
        return RehydrationCount;
    }
}

// --8<-- [start:doc-gq-close-handler]
internal sealed class ClosePlayerQuestHandler
    : IZLinkSpotPacketHandler<PlayerQuestSpot, ClosePlayerQuestMsg>
{
    public ValueTask HandleAsync(
        PlayerQuestSpot spot,
        ClosePlayerQuestMsg message,
        CancellationToken cancellationToken
    )
    {
        _ = message;
        _ = spot.Context.CloseAsync(cancellationToken);
        return ValueTask.CompletedTask;
    }
}

// --8<-- [end:doc-gq-close-handler]

// --8<-- [start:doc-gq-apply-handler]
internal sealed class ApplyGameplayEventHandler
    : IZLinkSpotPacketHandler<PlayerQuestSpot, GameplayMsg>
{
    public ValueTask HandleAsync(
        PlayerQuestSpot spot,
        GameplayMsg message,
        CancellationToken cancellationToken
    )
    {
        return spot.ApplyGameplayEventAsync(message, cancellationToken);
    }
}

// --8<-- [end:doc-gq-apply-handler]

internal sealed class SyncQuestProgressHandler
    : IZLinkSpotRequestHandler<PlayerQuestSpot, SyncQuestProgressReq, SyncQuestProgressRes>
{
    public ValueTask<SyncQuestProgressRes> HandleAsync(
        PlayerQuestSpot spot,
        SyncQuestProgressReq request,
        CancellationToken cancellationToken
    )
    {
        return spot.SyncAsync(request, cancellationToken);
    }
}
