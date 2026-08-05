namespace PubSub.Client.Support;

internal static class Evidence
{
    public static bool IsEvent(string line, string runId, string topic)
    {
        return line.Contains("event|", StringComparison.Ordinal)
               && line.Contains($"run={runId}", StringComparison.Ordinal)
               && line.Contains($"topic={topic}", StringComparison.Ordinal);
    }

    public static bool IsIgnored(string line, string runId, string topic)
    {
        return line.Contains("ignored|", StringComparison.Ordinal)
               && line.Contains($"run={runId}", StringComparison.Ordinal)
               && line.Contains($"topic={topic}", StringComparison.Ordinal);
    }

    public static int ExtractInt(string line, string key)
    {
        var marker = key + "=";
        var start = line.IndexOf(marker, StringComparison.Ordinal);
        if (start < 0) return 0;

        start += marker.Length;
        var end = line.IndexOf('|', start);
        var value = end < 0 ? line[start..] : line[start..end];
        return int.Parse(value);
    }

    public static IReadOnlyList<int> CommonContiguousSequence(
        string[][] snapshots,
        string runId,
        string topic,
        int min,
        int max)
    {
        var common = snapshots
            .Select(lines => lines
                .Where(line => IsEvent(line, runId, topic))
                .Select(line => ExtractInt(line, "seq"))
                .Where(seq => seq >= min && seq <= max)
                .ToHashSet())
            .Aggregate((left, right) =>
            {
                left.IntersectWith(right);
                return left;
            });
        var sorted = common.Order().ToArray();
        var best = new List<int>();
        var current = new List<int>();
        foreach (var seq in sorted)
            if (current.Count == 0 || current[^1] + 1 == seq)
            {
                current.Add(seq);
            }
            else
            {
                if (current.Count > best.Count) best = [.. current];

                current = [seq];
            }

        if (current.Count > best.Count) best = current;

        return best;
    }
}