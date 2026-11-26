using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Table;

public sealed class TableEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<EnterTableHandler, PlayerActor>(nameof(JoinTableRequest));
        Context.Handlers.AddActorPacket<EntryFindMatchHandler, PlayerActor>(nameof(FindMatch));
        Context.Handlers.AddActorPacket<EntryCancelMatchHandler, PlayerActor>(nameof(CancelMatch));
        Context.Handlers.AddActorPacket<EntryMatchAssignedHandler, PlayerActor>(
            nameof(MatchAssigned)
        );
        Context.Handlers.AddActorPacket<EntryMatchCancelledHandler, PlayerActor>(
            nameof(MatchCancelled)
        );
    }

    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken ct) =>
        actor.DisconnectAsync(ct);

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

public sealed class EnterTableHandler(IZLinkSpotManager spots)
    : IZLinkEntrySpotActorSendHandler<TableEntry, PlayerActor, JoinTableRequest>
{
    public async ValueTask HandleAsync(
        TableEntry entry,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinTableRequest request,
        CancellationToken cancellationToken
    )
    {
        if (actor.Context.ActorId != request.PlayerId)
            throw new InvalidDataException("Player identity does not match the bound Actor.");
        if (request.Reconnect)
        {
            await actor
                .Context.BoundSession.Send(new ActionRejected(Rejection.NotSeated, 0))
                .Async();
            return;
        }
        var created = await spots
            .GetOrCreate(request.TableId, GameNames.TableType)
            .InMesh(GameNames.WorldMesh)
            .Async(cancellationToken);
        if (created.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Table creation rejected.");
        await actor.JoinFixedAsync(request, cancellationToken);
    }
}
