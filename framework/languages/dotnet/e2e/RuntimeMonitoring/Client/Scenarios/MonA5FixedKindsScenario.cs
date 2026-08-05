// Verifies Redis location health events and admitted-route continuity.
using System.Diagnostics;
using RuntimeMonitoring.Client.Support;
using RuntimeMonitoring.Shared;
using Zlink.HttpClient;

namespace RuntimeMonitoring.Client.Scenarios;

internal static class MonA5FixedKindsScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        using var service = ZLinkHttpClient.Create(options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(35))
            .Build();
        var baseline = await SnapshotAsync(service);
        var baselineEvidence =
            (await service.Get("/evidence").Async<string[]>()).Body.Length;
        ZlinkStreamAssert.Ensure(
            baseline.IsReady
            && baseline.Peers.Any(peer =>
                peer.Rid.StartsWith("svc-b-", StringComparison.Ordinal)
                && peer.State == "Ready"),
            "MON-A5 baseline RouteMesh status was not ready.");

        await RunDockerAsync("pause", options.RedisContainer);
        try
        {
            await WaitForLocationEventAsync(service, baselineEvidence, "degraded");
            var duringOutage = await SnapshotAsync(service);
            ZlinkStreamAssert.Ensure(
                duringOutage.IsReady
                && duringOutage.Channels.Single(channel =>
                    channel.ChannelName == RuntimeMonitoringNames.Channel).IsReady,
                "MON-A5 store outage incorrectly removed the admitted messaging path.");

            var request = await service.Post("/profile/request")
                .Body(new ProfileReq("during-outage", "mon-a5-during-outage"))
                .Async<ProfileRes>();
            ZlinkStreamAssert.Ensure(
                request.Body.Marker == "mon-a5-during-outage",
                "MON-A5 admitted messaging did not continue during the store outage.");
        }
        finally
        {
            await RunDockerAsync("unpause", options.RedisContainer);
        }

        await WaitForLocationEventAsync(service, baselineEvidence, "ready");
        var recovered = await SnapshotAsync(service);
        ZlinkStreamAssert.Ensure(
            recovered.IsReady
            && recovered.Peers.Any(peer =>
                peer.Rid.StartsWith("svc-b-", StringComparison.Ordinal)
                && peer.State == "Ready"),
            "MON-A5 ready topology did not remain available after store recovery.");

        Console.WriteLine("scenario MON-A5 passed");
    }

    private static async Task<MeshRuntimeSnapshotRes> SnapshotAsync(
        ZLinkHttpClient service)
        => (await service.Get(
                $"/runtime/snapshot/{RuntimeMonitoringNames.Channel}")
            .Async<MeshRuntimeSnapshotRes>()).Body;

    private static async Task WaitForLocationEventAsync(
        ZLinkHttpClient service,
        int afterIndex,
        string state)
    {
        var evidence = (await service.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(
                [
                    $"source={RuntimeMonitoringNames.LocationRuntimeSource}",
                    "identifier=zlink.runtime.location.store_changed",
                    $"reason={state}"
                ],
                [],
                TimeoutMilliseconds: 3000,
                AfterIndex: afterIndex))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.Any(line =>
                line.Contains(
                    "identifier=zlink.runtime.location.store_changed",
                    StringComparison.Ordinal)
                && line.Contains($"reason={state}", StringComparison.Ordinal)),
            $"MON-A5 '{state}' location event was missing.");
    }

    private static async Task RunDockerAsync(string verb, string container)
    {
        var start = new ProcessStartInfo("docker")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        start.ArgumentList.Add(verb);
        start.ArgumentList.Add(container);
        using var process = Process.Start(start)
                            ?? throw new InvalidOperationException(
                                $"Failed to run docker {verb}.");
        await process.WaitForExitAsync();
        if (process.ExitCode == 0)
            return;
        var error = await process.StandardError.ReadToEndAsync();
        throw new InvalidOperationException(
            $"docker {verb} {container} failed: {error}");
    }
}
