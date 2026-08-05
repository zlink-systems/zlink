// Verifies planned removal and crash recovery through public runtime status.
using System.Diagnostics;
using RuntimeMonitoring.Client.Support;
using RuntimeMonitoring.Shared;
using Zlink.HttpClient;

namespace RuntimeMonitoring.Client.Scenarios;

internal static class MonA4AvailabilityTransitionScenario
{
    public static async Task RunPlannedAsync(ClientOptions options)
    {
        using var observer = ZLinkHttpClient.Create(options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(35))
            .Build();

        await VerifyPlannedRemovalAsync(options, observer);
        Console.WriteLine("scenario MON-A4A passed");
    }

    public static async Task RunCrashAsync(ClientOptions options)
    {
        using var observer = ZLinkHttpClient.Create(options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(35))
            .Build();

        await VerifyCrashRecoveryAsync(options, observer);
        Console.WriteLine("scenario MON-A4B passed");
    }

    private static async Task VerifyPlannedRemovalAsync(
        ClientOptions options,
        ZLinkHttpClient observer)
    {
        const string rid = "svc-a4-normal";
        var evidenceBaseline = await EvidenceCountAsync(observer);
        ulong readySequence;

        await using (var first = await EphemeralService.StartAsync(options, rid))
        {
            var ready = await WaitForReadyPeerAsync(observer, rid);
            readySequence = ready.Sequence;
            ZlinkStreamAssert.Ensure(
                Channel(ready).ReadyTargetCount >= 2,
                "MON-A4 planned source was not represented as a ready target.");

            var drain = await first.DrainAsync();
            ZlinkStreamAssert.Ensure(
                drain.Result == "Drained",
                $"MON-A4 planned source returned {drain.Result}/{drain.Reason}.");
        }

        var removed = await WaitUntilNotReadyAsync(observer, rid);
        ZlinkStreamAssert.Ensure(
            removed.Sequence > readySequence,
            "MON-A4 planned removal did not advance the status sequence.");

        await using var replacement = await EphemeralService.StartAsync(options, rid);
        var restored = await WaitForReadyPeerAsync(observer, rid);
        ZlinkStreamAssert.Ensure(
            restored.Sequence > removed.Sequence
            && restored.Peers.Count(peer =>
                peer.Rid.StartsWith($"{rid}-", StringComparison.Ordinal)
                && peer.State == "Ready") == 1
            && Channel(restored).ReadyTargetCount >= 2,
            "MON-A4 planned replacement did not converge.");
        await AssertPeerEventSequenceAsync(observer, evidenceBaseline, rid);
    }

    private static async Task VerifyCrashRecoveryAsync(
        ClientOptions options,
        ZLinkHttpClient observer)
    {
        var beforeCrash = await WaitForReadyPeerAsync(observer, "svc-b");
        var evidenceBaseline = await EvidenceCountAsync(observer);

        using (var process = Process.GetProcessById(options.ServiceBProcessId))
        {
            process.Kill(entireProcessTree: true);
            await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(10));
        }

        var unavailable = await WaitUntilNotReadyAsync(observer, "svc-b");
        ZlinkStreamAssert.Ensure(
            unavailable.Sequence > beforeCrash.Sequence,
            "MON-A4 crash removal did not advance the status sequence.");

        var started = Stopwatch.GetTimestamp();
        var followUp = await observer.Post("/profile/request")
            .Body(new ProfileReq("after-crash", "mon-a4-after-crash"))
            .Async<ProfileRes>();
        ZlinkStreamAssert.Ensure(
            Stopwatch.GetElapsedTime(started) < TimeSpan.FromSeconds(3)
            && followUp.Body.ProviderRid == "svc-a",
            "MON-A4 follow-up request did not reach a bounded live provider.");

        await using var replacement = await EphemeralService.StartAsync(options, "svc-b");
        var restored = await WaitForReadyPeerAsync(observer, "svc-b");
        ZlinkStreamAssert.Ensure(
            restored.Sequence > unavailable.Sequence
            && restored.Peers.Count(peer =>
                peer.Rid.StartsWith("svc-b-", StringComparison.Ordinal)
                && peer.State == "Ready") == 1
            && Channel(restored).ReadyTargetCount >= 2,
            "MON-A4 crash replacement did not restore the ready topology.");
        await AssertPeerEventSequenceAsync(observer, evidenceBaseline, "svc-b");
    }

    private static MeshRuntimeChannelRes Channel(MeshRuntimeSnapshotRes status)
        => status.Channels.Single(channel =>
            channel.ChannelName == RuntimeMonitoringNames.Channel);

    private static async Task<int> EvidenceCountAsync(ZLinkHttpClient service)
        => (await service.Get("/evidence").Async<string[]>()).Body.Length;

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
            if (elapsed.Elapsed >= TimeSpan.FromSeconds(15))
                throw new InvalidOperationException(
                    $"MON-A4 peer '{rid}' did not become ready.");
            await Task.Delay(TimeSpan.FromMilliseconds(50));
        }
    }

    private static async Task<MeshRuntimeSnapshotRes> WaitUntilNotReadyAsync(
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
            if (elapsed.Elapsed >= TimeSpan.FromSeconds(15))
                throw new InvalidOperationException(
                    $"MON-A4 peer '{rid}' remained ready.");
            await Task.Delay(TimeSpan.FromMilliseconds(50));
        }
    }

    private static async Task AssertPeerEventSequenceAsync(
        ZLinkHttpClient service,
        int afterIndex,
        string rid)
    {
        var evidence = (await service.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(
                [
                    "identifier=zlink.runtime.mesh_node.peer_changed",
                    $"routing={rid}"
                ],
                [],
                TimeoutMilliseconds: 3000,
                AfterIndex: afterIndex))
            .Async<string[]>()).Body;
        var sequence = evidence
            .Where(line =>
                line.Contains(
                    $"source={RuntimeMonitoringNames.Channel}",
                    StringComparison.Ordinal)
                && line.Contains(
                    "identifier=zlink.runtime.mesh_node.peer_changed",
                    StringComparison.Ordinal)
                && line.Contains($"routing={rid}", StringComparison.Ordinal))
            .Select(ParseSequence)
            .Where(static value => value > 0)
            .ToArray();
        ZlinkStreamAssert.Ensure(
            sequence.Length >= 2
            && sequence.Zip(sequence.Skip(1), static (left, right) => right > left)
                .All(static increasing => increasing),
            $"MON-A4 peer event sequence for '{rid}' was not strictly increasing.");
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
