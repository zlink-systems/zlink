namespace Zlink.Framework.Runtime.Messaging;

internal enum ZLinkOneWaySubmitStatus
{
    Submitted = 0,
    Backpressured = 1,
    TimedOut = 2,
    TargetNotFound = 3,
    RouteNotConnected = 4,
    Shutdown = 5,

    /// <summary>
    /// A conditional send (send-if-bound-to) found no current session bound
    /// under the expected binding token, so nothing was submitted. This is
    /// the designed stale-binding drop — the sync SendIfBoundTo path reports
    /// the same case as success — but it is not a delivery signal, so it is
    /// distinguishable from <see cref="Submitted"/> instead of masquerading
    /// as one.
    /// </summary>
    SkippedNotBound = 6
}

internal readonly record struct ZLinkOneWaySubmitResult(ZLinkOneWaySubmitStatus Status);

internal static class ZLinkOneWaySubmitOutcome
{
    public static async ValueTask EnsureAcceptedAsync(
        this ValueTask<ZLinkOneWaySubmitResult> pending,
        string operationName,
        ZLinkFrameworkErrorKind targetNotFoundKind = ZLinkFrameworkErrorKind.NotFound)
    {
        var result = await pending.ConfigureAwait(false);
        EnsureAccepted(result, operationName, targetNotFoundKind);
    }

    public static void EnsureAccepted(
        ZLinkOneWaySubmitResult result,
        string operationName,
        ZLinkFrameworkErrorKind targetNotFoundKind = ZLinkFrameworkErrorKind.NotFound)
    {
        switch (result.Status)
        {
            case ZLinkOneWaySubmitStatus.Submitted:
                return;
            //  The stale-binding drop is by design (the sync SendIfBoundTo
            //  contract treats "no longer bound" as nothing-to-do), so it is
            //  accepted here — but callers that need a delivery signal can
            //  now see it is not Submitted.
            case ZLinkOneWaySubmitStatus.SkippedNotBound:
                return;
            case ZLinkOneWaySubmitStatus.Backpressured:
            case ZLinkOneWaySubmitStatus.TimedOut:
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    $"{operationName} timed out before local admission completed.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            case ZLinkOneWaySubmitStatus.TargetNotFound:
                throw new ZLinkFrameworkException(
                    targetNotFoundKind,
                    $"{operationName} failed because the target was not found.");
            case ZLinkOneWaySubmitStatus.RouteNotConnected:
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    $"{operationName} failed because the target route is not connected.",
                    ZLinkRetryAdvice.RetryAfterBackoff);
            case ZLinkOneWaySubmitStatus.Shutdown:
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ShuttingDown,
                    $"{operationName} failed because the runtime is shutting down.");
            default:
                throw new InvalidOperationException(
                    $"{operationName} returned unknown one-way submit status '{result.Status}'.");
        }
    }
}

internal static class ZLinkMeshCallSupport
{
    public static bool TryMapSubmitFailure(
        ZLinkFrameworkException failure,
        out ZLinkOneWaySubmitResult result)
    {
        ZLinkOneWaySubmitStatus? status = failure.Kind switch
        {
            ZLinkFrameworkErrorKind.NotFound => ZLinkOneWaySubmitStatus.TargetNotFound,
            ZLinkFrameworkErrorKind.Unavailable => ZLinkOneWaySubmitStatus.RouteNotConnected,
            ZLinkFrameworkErrorKind.ShuttingDown => ZLinkOneWaySubmitStatus.Shutdown,
            _ => null
        };
        result = status is { } mapped ? new ZLinkOneWaySubmitResult(mapped) : default;
        return status is not null;
    }
}
