// Verifies Config 7 MON-A2 peer removal and same-RID replacement.
using System.Diagnostics;
using RuntimeMonitoring.Client.Support;
using RuntimeMonitoring.Shared;
using Zlink.HttpClient;

namespace RuntimeMonitoring.Client.Scenarios;

internal static class MonA2RegistryEventsScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        using var observer = ZLinkHttpClient.Create(options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(35))
            .Build();
        var evidenceBaseline =
            (await observer.Get("/evidence").Async<string[]>()).Body.Length;
        ulong firstReadySequence;

        await using (var first = await EphemeralService.StartAsync(options, "svc-b"))
        {
            var firstReady = await WaitForReadyPeerAsync(observer, "svc-b");
            firstReadySequence = firstReady.Sequence;
            var peer = firstReady.Peers.Single(candidate =>
                candidate.Rid.StartsWith("svc-b-", StringComparison.Ordinal)
                && candidate.State == "Ready");
            ZlinkStreamAssert.Ensure(
                peer.UnavailableReason is null,
                "MON-A2 first peer was not reported as ready.");
        }

        var removed = await WaitUntilPeerMissingAsync(observer, "svc-b");
        ZlinkStreamAssert.Ensure(
            removed.Sequence > firstReadySequence,
            "MON-A2 removal did not advance the status sequence.");

        await using var replacement = await EphemeralService.StartAsync(options, "svc-b");
        var replacementReady = await WaitForReadyPeerAsync(observer, "svc-b");
        ZlinkStreamAssert.Ensure(
            replacementReady.Sequence > removed.Sequence
            && replacementReady.Peers.Count(peer =>
                peer.Rid.StartsWith("svc-b-", StringComparison.Ordinal)
                && peer.State == "Ready") == 1,
            "MON-A2 replacement did not publish one ready peer with the same RID.");

        var evidence = (await observer.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(
                [
                    "identifier=zlink.runtime.mesh_node.peer_changed",
                    "routing=svc-b",
                    "kind=ConnectionReady"
                ],
                [],
                TimeoutMilliseconds: 3000,
                AfterIndex: evidenceBaseline))
            .Async<string[]>()).Body;
        var sequences = evidence
            .Where(line =>
                line.Contains("identifier=zlink.runtime.mesh_node.peer_changed",
                    StringComparison.Ordinal)
                && line.Contains(
                    $"source={RuntimeMonitoringNames.Channel}",
                    StringComparison.Ordinal)
                && line.Contains("routing=svc-b", StringComparison.Ordinal))
            .Select(ParseSequence)
            .Where(static sequence => sequence > 0)
            .ToArray();
        ZlinkStreamAssert.Ensure(
            sequences.Length >= 3
            && sequences.Zip(sequences.Skip(1), static (left, right) => right > left)
                .All(static increasing => increasing),
            "MON-A2 peer event sequence was not strictly increasing.");

        Console.WriteLine("scenario MON-A2 passed");
    }

    private static async Task<MeshRuntimeSnapshotRes> SnapshotAsync(
        ZLinkHttpClient service)
        => (await service.Get(
                $"/runtime/snapshot/{RuntimeMonitoringNames.Channel}")
            .Async<MeshRuntimeSnapshotRes>()).Body;

    private static async Task<MeshRuntimeSnapshotRes> WaitForReadyPeerAsync(
        ZLinkHttpClient service,
        string rid)
    {
        var elapsed = Stopwatch.StartNew();
        while (true)
        {
            var status = await SnapshotAsync(service);
            if (status.Peers.Any(peer =>
                    peer.Rid.StartsWith($"{rid}-", StringComparison.Ordinal)
                    && peer.State == "Ready"))
                return status;
            if (elapsed.Elapsed >= TimeSpan.FromSeconds(3))
                throw new InvalidOperationException(
                    $"MON-A2 peer '{rid}' did not become ready.");
            await Task.Delay(TimeSpan.FromMilliseconds(50));
        }
    }

    private static async Task<MeshRuntimeSnapshotRes> WaitUntilPeerMissingAsync(
        ZLinkHttpClient service,
        string rid)
    {
        var elapsed = Stopwatch.StartNew();
        while (true)
        {
            var status = await SnapshotAsync(service);
            if (!status.Peers.Any(peer =>
                    peer.Rid.StartsWith($"{rid}-", StringComparison.Ordinal)
                    && peer.State == "Ready"))
                return status;
            if (elapsed.Elapsed >= TimeSpan.FromSeconds(3))
                throw new InvalidOperationException(
                    $"MON-A2 peer '{rid}' remained ready.");
            await Task.Delay(TimeSpan.FromMilliseconds(50));
        }
    }

    private static ulong ParseSequence(string line)
    {
        const string prefix = "|sequence=";
        var start = line.IndexOf(prefix, StringComparison.Ordinal);
        if (start < 0)
            return 0;
        start += prefix.Length;
        var end = line.IndexOf('|', start);
        var text = end < 0 ? line[start..] : line[start..end];
        return ulong.TryParse(text, out var value) ? value : 0;
    }
}
