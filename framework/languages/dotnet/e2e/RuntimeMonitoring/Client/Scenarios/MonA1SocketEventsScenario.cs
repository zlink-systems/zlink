// Verifies Config 7 MON-A1 with two immutable public RouteMesh statuses.
using System.Diagnostics;
using RuntimeMonitoring.Client.Support;
using RuntimeMonitoring.Shared;
using Zlink.HttpClient;

namespace RuntimeMonitoring.Client.Scenarios;

internal static class MonA1SocketEventsScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        using var observer = ZLinkHttpClient.Create(options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(35))
            .Build();

        var baseline = await SnapshotAsync(observer);
        AssertBaseline(baseline);
        var evidenceBaseline =
            (await observer.Get("/evidence").Async<string[]>()).Body.Length;

        await using var serviceB = await EphemeralService.StartAsync(options, "svc-b");
        var ready = await WaitForPeerAsync(observer, "svc-b");
        var peer = ready.Peers.Single(candidate =>
            candidate.Rid.StartsWith("svc-b-", StringComparison.Ordinal)
            && candidate.State == "Ready");

        ZlinkStreamAssert.Ensure(
            ready.Sequence > baseline.Sequence
            && baseline.Peers.Length == 0
            && peer.UnavailableReason is null,
            "MON-A1 immutable status or ready peer state was incomplete.");

        var channel = ready.Channels.Single(candidate =>
            candidate.ChannelName == RuntimeMonitoringNames.Channel);
        ZlinkStreamAssert.Ensure(
            channel.IsReady && channel.ReadyTargetCount == 2,
            "MON-A1 channel status did not include both ready targets.");

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
        ZlinkStreamAssert.Ensure(
            evidence.Any(line =>
                line.Contains("identifier=zlink.runtime.mesh_node.peer_changed",
                    StringComparison.Ordinal)
                && line.Contains("routing=svc-b", StringComparison.Ordinal)
                && line.Contains("sequence=", StringComparison.Ordinal)),
            "MON-A1 typed peer event was not observed.");

        Console.WriteLine("scenario MON-A1 passed");
    }

    private static void AssertBaseline(MeshRuntimeSnapshotRes status)
    {
        var channel = status.Channels.Single(candidate =>
            candidate.ChannelName == RuntimeMonitoringNames.Channel);
        ZlinkStreamAssert.Ensure(
            status.MeshName == RuntimeMonitoringNames.Channel
            && status.State == "Ready"
            && status.IsReady
            && status.ReadyPeerCount == 0
            && status.Sequence > 0
            && status.ObservedAt != default
            && status.Peers.Length == 0
            && channel.IsReady
            && channel.ReadyTargetCount == 1
            && !status.Placement.IsAvailable,
            "MON-A1 baseline RouteMesh status was incomplete.");
    }

    private static async Task<MeshRuntimeSnapshotRes> SnapshotAsync(
        ZLinkHttpClient service)
        => (await service.Get(
                $"/runtime/snapshot/{RuntimeMonitoringNames.Channel}")
            .Async<MeshRuntimeSnapshotRes>()).Body;

    private static async Task<MeshRuntimeSnapshotRes> WaitForPeerAsync(
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
                    $"MON-A1 peer '{rid}' did not become ready.");
            await Task.Delay(TimeSpan.FromMilliseconds(50));
        }
    }
}
