using MyGame.Shared;

namespace MyGame.Match.Domain;

public sealed class MatchmakingState
{
    public Dictionary<string, List<QueueRequest>> Waiting { get; set; } =
        new(StringComparer.Ordinal);
    public long RoomSequence { get; set; }

    public void Enqueue(QueueRequest request)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(request.Mode);
        if (Waiting.Values.Any(queue => queue.Any(p => p.PlayerId == request.PlayerId)))
            return;
        if (!Waiting.TryGetValue(request.Mode, out var queue))
            Waiting.Add(request.Mode, queue = []);
        queue.Add(request);
    }

    public QueueRequest[] Pair()
    {
        foreach (var queue in Waiting.Values)
            if (queue.Count >= GameNames.PlayersPerRoom)
                return queue.Take(GameNames.PlayersPerRoom).ToArray();
        return [];
    }

    public string AllocateRoom() => GameNames.RoomPrefix + checked(++RoomSequence);

    public void Remove(QueueRequest[] pair)
    {
        var mode = pair[0].Mode;
        var queue = Waiting[mode];
        queue.RemoveRange(0, GameNames.PlayersPerRoom);
        if (queue.Count == 0)
            Waiting.Remove(mode);
    }
}
