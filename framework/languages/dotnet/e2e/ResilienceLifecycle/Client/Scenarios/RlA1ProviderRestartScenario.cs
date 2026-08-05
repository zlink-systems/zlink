// Verifies RL-A1 Provider Restart behavior.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

// RL-A1 verifies recovery when the same provider endpoint is restarted.
internal static class RlA1ProviderRestartScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient registry,
        ResilienceProcessManager processes,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        // RL-A1 deliberately has one serving provider. api-a remains alive only
        // as harness infrastructure and is excluded from transport selection.
        await providerA.Post("/admin/weight/exclude").AsyncRaw();
        await providerA.Post("/admin/weight/wait").Body(new WeightWaitReq(0)).AsyncRaw();

        var oldRows = (await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>()).Body;
        var oldRoutingId = oldRows.Single().RoutingId;

        var acceptedMarker = $"rl-a1-accepted-{Guid.NewGuid():N}";
        var accepted = consumer.Post("/profile/request")
            .Body(new ProfileReq("slow", acceptedMarker))
            .Async<ProfileRes>();
        await providerB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-start|rid=api-b|marker={acceptedMarker}"], []))
            .Async<string[]>();
        await processes.StopProviderBWithSigtermAsync();
        var acceptedReply = (await accepted).Body;
        ZlinkStreamAssert.Ensure(
            acceptedReply.ProviderRid == "api-b" && acceptedReply.Marker == acceptedMarker,
            "RL-A1 accepted request did not complete inside graceful shutdown.");

        await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 0))
            .Async<TopologyEntryRes[]>();

        var down = (await consumer.Post("/profile/request/attempt/1000")
            .Body(new ProfileReq("fast", "rl-a1-down"))
            .Async<ProfileAttemptRes>()).Body;
        // Spec 06 and the common E2E contract allow either NotFound for a
        // missing route or Unavailable for a registered route with no ready
        // target. Both are terminal here; the request must not be resent.
        ZlinkStreamAssert.Ensure(
            down is { Reply: null, ErrorKind: "NotFound" or "Unavailable" },
            $"RL-A1 down-window result was '{down.ErrorKind}', expected NotFound or Unavailable.");

        var connectionCount = (await consumer.Get("/connections").Async<string[]>()).Body.Length;
        var restarted = await processes.StartProviderBAsync();
        var newRows = (await registry.Post("/topology/wait")
            .Body(new TopologyWaitReq("api-b", "Ready", 1))
            .Async<TopologyEntryRes[]>()).Body;
        var newRow = newRows.Single();
        // Automatic discovery issues a new lifecycle RID on restart; the endpoint
        // is the one the provider was restarted on.
        ZlinkStreamAssert.Ensure(
            newRow.Endpoint == restarted.Endpoint && newRow.RoutingId != oldRoutingId,
            "RL-A1 restart did not publish the same endpoint under a new RID.");
        await consumer.Post("/connections/wait")
            .Body(new ConnectionWaitReq(
                ["kind=ConnectionReady", $"remote={restarted.Endpoint}"], connectionCount))
            .Async<string[]>();

        for (var i = 0; i < 20; i++)
        {
            var reply = (await consumer.Post("/profile/request")
                .Body(new ProfileReq("fast", $"rl-a1-restored-{i}"))
                .Async<ProfileRes>()).Body;
            ZlinkStreamAssert.Ensure(reply.ProviderRid == "api-b", "RL-A1 follow-up used an unexpected provider.");
        }

        await providerA.Post("/admin/weight/include").AsyncRaw();

        Console.WriteLine("scenario RL-A1 passed");
    }
}
