using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Table;

public sealed class TableEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure() =>
        Context.Handlers.AddActorPacket<EnterTableHandler, PlayerActor>(nameof(JoinTableRequest));

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
        var created = await spots
            .GetOrCreate(request.TableId, GameNames.TableType)
            .InMesh(GameNames.WorldMesh)
            .Async(cancellationToken);
        if (created.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Table creation rejected.");
        actor.Context.JoinSpot(request.TableId, request).Defer();
    }
}
