// Verifies MON-B1/B2 publish delivery without publish-specific monitoring.
using System.Diagnostics;
using System.Reflection;
using System.Text;
using RuntimeMonitoring.Client.Support;
using RuntimeMonitoring.Shared;
using Zlink.Framework.Contracts.Configuration;
using Zlink.HttpClient;

namespace RuntimeMonitoring.Client.Scenarios;

internal static class MonBPublishMonitoringAbsenceScenario
{
    private static readonly string[] ForbiddenNames =
    [
        "ZLinkLogicalMulticastSnapshot",
        "RemoteSnapshotCount",
        "RemoteAdmittedCount",
        "RemoteDroppedCount",
        "RemoteUnreachableCount",
        "LocalSnapshotCount",
        "LocalAdmittedCount",
        "LocalDroppedCount",
        "zlink.mesh_node.multicast.submits",
        "zlink.mesh_node.multicast.targets",
        "zlink.mesh_node.multicast.pending",
        "zlink.mesh_node.multicast.backpressures",
        "zlink.mesh_node.multicast.drops",
        "zlink.runtime.mesh_node.multicast_backpressured",
        "zlink.runtime.mesh_node.multicast_dropped"
    ];

    public static Task RunZeroTargetAsync(ClientOptions options) =>
        RunAsync(options, "MON-B1", "monitor.dynamic", createSubject: true);

    public static Task RunLocalTargetAsync(ClientOptions options) =>
        RunAsync(options, "MON-B2", "monitor.dynamic", createSubject: true);

    private static async Task RunAsync(
        ClientOptions options,
        string scenario,
        string topic,
        bool createSubject)
    {
        AssertPublicContractShape();

        using var service = ZLinkHttpClient.Create(options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(10))
            .Build();
        using var target = ZLinkHttpClient.Create(
                scenario == "MON-B1" ? options.ServiceBUrl : options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(35))
            .Build();
        using var accepted = scenario == "MON-B1"
            ? ZLinkHttpClient.Create(options.ServiceCUrl).Timeout(TimeSpan.FromSeconds(35)).Build()
            : null;
        var evidenceStart = (await target.Get("/evidence").Async<string[]>()).Body.Length;
        var acceptedEvidenceStart = accepted is null
            ? 0
            : (await accepted.Get("/evidence").Async<string[]>()).Body.Length;

        if (createSubject)
        {
            await target.Post("/admin/application-gate/reset").Async<object>();
            EnsureSuccess(
                await target.Post("/admin/subject/create/monitor-blocked").AsyncRaw(),
                $"{scenario} blocked subject creation");
            if (accepted is null)
            {
                EnsureSuccess(
                    await target.Post("/admin/subject/create/monitor-free").AsyncRaw(),
                    $"{scenario} free subject creation");
            }
            else
            {
                EnsureSuccess(
                    await accepted.Post("/admin/subject/create/monitor-free").AsyncRaw(),
                    $"{scenario} remote accepted subject creation");
            }
            if (scenario == "MON-B1")
                await WaitForReadyChannelAsync(service);
            if (scenario == "MON-B1")
            {
                await Task.Delay(250);
                EnsureSuccess(
                    await service.Post("/spot/publish/monitor.blocker")
                        .Body(new ProfileEvent(new string('b', 1024), "mon-b1-blocker"))
                        .AsyncRaw(),
                    "MON-B1 blocker publish");
                await WaitForEvidenceAsync(
                    target,
                    "application-gate-enter|spot=monitor-blocked|topic=monitor.blocker",
                    evidenceStart);
                await WaitForApplicationReceivePausedAsync(target);
            }
        }

        var topologyBeforePublish = (await service.Get(
                $"/runtime/snapshot/{RuntimeMonitoringNames.SpotChannel}")
            .Async<MeshRuntimeSnapshotRes>()).Body;

        EnsureSuccess(
            await service.Post($"/spot/publish/{topic}")
                .Body(new ProfileEvent("publish", scenario.ToLowerInvariant()))
                .AsyncRaw(),
            $"{scenario} publish");

        if (createSubject)
        {
            if (accepted is not null)
                await WaitForMarkerAsync(accepted, "monitor-free", topic, scenario, acceptedEvidenceStart);
            EnsureSuccess(
                await target.Post("/admin/application-gate/release").AsyncRaw(),
                $"{scenario} gate release");
            await WaitForMarkerAsync(target, "monitor-blocked", topic, scenario, evidenceStart);
        }

        var snapshotResponse = await service.Get(
                $"/runtime/snapshot/{RuntimeMonitoringNames.SpotChannel}")
            .AsyncRaw();
        EnsureSuccess(snapshotResponse, $"{scenario} snapshot");
        var snapshotJson = snapshotResponse.Body;
        ZlinkStreamAssert.Ensure(
            snapshotJson.Contains("\"peers\"", StringComparison.Ordinal)
            && snapshotJson.Contains("\"channels\"", StringComparison.Ordinal)
            && snapshotJson.Contains("\"placement\"", StringComparison.Ordinal),
            $"{scenario} generic RouteMesh monitoring fields are missing: {snapshotJson}");
        AssertForbiddenTextAbsent(snapshotJson, $"{scenario} snapshot");
        var topologyAfterPublish = (await service.Get(
                $"/runtime/snapshot/{RuntimeMonitoringNames.SpotChannel}")
            .Async<MeshRuntimeSnapshotRes>()).Body;
        AssertTopologyUnchanged(topologyBeforePublish, topologyAfterPublish, scenario);

        var evidence = (await target.Get("/evidence").Async<string[]>()).Body
            .Skip(evidenceStart)
            .ToArray();
        if (createSubject)
        {
            ZlinkStreamAssert.Ensure(
                evidence.Count(line => line.Contains(
                    $"logical-publish|spot=monitor-blocked|topic={topic}|marker={scenario.ToLowerInvariant()}",
                    StringComparison.Ordinal)) == 1,
                $"{scenario} blocked target did not process the event exactly once.");
            if (accepted is not null)
            {
                var acceptedEvidence = (await accepted.Get("/evidence").Async<string[]>()).Body
                    .Skip(acceptedEvidenceStart)
                    .ToArray();
                ZlinkStreamAssert.Ensure(
                    acceptedEvidence.Count(line => line.Contains(
                        $"logical-publish|spot=monitor-free|topic={topic}|marker={scenario.ToLowerInvariant()}",
                        StringComparison.Ordinal)) == 1,
                    $"{scenario} accepted target did not process the event exactly once.");
            }
        }
        AssertForbiddenTextAbsent(string.Join('\n', evidence), $"{scenario} events");

        Console.WriteLine($"scenario {scenario} passed");
    }

    private static void EnsureSuccess(RawHttpResponse response, string operation) =>
        ZlinkStreamAssert.Ensure(
            response.Status is >= 200 and < 300,
            $"{operation} failed with HTTP status {response.Status}: {response.Body}");

    private static async Task WaitForMarkerAsync(
        ZLinkHttpClient service,
        string spotId,
        string topic,
        string scenario,
        int afterIndex)
    {
        var marker = $"logical-publish|spot={spotId}|topic={topic}|marker={scenario.ToLowerInvariant()}";
        var elapsed = Stopwatch.StartNew();
        while (elapsed.Elapsed < TimeSpan.FromSeconds(5))
        {
            var lines = (await service.Get("/evidence").Async<string[]>()).Body;
            if (lines.Skip(afterIndex).Any(line => line.Contains(marker, StringComparison.Ordinal)))
                return;
            await Task.Delay(50);
        }
        throw new InvalidOperationException(
            $"{scenario} local subscriber did not process the event.");
    }

    private static async Task WaitForEvidenceAsync(
        ZLinkHttpClient service,
        string expected,
        int afterIndex)
    {
        var elapsed = Stopwatch.StartNew();
        while (elapsed.Elapsed < TimeSpan.FromSeconds(10))
        {
            var lines = (await service.Get("/evidence").Async<string[]>()).Body;
            if (lines.Skip(afterIndex).Any(line =>
                    line.Contains(expected, StringComparison.Ordinal)))
                return;
            await Task.Delay(50);
        }
        throw new InvalidOperationException(
            $"Evidence '{expected}' was not observed.");
    }

    private static async Task WaitForApplicationReceivePausedAsync(
        ZLinkHttpClient service)
    {
        var elapsed = Stopwatch.StartNew();
        while (elapsed.Elapsed < TimeSpan.FromSeconds(10))
        {
            var status = (await service.Get("/runtime/host/status")
                .Async<HostRuntimeSnapshotRes>()).Body;
            if (status.ApplicationReceivePaused)
                return;
            await Task.Delay(50);
        }
        throw new InvalidOperationException(
            "MON-B1 blocked target did not report ApplicationReceivePaused.");
    }

    private static async Task WaitForReadyChannelAsync(ZLinkHttpClient service)
    {
        var elapsed = Stopwatch.StartNew();
        while (elapsed.Elapsed < TimeSpan.FromSeconds(10))
        {
            var snapshot = (await service.Get(
                    $"/runtime/snapshot/{RuntimeMonitoringNames.SpotChannel}")
                .Async<MeshRuntimeSnapshotRes>()).Body;
            var channel = snapshot.Channels.Single(candidate =>
                candidate.ChannelName == RuntimeMonitoringNames.SpotChannel);
            if (channel.IsReady && channel.ReadyTargetCount >= 2)
                return;
            await Task.Delay(50);
        }
        throw new InvalidOperationException(
            "MON-B1 remote target channel did not become ready.");
    }

    private static void AssertPublicContractShape()
    {
        var snapshot = typeof(ZLinkRouteMeshStatus);
        var runtimeEvent = typeof(ZLinkPeerStatus);
        var assembly = snapshot.Assembly;

        ZlinkStreamAssert.Ensure(
            snapshot.GetProperty("Multicast", BindingFlags.Public | BindingFlags.Instance)
                is null,
            "MeshNode snapshot still exposes Publish monitoring.");
        ZlinkStreamAssert.Ensure(
            assembly.GetType("Zlink.Framework.Contracts.Configuration.ZLinkLogicalMulticastSnapshot")
                is null,
            "Publish monitoring snapshot type still exists.");

        var publicMembers = string.Join(
            '\n',
            snapshot.GetMembers(BindingFlags.Public | BindingFlags.Instance)
                .Concat(runtimeEvent.GetMembers(BindingFlags.Public | BindingFlags.Instance))
                .Select(member => member.Name));
        AssertForbiddenTextAbsent(publicMembers, "public monitoring contract");

        var bytes = File.ReadAllBytes(assembly.Location);
        AssertForbiddenTextAbsent(
            Encoding.UTF8.GetString(bytes),
            "framework metric/event literals");
        AssertForbiddenTextAbsent(
            Encoding.Unicode.GetString(bytes),
            "framework metric/event literals");
    }

    private static void AssertForbiddenTextAbsent(string text, string source)
    {
        foreach (var forbidden in ForbiddenNames)
        {
            ZlinkStreamAssert.Ensure(
                !text.Contains(forbidden, StringComparison.OrdinalIgnoreCase),
                $"{source} contains removed Publish monitoring name '{forbidden}'.");
        }
    }

    private static void AssertTopologyUnchanged(
        MeshRuntimeSnapshotRes before,
        MeshRuntimeSnapshotRes after,
        string scenario)
    {
        ZlinkStreamAssert.Ensure(
            before.MeshName == after.MeshName
            && before.State == after.State
            && before.IsReady == after.IsReady
            && before.ReadyPeerCount == after.ReadyPeerCount
            && before.Peers
                .OrderBy(peer => peer.Rid, StringComparer.Ordinal)
                .SequenceEqual(after.Peers.OrderBy(peer => peer.Rid, StringComparer.Ordinal))
            && before.Channels
                .OrderBy(channel => channel.ChannelName, StringComparer.Ordinal)
                .SequenceEqual(after.Channels.OrderBy(channel => channel.ChannelName, StringComparer.Ordinal))
            && before.Placement == after.Placement,
            $"{scenario} logical publish changed the public RouteMesh topology.");
    }
}
