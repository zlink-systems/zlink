// Verifies Config 7 MON-A3 ChannelName readiness against actual selection.
using System.Diagnostics;
using RuntimeMonitoring.Client.Support;
using RuntimeMonitoring.Shared;
using Zlink.HttpClient;

namespace RuntimeMonitoring.Client.Scenarios;

internal static class MonA3SpotEventsScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        using var serviceA = ZLinkHttpClient.Create(options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(35)).Build();
        using var serviceB = ZLinkHttpClient.Create(options.ServiceBUrl)
            .Timeout(TimeSpan.FromSeconds(35)).Build();

        AssertChannel(await WaitForReadyTargetsAsync(serviceA, 1), 1);
        AssertChannel(await WaitForReadyTargetsAsync(serviceB, 1), 1);
        var evidenceBaseline =
            (await serviceA.Get("/evidence").Async<string[]>()).Body.Length;
        ZlinkStreamAssert.Ensure(
            await ObserveProviderAsync(serviceA, "svc-b", 8),
            "MON-A3 baseline selection never reached svc-b.");
        ZlinkStreamAssert.Ensure(
            await ObserveProviderAsync(serviceB, "svc-a", 8),
            "MON-A3 remote caller did not select svc-a.");

        await serviceB.Post("/admin/weight/exclude").AsyncRaw();
        AssertChannel(await WaitForReadyTargetsAsync(serviceA, 0), 0);
        AssertChannel(await WaitForReadyTargetsAsync(serviceB, 1), 1);
        await AssertNoSelectableTargetAsync(serviceA);

        var eventEvidence = (await serviceA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(
                [
                    "identifier=zlink.runtime.mesh_node.channel_changed",
                    $"channel={RuntimeMonitoringNames.Channel}"
                ],
                [],
                TimeoutMilliseconds: 3000,
                AfterIndex: evidenceBaseline))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            eventEvidence.Any(line =>
                line.Contains("identifier=zlink.runtime.mesh_node.channel_changed",
                    StringComparison.Ordinal)
                && line.Contains(
                    $"channel={RuntimeMonitoringNames.Channel}",
                    StringComparison.Ordinal)),
            "MON-A3 channel_changed event was missing.");

        await serviceB.Post("/admin/weight/include").AsyncRaw();
        AssertChannel(await WaitForReadyTargetsAsync(serviceA, 1), 1);
        AssertChannel(await WaitForReadyTargetsAsync(serviceB, 1), 1);
        ZlinkStreamAssert.Ensure(
            await ObserveProviderAsync(serviceA, "svc-b", 8),
            "MON-A3 restored svc-b did not become selectable.");
        ZlinkStreamAssert.Ensure(
            await ObserveProviderAsync(serviceB, "svc-a", 8),
            "MON-A3 restored remote caller did not select svc-a.");

        Console.WriteLine("scenario MON-A3 passed");
    }

    private static void AssertChannel(
        MeshRuntimeSnapshotRes status,
        int readyTargets)
    {
        var channel = status.Channels.Single(candidate =>
            candidate.ChannelName == RuntimeMonitoringNames.Channel);
        ZlinkStreamAssert.Ensure(
            channel.IsReady == readyTargets > 0
                && channel.ReadyTargetCount == readyTargets,
            $"MON-A3 channel status was ready={channel.ReadyTargetCount},"
            + $" isReady={channel.IsReady}.");
    }

    private static async Task<bool> ObserveProviderAsync(
        ZLinkHttpClient service,
        string expectedRid,
        int attempts)
    {
        for (var attempt = 0; attempt < attempts; attempt++)
        {
            var response = (await service.Post("/profile/request")
                .Body(new ProfileReq($"weight-{attempt}", $"mon-a3-{attempt}"))
                .Async<ProfileRes>()).Body;
            if (response.ProviderRid == expectedRid)
                return true;
        }
        return false;
    }

    private static async Task AssertNoSelectableTargetAsync(
        ZLinkHttpClient service)
    {
        var response = await service.Post("/profile/request")
            .Body(new ProfileReq("weight-0-unavailable", "mon-a3-unavailable"))
            .AsyncRaw();
        ZlinkStreamAssert.Ensure(
            response.Status == 500,
            $"MON-A3 expected a terminal no-target response, got HTTP {response.Status}.");
    }

    private static async Task<MeshRuntimeSnapshotRes> WaitForReadyTargetsAsync(
        ZLinkHttpClient service,
        int expected)
    {
        var elapsed = Stopwatch.StartNew();
        while (true)
        {
            var status = (await service.Get(
                    $"/runtime/snapshot/{RuntimeMonitoringNames.Channel}")
                .Async<MeshRuntimeSnapshotRes>()).Body;
            var channel = status.Channels.Single(candidate =>
                candidate.ChannelName == RuntimeMonitoringNames.Channel);
            if (channel.ReadyTargetCount == expected)
                return status;
            if (elapsed.Elapsed >= TimeSpan.FromSeconds(3))
                throw new InvalidOperationException(
                    "MON-A3 channel status did not reach the expected state.");
            await Task.Delay(TimeSpan.FromMilliseconds(50));
        }
    }
}
