using Bingo.Server.Play.Domain.Bingo;
using Bingo.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Errors;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Notifications;

internal sealed class BingoNotificationPublisher
{
    private readonly ILogger<BingoNotificationPublisher> _logger;

    public BingoNotificationPublisher(ILogger<BingoNotificationPublisher> logger)
    {
        _logger = logger;
    }

    public async ValueTask PublishAsync(
        IReadOnlyList<BingoRoomEvent> events,
        CancellationToken cancellationToken
    )
    {
        foreach (var roomEvent in events)
            await PublishAsync(roomEvent, cancellationToken);
    }

    private async ValueTask PublishAsync(
        BingoRoomEvent roomEvent,
        CancellationToken cancellationToken
    )
    {
        switch (roomEvent.Kind)
        {
            case BingoRoomEventKind.PlayerJoined:
            {
                var notification = new PlayerJoinedNotify
                {
                    RoomId = roomEvent.State.RoomId,
                    ActorId =
                        roomEvent.JoinedActorId
                        ?? throw new InvalidOperationException("Joined actor is required."),
                    DisplayName =
                        roomEvent.JoinedDisplayName
                        ?? throw new InvalidOperationException("Joined display name is required."),
                    Seat = roomEvent.Seat,
                    IsHost = roomEvent.IsHost,
                    State = roomEvent.State,
                };
                try
                {
                    await roomEvent.Recipient.Context.BoundSession.Send(notification).Async();
                }
                catch (ZLinkFrameworkException error)
                {
                    _logger.LogWarning(
                        error,
                        "bingo-player-joined-notify-failed room={RoomId} recipient={RecipientActorId}",
                        roomEvent.State.RoomId,
                        roomEvent.Recipient.ActorId
                    );
                }
                break;
            }
            case BingoRoomEventKind.GameStarted:
                await roomEvent
                    .Recipient.Context.BoundSession.Send(
                        new BingoGameStartedNotify { State = roomEvent.State }
                    )
                    .Async();
                break;
            // --8<-- [start:doc-bingo-bound-push]
            case BingoRoomEventKind.NumberDrawn:
                await roomEvent
                    .Recipient.Context.BoundSession.Send(
                        new BingoNumberDrawnNotify
                        {
                            RoomId = roomEvent.State.RoomId,
                            DrawSeq = roomEvent.State.DrawSeq,
                            Number = roomEvent.DrawnNumber,
                            State = roomEvent.State,
                        }
                    )
                    .Async();
                break;
            // --8<-- [end:doc-bingo-bound-push]
            case BingoRoomEventKind.GameEnded:
                await roomEvent
                    .Recipient.Context.BoundSession.Send(
                        new BingoGameEndedNotify { State = roomEvent.State }
                    )
                    .Async();
                break;
            default:
                throw new InvalidOperationException($"Unsupported event kind {roomEvent.Kind}.");
        }
    }
}
