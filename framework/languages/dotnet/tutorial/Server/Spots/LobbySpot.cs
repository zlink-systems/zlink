using Tutorial.Server.Actors;
using Zlink.Framework.Contracts.Spots;

namespace Tutorial.Server.Spots;

// --8<-- [start:entry-spot]
// Every new player lands here before joining a room, and returns here after
// leaving one. A node that hosts players registers exactly one of these.
public sealed class LobbySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot<Player>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    // Runs after the move is committed, on the side the player arrived at.
    public ValueTask OnJoinedActorAsync(Player player, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    // Runs on the side the player left. The player still exists elsewhere.
    public ValueTask OnLeaveActorAsync(Player player, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}
// --8<-- [end:entry-spot]
