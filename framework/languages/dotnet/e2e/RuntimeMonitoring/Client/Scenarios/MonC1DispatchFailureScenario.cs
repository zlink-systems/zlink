// Verifies MON-C1 claim progress and observer isolation behavior.
using RuntimeMonitoring.Client.Support;
using RuntimeMonitoring.Shared;
using Zlink.HttpClient;

namespace RuntimeMonitoring.Client.Scenarios;

internal static class MonC1DispatchFailureScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        using var serviceA = ZLinkHttpClient.Create(options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(35))
            .Build();
        using var serviceB = ZLinkHttpClient.Create(options.ServiceBUrl)
            .Timeout(TimeSpan.FromSeconds(35))
            .Build();

        await serviceA.Post(
                $"/runtime/observer/start/{RuntimeMonitoringNames.SpotChannel}")
            .Async<ObserverIsolationStatusRes>();
        await serviceA.Post("/admin/application-gate/reset").Async<object>();
        await serviceB.Post("/admin/spot-weight/exclude").Async<object>();

        var evidenceBaseline =
            (await serviceA.Get("/evidence").Async<string[]>()).Body.Length;
        var blockedRequest = serviceA.Post("/spot/profile/request")
            .Body(new ProfileReq("application-gate", "mon-c1-gated"))
            .Timeout(TimeSpan.FromSeconds(30))
            .Async<ProfileRes>();
        await serviceA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(
                ["application-gate-enter|rid=svc-a|marker=mon-c1-gated"],
                [],
                TimeoutMilliseconds: 3000,
                AfterIndex: evidenceBaseline))
            .Async<string[]>();

        var before = (await serviceA.Get(
                $"/runtime/snapshot/{RuntimeMonitoringNames.SpotChannel}")
            .Async<MeshRuntimeSnapshotRes>()).Body;
        ZlinkStreamAssert.Ensure(
            before.IsReady && before.Placement.IsAvailable,
            "MON-C1 RouteMesh readiness regressed while an application handler was blocked.");
        ZlinkStreamAssert.Ensure(
            !blockedRequest.IsCompleted,
            "MON-C1 application gate did not hold the request handler.");

        await serviceB.Post("/admin/spot-weight/include").Async<object>();
        await serviceA.Post("/admin/spot-weight/exclude").Async<object>();
        var terminalReply = (await serviceB.Post("/spot/profile/request")
            .Body(new ProfileReq("parallel", "mon-c1-terminal"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            terminalReply.ProviderRid == "svc-b"
            && terminalReply.Marker == "mon-c1-terminal",
            "MON-C1 separate request did not complete while the application gate was held.");
        ZlinkStreamAssert.Ensure(
            !blockedRequest.IsCompleted,
            "MON-C1 application gate was released before the separate request completed.");

        for (var cycle = 0; cycle < 8; cycle++)
        {
            await serviceB.Post("/admin/spot-weight/exclude").Async<object>();
            _ = await serviceA.Get(
                    $"/runtime/snapshot/{RuntimeMonitoringNames.SpotChannel}")
                .Async<MeshRuntimeSnapshotRes>();
            await Task.Delay(125);
            await serviceB.Post("/admin/spot-weight/include").Async<object>();
            _ = await serviceA.Get(
                    $"/runtime/snapshot/{RuntimeMonitoringNames.SpotChannel}")
                .Async<MeshRuntimeSnapshotRes>();
            await Task.Delay(125);
        }

        var active = (await serviceA.Get(
                $"/runtime/snapshot/{RuntimeMonitoringNames.SpotChannel}")
            .Async<MeshRuntimeSnapshotRes>()).Body;
        ZlinkStreamAssert.Ensure(
            active.IsReady && active.Sequence >= before.Sequence,
            "MON-C1 runtime status did not progress with the application gate held.");

        var beforeRelease = await WaitForObserverAsync(
            serviceA,
            status => status.NormalEventCount >= 2,
            "MON-C1 normal observer did not progress.");
        ZlinkStreamAssert.Ensure(
            !beforeRelease.SlowConsumerReleased
            && beforeRelease.NormalLatestSequence > 0,
            "MON-C1 slow observer blocked the normal observer.");

        await serviceA.Post("/runtime/observer/release")
            .Async<ObserverIsolationStatusRes>();
        var afterRelease = await WaitForObserverAsync(
            serviceA,
            status => status.SlowConsumerFailed
                      && status.SlowLatestSequence > 0,
            "MON-C1 slow consumer failure was not isolated.");
        ZlinkStreamAssert.Ensure(
            afterRelease.NormalLatestSequence
            >= beforeRelease.NormalLatestSequence,
            "MON-C1 normal observer regressed after slow consumer failure.");

        var resynced = (await serviceA.Get(
                $"/runtime/snapshot/{RuntimeMonitoringNames.SpotChannel}")
            .Async<MeshRuntimeSnapshotRes>()).Body;
        ZlinkStreamAssert.Ensure(
            resynced.Sequence >= afterRelease.SlowLatestSequence
            && resynced.Channels.Any(channel =>
                channel.ChannelName == RuntimeMonitoringNames.SpotChannel),
            "MON-C1 snapshot resync did not retain the latest channel state.");

        await serviceA.Post("/admin/application-gate/release").Async<object>();
        var gatedReply = (await blockedRequest).Body;
        ZlinkStreamAssert.Ensure(
            gatedReply.ProviderRid == "svc-a"
            && gatedReply.Marker == "mon-c1-gated",
            "MON-C1 gated request did not complete after release.");

        var followUp = (await serviceB.Post("/spot/profile/request")
            .Body(new ProfileReq("recovery", "mon-c1-recovery"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            followUp.Value == "profile:recovery",
            "MON-C1 messaging stopped after the observer consumer failure.");

        await serviceA.Post("/admin/spot-weight/include").Async<object>();
        Console.WriteLine("scenario MON-C1 passed");
    }

    private static async Task<ObserverIsolationStatusRes> WaitForObserverAsync(
        ZLinkHttpClient service,
        Func<ObserverIsolationStatusRes, bool> predicate,
        string failureMessage)
    {
        for (var attempt = 0; attempt < 100; attempt++)
        {
            var status = (await service.Get("/runtime/observer/status")
                .Async<ObserverIsolationStatusRes>()).Body;
            if (predicate(status))
                return status;
            await Task.Delay(100);
        }
        throw new InvalidOperationException(failureMessage);
    }
}
