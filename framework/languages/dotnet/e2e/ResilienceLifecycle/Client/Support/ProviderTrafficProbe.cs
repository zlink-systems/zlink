using ResilienceLifecycle.Shared;
using Zlink.HttpClient;

namespace ResilienceLifecycle.Client.Support;

/// <summary>
/// Drives consumer traffic until a specific provider proves it received it.
/// A store row appearing (topology/wait) precedes the consumer's own
/// reconcile dial by up to a poll tick, so a fixed batch fired right after
/// the row shows up can land entirely on the surviving provider.
/// </summary>
internal static class ProviderTrafficProbe
{
    public static async Task WaitUntilProviderExcludedAsync(
        ZLinkHttpClient consumer,
        string excludedProviderRid,
        string markerPrefix,
        string scenario)
    {
        using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var consecutiveSurvivorReplies = 0;
        for (var probe = 0; consecutiveSurvivorReplies < 20; probe++)
        {
            try
            {
                var reply = (await consumer.Post("/profile/request/timeout/1000")
                    .Body(new ProfileReq("fast", $"{markerPrefix}-{probe}"))
                    .Async<ProfileRes>(deadline.Token)).Body;
                consecutiveSurvivorReplies = reply.ProviderRid == excludedProviderRid
                    ? 0
                    : consecutiveSurvivorReplies + 1;
            }
            catch (Exception) when (!deadline.IsCancellationRequested)
            {
                consecutiveSurvivorReplies = 0;
            }

            if (deadline.IsCancellationRequested) break;
        }

        if (consecutiveSurvivorReplies < 20)
            throw new InvalidOperationException(
                $"{scenario}: consumer did not converge after provider '{excludedProviderRid}' changed its runtime weight.");
    }

    public static async Task<ProfileRes> RequestWithoutRetryAsync(
        ZLinkHttpClient consumer,
        ProfileReq request,
        CancellationToken cancellationToken = default) =>
        (await consumer.Post("/profile/request/timeout/1000")
            .Body(request)
            .Async<ProfileRes>(cancellationToken)).Body;

    public static async Task DriveUntilProviderServesAsync(
        ZLinkHttpClient consumer,
        ZLinkHttpClient provider,
        string markerPrefix,
        string scenario,
        string? evidencePattern = null)
    {
        var pattern = evidencePattern ?? $"marker={markerPrefix}-";
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        var evidence = provider.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([pattern], [], 30000))
            .Async<string[]>(timeout.Token)
            .AsTask();
        for (var round = 0; round < 300; round++)
        {
            var marker = $"{markerPrefix}-{round}";
            var reply = (await consumer.Post("/profile/request")
                .Body(new ProfileReq("fast", marker))
                .Async<ProfileRes>()).Body;
            ZlinkStreamAssert.Ensure(
                reply.Value == "profile:fast",
                $"{scenario} request returned an unexpected value.");

            if (evidence.IsCompleted) break;
            await Task.WhenAny(evidence, Task.Delay(100, timeout.Token));
        }

        var snapshot = (await evidence).Body;
        ZlinkStreamAssert.Ensure(
            snapshot.Any(entry => entry.Contains(pattern, StringComparison.Ordinal)),
            $"{scenario}: provider did not receive '{markerPrefix}' traffic before the probe deadline.");
    }
}
