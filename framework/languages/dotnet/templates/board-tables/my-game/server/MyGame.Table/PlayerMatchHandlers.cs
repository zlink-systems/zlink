using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Table;

public sealed class EntryFindMatchHandler
    : IZLinkEntrySpotActorSendHandler<TableEntry, PlayerActor, FindMatch>
{
    public ValueTask HandleAsync(
        TableEntry spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        FindMatch message,
        CancellationToken ct
    ) => actor.FindAsync(ct);
}

public sealed class EntryCancelMatchHandler
    : IZLinkEntrySpotActorSendHandler<TableEntry, PlayerActor, CancelMatch>
{
    public ValueTask HandleAsync(
        TableEntry spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        CancelMatch message,
        CancellationToken ct
    ) => actor.CancelAsync(message.TicketId, MatchNames.Cancelled, ct);
}

public sealed class EntryMatchAssignedHandler
    : IZLinkEntrySpotActorSendHandler<TableEntry, PlayerActor, MatchAssigned>
{
    public ValueTask HandleAsync(
        TableEntry spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MatchAssigned message,
        CancellationToken ct
    )
    {
        actor.Assign(message);
        return ValueTask.CompletedTask;
    }
}

public sealed class EntryMatchCancelledHandler
    : IZLinkEntrySpotActorSendHandler<TableEntry, PlayerActor, MatchCancelled>
{
    public ValueTask HandleAsync(
        TableEntry spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MatchCancelled message,
        CancellationToken ct
    ) => actor.NotifyCancellationAsync(message);
}

public sealed class TableFindMatchHandler
    : IZLinkSpotActorSendHandler<TableSpot, PlayerActor, FindMatch>
{
    public ValueTask HandleAsync(
        TableSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        FindMatch message,
        CancellationToken ct
    ) => actor.FindAsync(ct);
}

public sealed class TableCancelMatchHandler
    : IZLinkSpotActorSendHandler<TableSpot, PlayerActor, CancelMatch>
{
    public ValueTask HandleAsync(
        TableSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        CancelMatch message,
        CancellationToken ct
    ) => actor.CancelAsync(message.TicketId, MatchNames.Cancelled, ct);
}

public sealed class TableMatchAssignedHandler
    : IZLinkSpotActorSendHandler<TableSpot, PlayerActor, MatchAssigned>
{
    public ValueTask HandleAsync(
        TableSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MatchAssigned message,
        CancellationToken ct
    )
    {
        actor.Assign(message);
        return ValueTask.CompletedTask;
    }
}

public sealed class TableMatchCancelledHandler
    : IZLinkSpotActorSendHandler<TableSpot, PlayerActor, MatchCancelled>
{
    public ValueTask HandleAsync(
        TableSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MatchCancelled message,
        CancellationToken ct
    ) => actor.NotifyCancellationAsync(message);
}
