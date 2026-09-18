using Tutorial.Shared;
using Zlink.Framework.Contracts.Spots;

namespace Tutorial.Server.Spots;

// Spot handlers live in their own classes and take the target room as the first
// argument. They are discovered by scanning the assembly that holds the room
// type, so registering them again in Configure() is rejected at startup.

// --8<-- [start:spot-handlers]
public sealed class PostChatHandler
    : IZLinkSpotPacketHandler<
        GameRoom, PostChat>
{
    public ValueTask HandleAsync(
        GameRoom room,
        PostChat message,
        CancellationToken cancellationToken)
    {
        var line = $"{message.PlayerId}: "
            + message.Text;
        room.Append(line);
        return ValueTask.CompletedTask;
    }
}

// The return value is the reply. This handler only reads.
public sealed class GetRoomStateHandler
    : IZLinkSpotRequestHandler<
        GameRoom, GetRoomState, RoomState>
{
    public ValueTask<RoomState> HandleAsync(
        GameRoom room,
        GetRoomState request,
        CancellationToken cancellationToken)
    {
        var chat = room.Chat.ToArray();
        var state = new RoomState(
            room.Title, chat);
        return ValueTask.FromResult(state);
    }
}
// --8<-- [end:spot-handlers]
