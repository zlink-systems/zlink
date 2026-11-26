using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;

namespace MyGame.Session;

public sealed class PlayerSession(
    IZLinkSessionContext context,
    IZLinkActorManager actors,
    IZLinkActorClient actorClient,
    IZLinkSpotManager spots
) : IZLinkSession
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
        if (
            dispatch.PacketName
            is not (
                nameof(JoinWorldRequest)
                or nameof(MoveRequest)
                or nameof(CreatePartyRequest)
                or nameof(JoinPartyRequest)
                or nameof(EnterDungeonRequest)
                or nameof(AdvanceRequest)
                or nameof(ViewRequest)
                or nameof(LeaveDungeonRequest)
                or nameof(SessionInfoRequest)
            )
        )
        {
            await Context.Client.Send(new ActionRejected("packet-not-allowed")).Async();
            return;
        }
        if (Context.Actors.Bound.Count == 0)
        {
            if (dispatch.PacketName != nameof(JoinWorldRequest))
                throw new InvalidOperationException(
                    "JoinWorldRequest must be the first player packet."
                );

            var join = payload.Decode<JoinWorldRequest>();
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

        if (dispatch.PacketName == nameof(LeaveDungeonRequest))
        {
            var actor = Context.Actors.Bound.Single().Ref;
            var left = await actorClient
                .RequestToActor(actor.ActorId, new LeaveDungeonRequest())
                .Async<DungeonLeft>(cancellationToken);

            // Close outside the Dungeon turn, after its membership change completes.
            if (left.RemainingPlayers == 0)
            {
                var dungeon = await spots.FindAsync(left.DungeonId, cancellationToken);
                if (dungeon is { } current && current.ObjectGeneration == left.ObjectGeneration)
                    await spots.CloseAsync(current, cancellationToken);
            }

            await Context.Client.Reply(left).Async();
            return;
        }

        // Relaying the original packet establishes the authoritative actor's session route.
        await Context.Actors.Bound.Single().RelayAsync(payload);
    }
}
