using Bingo.Server.Play.Domain.Bingo;
using Bingo.Server.Play.Infrastructure.ZLink.Actors;
using Bingo.Shared.Contracts;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Notifications;

internal sealed record BingoRoomEvent(
    BingoRoomEventKind Kind,
    PlayerActor Recipient,
    BingoRoomState State,
    string? JoinedActorId = null,
    string? JoinedDisplayName = null,
    int Seat = -1,
    bool IsHost = false,
    int DrawnNumber = 0);