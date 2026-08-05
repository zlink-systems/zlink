using GameQuest.GameApi.Application;
using GameQuest.GameApi.Infrastructure.Store;
using GameQuest.Server.Configuration;
using GameQuest.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;

namespace GameQuest.GameApi.Session;

[ZLinkSpotActorRequestHandler(nameof(GetQuestProgressReq))]
internal sealed class GetQuestProgressHandler(GameQuestStore store)
    : IZLinkEntrySpotActorRequestHandler<
        GameQuestEntrySpot,
        PlayerSessionActor,
        GetQuestProgressReq,
        GetQuestProgressRes>
{
    public async ValueTask<GetQuestProgressRes> HandleAsync(
        GameQuestEntrySpot entrySpot,
        PlayerSessionActor actor,
        IZLinkMessageContext context,
        GetQuestProgressReq request,
        CancellationToken cancellationToken)
    {
        actor.EnsurePlayer(request.PlayerId);
        return new GetQuestProgressRes(
            await store.ReadProjectionAsync(actor.ActorId, cancellationToken));
    }
}

internal sealed class JoinSessionHandler(
    JoinQuestSessionUseCase joinSessions,
    IZLinkActorManager actors)
    : IZLinkSessionPacketHandler<IZLinkSessionContext, JoinSessionReq>
{
    public async ValueTask HandleAsync(
        IZLinkSessionContext context,
        ZLinkSessionDispatchContext dispatch,
        JoinSessionReq request,
        CancellationToken cancellationToken)
    {
        var actor = (await actors.GetOrCreate(request.PlayerId, SampleNames.SessionActorType)
            .Request(request).Async(cancellationToken)) switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            _ => throw new InvalidOperationException("Session Actor creation was rejected.")
        };
        _ = await context.Actors.BindOrGetAsync(actor, cancellationToken);
        await context.Client.Reply(await joinSessions.ExecuteAsync(request.PlayerId, cancellationToken))
            .Async(cancellationToken);
    }
}

[ZLinkSpotActorRequestHandler(nameof(KillMonsterReq))]
internal sealed class KillMonsterHandler(GameplayActionService actions)
    : IZLinkEntrySpotActorRequestHandler<GameQuestEntrySpot, PlayerSessionActor, KillMonsterReq, KillMonsterRes>
{
    public async ValueTask<KillMonsterRes> HandleAsync(
        GameQuestEntrySpot entrySpot,
        PlayerSessionActor actor,
        IZLinkMessageContext context,
        KillMonsterReq request,
        CancellationToken cancellationToken)
    {
        actor.EnsurePlayer(request.PlayerId);
        return new KillMonsterRes(await actions.KillMonsterAsync(
            request.PlayerId,
            request.MonsterId,
            request.AreaId,
            request.IdempotencyKey,
            cancellationToken));
    }
}

[ZLinkSpotActorSendHandler(nameof(CollectItemReq))]
internal sealed class CollectItemHandler(GameplayActionService actions)
    : IZLinkEntrySpotActorSendHandler<GameQuestEntrySpot, PlayerSessionActor, CollectItemReq>
{
    public async ValueTask HandleAsync(
        GameQuestEntrySpot entrySpot,
        PlayerSessionActor actor,
        IZLinkMessageContext context,
        CollectItemReq message,
        CancellationToken cancellationToken)
    {
        actor.EnsurePlayer(message.PlayerId);
        await actions.CollectItemAsync(
            message.PlayerId,
            message.ItemId,
            message.Count,
            message.IdempotencyKey,
            cancellationToken);
    }
}

[ZLinkSpotActorSendHandler(nameof(EnterAreaReq))]
internal sealed class EnterAreaHandler(GameplayActionService actions)
    : IZLinkEntrySpotActorSendHandler<GameQuestEntrySpot, PlayerSessionActor, EnterAreaReq>
{
    public async ValueTask HandleAsync(
        GameQuestEntrySpot entrySpot,
        PlayerSessionActor actor,
        IZLinkMessageContext context,
        EnterAreaReq message,
        CancellationToken cancellationToken)
    {
        actor.EnsurePlayer(message.PlayerId);
        await actions.EnterAreaAsync(
            message.PlayerId,
            message.AreaId,
            message.IdempotencyKey,
            cancellationToken);
    }
}

[ZLinkSpotActorRequestHandler(nameof(SyncQuestProgressReq))]
internal sealed class SyncQuestProgressHandler(IQuestProgressSynchronizer quests)
    : IZLinkEntrySpotActorRequestHandler<
        GameQuestEntrySpot,
        PlayerSessionActor,
        SyncQuestProgressReq,
        SyncQuestProgressRes>
{
    public async ValueTask<SyncQuestProgressRes> HandleAsync(
        GameQuestEntrySpot entrySpot,
        PlayerSessionActor actor,
        IZLinkMessageContext context,
        SyncQuestProgressReq request,
        CancellationToken cancellationToken)
    {
        actor.EnsurePlayer(request.PlayerId);
        return await quests.SyncAsync(actor.ActorId, cancellationToken);
    }
}
