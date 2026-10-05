using Systems.Zlink.Framework.Runtime.Protocol;
using FailureCode = Systems.Zlink.Framework.Runtime.Protocol.ServiceWireConstants.FrameworkErrorCode;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRequestFailureMapper
{
    internal readonly record struct WireFailureMapping(
        ZLinkFrameworkErrorKind Kind,
        FailureCode Code,
        RequestResult Result,
        bool Receive,
        bool Send,
        FailureCode? CodeOnlyCode = null
    );

    // Error model §2.1 owns all receive codes and outgoing representatives.
    private static readonly WireFailureMapping[] WireFailureMappings =
    [
        new(
            ZLinkFrameworkErrorKind.NotFound,
            FailureCode.RequestTargetNotFound,
            RequestResult.NotFound,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.NotFound,
            FailureCode.ActorRouteNotFound,
            RequestResult.NotFound,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.NotFound,
            FailureCode.SpotRouteNotFound,
            RequestResult.NotFound,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.NotFound,
            FailureCode.HandlerNotFound,
            RequestResult.NotFound,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.NotFound,
            FailureCode.RouteHandlerNotFound,
            RequestResult.NotFound,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.NotFound,
            FailureCode.ActorDispatchHandlerNotFound,
            RequestResult.NotFound,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.AlreadyExists,
            FailureCode.ActorAlreadyExists,
            RequestResult.Conflict,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.TypeMismatch,
            FailureCode.SpotTypeMismatch,
            RequestResult.Conflict,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.TypeMismatch,
            FailureCode.ActorTypeMismatch,
            RequestResult.Conflict,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.Rejected,
            FailureCode.RequestRejected,
            RequestResult.Rejected,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.Rejected,
            FailureCode.ActorCreateRejected,
            RequestResult.Rejected,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            FailureCode.RouteNotConnected,
            RequestResult.InternalError,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            FailureCode.WorkerQueueFull,
            RequestResult.Rejected,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            FailureCode.ActorLocationStale,
            RequestResult.Conflict,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            FailureCode.SpotMoving,
            RequestResult.Conflict,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.DeadlineExceeded,
            FailureCode.WorkerTimedOut,
            RequestResult.InternalError,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.ProtocolError,
            FailureCode.RequestProtocolError,
            RequestResult.ProtocolError,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.ProtocolError,
            FailureCode.PayloadDecodeFailed,
            RequestResult.ProtocolError,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            FailureCode.ActorSessionNotBound,
            RequestResult.NotFound,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            FailureCode.SpotGenerationStale,
            RequestResult.Conflict,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.DataLost,
            FailureCode.RelocationDataLost,
            RequestResult.InternalError,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.InternalFailure,
            FailureCode.RequestFailed,
            RequestResult.InternalError,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.InternalFailure,
            FailureCode.WorkerFailed,
            RequestResult.InternalError,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.InternalFailure,
            FailureCode.ActorCreateFailed,
            RequestResult.InternalError,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.InternalFailure,
            FailureCode.SpotCreateFailed,
            RequestResult.InternalError,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.NotConfigured,
            FailureCode.RequestFailed,
            RequestResult.InternalError,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.ShuttingDown,
            FailureCode.None,
            RequestResult.Terminated,
            false,
            true,
            FailureCode.RouteNotConnected
        ),
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            FailureCode.None,
            RequestResult.InvalidState,
            false,
            true,
            FailureCode.RequestFailed
        ),
    ];
    internal static ReadOnlySpan<WireFailureMapping> Mappings => WireFailureMappings;

    public static (RequestResult Result, FailureCode FailureCode) TargetFailureReply(
        Exception error
    )
    {
        var row = TargetFailureMapping(error);
        return (row.Result, row.Code);
    }

    private static WireFailureMapping TargetFailureMapping(Exception error)
    {
        var framework = error as ZLinkFrameworkException;
        var kind =
            error is Zlink.Framework.Runtime.Locations.ZLinkRelocationDataLostException
                ? ZLinkFrameworkErrorKind.DataLost
                : framework?.Kind ?? ZLinkFrameworkErrorKind.InternalFailure;
        var cause = framework;
        while (cause is not null)
        {
            if (cause.FrameworkFailureCode != (int)FailureCode.None)
                foreach (var row in WireFailureMappings)
                    if (
                        row.Receive
                        && row.Kind == kind
                        && (int)row.Code == cause.FrameworkFailureCode
                    )
                        return row;
            cause = cause.InnerException as ZLinkFrameworkException;
        }
        foreach (var row in WireFailureMappings)
            if (row.Send && row.Kind == kind)
                return row;
        throw new InvalidOperationException("The failure representative mapping is missing.");
    }

    public static FailureCode TargetFailureCode(Exception error)
    {
        var row = TargetFailureMapping(error);
        return row.CodeOnlyCode ?? row.Code;
    }

    public static Exception CreateChannelCompletionException(
        RequestResult result,
        string operationName
    )
    {
        //  A select-one channel reports NotFound when applying eligibility and
        //  drain left no member to pick. The send path and its connection are
        //  still there, so the spec names that Unavailable rather than NotFound
        //  (06-framework-api "no eligible select-one member"). NotFound stays
        //  for a named target, which the node-direct mapper below still covers.
        if (result == RequestResult.NotFound)
            return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"{operationName} failed because the channel had no eligible member.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                CreateRequestException(result)
            );
        return CreateCompletionException(result, operationName);
    }

    // Recognized fine codes retain their kind on every reply path.
    public static Exception CreateChannelCompletionException(
        RequestResult result,
        int failureErrno,
        string operationName
    )
    {
        if (ClassifyFineFailure(failureErrno) is { } kind)
            return new ZLinkFrameworkException(
                kind,
                operationName,
                innerException: CreateRequestException(result)
            )
            {
                FrameworkFailureCode = failureErrno,
            };
        return CreateChannelCompletionException(result, operationName);
    }

    //  Ownership-aware remote-reply mapper. A remote request reply may carry a
    //  Framework fine failure code (ServiceWireConstants.FrameworkErrorCode) that
    //  refines the coarse terminal (spec 32-framework-error-model:81-118). This
    //  overload is REMOTE-only, exactly like the string-only overload below: every
    //  caller translates a peer reply header (C++ reply_header_exception), never a
    //  source-owned bounded resource. When the reply carried no fine code (errno 0
    //  = None) or an unrecognised one, it falls through to the coarse terminal map,
    //  which classifies remote owner and route conditions as Unavailable.
    public static Exception CreateCompletionException(
        RequestResult result,
        int failureErrno,
        string operationName
    )
    {
        if (ClassifyFineFailure(failureErrno) is not { } kind)
            return CreateCompletionException(result, operationName);
        return new ZLinkFrameworkException(
            kind,
            $"{operationName} failed with result '{result}' "
                + $"(framework error code {failureErrno}).",
            innerException: CreateRequestException(result)
        )
        {
            FrameworkFailureCode = failureErrno,
        };
    }

    //  Fine failure-code table shared with ZLinkBackendSpotNodeWrapper's
    //  join/create/destroy/close classification so both surfaces stay in lockstep
    //  (spec 32-framework-error-model). Returns null for None(0) or any code with
    //  no fine refinement, leaving the coarse terminal to classify.
    internal static ZLinkFrameworkErrorKind? ClassifyFineFailure(int failureErrno)
    {
        foreach (var row in WireFailureMappings)
            if (row.Receive && (int)row.Code == failureErrno)
                return row.Kind;
        return null;
    }

    internal static ZLinkFrameworkErrorKind RelocationFailureKind(FailureCode code) =>
        ClassifyFineFailure((int)code) ?? ZLinkFrameworkErrorKind.ProtocolError;

    public static ZLinkFrameworkException CreateCompletionException(
        RequestResult result,
        string operationName,
        Exception? cause = null
    )
    {
        return result switch
        {
            RequestResult.TimedOut => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                $"{operationName} timed out.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                cause ?? CreateRequestException(result)
            ),
            RequestResult.NotConnected => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"{operationName} failed because the target route is not connected.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                cause ?? CreateRequestException(result)
            ),
            RequestResult.NotFound => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"{operationName} failed because the target was not found.",
                innerException: cause ?? CreateRequestException(result)
            ),
            //  Spec 32-framework-error-model:99-103 — a terminal-only `Conflict`/
            //  `Busy` on a remote request reply reflects the target's queue/owner
            //  state, a resource this runtime does not own, so it is `Unavailable`
            //  and stays retryable via RetryAfterBackoff.
            RequestResult.Conflict or RequestResult.Busy => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"{operationName} was refused with a transient result '{result}'.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                cause ?? CreateRequestException(result)
            ),
            RequestResult.Rejected => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"{operationName} was rejected with result '{result}'.",
                ZLinkRetryAdvice.DoNotRetry,
                cause ?? CreateRequestException(result)
            ),
            RequestResult.Backpressured => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                $"{operationName} exceeded its admission deadline.",
                ZLinkRetryAdvice.DoNotRetry,
                cause ?? CreateRequestException(result)
            ),
            RequestResult.ProtocolError => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ProtocolError,
                $"{operationName} failed with a protocol error.",
                innerException: cause ?? CreateRequestException(result)
            ),
            RequestResult.Terminated => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                $"{operationName} failed because the runtime is shutting down.",
                innerException: cause ?? CreateRequestException(result)
            ),
            RequestResult.InvalidArgument or RequestResult.InvalidState =>
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"{operationName} failed because the operation is invalid in the current state.",
                    innerException: cause ?? CreateRequestException(result)
                ),
            RequestResult.NotSupported or RequestResult.InternalError =>
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InternalFailure,
                    $"{operationName} failed with result '{result}'.",
                    innerException: cause ?? CreateRequestException(result)
                ),
            _ => new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                $"{operationName} failed with result '{result}'."
            ),
        };
    }

    //  Channel selection raises NotFound when applying eligibility and drain
    //  left no member to pick. The send path and its connection are still
    //  there, so the spec ends that as Unavailable
    //  (06-framework-api "no eligible select-one member"). Node-direct callers
    //  keep CreateSubmitException, where NotFound still means a named target is
    //  absent.
    public static Exception CreateChannelSubmitException(
        ZlinkSubmitException error,
        string operationName
    )
    {
        if (error.Result == ZlinkSubmitException.ErrorCode.NotFound)
            return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Unavailable,
                $"{operationName} failed because the channel had no eligible member.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                error
            );
        return CreateSubmitException(error, operationName);
    }

    public static Exception CreateSubmitException(
        ZlinkSubmitException error,
        string operationName,
        bool completionFailure = false
    ) => ZLinkSubmitFailureMapper.CreateException(error, operationName, completionFailure);

    public static Exception CreateSubmitTimeoutException(
        Exception? lastSubmitFailure,
        string operationName
    )
    {
        if (lastSubmitFailure is ZlinkSubmitException submitError)
            return CreateSubmitException(submitError, operationName);

        return lastSubmitFailure is null
            ? new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                $"{operationName} timed out before the socket became writable.",
                ZLinkRetryAdvice.RetryAfterBackoff
            )
            : new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                $"{operationName} timed out before the socket became writable.",
                ZLinkRetryAdvice.RetryAfterBackoff,
                lastSubmitFailure
            );
    }

    public static ZLinkFrameworkException CreateTimedOutRequestException(
        string operationName,
        Exception? innerException = null
    ) =>
        new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.DeadlineExceeded,
            operationName,
            ZLinkRetryAdvice.RetryAfterBackoff,
            innerException
        );

    public static ZLinkFrameworkException CreateShutdownRequestException(
        string operationName,
        Exception? innerException = null
    ) =>
        new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.ShuttingDown,
            operationName,
            innerException: innerException
        );

    private static ZlinkRequestException CreateRequestException(RequestResult result)
    {
        return new ZlinkRequestException((ZlinkRequestException.ErrorCode)(int)result);
    }
}
