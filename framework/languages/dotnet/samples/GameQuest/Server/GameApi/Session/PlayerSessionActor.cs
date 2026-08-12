using GameQuest.Server.Configuration;
using GameQuest.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace GameQuest.GameApi.Session;

internal sealed class PlayerSessionActor(
    string actorId,
    IZLinkActorContext context) : IZLinkActor
{
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;

    public void EnsurePlayer(string playerId)
    {
        if (!string.Equals(ActorId, playerId, StringComparison.Ordinal))
            throw new InvalidOperationException(
                $"Bound player '{ActorId}' cannot act for player '{playerId}'.");
    }
}

internal sealed class PlayerSessionActorFactory : IZLinkActorFactory<PlayerSessionActor>
{
    public ValueTask<PlayerSessionActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default) =>
        ValueTask.FromResult(new PlayerSessionActor(context.ActorId, context));
}

internal sealed class GameQuestEntrySpot(
    IZLinkEntrySpotContext context,
    ILogger<GameQuestEntrySpot> logger) : IZLinkEntrySpot<PlayerSessionActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        PlayerSessionActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        logger.LogInformation("gamequest session actor created player={PlayerId}", actor.ActorId);
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

    public ValueTask OnJoinedActorAsync(PlayerSessionActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(PlayerSessionActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(PlayerSessionActor actor, CancellationToken cancellationToken)
    {
        logger.LogInformation("gamequest session actor disconnected player={PlayerId}", actor.ActorId);
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotActorSendHandler(nameof(QuestProgressMsg))]
internal sealed class QuestProgressMsgActorHandler
    : IZLinkEntrySpotActorSendHandler<
        GameQuestEntrySpot,
        PlayerSessionActor,
        QuestProgressMsg>
{
    public async ValueTask HandleAsync(
        GameQuestEntrySpot entrySpot,
        PlayerSessionActor actor,
        IZLinkMessageContext context,
        QuestProgressMsg message,
        CancellationToken cancellationToken)
    {
        await actor.Context.BoundSession
            .Send(new QuestProgressNotify(message.PlayerId, message.Progress))
            .Async(cancellationToken);
    }
}

[ZLinkSpotActorSendHandler(nameof(QuestCompletedMsg))]
internal sealed class QuestCompletedMsgActorHandler
    : IZLinkEntrySpotActorSendHandler<
        GameQuestEntrySpot,
        PlayerSessionActor,
        QuestCompletedMsg>
{
    public async ValueTask HandleAsync(
        GameQuestEntrySpot entrySpot,
        PlayerSessionActor actor,
        IZLinkMessageContext context,
        QuestCompletedMsg message,
        CancellationToken cancellationToken)
    {
        await actor.Context.BoundSession
            .Send(new QuestCompletedNotify(
                message.PlayerId,
                message.Progress,
                message.RewardGranted))
            .Async(cancellationToken);
    }
}
