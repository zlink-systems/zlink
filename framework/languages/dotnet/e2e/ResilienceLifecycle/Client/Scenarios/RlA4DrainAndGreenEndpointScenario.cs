// Verifies RL-A4 rolling replacement with a ready green target before old-target drain.
using System.Collections.Concurrent;
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-A4 verifies a serving green target before each old provider drains.
internal static class RlA4DrainAndGreenEndpointScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var connectionCount = (await consumer.Get("/connections").Async<string[]>()).Body.Length;
        var green = await processes.StartProviderBGreenAsync();
        using var greenProvider = ZLinkHttpClient.Create(green.Url).Timeout(TimeSpan.FromMinutes(5)).Build();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-green", "Ready", 1))
            .Async<TopologyEntryRes[]>();
        await consumer.Post("/connections/wait")
            .Body(new ConnectionWaitReq(
                ["kind=ConnectionReady", $"remote={green.Endpoint}"], connectionCount))
            .Async<string[]>();
        await ProviderTrafficProbe.DriveUntilProviderServesAsync(
            consumer, greenProvider, "rl-a4-green-ready", "RL-A4 green readiness");

        var replies = new ConcurrentQueue<ProfileRes>();
        var failures = new ConcurrentQueue<Exception>();
        using var trafficStop = new CancellationTokenSource();
        var traffic = DriveTrafficAsync(consumer, replies, failures, trafficStop.Token);

        await DrainAndStopAsync(providerB, registry, "api-b", processes.WaitInitialProviderBExitedAsync);
        await AssertServingAsync(registry, "api-green");
        await DrainAndStopAsync(providerA, registry, "api-a", processes.WaitInitialProviderAExitedAsync);
        await AssertServingAsync(registry, "api-green");

        trafficStop.Cancel();
        await traffic;
        ZlinkStreamAssert.Ensure(failures.IsEmpty,
            $"RL-A4 continuous traffic failed: {failures.FirstOrDefault()?.Message}");
        ZlinkStreamAssert.Ensure(replies.Count >= 10, "RL-A4 did not observe enough fixed-interval traffic.");

        for (var index = 0; index < 20; index++)
        {
            var reply = (await consumer.Post("/profile/request")
                .Body(new ProfileReq("fast", $"rl-a4-final-{index}"))
                .Async<ProfileRes>()).Body;
            ZlinkStreamAssert.Ensure(reply.ProviderRid == "api-green",
                "RL-A4 final traffic was handled by an old provider.");
        }
        await greenProvider.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(["profile-request|rid=api-green|marker=rl-a4-final-19"], []))
            .Async<string[]>();

        Console.WriteLine("scenario RL-A4 passed");
    }

    private static async Task DrainAndStopAsync(
        ZLinkHttpClient provider,
        ZLinkHttpClient registry,
        string rid,
        Func<Task> waitExited)
    {
        var drainTask = provider.Post("/admin/graceful-drain").Async<DrainResultRes>().AsTask();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq(rid, "Ready", 1, ExpectedDraining: true))
            .Async<TopologyEntryRes[]>();
        var result = (await drainTask).Body;
        ZlinkStreamAssert.Ensure(result.Result == "Drained",
            $"RL-A4 {rid} did not reach terminal Drained: {result.Result}/{result.Reason}.");
        await waitExited();
        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq(rid, "Ready", 0))
            .Async<TopologyEntryRes[]>();
    }

    private static async Task AssertServingAsync(ZLinkHttpClient registry, string rid)
    {
        var rows = (await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq(rid, "Ready", 1, ExpectedDraining: false))
            .Async<TopologyEntryRes[]>()).Body;
        ZlinkStreamAssert.Ensure(rows.Length == 1, "RL-A4 lost its serving green target.");
    }

    private static async Task DriveTrafficAsync(
        ZLinkHttpClient consumer,
        ConcurrentQueue<ProfileRes> replies,
        ConcurrentQueue<Exception> failures,
        CancellationToken cancellationToken)
    {
        var index = 0;
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                var reply = (await consumer.Post("/profile/request")
                    .Body(new ProfileReq("fast", $"rl-a4-continuous-{index++}"))
                    .Async<ProfileRes>(cancellationToken)).Body;
                replies.Enqueue(reply);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (Exception error)
            {
                failures.Enqueue(error);
            }

            try
            {
                await Task.Delay(TimeSpan.FromMilliseconds(50), cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
        }
    }
}
