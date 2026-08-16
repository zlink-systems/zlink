using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRequestFailureMapper
{
    public static Exception CreateChannelCompletionException(
        RequestResult result,
        string operationName)
    {
        return CreateCompletionException(result, operationName);
    }

    //  Ownership-aware remote-reply mapper. A remote request reply may carry a
    //  Framework fine failure code (ServiceWireConstants.FrameworkErrorCode) that
    //  refines the coarse terminal (spec 32-framework-error-model:81-118). This
    //  overload is REMOTE-only, exactly like the string-only overload below: every
    //  caller translates a peer reply header (C++ reply_header_exception), never a
    //  source-owned bounded resource. When the reply carried no fine code (errno 0
    //  = None) or an unrecognised one, it falls through to the coarse terminal map,
    //  which preserves Backpressured(113)+None -> CapacityExceeded (placement/
    //  admission capacity) and Conflict/Busy -> Unavailable (remote owner/queue).
    public static Exception CreateCompletionException(
        RequestResult result,
        int failureErrno,
        string operationName)
    {
        if (ClassifyFineFailure(failureErrno) is not { } kind)
            return CreateCompletionException(result, operationName);
        return new ZLinkFrameworkException(
            kind,
            $"{operationName} failed with result '{result}' "
            + $"(framework error code {failureErrno}).",
            innerException: CreateRequestException(result));
    }

    //  Fine failure-code table shared with ZLinkBackendSpotNodeWrapper's
    //  join/create/destroy/close classification so both surfaces stay in lockstep
    //  (spec 32-framework-error-model). Returns null for None(0) or any code with
    //  no fine refinement, leaving the coarse terminal to classify.
    internal static ZLinkFrameworkErrorKind? ClassifyFineFailure(int failureErrno)
    {
        return (ServiceWireConstants.FrameworkErrorCode)failureErrno switch
        {
            ServiceWireConstants.FrameworkErrorCode.ActorRouteNotFound
                or ServiceWireConstants.FrameworkErrorCode.HandlerNotFound
                or ServiceWireConstants.FrameworkErrorCode.RequestTargetNotFound =>
                ZLinkFrameworkErrorKind.NotFound,
            ServiceWireConstants.FrameworkErrorCode.ActorAlreadyExists =>
                ZLinkFrameworkErrorKind.AlreadyExists,
            ServiceWireConstants.FrameworkErrorCode.ActorTypeMismatch
                or ServiceWireConstants.FrameworkErrorCode.SpotTypeMismatch =>
                ZLinkFrameworkErrorKind.TypeMismatch,
            //  actorSessionNotBound(8): a bound-session precondition was violated,
            //  an invalid operation in the current state — not NotFound/Rejected
            //  (spec 32-framework-error-model; matches the Java reference table).
            ServiceWireConstants.FrameworkErrorCode.ActorSessionNotBound =>
                ZLinkFrameworkErrorKind.InvalidOperation,
            ServiceWireConstants.FrameworkErrorCode.ActorCreateRejected
                or ServiceWireConstants.FrameworkErrorCode.RequestRejected =>
                ZLinkFrameworkErrorKind.Rejected,
            ServiceWireConstants.FrameworkErrorCode.PayloadDecodeFailed
                or ServiceWireConstants.FrameworkErrorCode.RequestProtocolError =>
                ZLinkFrameworkErrorKind.ProtocolError,
            //  workerQueueFull(18) on a remote reply is the target's queue state,
            //  a resource this runtime does not own -> Unavailable, not
            //  CapacityExceeded (spec 32-framework-error-model:99-108).
            ServiceWireConstants.FrameworkErrorCode.ActorLocationStale
                or ServiceWireConstants.FrameworkErrorCode.RouteNotConnected
                or ServiceWireConstants.FrameworkErrorCode.WorkerQueueFull
                or ServiceWireConstants.FrameworkErrorCode.SpotMoving =>
                ZLinkFrameworkErrorKind.Unavailable,
            ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale =>
                ZLinkFrameworkErrorKind.InvalidOperation,
            ServiceWireConstants.FrameworkErrorCode.RelocationDataLost =>
                ZLinkFrameworkErrorKind.DataLost,
            ServiceWireConstants.FrameworkErrorCode.WorkerTimedOut =>
                ZLinkFrameworkErrorKind.DeadlineExceeded,
            ServiceWireConstants.FrameworkErrorCode.RequestFailed
                or ServiceWireConstants.FrameworkErrorCode.WorkerFailed =>
                ZLinkFrameworkErrorKind.InternalFailure,
            _ => null,
        };
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
            //  Spec 32-framework-error-model:99-103 — a terminal-only `Conflict`/
            //  `Busy` on a remote request reply reflects the target's queue/owner
            //  state, a resource this runtime does not own, so it is `Unavailable`
            //  (a source-owned bounded resource would be `CapacityExceeded`, but
            //  that must be proven, not assumed from the coarse terminal). Stays
            //  retryable via RetryAfterBackoff.
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
            RequestResult.InvalidArgument or RequestResult.InvalidState => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"{operationName} failed because the operation is invalid in the current state.",
                innerException: CreateRequestException(result)),
            RequestResult.NotSupported or RequestResult.InternalError => new ZLinkFrameworkException(
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
            ZlinkSubmitException.ErrorCode.NotAdmitted => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"{operationName} submit was rejected with result '{error.Result}'.",
                innerException: error),
            //  Spec 32-framework-error-model:51-56 — an invalid argument, a
            //  closed/invalid handle, a wrong-state submit, or a thread-affinity
            //  violation is an InvalidOperation, not a policy Rejected or an
            //  InternalFailure. Matches C++ submit_result_mapper.
            ZlinkSubmitException.ErrorCode.InvalidState
                or ZlinkSubmitException.ErrorCode.InvalidArgument
                or ZlinkSubmitException.ErrorCode.InvalidHandle
                or ZlinkSubmitException.ErrorCode.ThreadViolation => new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"{operationName} submit failed because the operation is invalid in the current state.",
                    innerException: error),
            ZlinkSubmitException.ErrorCode.NotSupported
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
