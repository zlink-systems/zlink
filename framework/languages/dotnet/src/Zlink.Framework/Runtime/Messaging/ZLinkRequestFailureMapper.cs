namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRequestFailureMapper
{
    public static Exception CreateChannelCompletionException(
        RequestResult result,
        string operationName)
    {
        return CreateCompletionException(result, operationName);
    }

    public static Exception CreateCompletionException(
        RequestResult result,
        string operationName)
    {
        return result switch
        {
            RequestResult.TimedOut => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                $"{operationName} timed out.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                CreateRequestException(result)),
            RequestResult.NotConnected => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"{operationName} failed because the target route is not connected.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                CreateRequestException(result)),
            RequestResult.NotFound => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"{operationName} failed because the target was not found.",
                innerException: CreateRequestException(result)),
            //  Spec 06 §13.1의 `Rejected`는 application admission 정책이 거절했다는
            //  뜻이다. `Conflict`와 `Busy`는 재시도하면 풀리는 일시 상태이므로 같은
            //  kind로 뭉치면 안 된다. Kind만 실어 나르는 경로(예: Actor join 완료
            //  통지)에서는 retry advice가 사라져 일시 충돌이 정책 거절로 도착한다.
            RequestResult.Conflict or RequestResult.Busy => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"{operationName} was refused with a transient result '{result}'.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                CreateRequestException(result)),
            RequestResult.Rejected => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"{operationName} was rejected with result '{result}'.",
                ZLinkRetryAdvice.DoNotRetry,
                CreateRequestException(result)),
            RequestResult.Backpressured => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.CapacityExceeded,
                $"{operationName} exceeded the bounded completion capacity.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                CreateRequestException(result)),
            RequestResult.ProtocolError => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                $"{operationName} failed with a protocol error.",
                innerException: CreateRequestException(result)),
            RequestResult.Terminated => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                $"{operationName} failed because the runtime is shutting down.",
                innerException: CreateRequestException(result)),
            RequestResult.InvalidArgument or RequestResult.InvalidState or RequestResult.NotSupported
                or RequestResult.InternalError => new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InternalFailure,
                    $"{operationName} failed with result '{result}'.",
                    innerException: CreateRequestException(result)),
            _ => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                $"{operationName} failed with result '{result}'.")
        };
    }

    public static Exception CreateSubmitException(
        ZlinkSubmitException error,
        string operationName)
    {
        return error.Result switch
        {
            ZlinkSubmitException.ErrorCode.NotConnected => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"{operationName} failed because the target route is not connected.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                error),
            ZlinkSubmitException.ErrorCode.NotFound => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"{operationName} failed because the target was not found.",
                innerException: error),
            ZlinkSubmitException.ErrorCode.Backpressured => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                $"{operationName} timed out while the socket was backpressured.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                error),
            ZlinkSubmitException.ErrorCode.Terminated => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                $"{operationName} failed because the runtime is shutting down.",
                innerException: error),
            ZlinkSubmitException.ErrorCode.NotAdmitted
                or ZlinkSubmitException.ErrorCode.InvalidState => new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    $"{operationName} submit was rejected with result '{error.Result}'.",
                    innerException: error),
            ZlinkSubmitException.ErrorCode.InvalidArgument
                or ZlinkSubmitException.ErrorCode.InvalidHandle
                or ZlinkSubmitException.ErrorCode.NotSupported
                or ZlinkSubmitException.ErrorCode.ThreadViolation
                or ZlinkSubmitException.ErrorCode.OutOfMemory
                or ZlinkSubmitException.ErrorCode.SeqExhausted
                or ZlinkSubmitException.ErrorCode.InternalError => new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InternalFailure,
                    $"{operationName} submit failed with result '{error.Result}'.",
                    innerException: error),
            _ => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                $"{operationName} submit failed with result '{error.Result}'.",
                innerException: error)
        };
    }

    public static Exception CreateSubmitTimeoutException(
        Exception? lastSubmitFailure,
        string operationName)
    {
        if (lastSubmitFailure is ZlinkSubmitException submitError)
            return CreateSubmitException(submitError, operationName);

        return lastSubmitFailure is null
            ? new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                $"{operationName} timed out before the socket became writable.",
                ZLinkRetryAdvice.RetryAfterBackoff)
            : new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                $"{operationName} timed out before the socket became writable.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                lastSubmitFailure);
    }

    public static ZLinkFrameworkException CreateTimedOutRequestException(
        string operationName,
        Exception? innerException = null) =>
        new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.DeadlineExceeded,
            operationName,
            ZLinkRetryAdvice.RetryAfterBackoff,
            innerException);

    public static ZLinkFrameworkException CreateShutdownRequestException(
        string operationName,
        Exception? innerException = null) =>
        new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.ShuttingDown,
            operationName,
            innerException: innerException);

    private static ZlinkRequestException CreateRequestException(RequestResult result)
    {
        return new ZlinkRequestException((ZlinkRequestException.ErrorCode)(int)result);
    }
}
