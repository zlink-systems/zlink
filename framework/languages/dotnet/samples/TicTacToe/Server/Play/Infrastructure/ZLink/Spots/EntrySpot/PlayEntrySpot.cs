using TicTacToe.Server.Play.Infrastructure.ZLink.Actors;
using TicTacToe.Server.Play.Infrastructure.ZLink.Spots.EntrySpot.Handlers;
using TicTacToe.Server.Configuration;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Spots.EntrySpot;

// --8<-- [start:doc-entry-spot]
internal sealed class PlayEntrySpot(
    IZLinkEntrySpotContext context,
    ILogger<PlayEntrySpot> logger) : IZLinkEntrySpot<PlayActor>
{
    private readonly MilestoneObserverRegistry _milestoneObservers = new();

    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<PlayActorJoinGameHandler, PlayActor>();
        Context.Handlers.AddActorPacket<PlayActorObserveMilestoneHandler, PlayActor>();
        Context.Handlers.AddSubscribe<PlayerWinMilestoneEventHandler>(
            SampleTopics.PlayerMilestoneChannel,
            SampleTopics.PlayerMilestone);
    }

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        PlayActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        actor.ApplyPlayer(createRequest.Decode<PlayerInfo>());
        logger.LogInformation(
            "entry spot: actor created. actor={ActorId}",
            actor.ActorId);
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));
    }

    public async ValueTask OnJoinedActorAsync(
        PlayActor actor,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "entry spot: actor joined. actor={ActorId}",
            actor.ActorId);
        if (!actor.DestroyAfterEntrySpotJoin) return;

        logger.LogInformation(
            "entry spot: actor destroy requested. actor={ActorId}",
            actor.ActorId);
        // Finished-room cleanup is server lifecycle work. It must finish even
        // when the client closes immediately after receiving the leave reply.
        await Context.DestroyActorAsync(actor, CancellationToken.None);
        logger.LogInformation(
            "entry spot: actor destroy completed. actor={ActorId}",
            actor.ActorId);
    }

    public ValueTask OnLeaveActorAsync(
        PlayActor actor,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "entry spot: actor left. actor={ActorId}",
            actor.ActorId);
        _milestoneObservers.Remove(actor);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectActorAsync(
        PlayActor actor,
        CancellationToken cancellationToken)
    {
        actor.MarkDisconnected();
        _milestoneObservers.Remove(actor);
        logger.LogInformation(
            "entry spot: actor disconnected. actor={ActorId}",
            actor.ActorId);
        return ValueTask.CompletedTask;
    }

    public ValueTask SubscribeMilestoneAsync(
        PlayActor actor,
        CancellationToken cancellationToken)
    {
        _milestoneObservers.Subscribe(actor);
        logger.LogInformation(
            "entry spot: milestone observer subscribed. actor={ActorId}",
            actor.ActorId);
        return ValueTask.CompletedTask;
    }

    public async ValueTask NotifyMilestoneAsync(
        PlayerWinMilestoneEvent milestone,
        CancellationToken cancellationToken)
    {
        await _milestoneObservers.NotifyAsync(
            milestone,
            cancellationToken);
    }

    private sealed class MilestoneObserverRegistry
    {
        private readonly Dictionary<string, PlayActor> _observers = new(StringComparer.Ordinal);

        public void Subscribe(PlayActor actor)
        {
            _observers[actor.ActorId] = actor;
        }

        public void Remove(PlayActor actor)
        {
            _observers.Remove(actor.ActorId);
        }

        public async ValueTask NotifyAsync(
            PlayerWinMilestoneEvent milestone,
            CancellationToken cancellationToken)
        {
            var notify = new WinMilestoneNotify(
                milestone.RoomId,
                milestone.ActorId,
                milestone.DisplayName,
                milestone.Wins);

            var observers = _observers.Values.ToArray();
            foreach (var observer in observers)
                await observer.Context.BoundSession.Send(notify)
                    .Async(cancellationToken);
        }
    }
}
// --8<-- [end:doc-entry-spot]
