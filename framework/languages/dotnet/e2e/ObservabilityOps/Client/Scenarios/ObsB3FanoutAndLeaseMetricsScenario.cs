// Verifies OBS-B3 Fanout And Lease Metrics behavior.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using StackExchange.Redis;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsB3FanoutAndLeaseMetricsScenario
{
    private static readonly string[] ForbiddenTags =
        ["correlation_id", "flow_id", "actor_id", "spot_id"];

    private static readonly string[] ForbiddenPublishMetrics =
    [
        "zlink.fanout.",
        "zlink.mesh_node.multicast."
    ];

    public static async Task RunAsync(ScenarioContext context)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var owner = $"workflow-b3-owner-{suffix}";
        await context.WorkflowA.Post("/workflows").Body(new CreateWorkflowReq(owner)).AsyncRaw();
        await context.WorkflowA.Post("/workflows")
            .Body(new CreateWorkflowReq($"workflow-b3-a-{suffix}", "subscriber")).AsyncRaw();
        await context.WorkflowB.Post("/workflows")
            .Body(new CreateWorkflowReq($"workflow-b3-b-{suffix}", "subscriber")).AsyncRaw();
        await context.WorkflowA.Post($"/workflows/{owner}/advance")
            .Body(new AdvanceWorkflowReq("obs-b3-state")).AsyncRaw();
        await context.WorkflowA.Post($"/workflows/{owner}/publish")
            .Body(new PublishProjectionReq("obs-b3-fanout")).AsyncRaw();
        var marker = $"rid={owner}|version=1|marker=obs-b3-fanout";
        await context.WorkflowA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([marker], [["projection-received|"]])).AsyncRaw();
        await context.WorkflowB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([marker], [["projection-received|"]])).AsyncRaw();

        await using (var redis = await ConnectionMultiplexer.ConnectAsync(context.Options.RedisEndpoint))
        {
            await redis.GetDatabase().ExecuteAsync("CLIENT", "PAUSE", 11000, "ALL");
        }
        await WaitForLeaseLatenessAsync(context);

        var afterA = await EvidenceAsync(context.WorkflowA);
        var afterB = await EvidenceAsync(context.WorkflowB);
        var all = afterA.Metrics.Concat(afterB.Metrics).ToArray();
        ZlinkStreamAssert.Ensure(
            all.All(sample => ForbiddenPublishMetrics.All(prefix =>
                !sample.Name.StartsWith(prefix, StringComparison.Ordinal))),
            "OBS-B3 exposed a publish-specific metric.");
        ZlinkStreamAssert.Ensure(all.All(sample => ForbiddenTags.All(tag => !sample.Tags.ContainsKey(tag))),
            "OBS-B3 a metric exposed a forbidden high-cardinality tag.");
        ZlinkStreamAssert.Ensure(all.Any(sample => sample.Name == "zlink.location.owner_lease.renew.lateness"
                                                  && sample.Max >= 0.5m),
            "OBS-B3 external Redis pause did not produce lease renewal lateness.");
        Console.WriteLine("scenario OBS-B3 passed");
    }

    private static async Task<EvidenceSnapshot> EvidenceAsync(Zlink.HttpClient.ZLinkHttpClient client) =>
        (await client.Get("/evidence").Async<EvidenceSnapshot>()).Body;

    private static async Task WaitForLeaseLatenessAsync(ScenarioContext context)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(30);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var evidence = (await EvidenceAsync(context.WorkflowA)).Metrics
                .Concat((await EvidenceAsync(context.WorkflowB)).Metrics);
            if (evidence.Any(sample => sample.Name == "zlink.location.owner_lease.renew.lateness"
                                       && sample.Max >= 0.5m)) return;

            await Task.Delay(TimeSpan.FromMilliseconds(100));
        }

        throw new TimeoutException("OBS-B3 lease renewal lateness evidence did not arrive.");
    }
}
