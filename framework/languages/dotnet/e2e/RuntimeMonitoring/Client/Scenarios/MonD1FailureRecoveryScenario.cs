// Verifies MON-D1 public validation and repeated failure recovery behavior.
using System.Diagnostics;
using System.Net.Sockets;
using RuntimeMonitoring.Client.Support;
using RuntimeMonitoring.Shared;
using Zlink.Framework.E2E.Configuration;
using Zlink.HttpClient;

namespace RuntimeMonitoring.Client.Scenarios;

internal static class MonD1FailureRecoveryScenario
{
    public static async Task RunValidationAsync(ClientOptions options)
    {
        using var observer = ZLinkHttpClient.Create(options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(40))
            .Build();
        var validation =
            (await observer.Get("/runtime/validate")
                .Async<RuntimeValidationRes>()).Body;
        ZlinkStreamAssert.Ensure(
            validation.MissingSnapshotRejected
            && validation.MissingObserverRejected
            && validation.RegisteredObserverProducedStatus,
            "MON-D1 public snapshot or observer validation accepted an invalid call.");

        Console.WriteLine("scenario MON-D1A passed");
    }

    public static async Task RunCrashRecoveryAsync(ClientOptions options)
    {
        using var observer = ZLinkHttpClient.Create(options.ServiceUrl)
            .Timeout(TimeSpan.FromSeconds(40))
            .Build();

        var serviceBUri = new Uri(options.ServiceBUrl);
        var serviceBChannelUri = new Uri(options.ServiceBChannelEndpoint);
        Process? current = Process.GetProcessById(options.ServiceBProcessId);
        var lastSequence = 0UL;
        try
        {
            for (var cycle = 1; cycle <= 3; cycle++)
            {
                var readyBefore = await WaitForReadyPeerAsync(observer, "svc-b");
                var baseline =
                    (await observer.Get("/evidence").Async<string[]>()).Body.Length;

                current.Kill(entireProcessTree: true);
                await current.WaitForExitAsync()
                    .WaitAsync(TimeSpan.FromSeconds(10));
                current.Dispose();
                current = null;
                await WaitForPortStateAsync(
                    serviceBUri.Host,
                    serviceBUri.Port,
                    false,
                    $"MON-D1 cycle {cycle} expected service-b HTTP port to close.");
                var unavailable = await WaitUntilNotReadyAsync(
                    observer,
                    "svc-b");
                ZlinkStreamAssert.Ensure(
                    unavailable.Sequence > readyBefore.Sequence
                    && !unavailable.Peers.Any(peer =>
                        peer.Rid.StartsWith("svc-b-", StringComparison.Ordinal)
                        && peer.State == "Ready"),
                    $"MON-D1 cycle {cycle} retained the failed peer as ready.");

                current = StartServiceB(options, cycle);
                await WaitForPortStateAsync(
                    serviceBUri.Host,
                    serviceBUri.Port,
                    true,
                    $"MON-D1 cycle {cycle} expected service-b to restart.");
                await WaitForPortStateAsync(
                    serviceBChannelUri.Host,
                    serviceBChannelUri.Port,
                    true,
                    $"MON-D1 cycle {cycle} expected the channel endpoint to restart.");
                var recovered = await WaitForReadyPeerAsync(observer, "svc-b");
                ZlinkStreamAssert.Ensure(
                    recovered.Sequence > unavailable.Sequence
                    && recovered.Peers.Count(peer =>
                        peer.Rid.StartsWith("svc-b-", StringComparison.Ordinal)
                        && peer.State == "Ready") == 1
                    && recovered.Channels.Any(channel =>
                        channel.ChannelName == RuntimeMonitoringNames.Channel
                        && channel.IsReady),
                    $"MON-D1 cycle {cycle} snapshot did not resync to the replacement peer.");

                using var activeServiceB =
                    ZLinkHttpClient.Create(options.ServiceBUrl)
                        .Timeout(TimeSpan.FromSeconds(35))
                        .Build();
                await activeServiceB.Post("/admin/weight/exclude")
                    .Async<object>();
                await Task.Delay(150);
                await activeServiceB.Post("/admin/weight/include")
                    .Async<object>();

                var cycleEvidence = (await observer.Post("/evidence/wait")
                    .Body(new EvidenceWaitReq(
                        [
                            $"source={RuntimeMonitoringNames.Channel}",
                            "identifier=zlink.runtime.mesh_node.peer_changed",
                            "identifier=zlink.runtime.mesh_node.channel_changed",
                            "routing=svc-b"
                        ],
                        [],
                        TimeoutMilliseconds: 3000,
                        AfterIndex: baseline))
                    .Async<string[]>()).Body;
                var eventLines = cycleEvidence.Where(line =>
                        line.Contains(
                            $"source={RuntimeMonitoringNames.Channel}",
                            StringComparison.Ordinal)
                        && line.Contains(
                            "identifier=zlink.runtime.",
                            StringComparison.Ordinal))
                    .ToArray();
                var sequences = eventLines
                    .Select(ParseSequence)
                    .Where(static sequence => sequence > 0)
                    .Distinct()
                    .ToArray();
                ZlinkStreamAssert.Ensure(
                    sequences.Length >= 2
                    && sequences.All(sequence => sequence > lastSequence)
                    && sequences.Zip(
                            sequences.Skip(1),
                            static (left, right) => right > left)
                        .All(static increasing => increasing),
                    $"MON-D1 cycle {cycle} event sequence was not strictly increasing.");
                lastSequence = sequences[^1];
                ZlinkStreamAssert.Ensure(
                    eventLines.All(line =>
                        !line.Contains("mon-d1-payload", StringComparison.Ordinal)
                        && !line.Contains("profile-request|", StringComparison.Ordinal)
                        && !line.Contains("|value=", StringComparison.Ordinal)),
                    $"MON-D1 cycle {cycle} copied application metadata into a runtime event.");
            }

            await observer.Post("/admin/weight/exclude").Async<object>();
            using var finalServiceB =
                ZLinkHttpClient.Create(options.ServiceBUrl)
                    .Timeout(TimeSpan.FromSeconds(35))
                    .Build();
            var reply = (await finalServiceB.Post("/profile/request")
                .Body(new ProfileReq("restart", "mon-d1-payload"))
                .Async<ProfileRes>()).Body;
            ZlinkStreamAssert.Ensure(
                reply.ProviderRid == "svc-b"
                && reply.Marker == "mon-d1-payload"
                && reply.Value == "profile:restart",
                "MON-D1 messaging did not recover after three failure cycles.");
        }
        finally
        {
            await PostBestEffortAsync(observer, "/admin/weight/include");
            using var activeServiceB = ZLinkHttpClient.Create(options.ServiceBUrl)
                .Timeout(TimeSpan.FromSeconds(20))
                .Build();
            await PostBestEffortAsync(activeServiceB, "/shutdown");
            if (current is not null)
            {
                try
                {
                    await current.WaitForExitAsync()
                        .WaitAsync(TimeSpan.FromSeconds(5));
                }
                catch (TimeoutException)
                {
                    if (!current.HasExited)
                        current.Kill(true);
                    await current.WaitForExitAsync();
                }
                current.Dispose();
            }
        }

        Console.WriteLine("scenario MON-D1B passed");
    }

    private static Process StartServiceB(ClientOptions options, int cycle)
    {
        var stdout = Path.Combine(
            options.LogDir,
            $"svc-b-restart-{cycle}.stdout.log");
        var stderr = Path.Combine(
            options.LogDir,
            $"svc-b-restart-{cycle}.stderr.log");
        var startInfo = new ProcessStartInfo("dotnet")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("run");
        startInfo.ArgumentList.Add("--no-build");
        startInfo.ArgumentList.Add("--project");
        startInfo.ArgumentList.Add(options.ServiceProject);
        startInfo.ArgumentList.Add("--");
        startInfo.ArgumentList.Add("--config");
        startInfo.ArgumentList.Add(E2eConfiguration.Write(
            options.ConfigDir,
            $"svc-b-restart-{cycle}",
            new RestartServiceOptions(
                "service",
                options.ServiceBUrl,
                options.LogDir,
                "svc-b",
                Path.Combine(
                    options.LogDir,
                    $"svc-b-restart-{cycle}.evidence.log"),
                options.RedisEndpoint,
                options.RedisKeyPrefix,
                options.ServiceBChannelEndpoint,
                options.ServiceBSpotRouterEndpoint,
                options.ServiceBSpotPubEndpoint)));

        var process = Process.Start(startInfo)
                      ?? throw new InvalidOperationException(
                          "Failed to restart service-b.");
        _ = Task.Run(async () =>
            await File.WriteAllTextAsync(
                stdout,
                await process.StandardOutput.ReadToEndAsync()));
        _ = Task.Run(async () =>
            await File.WriteAllTextAsync(
                stderr,
                await process.StandardError.ReadToEndAsync()));
        return process;
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
            var snapshot = await SnapshotAsync(service);
            if (snapshot.Peers.Any(peer =>
                    peer.Rid.StartsWith($"{rid}-", StringComparison.Ordinal)
                    && peer.State == "Ready"))
                return snapshot;
            if (elapsed.Elapsed >= TimeSpan.FromSeconds(15))
                throw new InvalidOperationException(
                    $"MON-D1 peer '{rid}' did not become ready.");
            await Task.Delay(50);
        }
    }

    private static async Task<MeshRuntimeSnapshotRes> WaitUntilNotReadyAsync(
        ZLinkHttpClient service,
        string rid)
    {
        var elapsed = Stopwatch.StartNew();
        while (true)
        {
            var snapshot = await SnapshotAsync(service);
            if (!snapshot.Peers.Any(peer =>
                    peer.Rid.StartsWith($"{rid}-", StringComparison.Ordinal)
                    && peer.State == "Ready"))
                return snapshot;
            if (elapsed.Elapsed >= TimeSpan.FromSeconds(15))
                throw new InvalidOperationException(
                    $"MON-D1 peer '{rid}' remained ready.");
            await Task.Delay(50);
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

    private static async Task PostBestEffortAsync(
        ZLinkHttpClient http,
        string path)
    {
        try
        {
            await http.Post(path).Async<object>();
        }
        catch (HttpRequestException)
        {
        }
        catch (TaskCanceledException)
        {
        }
        catch (Zlink.Framework.Contracts.Errors.ZLinkFrameworkException)
        {
        }
    }

    private static async Task WaitForPortStateAsync(
        string host,
        int port,
        bool shouldBeOpen,
        string failureMessage)
    {
        for (var attempt = 0; attempt < 30; attempt++)
        {
            if (await CanConnectAsync(host, port) == shouldBeOpen)
                return;
            await Task.Delay(100);
        }
        throw new InvalidOperationException(failureMessage);
    }

    private static async Task<bool> CanConnectAsync(string host, int port)
    {
        try
        {
            using var client = new TcpClient();
            await client.ConnectAsync(host, port)
                .WaitAsync(TimeSpan.FromMilliseconds(200));
            return true;
        }
        catch (SocketException)
        {
            return false;
        }
        catch (TimeoutException)
        {
            return false;
        }
    }
}

internal sealed record RestartServiceOptions(
    string Role,
    string HttpUrl,
    string LogDir,
    string Rid,
    string EvidenceFile,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string ChannelEndpoint,
    string SpotRouterEndpoint,
    string SpotPubEndpoint);
