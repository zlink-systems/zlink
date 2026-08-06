// Verifies RL-E4 request completion is terminal exactly once during races.
using ResilienceLifecycle.Client.Support;
using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Scenarios;

internal static class RlE4ConnectionLossRaceScenario
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

        await RunBeforeAdmissionAsync(options, providerB);
        await RunAfterHandlerEntryAsync(options, providerB);
        await RunBeforeReplyAsync(options, providerB);

        await providerA.Post("/admin/weight/include").AsyncRaw();
        await providerA.Post("/admin/weight/wait")
            .Body(new WeightWaitReq(100))
            .AsyncRaw();
        Console.WriteLine("scenario RL-E4 passed");
    }

    private static async Task RunBeforeAdmissionAsync(
        ClientOptions options,
        ZLinkHttpClient providerB)
    {
        var marker = $"rl-e4-before-admission-{Guid.NewGuid():N}";
        await providerB.Post($"/admin/profile/hold/{marker}").AsyncRaw();
        await using var session = await EphemeralRouteSession.StartAsync(options, "e4-before-admission");
        var request = CaptureAsync(session.RequestAsync(new ProfileReq("held", marker)));

        // Close the only physical connection immediately after submission.
        // The request may be rejected before admission or race with handler
        // entry; both paths must produce one terminal and never be retried.
        await session.DisposeAsync();
        await providerB.Post($"/admin/profile/release/{marker}").AsyncRaw();
        await AssertSingleTerminalAsync(providerB, marker, request, "before-admission");
    }

    private static async Task RunAfterHandlerEntryAsync(
        ClientOptions options,
        ZLinkHttpClient providerB)
    {
        var marker = $"rl-e4-after-entry-{Guid.NewGuid():N}";
        await providerB.Post($"/admin/profile/hold/{marker}").AsyncRaw();
        await using var session = await EphemeralRouteSession.StartAsync(options, "e4-after-entry");
        var request = CaptureAsync(session.RequestAsync(new ProfileReq("held", marker)));
        await providerB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-start|rid=api-b|marker={marker}"], []))
            .Async<string[]>();

        await session.DisposeAsync();
        await providerB.Post($"/admin/profile/release/{marker}").AsyncRaw();
        await AssertSingleTerminalAsync(providerB, marker, request, "after-handler-entry");
    }

    private static async Task RunBeforeReplyAsync(
        ClientOptions options,
        ZLinkHttpClient providerB)
    {
        var marker = $"rl-e4-before-reply-{Guid.NewGuid():N}";
        await providerB.Post($"/admin/profile/hold/{marker}").AsyncRaw();
        await using var session = await EphemeralRouteSession.StartAsync(options, "e4-before-reply");
        var request = CaptureAsync(session.RequestAsync(new ProfileReq("held", marker)));
        await providerB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([$"profile-start|rid=api-b|marker={marker}"], []))
            .Async<string[]>();

        var release = providerB.Post($"/admin/profile/release/{marker}").AsyncRaw().AsTask();
        var close = session.DisposeAsync().AsTask();
        await Task.WhenAll(release, close);
        await AssertSingleTerminalAsync(providerB, marker, request, "before-reply");
    }

    private static async Task AssertSingleTerminalAsync(
        ZLinkHttpClient providerB,
        string marker,
        Task<(ProfileRes? Reply, Exception? Error)> request,
        string variant)
    {
        var outcome = await request.WaitAsync(TimeSpan.FromSeconds(10));
        ZlinkStreamAssert.Ensure(
            (outcome.Reply is not null) ^ (outcome.Error is not null),
            $"RL-E4 {variant} did not produce exactly one public terminal.");

        var evidence = (await providerB.Get("/evidence").Async<string[]>()).Body;
        var completions = evidence.Count(line =>
            line.Contains($"profile-request|rid=api-b|marker={marker}", StringComparison.Ordinal));
        ZlinkStreamAssert.Ensure(
            completions <= 1,
            $"RL-E4 {variant} handler completed more than once: {completions}.");
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
