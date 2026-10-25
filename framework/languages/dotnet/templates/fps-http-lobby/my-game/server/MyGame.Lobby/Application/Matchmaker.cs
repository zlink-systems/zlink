using MyGame.Shared;

namespace MyGame.Lobby.Application;

public interface IRoomProvisioner
{
    Task<MatchResponse> PrepareAsync(string roomId, string[] members, CancellationToken ct);
}

// HTTP callbacks are concurrent; this lock owns only the application lobby queue.
public sealed class Matchmaker(IRoomProvisioner rooms)
{
    private sealed record Pending(string PlayerId, TaskCompletionSource<MatchResponse> Completion);
    private readonly object gate = new();
    private readonly Dictionary<string, LinkedList<Pending>> queues = new(StringComparer.Ordinal);
    private readonly HashSet<string> participants = new(StringComparer.Ordinal);

    public async Task<MatchResponse> MatchAsync(MatchRequest request, CancellationToken ct)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(request.PlayerId);
        ArgumentException.ThrowIfNullOrWhiteSpace(request.Mode);
        Pending? partner = null;
        string? roomId = null;
        LinkedListNode<Pending>? pending = null;
        lock (gate)
        {
            if (!participants.Add(request.PlayerId))
                throw new ArgumentException("Player already has a pending match request.");
            if (!queues.TryGetValue(request.Mode, out var queue))
                queues.Add(request.Mode, queue = new());
            if (queue.First is { } first)
            {
                partner = first.Value;
                queue.RemoveFirst();
                roomId = GameNames.RoomPrefix + Guid.NewGuid().ToString("N");
                queues.Remove(request.Mode);
            }
            else
                pending = queue.AddLast(new Pending(request.PlayerId,
                    new TaskCompletionSource<MatchResponse>(TaskCreationOptions.RunContinuationsAsynchronously)));
        }
        try
        {
            if (partner is not null)
            {
                try
                {
                    var assignment = await rooms.PrepareAsync(roomId!, new[] { partner.PlayerId, request.PlayerId }, ct);
                    partner.Completion.TrySetResult(assignment);
                    return assignment;
                }
                catch (Exception error)
                {
                    partner.Completion.TrySetException(error);
                    throw;
                }
            }
            return await pending!.Value.Completion.Task.WaitAsync(ct);
        }
        finally
        {
            lock (gate)
            {
                participants.Remove(request.PlayerId);
                if (pending?.List is { } queue)
                {
                    queue.Remove(pending);
                    if (queue.Count == 0)
                        queues.Remove(request.Mode);
                }
            }
        }
    }
}
