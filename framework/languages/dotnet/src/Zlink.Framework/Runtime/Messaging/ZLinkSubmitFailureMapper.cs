namespace Zlink.Framework.Runtime.Messaging;

// Owns the binding submit-to-request projection. RequestFailureMapper owns
// the public error kind; async admission waits remain binding-owned.
internal static class ZLinkSubmitFailureMapper
{
    public static ZLinkFrameworkException CreateChannelException(
        SubmitResult result,
        string targetDescription
    )
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
                ZLinkRetryAdvice.RetryAfterBackoff
            );
        return CreateException(result, targetDescription);
    }

    public static ZLinkFrameworkException CreateFanoutException(ZlinkSubmitException error) =>
        error.Result == ZlinkSubmitException.ErrorCode.Backpressured
            ? ZLinkRequestFailureMapper.CreateCompletionException(
                RequestResult.TimedOut,
                "Fanout publish",
                error
            )
            : CreateException(error, "Fanout publish");

    public static bool AcceptOrThrow(SubmitResult result, string targetDescription)
    {
        return result switch
        {
            SubmitResult.Ok => true,
            SubmitResult.Backpressured => false,
            _ => throw CreateException(result, targetDescription),
        };
    }

    public static ZLinkFrameworkException CreateException(
        SubmitResult result,
        string targetDescription
    ) =>
        CreateException(
            new ZlinkSubmitException((ZlinkSubmitException.ErrorCode)(int)result),
            targetDescription,
            completionFailure: false
        );

    public static RequestResult ToRequestResult(SubmitResult result, bool completionFailure) =>
        result switch
        {
            SubmitResult.Ok => RequestResult.Ok,
            SubmitResult.Backpressured => RequestResult.NotConnected,
            SubmitResult.NotFound => completionFailure
                ? RequestResult.NotConnected
                : RequestResult.NotFound,
            SubmitResult.NotConnected => RequestResult.NotConnected,
            SubmitResult.NotAdmitted => RequestResult.Rejected,
            SubmitResult.Terminated => RequestResult.Terminated,
            SubmitResult.InvalidState => RequestResult.InvalidState,
            SubmitResult.InvalidArgument
            or SubmitResult.InvalidHandle
            or SubmitResult.ThreadViolation => RequestResult.InvalidArgument,
            SubmitResult.NotSupported => RequestResult.NotSupported,
            _ => RequestResult.InternalError,
        };

    public static ZLinkFrameworkException CreateException(
        ZlinkSubmitException error,
        string operationName,
        bool completionFailure = false
    ) =>
        ZLinkRequestFailureMapper.CreateCompletionException(
            ToRequestResult((SubmitResult)(int)error.Result, completionFailure),
            operationName,
            error
        );
}
