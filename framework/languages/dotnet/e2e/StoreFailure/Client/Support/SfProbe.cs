using System.Diagnostics;
using StoreFailure.Shared;
using Zlink.HttpClient;

namespace StoreFailure.Client.Support;

/// <summary>
/// Reads the public runtime-query surface of a node over HTTP and waits
/// for expected states. Every SF verdict goes through these endpoints,
/// the request path, or evidence — never framework internals.
/// </summary>
internal static class SfProbe
{
    public static PeerRowsWaitReq PeerRows(
        TimeSpan timeout,
        string[]? present = null,
        string[]? absent = null,
        string[]? draining = null) =>
        new(present ?? [], absent ?? [], draining ?? [], ToMilliseconds(timeout));

    public static RuntimeStatusWaitReq Status(
        TimeSpan timeout,
        bool? storeHealthy = null,
        bool? ownerLeaseHealthy = null,
        bool requireLastRefresh = false,
        DateTimeOffset? lastRefreshAfter = null) =>
        new(storeHealthy, ownerLeaseHealthy, requireLastRefresh,
            ToMilliseconds(timeout), lastRefreshAfter);

    public static async Task<RuntimeStatusRes> GetStatusAsync(ZLinkHttpClient node)
    {
        return (await node.Get("/query/status").Async<RuntimeStatusRes>()).Body;
    }

    public static async Task<PeerRowRes[]?> TryGetPeersAsync(ZLinkHttpClient node)
    {
        try
        {
            return (await node.Get("/query/peers").Async<PeerRowRes[]>()).Body;
        }
        catch
        {
            // 503: the store is unreachable, so the raw row list is too.
            return null;
        }
    }

    public static async Task<PeerRowRes[]> WaitPeersAsync(
        ZLinkHttpClient node,
        PeerRowsWaitReq request,
        string failure)
    {
        try
        {
            return (await node.Post("/query/peers/wait")
                .Body(request)
                .Async<PeerRowRes[]>()).Body;
        }
        catch (Exception error)
        {
            throw new InvalidOperationException(failure, error);
        }
    }

    public static async Task<RouteReadyRes> WaitRouteReadyAsync(
        ZLinkHttpClient node,
        int minimumReadyMembers,
        string[]? readyRids,
        string[]? notReadyRids,
        TimeSpan timeout,
        string failure)
    {
        try
        {
            return (await node.Post("/query/routes/wait")
                .Body(new RouteReadyWaitReq(
                    minimumReadyMembers,
                    readyRids ?? [],
                    notReadyRids ?? [],
                    ToMilliseconds(timeout)))
                .Async<RouteReadyRes>()).Body;
        }
        catch (Exception error)
        {
            throw new InvalidOperationException(failure, error);
        }
    }

    public static Task<RouteReadyRes> WaitProviderRoutesAsync(
        ZLinkHttpClient node,
        TimeSpan timeout,
        string failure) =>
        WaitRouteReadyAsync(
            node,
            minimumReadyMembers: 2,
            readyRids: ["api-a", "api-b"],
            notReadyRids: null,
            timeout: timeout,
            failure: failure);

    public static async Task<RuntimeStatusRes> WaitStatusAsync(
        ZLinkHttpClient node,
        RuntimeStatusWaitReq request,
        string failure)
    {
        try
        {
            return (await node.Post("/query/status/wait")
                .Body(request)
                .Async<RuntimeStatusRes>()).Body;
        }
        catch (Exception error)
        {
            throw new InvalidOperationException(failure, error);
        }
    }

    public static bool HasRid(PeerRowRes[] peers, string rid) =>
        peers.Any(row => string.Equals(row.Rid, rid, StringComparison.Ordinal));

    private static int ToMilliseconds(TimeSpan timeout) =>
        (int)Math.Clamp(Math.Ceiling(timeout.TotalMilliseconds), 1, 60000);

    /// <summary>
    /// One request through the consumer with a bounded per-request
    /// timeout and no consumer-side retry: the strict probe for "the
    /// established path still works right now".
    /// </summary>
    public static async Task<ProfileRes> RequestAsync(
        ZLinkHttpClient consumer,
        string marker,
        int timeoutMilliseconds = 3000)
    {
        return (await consumer.Post($"/profile/request/timeout/{timeoutMilliseconds}")
            .Body(new ProfileReq("fast", marker))
            .Async<ProfileRes>()).Body;
    }

    /// <summary>
    /// Sends requests at a steady cadence for a window and asserts every
    /// one of them succeeded — the fail-static "no interruption" check.
    /// </summary>
    public static async Task<IReadOnlyList<ProfileRes>> DriveRequestsAsync(
        ZLinkHttpClient consumer,
        string markerPrefix,
        TimeSpan window,
        string scenario)
    {
        var replies = new List<ProfileRes>();
        var elapsed = Stopwatch.StartNew();
        var index = 0;
        while (elapsed.Elapsed < window)
        {
            var marker = $"{markerPrefix}-{index++}";
            var reply = await RequestAsync(consumer, marker);
            ZlinkStreamAssert.Ensure(
                reply.Value == "profile:fast",
                $"{scenario}: request '{marker}' returned an unexpected value.");
            replies.Add(reply);
            await Task.Delay(150);
        }

        ZlinkStreamAssert.Ensure(replies.Count > 0, $"{scenario}: the request window produced no traffic.");
        return replies;
    }
}
