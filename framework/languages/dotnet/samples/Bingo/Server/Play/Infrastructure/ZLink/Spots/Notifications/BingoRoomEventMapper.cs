using Bingo.Server.Play.Domain.Bingo;
using Bingo.Server.Play.Infrastructure.ZLink.Actors;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Notifications;

internal sealed class BingoRoomEventMapper
{
    public IReadOnlyList<BingoRoomEvent> Map(
        IReadOnlyList<BingoGameEvent> events,
        IReadOnlyDictionary<string, PlayerActor> actors)
    {
        return events.Select(gameEvent => Map(gameEvent, actors)).ToArray();
    }

    private static BingoRoomEvent Map(
        BingoGameEvent gameEvent,
        IReadOnlyDictionary<string, PlayerActor> actors)
    {
        if (!actors.TryGetValue(gameEvent.RecipientActorId, out var recipient))
            throw new InvalidOperationException($"Recipient actor '{gameEvent.RecipientActorId}' is not joined.");

        return new BingoRoomEvent(
            gameEvent.Kind,
            recipient,
            gameEvent.State,
            gameEvent.JoinedActorId,
            gameEvent.JoinedDisplayName,
            gameEvent.Seat,
            gameEvent.IsHost,
            gameEvent.DrawnNumber);
    }
}