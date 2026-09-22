using System.Collections.Concurrent;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace EngineLobby;

public sealed class LobbySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot<ParticipantActor>
{
    private readonly ConcurrentDictionary<string, ParticipantActor> _participants = new(
        StringComparer.Ordinal
    );

    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        ParticipantActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken
    )
    {
        var join = createRequest.Decode<Join>();
        actor.SetName(join.Name);
        _participants[actor.Context.ActorId] = actor;
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask OnJoinedActorAsync(ParticipantActor actor, CancellationToken cancellationToken)
    {
        _participants[actor.Context.ActorId] = actor;
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(ParticipantActor actor, CancellationToken cancellationToken)
    {
        _participants.TryRemove(actor.Context.ActorId, out _);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectActorAsync(
        ParticipantActor actor,
        CancellationToken cancellationToken
    )
    {
        _participants.TryRemove(actor.Context.ActorId, out _);
        return ValueTask.CompletedTask;
    }

    public async ValueTask BroadcastAsync(
        ChatNotify notification,
        CancellationToken cancellationToken
    )
    {
        foreach (var participant in _participants.Values.ToArray())
        {
            try
            {
                await participant.Context.BoundSession.Send(notification).Async(cancellationToken);
            }
            catch (ZLinkFrameworkException error)
                when (error.Kind == ZLinkFrameworkErrorKind.InvalidOperation) { }
        }
    }
}

public sealed class ChatHandler : IZLinkEntrySpotActorSendHandler<LobbySpot, ParticipantActor, Chat>
{
    public async ValueTask HandleAsync(
        LobbySpot lobby,
        ParticipantActor actor,
        IZLinkMessageContext context,
        Chat message,
        CancellationToken cancellationToken
    )
    {
        await lobby.BroadcastAsync(
            new ChatNotify(actor.Context.ActorId, actor.Name, message.Text),
            cancellationToken
        );
    }
}
