namespace Zlink.Framework.Runtime.Messaging;

internal enum ZLinkOneWaySubmitStatus
{
    Submitted = 0,
    Backpressured = 1,
    TimedOut = 2,
    TargetNotFound = 3,
    RouteNotConnected = 4,
    Shutdown = 5
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
