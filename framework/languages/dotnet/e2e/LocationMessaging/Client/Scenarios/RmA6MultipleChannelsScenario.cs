// Verifies RM-A6 Multiple Channels behavior.
using System.Text.Json;
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-A6 uses one consumer process for two RouteMesh instances. The client
// starts all application requests through that consumer's public endpoints so
// the scenario observes MeshName isolation at the framework boundary.
internal static class RmA6MultipleChannelsScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        await using var cluster = await DynamicClusterLauncher.StartAsync(options, "rm-a6");
        var profileA = await cluster.StartProviderAsync("api-a", "api-a");
        var profileB = await cluster.StartProviderAsync("api-b", "api-b");
        var workflow = await cluster.StartWorkflowAsync("workflow-a", "workflow-a");
        var consumer = await cluster.StartConsumerAsync(
            "consumer",
            registerWorkflowClient: true);

        using var requester = ZLinkHttpClient.Create(consumer.HttpUrl)
            .Timeout(TimeSpan.FromSeconds(40))
            .Build();
        using var profileAClient = ZLinkHttpClient.Create(profileA.HttpUrl)
            .Timeout(TimeSpan.FromSeconds(10))
            .Build();
        using var profileBClient = ZLinkHttpClient.Create(profileB.HttpUrl)
            .Timeout(TimeSpan.FromSeconds(10))
            .Build();
        using var workflowClient = ZLinkHttpClient.Create(workflow.HttpUrl)
            .Timeout(TimeSpan.FromSeconds(10))
            .Build();

        await WaitForReadyAsync(requester, "profile", 2);
        await WaitForReadyAsync(requester, "workflow", 1);

        var profileMarker = $"rm-a6-profile-before-{Guid.NewGuid():N}";
        var workflowMarker = $"rm-a6-workflow-before-{Guid.NewGuid():N}";
        var profileReply = (await requester.Post("/profile/request")
            .Body(new ProfileReq(profileMarker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            profileReply.Value == $"profile:{profileMarker}",
            "RM-A6 initial profile reply value mismatch.");
        ZlinkStreamAssert.Ensure(
            profileReply.ProviderRid is "api-a" or "api-b",
            "RM-A6 initial profile request reached an unexpected provider.");

        var workflowReply = (await requester.Post("/workflow/request")
            .Body(new WorkflowReq(workflowMarker))
            .Async<WorkflowRes>()).Body;
        ZlinkStreamAssert.Ensure(
            workflowReply.Value == $"workflow:{workflowMarker}",
            "RM-A6 initial workflow reply value mismatch.");
        ZlinkStreamAssert.Ensure(
            workflowReply.ProviderRid == "workflow-a",
            "RM-A6 initial workflow request reached an unexpected provider.");

        await ProviderEvidence.WaitFromEitherAsync(
            profileAClient,
            profileBClient,
            profileMarker);
        var workflowEvidenceBefore = await WaitForEvidenceAsync(
            workflowClient,
            workflowMarker);
        var workflowRowsBefore = await ReadRowsAsync(requester, "workflow");
        var workflowReadyBefore = await ReadReadyAsync(requester, "workflow", 1);
        AssertMeshRows(workflowRowsBefore, "workflow", "workflow-a");

        var stopped = await cluster.StopAsync(profileA);
        ZlinkStreamAssert.Ensure(
            stopped is { Result: "Stopped", Reason: null },
            $"RM-A6 profile-a did not reach terminal Stopped: {stopped.Result}/{stopped.Reason}.");

        await requester.Post("/locations/peers/wait")
            .Body(new PeerLocationWaitReq(
                "profile",
                "Router",
                "api-a",
                Present: false))
            .Async<PeerLocationRow[]>();
        await WaitForReadyAsync(requester, "profile", 1);
        var workflowReadyAfterProfileStop = await ReadReadyAsync(requester, "workflow", 1);
        ZlinkStreamAssert.Ensure(
            workflowReadyAfterProfileStop.Ready == workflowReadyBefore.Ready
            && workflowReadyAfterProfileStop.ReadyCount == workflowReadyBefore.ReadyCount,
            "RM-A6 profile provider removal changed workflow readiness.");

        var workflowRowsAfterProfileStop = await ReadRowsAsync(requester, "workflow");
        AssertMeshRows(workflowRowsAfterProfileStop, "workflow", "workflow-a");
        ZlinkStreamAssert.Ensure(
            RowSignature(workflowRowsAfterProfileStop) == RowSignature(workflowRowsBefore),
            "RM-A6 profile provider removal changed workflow location rows.");
        var workflowEvidenceAfterProfileStop = await ReadEvidenceAsync(workflowClient);
        ZlinkStreamAssert.Ensure(
            workflowEvidenceAfterProfileStop.SequenceEqual(workflowEvidenceBefore),
            "RM-A6 profile provider removal changed workflow handler evidence.");

        var profileAfterStopMarker = $"rm-a6-profile-after-{Guid.NewGuid():N}";
        var workflowAfterStopMarker = $"rm-a6-workflow-after-{Guid.NewGuid():N}";
        var profileAfterStop = (await requester.Post("/profile/request")
            .Body(new ProfileReq(profileAfterStopMarker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            profileAfterStop.Value == $"profile:{profileAfterStopMarker}"
            && profileAfterStop.ProviderRid == "api-b",
            "RM-A6 profile request after api-a shutdown did not use api-b.");

        var workflowAfterStop = (await requester.Post("/workflow/request")
            .Body(new WorkflowReq(workflowAfterStopMarker))
            .Async<WorkflowRes>()).Body;
        ZlinkStreamAssert.Ensure(
            workflowAfterStop.Value == $"workflow:{workflowAfterStopMarker}"
            && workflowAfterStop.ProviderRid == "workflow-a",
            "RM-A6 workflow request after profile shutdown did not use workflow-a.");

        var profileEvidenceAfterStop = await ProviderEvidence.WaitFromEitherAsync(
            profileAClient,
            profileBClient,
            profileAfterStopMarker);
        ZlinkStreamAssert.Ensure(
            profileEvidenceAfterStop.Any(line =>
                line.Contains("profile-request|", StringComparison.Ordinal)
                && line.Contains(profileAfterStopMarker, StringComparison.Ordinal)),
            "RM-A6 profile evidence after provider removal is missing.");
        var workflowEvidenceAfterStop = await WaitForEvidenceAsync(
            workflowClient,
            workflowAfterStopMarker);
        ZlinkStreamAssert.Ensure(
            workflowEvidenceAfterStop.Any(line =>
                line.Contains("workflow-request|rid=workflow-a", StringComparison.Ordinal)
                && line.Contains(workflowAfterStopMarker, StringComparison.Ordinal)),
            "RM-A6 workflow evidence after profile provider removal is missing.");
        ZlinkStreamAssert.Ensure(
            !profileEvidenceAfterStop.Any(line =>
                line.Contains("workflow-request|", StringComparison.Ordinal)
                && line.Contains(workflowAfterStopMarker, StringComparison.Ordinal)),
            "RM-A6 workflow request was recorded on a profile provider.");

        AssertMeshRows(await ReadRowsAsync(requester, "profile"), "profile", "api-b");
        AssertMeshRows(await ReadRowsAsync(requester, "workflow"), "workflow", "workflow-a");
    }

    private static async Task WaitForReadyAsync(
        ZLinkHttpClient client,
        string mesh,
        int count)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(45);
        while (DateTimeOffset.UtcNow < deadline)
        {
            try
            {
                var status = await ReadReadyAsync(client, mesh, count);
                if (status.Ready) return;
            }
            catch (ZLinkFrameworkException)
            {
                // The consumer HTTP process can be ready before its second
                // RouteMesh has registered its public runtime snapshot.
            }

            await Task.Delay(TimeSpan.FromMilliseconds(100));
        }

        throw new TimeoutException(
            $"RM-A6 {mesh} RouteMesh did not become ready with {count} target(s).");
    }

    private static async Task<ReadyStatus> ReadReadyAsync(
        ZLinkHttpClient client,
        string mesh,
        int count)
    {
        var json = (await client.Get($"/topology/ready?mesh={mesh}&count={count}")
                .Async<JsonElement>())
            .Body;
        return new ReadyStatus(
            json.GetProperty("ready").GetBoolean(),
            json.GetProperty("readyCount").GetInt32());
    }

    private static async Task<PeerLocationRow[]> ReadRowsAsync(
        ZLinkHttpClient client,
        string mesh)
        => (await client.Get($"/locations/peers?mesh={mesh}")
            .Async<PeerLocationRow[]>()).Body;

    private static async Task<string[]> ReadEvidenceAsync(ZLinkHttpClient client)
        => (await client.Get("/evidence").Async<string[]>()).Body;

    private static async Task<string[]> WaitForEvidenceAsync(
        ZLinkHttpClient client,
        string marker)
        => (await client.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(marker))
            .Async<string[]>()).Body;

    private static void AssertMeshRows(
        IReadOnlyList<PeerLocationRow> rows,
        string mesh,
        string expectedRidPrefix)
    {
        ZlinkStreamAssert.Ensure(
            rows.Count > 0 && rows.All(row => row.MeshName == mesh),
            $"RM-A6 {mesh} location query returned another MeshName.");
        ZlinkStreamAssert.Ensure(
            rows.Any(row => row.NodeRid?.StartsWith(
                $"{expectedRidPrefix}-",
                StringComparison.Ordinal) == true),
            $"RM-A6 {mesh} location query did not contain {expectedRidPrefix}.");
        if (expectedRidPrefix == "workflow-a")
        {
            ZlinkStreamAssert.Ensure(
                rows.All(row =>
                    row.NodeRid?.StartsWith("api-a-", StringComparison.Ordinal) != true
                    && row.NodeRid?.StartsWith("api-b-", StringComparison.Ordinal) != true),
                $"RM-A6 {mesh} location query mixed the profile MeshName.");
        }
        else
        {
            ZlinkStreamAssert.Ensure(
                rows.All(row =>
                    row.NodeRid?.StartsWith("workflow-a-", StringComparison.Ordinal) != true),
                $"RM-A6 {mesh} location query mixed the workflow MeshName.");
        }
    }

    private static string RowSignature(IEnumerable<PeerLocationRow> rows)
        => string.Join(
            "\n",
            rows.OrderBy(row => row.NodeRid, StringComparer.Ordinal)
                .Select(row =>
                    $"{row.MeshName}|{row.NodeRid}|{row.Role}|{row.Endpoint}|{row.Draining}|{row.State}"));

    private sealed record ReadyStatus(bool Ready, int ReadyCount);
}
