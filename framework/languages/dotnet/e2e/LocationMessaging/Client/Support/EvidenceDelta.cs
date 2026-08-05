namespace LocationMessaging.Client.Support;

internal static class EvidenceDelta
{
    public static int CountMatching(
        IReadOnlyCollection<string> after,
        IReadOnlyCollection<string> before,
        string prefix,
        string marker)
    {
        var beforeCount = before.Count(line => line.Contains(prefix, StringComparison.Ordinal)
                                               && line.Contains(marker, StringComparison.Ordinal));
        var afterCount = after.Count(line => line.Contains(prefix, StringComparison.Ordinal)
                                             && line.Contains(marker, StringComparison.Ordinal));
        return afterCount - beforeCount;
    }
}
