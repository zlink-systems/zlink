using Tutorial.Shared;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace Tutorial.Server.Spots;

// --8<-- [start:spot-class]
// A room owns its own state and is addressed by a global SpotId. Messages sent
// to one room run one at a time, so the fields below need no synchronization.
public sealed class GameRoom(
    IZLinkSpotContext context) : IZLinkSpot
{
    private readonly List<string> _chat = [];

    private string _title = "untitled";

    public IZLinkSpotContext Context { get; }
        = context;

    public string Title => _title;

    public IReadOnlyList<string> Chat => _chat;

    // Runs before the room accepts any message. Rejecting here means the create
    // call fails and no room exists. Omit this method to accept every request.
    public ValueTask<ZLinkSpotCreateResponse>
        OnCreateAsync(
            ZLinkMessage request,
            CancellationToken cancellationToken)
    {
        var room = request.Decode<OpenRoom>();
        _title = room.Title;
        return ValueTask.FromResult(
            ZLinkSpotCreateResponse.Accept());
    }

    public void Append(string line)
        => _chat.Add(line);
}
// --8<-- [end:spot-class]
