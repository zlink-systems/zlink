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
        //  A select-one channel reports NotFound when applying eligibility and
        //  drain left no member to pick. The send path and its connection are
        //  still there, so the spec ends that as Unavailable
        //  (06-framework-api "no eligible select-one member"). CreateException
        //  below keeps NotFound for a named target that is absent.
        if (result == SubmitResult.NotFound)
            return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"{targetDescription} had no eligible member.",
                ZLinkRetryAdvice.RetryAfterBackoff);
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
                : ZLinkRetryAdvice.DoNotRetry,
            innerException: new ZlinkSubmitException(
                (ZlinkSubmitException.ErrorCode)(int)result));
    }
}
