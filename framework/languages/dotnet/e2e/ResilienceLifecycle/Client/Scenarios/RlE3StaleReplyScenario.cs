// Verifies RL-E3 old connection replies cannot complete a new operation.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

internal static class RlE3StaleReplyScenario
{
    public static async Task RunAsync(
        ClientOptions options,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        await providerA.Post("/admin/weight/exclude").AsyncRaw();
        await providerA.Post("/admin/weight/wait")
            .Body(new WeightWaitReq(0))
            .AsyncRaw();
        var oldMarker = $"rl-e3-old-{Guid.NewGuid():N}";
        var newMarker = $"rl-e3-new-{Guid.NewGuid():N}";
        await providerB.Post($"/admin/profile/hold/{oldMarker}").AsyncRaw();

        var oldSession = await EphemeralRouteSession.StartAsync(options, "rl-e3-old");
        var oldRequest = CaptureAsync(oldSession.RequestAsync(
            new ProfileReq("held", oldMarker)));
        try
        {
            await providerB.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(
                    [$"profile-start|rid=api-b|marker={oldMarker}"],
                    []))
                .Async<string[]>();

            // Disposing the old Framework host closes its physical RouteMesh
            // connection without stopping the provider or releasing its gate.
            await oldSession.DisposeAsync();

            await using var replacement = await EphemeralRouteSession.StartAsync(
                options,
                "rl-e3-replacement");
            var newReply = await replacement.RequestAsync(
                new ProfileReq("fast", newMarker));
            ZlinkStreamAssert.Ensure(
                newReply.ProviderRid == "api-b"
                && newReply.Marker == newMarker
                && newReply.Value == "profile:fast",
                "RL-E3 replacement request did not receive its own reply.");

            await providerB.Post($"/admin/profile/release/{oldMarker}").AsyncRaw();
            var oldOutcome = await oldRequest;
            ZlinkStreamAssert.Ensure(
                oldOutcome.Reply is null && oldOutcome.Error is not null,
                "RL-E3 old request completed successfully after its connection was closed.");

            var evidence = (await providerB.Get("/evidence").Async<string[]>()).Body;
            ZlinkStreamAssert.Ensure(
                evidence.Count(line => line.Contains($"marker={oldMarker}", StringComparison.Ordinal)
                                       && line.Contains("profile-request|", StringComparison.Ordinal)) <= 1,
                "RL-E3 old request produced more than one provider completion evidence.");
        }
        finally
        {
            await providerB.Post($"/admin/profile/release/{oldMarker}").AsyncRaw();
            await providerA.Post("/admin/weight/include").AsyncRaw();
            await providerA.Post("/admin/weight/wait")
                .Body(new WeightWaitReq(100))
                .AsyncRaw();
        }

        Console.WriteLine("scenario RL-E3 passed");
    }

    private static async Task<(ProfileRes? Reply, Exception? Error)> CaptureAsync(
        ValueTask<ProfileRes> request)
    {
        try
        {
            return (await request, null);
        }
        catch (Exception error)
        {
            return (null, error);
        }
    }
}
