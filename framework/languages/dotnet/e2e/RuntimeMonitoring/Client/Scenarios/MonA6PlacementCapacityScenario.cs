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

        var actorA = await service.Post("/admin/actor/create/monitor-actor-a").AsyncRaw();
        ZlinkStreamAssert.Ensure(
            actorA.Status is >= 200 and < 300,
            "MON-A6 actor create did not succeed.");
        var subjectA = await service.Post("/admin/subject/create/monitor-subject-a").AsyncRaw();
        ZlinkStreamAssert.Ensure(
            subjectA.Status is >= 200 and < 300,
            "MON-A6 spot create did not succeed.");
        await WaitForPlacementAsync(service, 1, 1);

        var actorBRejected = await service.Post("/admin/actor/create/monitor-actor-b").AsyncRaw();
        ZlinkStreamAssert.Ensure(
            actorBRejected.Status >= 400,
            "MON-A6 actor capacity overflow was accepted.");
        var subjectBRejected = await service.Post("/admin/subject/create/monitor-subject-b").AsyncRaw();
        ZlinkStreamAssert.Ensure(
            subjectBRejected.Status >= 400,
            "MON-A6 spot capacity overflow was accepted.");

        var actorAClosed = await service.Post("/admin/actor/close/monitor-actor-a").AsyncRaw();
        ZlinkStreamAssert.Ensure(
            actorAClosed.Status is >= 200 and < 300,
            "MON-A6 actor close did not succeed.");
        var subjectAClosed = await service.Post("/admin/subject/close/monitor-subject-a").AsyncRaw();
        ZlinkStreamAssert.Ensure(
            subjectAClosed.Status is >= 200 and < 300,
            "MON-A6 spot close did not succeed.");
        await WaitForPlacementAsync(service, 0, 0);

        var actorB = await service.Post("/admin/actor/create/monitor-actor-b").AsyncRaw();
        ZlinkStreamAssert.Ensure(
            actorB.Status is >= 200 and < 300,
            "MON-A6 actor placement did not become available after close.");
        var subjectB = await service.Post("/admin/subject/create/monitor-subject-b").AsyncRaw();
        ZlinkStreamAssert.Ensure(
            subjectB.Status is >= 200 and < 300,
            "MON-A6 spot placement did not become available after close.");
        var final = await WaitForPlacementAsync(service, 1, 1);
        ZlinkStreamAssert.Ensure(
            final.Placement.IsAvailable,
            "MON-A6 placement remained unavailable after capacity was released.");

        Console.WriteLine("scenario MON-A6 passed");
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
