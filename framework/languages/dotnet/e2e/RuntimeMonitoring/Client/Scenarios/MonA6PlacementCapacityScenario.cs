// Verifies public placement counts and capacity outcomes.
using System.Diagnostics;
using RuntimeMonitoring.Client.Support;
using RuntimeMonitoring.Shared;
using Zlink.HttpClient;

namespace RuntimeMonitoring.Client.Scenarios;

internal static class MonA6PlacementCapacityScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        using var service = ZLinkHttpClient.Create(options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(35))
            .Build();

        await WaitForPlacementAsync(service, 0, 0);

        await ExpectSuccessAsync(
            service,
            "/admin/actor/create/monitor-actor-a",
            "MON-A6 actor create did not succeed.");
        await ExpectSuccessAsync(
            service,
            "/admin/subject/create/monitor-subject-a",
            "MON-A6 spot create did not succeed.");
        await WaitForPlacementAsync(service, 1, 1);

        await ExpectFailureAsync(
            service,
            "/admin/actor/create/monitor-actor-b",
            "MON-A6 actor capacity overflow was accepted.");
        await ExpectFailureAsync(
            service,
            "/admin/subject/create/monitor-subject-b",
            "MON-A6 spot capacity overflow was accepted.");

        await ExpectSuccessAsync(
            service,
            "/admin/actor/close/monitor-actor-a",
            "MON-A6 actor close did not succeed.");
        await ExpectSuccessAsync(
            service,
            "/admin/subject/close/monitor-subject-a",
            "MON-A6 spot close did not succeed.");
        await WaitForPlacementAsync(service, 0, 0);

        await ExpectSuccessAsync(
            service,
            "/admin/actor/create/monitor-actor-b",
            "MON-A6 actor placement did not become available after close.");
        await ExpectSuccessAsync(
            service,
            "/admin/subject/create/monitor-subject-b",
            "MON-A6 spot placement did not become available after close.");
        var final = await WaitForPlacementAsync(service, 1, 1);
        ZlinkStreamAssert.Ensure(
            final.Placement.IsAvailable,
            "MON-A6 placement remained unavailable after capacity was released.");

        Console.WriteLine("scenario MON-A6 passed");
    }

    private static async Task ExpectSuccessAsync(
        ZLinkHttpClient service,
        string path,
        string message)
    {
        var response = await service.Post(path).AsyncRaw();
        ZlinkStreamAssert.Ensure(response.Status is >= 200 and < 300, message);
    }

    private static async Task ExpectFailureAsync(
        ZLinkHttpClient service,
        string path,
        string message)
    {
        var response = await service.Post(path).AsyncRaw();
        ZlinkStreamAssert.Ensure(response.Status >= 400, message);
    }

    private static async Task<MeshRuntimeSnapshotRes> WaitForPlacementAsync(
        ZLinkHttpClient service,
        int actorCount,
        int spotCount)
    {
        var elapsed = Stopwatch.StartNew();
        MeshRuntimeSnapshotRes? last = null;
        while (true)
        {
            var snapshot = (await service.Get(
                    $"/runtime/snapshot/{RuntimeMonitoringNames.SpotChannel}")
                .Async<MeshRuntimeSnapshotRes>()).Body;
            last = snapshot;
            if (snapshot.Placement.ActiveActorCount == actorCount
                && snapshot.Placement.ActiveSpotCount == spotCount)
                return snapshot;
            if (elapsed.Elapsed >= TimeSpan.FromSeconds(15))
                throw new InvalidOperationException(
                    $"MON-A6 placement did not reach actors={actorCount}, spots={spotCount}; "
                    + $"actualActors={last.Placement.ActiveActorCount}, "
                    + $"actualSpots={last.Placement.ActiveSpotCount}, "
                    + $"available={last.Placement.IsAvailable}, "
                    + $"reason={last.Placement.UnavailableReason}.");
            await Task.Delay(TimeSpan.FromMilliseconds(50));
        }
    }
}
