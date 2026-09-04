namespace Zlink.Framework.Runtime.Messaging;

// Maps the binding submit result to the framework's typed error surface for
// the mesh submit paths. Ok/Backpressured are binding admission control flow
// and never reach this mapper; async send wait/retry stays binding-owned.
internal static class ZLinkSubmitFailureMapper
{
    public static ZLinkFrameworkException CreateChannelException(
        SubmitResult result,
        string targetDescription)
    {
        return CreateException(result, targetDescription);
    }

    public static bool AcceptOrThrow(SubmitResult result, string targetDescription)
    {
        return result switch
        {
            SubmitResult.Ok => true,
            SubmitResult.Backpressured => false,
            _ => throw CreateException(result, targetDescription)
        };
    }

    public static ZLinkFrameworkException CreateException(
        SubmitResult result, string targetDescription)
    {
        var kind = result switch
        {
            SubmitResult.NotFound => ZLinkFrameworkErrorKind.NotFound,
            SubmitResult.NotConnected => ZLinkFrameworkErrorKind.Unavailable,
            SubmitResult.NotAdmitted => ZLinkFrameworkErrorKind.Rejected,
            SubmitResult.Terminated => ZLinkFrameworkErrorKind.ShuttingDown,
            _ => ZLinkFrameworkErrorKind.InternalFailure
        };
        return new ZLinkFrameworkException(
            kind,
            $"Mesh submit to {targetDescription} failed with result '{result}'.",
            retryAdvice: result is SubmitResult.NotConnected
                ? ZLinkRetryAdvice.RetryAfterBackoff
                : ZLinkRetryAdvice.DoNotRetry);
    }
}
