using MyGame.Shared;

namespace MyGame.Lobby.Application;

public interface IRoomProvisioner
{
    Task<MatchResponse> PrepareAsync(string roomId, CancellationToken ct);
}

// HTTP callbacks run concurrently. This lock owns only the application lobby queue.
public sealed class Matchmaker(IRoomProvisioner rooms)
{
    private readonly object gate = new();
    private readonly Dictionary<string, LinkedList<TaskCompletionSource<MatchResponse>>> queues = new(StringComparer.Ordinal);
    private readonly HashSet<string> participants = new(StringComparer.Ordinal);
    private long sequence;

    public async Task<MatchResponse> MatchAsync(MatchRequest request, CancellationToken ct)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(request.PlayerId);
        ArgumentException.ThrowIfNullOrWhiteSpace(request.Mode);
        TaskCompletionSource<MatchResponse>? partner = null;
        string? roomId = null;
        LinkedListNode<TaskCompletionSource<MatchResponse>>? pending = null;
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
                roomId = GameNames.RoomPrefix + checked(++sequence);
                queues.Remove(request.Mode);
            }
            else pending = queue.AddLast(new TaskCompletionSource<MatchResponse>(TaskCreationOptions.RunContinuationsAsynchronously));
        }
        try
        {
            if (partner is not null)
            {
                try
                {
                    var assignment = await rooms.PrepareAsync(roomId!, ct);
                    partner.TrySetResult(assignment);
                    return assignment;
                }
                catch (Exception error)
                {
                    partner.TrySetException(error);
                    throw;
                }
            }
            return await pending!.Value.Task.WaitAsync(ct);
        }
        finally
        {
            lock (gate)
            {
                participants.Remove(request.PlayerId);
                if (pending?.List is { } queue)
                {
                    queue.Remove(pending);
                    if (queue.Count == 0) queues.Remove(request.Mode);
                }
            }
        }
    }
}
