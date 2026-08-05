namespace AutomaticTurnDispatch.Client.Support;

internal static class EvidenceOrder
{
    public static void ContainsInOrder(string[] evidence, string requestPrefix, string[] markers)
    {
        var requestLine = evidence.FirstOrDefault(line => line.Contains(requestPrefix, StringComparison.Ordinal));
        ZlinkStreamAssert.Ensure(requestLine is not null,
            $"No evidence found for request prefix '{requestPrefix}'.");
        var requestStart = requestLine!.IndexOf(requestPrefix, StringComparison.Ordinal);
        var requestEnd = requestLine.IndexOf('|', requestStart);
        var requestId = requestEnd < 0
            ? requestLine[requestStart..]
            : requestLine[requestStart..requestEnd];
        var cursor = -1;
        foreach (var marker in markers)
        {
            var index = Array.FindIndex(
                evidence,
                cursor + 1,
                line => line.Contains(requestId, StringComparison.Ordinal)
                        && line.Contains(marker, StringComparison.Ordinal));
            ZlinkStreamAssert.Ensure(index >= 0,
                $"Missing ordered marker '{marker}' for request '{requestId}'.");

            cursor = index;
        }
    }

    public static void ContainsExactRequestInOrder(string[] evidence, string requestId, string[] markers)
    {
        var cursor = -1;
        foreach (var marker in markers)
        {
            var index = Array.FindIndex(
                evidence,
                cursor + 1,
                line => line.Contains($"request={requestId}", StringComparison.Ordinal)
                        && line.Contains(marker, StringComparison.Ordinal));
            ZlinkStreamAssert.Ensure(index >= 0,
                $"Missing ordered marker '{marker}' for request '{requestId}'.");

            cursor = index;
        }
    }
}
