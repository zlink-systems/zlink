using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;
#if NETWORK_SYNC
using Zlink.NetworkSync.Server;
#endif

namespace MyGame.Session;

public sealed class PlayerSession(
    IZLinkSessionContext context,
    IZLinkActorManager actors,
    NodeSettings settings
)
    : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        Console.Error.WriteLine($"Session error: {error.Error}: {error.Message}");
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken
    )
    {
#if NETWORK_SYNC
        if (settings.NetworkSync && await NetworkSyncSession.TryRelayAsync(
                dispatch, payload, Context.Actors.Bound.SingleOrDefault()))
            return;
#endif
        var allowed = dispatch.PacketName switch
        {
            nameof(QueueRequest) or nameof(SessionInfoRequest) => true,
            nameof(RoomViewRequest) or nameof(MoveRequest) or nameof(ShootRequest) => !settings.NetworkSync,
            _ => false,
        };
        if (!allowed)
        {
            await Context.Client.Send(new ActionRejected(Rejection.PacketNotAllowed)).Async();
            return;
        }

        if (Context.Actors.Bound.Count == 0)
        {
            if (dispatch.PacketName != nameof(QueueRequest))
                throw new InvalidOperationException(
                    "QueueRequest must be the first player packet."
                );

            var join = payload.Decode<QueueRequest>();
            var result = await actors
                .GetOrCreate(join.PlayerId, GameNames.PlayerType)
                .InMesh(GameNames.WorldMesh)
                .Request(ZLinkMessage.Empty)
                .Async(cancellationToken);
            var actor = result switch
            {
                ZLinkActorCreateResult.Created created => created.Actor,
                ZLinkActorCreateResult.Existing existing => existing.Actor,
                _ => throw new InvalidOperationException("Player creation was rejected."),
            };
            await Context.Actors.BindOrGetAsync(actor, cancellationToken);
        }

        if (dispatch.PacketName == nameof(SessionInfoRequest))
        {
            var actor = Context.Actors.Bound.Single().Ref;
            await Context
                .Client.Reply(
                    new SessionInfo(Context.SessionId, actor.ActorId, actor.ObjectGeneration)
                )
                .Async();
            return;
        }

        // Relaying the original packet establishes the authoritative actor's session route.
        await Context.Actors.Bound.Single().RelayAsync(payload);
    }
}
