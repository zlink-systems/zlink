// Verifies that a ClientServer Server role has no outbound Client role.
using ResilienceLifecycle.Shared;
using Zlink.Framework.Contracts.Configuration;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

internal static class RlF4ClientServerRoleScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        await providerA.Post("/admin/clientserver/weight/include").AsyncRaw();
        await providerB.Post("/admin/clientserver/weight/exclude").AsyncRaw();
        await providerA.Post("/admin/clientserver/weight/wait")
            .Body(new WeightWaitReq(100))
            .AsyncRaw();
        await providerB.Post("/admin/clientserver/weight/wait")
            .Body(new WeightWaitReq(0))
            .AsyncRaw();
        await WaitForClientServerSelectionAsync(consumer);

        var serverOnlyMarker = $"rl-f4-server-only-{Guid.NewGuid():N}";
        var before = (await providerA.Get("/evidence").Async<string[]>()).Body;
        var serverOnly = (await providerA.Post("/profile/clientserver/request")
            .Body(new ProfileReq("server-only", serverOnlyMarker))
            .Async<ServerOnlyRequestRes>()).Body;
        ZlinkStreamAssert.Ensure(
            !serverOnly.Succeeded && serverOnly.ErrorKind == "NotFound",
            $"RL-F4 server-only ClientServer call returned an unexpected result: {serverOnly.ErrorKind}.");

        var after = (await providerA.Get("/evidence").Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            after.SequenceEqual(before),
            "RL-F4 server-only ClientServer call reached the provider handler.");

        var clientMarker = $"rl-f4-client-{Guid.NewGuid():N}";
        var clientReply = (await consumer.Post("/profile/clientserver/request")
            .Body(new ProfileReq("client", clientMarker))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(
            clientReply.ProviderRid == "api-a" && clientReply.Marker == clientMarker,
            "RL-F4 normal ClientServer call did not reach the selected server.");
        await providerA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-request|rid=api-a|marker={clientMarker}"], []))
            .Async<string[]>();

        Console.WriteLine("scenario RL-F4 passed");
    }

    private static async Task WaitForClientServerSelectionAsync(ZLinkHttpClient consumer)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                var status = (await consumer.Get("/clientserver/status")
                    .Async<ZLinkClientServerStatus>()).Body;
                if (status.ReadyTargetCount >= 1
                    && status.Targets.Any(target => target.Weight == 100)
                    && status.Targets.Any(target => target.Weight == 0))
                    return;
            }
            catch (Exception) when (DateTime.UtcNow < deadline)
            {
            }

            await Task.Delay(100);
        }

        throw new TimeoutException("RL-F4 ClientServer weight did not converge on the consumer.");
    }
}
