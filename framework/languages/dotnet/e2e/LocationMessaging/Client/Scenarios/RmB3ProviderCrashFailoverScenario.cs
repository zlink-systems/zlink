// Verifies RM-B3 provider crash failover while stale location rows remain visible.
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.Framework.Contracts.Errors;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-B3 proves that a crashed provider is fenced by owner-lease expiry while
// the remaining provider continues serving new untargeted requests.
internal static class RmB3ProviderCrashFailoverScenario
{
    public static async Task RunAsync(ClientOptions options)
    {
        await using var cluster = await DynamicClusterLauncher.StartAsync(options, "rm-b3");
        var providerA = await cluster.StartProviderAsync("api-a", "api-a");
        var providerB = await cluster.StartProviderAsync(
            "api-b",
            "api-b",
            routePeers: [providerA.RouteEndpoint]);
        var consumer = await cluster.StartConsumerAsync("consumer");
        using var requester = ZLinkHttpClient.Create(consumer.HttpUrl)
            .Timeout(TimeSpan.FromSeconds(90))
            .Build();
        using var providerAClient = ZLinkHttpClient.Create(providerA.HttpUrl)
            .Timeout(TimeSpan.FromSeconds(10))
            .Build();
        using var providerBClient = ZLinkHttpClient.Create(providerB.HttpUrl)
            .Timeout(TimeSpan.FromSeconds(10))
            .Build();

        await WaitForPeerAsync(requester, "api-a", present: true);
        await WaitForPeerAsync(requester, "api-b", present: true);
        var apiARouteRid = await ReadActualRouteRidAsync(
            providerBClient,
            "api-a-route");
        await ProveBothProvidersAsync(
            requester,
            providerAClient,
            providerBClient,
            providerA.ChannelEndpoint,
            providerB.ChannelEndpoint);

        var marker = $"rm-b3-transition-{Guid.NewGuid():N}";
        var inFlight = Enumerable.Range(0, 4)
            .Select(index =>
            {
                var value = $"{marker}-{index}";
                return (Value: value, Task: ObserveRequestAsync(requester, value));
            })
            .ToArray();
        var startedOnA = (await providerAClient.Post("/evidence/wait")
            .Body(new EvidenceWaitReq($"profile-request-start|rid=api-a|value={marker}"))
            .Async<string[]>()).Body;
        var startedValue = startedOnA
            .Select(line => TryReadValue(line, $"profile-request-start|rid=api-a|value={marker}"))
            .First(value => value is not null)!;

        await cluster.CrashAsync(providerA);
        var continuingMarker = $"rm-b3-continuing-{Guid.NewGuid():N}";
        var continuingTraffic = Enumerable.Range(0, 20)
            .Select(index => ObserveRequestAsync(requester, $"{continuingMarker}-{index}"))
            .ToArray();

        var transition = await Task.WhenAll(inFlight.Select(request => request.Task));
        ZlinkStreamAssert.Ensure(
            transition.All(result => result.Outcome is "api-a" or "api-b"
                or nameof(ZLinkFrameworkErrorKind.Unavailable)
                or "Timeout"),
            "RM-B3 in-flight request did not finish with a reply or bounded public request error.");
        var crashedRequest = transition.Single(result => result.Value == startedValue);
        ZlinkStreamAssert.Ensure(
            crashedRequest.Outcome is nameof(ZLinkFrameworkErrorKind.Unavailable) or "Timeout",
            $"RM-B3 request started on crashed api-a completed as '{crashedRequest.Outcome}'.");

        var continuingOutcomes = await Task.WhenAll(continuingTraffic);
        ZlinkStreamAssert.Ensure(
            continuingOutcomes.Any(result => result.Outcome == "api-b"),
            "RM-B3 did not keep serving new untargeted traffic on the remaining provider after crash.");
        await WaitForPeerAsync(requester, "api-a", present: true);
        var unexpectedContinuing = continuingOutcomes
            .Where(result => result.Outcome is not ("api-b"
                or nameof(ZLinkFrameworkErrorKind.Unavailable)
                or "Timeout"))
            .Select(result => result.Outcome)
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        ZlinkStreamAssert.Ensure(
            unexpectedContinuing.Length == 0,
            "RM-B3 continuing request completed with an outcome outside the public failover "
            + $"contract: {string.Join(", ", unexpectedContinuing)}.");

        await WaitForPeerAsync(requester, "api-a", present: false);
        await WaitForOnlyRemainingProviderAsync(requester);
        for (var index = 0; index < 20; index++)
        {
            var value = $"rm-b3-after-{index}";
            var reply = await ObserveRequestAsync(requester, value);
            ZlinkStreamAssert.Ensure(
                reply.Outcome == "api-b",
                "RM-B3 post-expiry request did not use the remaining provider.");
        }

        var targeted = await RequestTargetAsync(
            providerBClient,
            apiARouteRid,
            "rm-b3-target-expired");
        ZlinkStreamAssert.Ensure(
            targeted.ErrorKind is nameof(ZLinkFrameworkErrorKind.NotFound)
                or nameof(ZLinkFrameworkErrorKind.Unavailable),
            $"RM-B3 removed automatic member completed with unexpected '{targeted.ErrorKind}'.");
        var missing = await RequestTargetAsync(
            providerBClient,
            "api-missing",
            "rm-b3-target-missing");
        ZlinkStreamAssert.Ensure(
            missing.ErrorKind == nameof(ZLinkFrameworkErrorKind.NotFound),
            "RM-B3 unknown route target did not report NotFound.");
    }

    private static async Task ProveBothProvidersAsync(
        ZLinkHttpClient requester,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB,
        string providerAEndpoint,
        string providerBEndpoint)
    {
        // Spec 24 §7 keeps the endpoint out of public status, so peers are
        // identified by their automatic RID, which carries the role prefix.
        await WaitConnectionReadyAsync(requester, "api-a-");
        await WaitConnectionReadyAsync(requester, "api-b-");

        var marker = $"rm-b3-before-{Guid.NewGuid():N}";
        for (var index = 0; index < 40; index++)
            _ = await requester.Post("/profile/request")
                .Body(new ProfileReq($"{marker}-{index}"))
                .Async<ProfileRes>();

        var a = (await providerA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq($"profile-request|rid=api-a|value={marker}"))
            .Async<string[]>()).Body;
        var b = (await providerB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq($"profile-request|rid=api-b|value={marker}"))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(a.Length > 0 && b.Length > 0, "RM-B3 expected both providers before crash.");
    }

    private static async Task WaitConnectionReadyAsync(
        ZLinkHttpClient requester,
        string routingPrefix)
    {
        _ = await requester.Post("/connections/wait")
            .Body(new EvidenceWaitReq(
                "monitor-mesh|source=profile|kind=ConnectionReady"
                + $"|remote=|routing={routingPrefix}"))
            .Async<string[]>();
    }

    private static async Task<RequestOutcome> ObserveRequestAsync(ZLinkHttpClient requester, string value)
    {
        var result = (await requester.Post("/profile/request/outcome")
            .Body(new ProfileReq(value))
            .Async<RequestOutcomeRes>()).Body;
        return new RequestOutcome(result.Value, result.Outcome);
    }

    private static string? TryReadValue(string line, string prefix)
        => line.Contains(prefix, StringComparison.Ordinal)
            ? line[(line.IndexOf("|value=", StringComparison.Ordinal) + "|value=".Length)..]
            : null;

    private static async Task WaitForOnlyRemainingProviderAsync(ZLinkHttpClient requester)
    {
        var consecutive = 0;
        for (var attempt = 0; attempt < 20 && consecutive < 2; attempt++)
        {
            var outcome = await ObserveRequestAsync(requester, $"rm-b3-ready-{attempt}");
            consecutive = outcome.Outcome == "api-b" ? consecutive + 1 : 0;
        }

        ZlinkStreamAssert.Ensure(
            consecutive == 2,
            "RM-B3 target-free messaging did not converge to the remaining provider after lease expiry.");
    }

    private static Task WaitForPeerAsync(ZLinkHttpClient requester, string rid, bool present)
        => requester.Post("/locations/peers/wait")
            .Body(new PeerLocationWaitReq(
                "profile",
                "Router",
                rid,
                present,
                TimeoutMilliseconds: present ? 30000 : 60000))
            .Async<PeerLocationRow[]>()
            .AsTask();

    private static async Task<ExpectedFailureRes> RequestTargetAsync(
        ZLinkHttpClient provider,
        string targetRid,
        string value) =>
        (await provider.Post("/profile/route/target")
            .Body(new TargetedRoutePing(targetRid, value))
            .Async<ExpectedFailureRes>()).Body;

    private static async Task<string> ReadActualRouteRidAsync(
        ZLinkHttpClient provider,
        string ridPrefix)
    {
        var rows = (await provider.Post("/locations/peers/wait")
            .Body(new PeerLocationWaitReq(
                "profile.route",
                "Router",
                ridPrefix,
                Present: true))
            .Async<PeerLocationRow[]>()).Body;
        return rows.Single(row =>
                row.NodeRid is not null
                && row.NodeRid.StartsWith($"{ridPrefix}-", StringComparison.Ordinal))
            .NodeRid!;
    }

    private sealed record RequestOutcome(string Value, string Outcome);
}
