using Tutorial.Shared;
using Zlink.Framework.Contracts.Spots;

namespace Tutorial.Server.Spots;

// --8<-- [start:instance-spot-class]
// Unlike a room, a match queue is never created explicitly. The first message
// addressed to a queue id brings it into being and is then handled by it.
// Players do not join it as members; it only processes requests.
public sealed class MatchQueue(IZLinkInstanceSpotContext context) : IZLinkInstanceSpot
{
    private readonly List<string> _waiting = [];

    public IZLinkInstanceSpotContext Context { get; } = context;

    public int Waiting => _waiting.Count;

    public void Enqueue(string playerId) => _waiting.Add(playerId);
}

// --8<-- [end:instance-spot-class]

// --8<-- [start:instance-spot-handler]
// Handlers are written the same way as room handlers.
public sealed class JoinMatchQueueHandler
    : IZLinkSpotRequestHandler<MatchQueue, JoinMatchQueue, MatchQueueStatus>
{
    public ValueTask<MatchQueueStatus> HandleAsync(
        MatchQueue queue,
        JoinMatchQueue request,
        CancellationToken cancellationToken
    )
    {
        queue.Enqueue(request.PlayerId);
        return ValueTask.FromResult(new MatchQueueStatus(queue.Waiting));
    }
}
// --8<-- [end:instance-spot-handler]
