using Systems.Zlink.Framework.Runtime.Protocol;
using FailureCode = Systems.Zlink.Framework.Runtime.Protocol.ServiceWireConstants.FrameworkErrorCode;

namespace Zlink.Framework.Runtime.Messaging;

internal static class ZLinkRequestFailureMapper
{
    internal enum FailureContext
    {
        General,
        ActorTarget,
        ActorRelocation,
        SpotRelocation,
        SpotControl,
        ActorCreate,
        ActorDestroy,
        ActorJoin,
        RelocationReceive,
    }

    internal readonly record struct WireFailureMapping(
        ZLinkFrameworkErrorKind? Kind,
        FailureCode Code,
        RequestResult Result,
        FailureContext Context,
        bool Receive,
        bool Send
    );

    // General rows own outgoing representatives and incoming aliases. Context rows
    // preserve the existing object-control and relocation wire contracts.
    private static readonly WireFailureMapping[] WireFailureMappings =
    [
        new(
            ZLinkFrameworkErrorKind.NotFound,
            FailureCode.RequestTargetNotFound,
            RequestResult.NotFound,
            FailureContext.General,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.AlreadyExists,
            FailureCode.ActorAlreadyExists,
            RequestResult.Conflict,
            FailureContext.General,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.TypeMismatch,
            FailureCode.SpotTypeMismatch,
            RequestResult.Conflict,
            FailureContext.General,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.Rejected,
            FailureCode.RequestRejected,
            RequestResult.Rejected,
            FailureContext.General,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            FailureCode.RouteNotConnected,
            RequestResult.InternalError,
            FailureContext.General,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.DeadlineExceeded,
            FailureCode.WorkerTimedOut,
            RequestResult.InternalError,
            FailureContext.General,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.ProtocolError,
            FailureCode.RequestProtocolError,
            RequestResult.ProtocolError,
            FailureContext.General,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.DataLost,
            FailureCode.RelocationDataLost,
            RequestResult.InternalError,
            FailureContext.General,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.InternalFailure,
            FailureCode.RequestFailed,
            RequestResult.InternalError,
            FailureContext.General,
            true,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.ShuttingDown,
            FailureCode.None,
            RequestResult.Terminated,
            FailureContext.General,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            FailureCode.None,
            RequestResult.InvalidState,
            FailureContext.General,
            false,
            true
        ),
        // NotConfigured has no general representative; it uses the existing fallback.
        new(
            null,
            FailureCode.RequestFailed,
            RequestResult.InternalError,
            FailureContext.General,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.NotFound,
            FailureCode.ActorRouteNotFound,
            RequestResult.NotFound,
            FailureContext.General,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.NotFound,
            FailureCode.HandlerNotFound,
            RequestResult.NotFound,
            FailureContext.General,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.TypeMismatch,
            FailureCode.ActorTypeMismatch,
            RequestResult.Conflict,
            FailureContext.General,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            FailureCode.ActorSessionNotBound,
            RequestResult.NotFound,
            FailureContext.General,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.Rejected,
            FailureCode.ActorCreateRejected,
            RequestResult.Rejected,
            FailureContext.General,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.ProtocolError,
            FailureCode.PayloadDecodeFailed,
            RequestResult.ProtocolError,
            FailureContext.General,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            FailureCode.ActorLocationStale,
            RequestResult.Conflict,
            FailureContext.General,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            FailureCode.WorkerQueueFull,
            RequestResult.Rejected,
            FailureContext.General,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            FailureCode.SpotMoving,
            RequestResult.Conflict,
            FailureContext.General,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            FailureCode.SpotGenerationStale,
            RequestResult.Conflict,
            FailureContext.General,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.InternalFailure,
            FailureCode.WorkerFailed,
            RequestResult.InternalError,
            FailureContext.General,
            true,
            false
        ),
        new(
            ZLinkFrameworkErrorKind.TypeMismatch,
            FailureCode.ActorTypeMismatch,
            RequestResult.Conflict,
            FailureContext.ActorTarget,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            FailureCode.ActorLocationStale,
            RequestResult.Conflict,
            FailureContext.ActorRelocation,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.NotConfigured,
            FailureCode.HandlerNotFound,
            RequestResult.NotFound,
            FailureContext.ActorRelocation,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.ShuttingDown,
            FailureCode.RequestFailed,
            RequestResult.InternalError,
            FailureContext.ActorRelocation,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            FailureCode.SpotGenerationStale,
            RequestResult.Conflict,
            FailureContext.SpotRelocation,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.NotConfigured,
            FailureCode.HandlerNotFound,
            RequestResult.NotFound,
            FailureContext.SpotRelocation,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.ShuttingDown,
            FailureCode.RequestFailed,
            RequestResult.InternalError,
            FailureContext.SpotRelocation,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.TypeMismatch,
            FailureCode.ActorTypeMismatch,
            RequestResult.Conflict,
            FailureContext.ActorRelocation,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            FailureCode.SpotGenerationStale,
            RequestResult.Conflict,
            FailureContext.SpotControl,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            FailureCode.SpotMoving,
            RequestResult.Conflict,
            FailureContext.SpotControl,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.TypeMismatch,
            FailureCode.SpotTypeMismatch,
            RequestResult.Conflict,
            FailureContext.SpotControl,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.AlreadyExists,
            FailureCode.SpotCreateFailed,
            RequestResult.InternalError,
            FailureContext.SpotControl,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.ProtocolError,
            FailureCode.RequestProtocolError,
            RequestResult.ProtocolError,
            FailureContext.SpotControl,
            false,
            true
        ),
        new(
            null,
            FailureCode.RequestFailed,
            RequestResult.InternalError,
            FailureContext.SpotControl,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.TypeMismatch,
            FailureCode.ActorTypeMismatch,
            RequestResult.Conflict,
            FailureContext.ActorCreate,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.AlreadyExists,
            FailureCode.ActorAlreadyExists,
            RequestResult.Conflict,
            FailureContext.ActorCreate,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.ProtocolError,
            FailureCode.RequestProtocolError,
            RequestResult.ProtocolError,
            FailureContext.ActorCreate,
            false,
            true
        ),
        new(
            null,
            FailureCode.ActorCreateFailed,
            RequestResult.InternalError,
            FailureContext.ActorCreate,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.NotFound,
            FailureCode.ActorRouteNotFound,
            RequestResult.NotFound,
            FailureContext.ActorDestroy,
            false,
            true
        ),
        new(
            null,
            FailureCode.ActorLocationStale,
            RequestResult.Conflict,
            FailureContext.ActorDestroy,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.TypeMismatch,
            FailureCode.ActorTypeMismatch,
            RequestResult.Conflict,
            FailureContext.ActorJoin,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.ProtocolError,
            FailureCode.RequestProtocolError,
            RequestResult.ProtocolError,
            FailureContext.ActorJoin,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            FailureCode.ActorLocationStale,
            RequestResult.Conflict,
            FailureContext.ActorJoin,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.NotFound,
            FailureCode.ActorRouteNotFound,
            RequestResult.NotFound,
            FailureContext.ActorJoin,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            FailureCode.RouteNotConnected,
            RequestResult.InternalError,
            FailureContext.ActorJoin,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.Rejected,
            FailureCode.RequestRejected,
            RequestResult.Rejected,
            FailureContext.ActorJoin,
            false,
            true
        ),
        new(
            null,
            FailureCode.RequestFailed,
            RequestResult.InternalError,
            FailureContext.ActorJoin,
            false,
            true
        ),
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            FailureCode.None,
            RequestResult.InvalidState,
            FailureContext.RelocationReceive,
            true,
            false
        ),
    ];

    internal static ReadOnlySpan<WireFailureMapping> Mappings => WireFailureMappings;

    public static (RequestResult Result, FailureCode FailureCode) TargetFailureReply(
        Exception error,
        byte objectKind = 2,
        FailureContext context = FailureContext.General
    )
    {
        if (context == FailureContext.General && objectKind == 1)
            context = FailureContext.ActorTarget;
        var kind = error is ZLinkFrameworkException framework
            ? framework.Kind
            : (ZLinkFrameworkErrorKind?)null;
        if (
            error is Zlink.Framework.Runtime.Locations.ZLinkRelocationDataLostException
            && context is FailureContext.ActorRelocation or FailureContext.SpotRelocation
        )
            kind = ZLinkFrameworkErrorKind.DataLost;
        WireFailureMapping? contextFallback = null;
        WireFailureMapping? generalMapping = null;
        WireFailureMapping? generalFallback = null;
        foreach (var row in WireFailureMappings)
        {
            if (!row.Send)
                continue;
            if (row.Context == context)
            {
                if (row.Kind == kind)
                    return (row.Result, row.Code);
                if (row.Kind is null)
                    contextFallback = row;
            }
            if (row.Context == FailureContext.General)
            {
                if (row.Kind == kind)
                    generalMapping = row;
                if (row.Kind is null)
                    generalFallback = row;
            }
        }
        var mapped =
            contextFallback
            ?? generalMapping
            ?? generalFallback
            ?? throw new InvalidOperationException(
                "The general failure fallback mapping is missing."
            );
        return (mapped.Result, mapped.Code);
    }

    public static FailureCode TargetFailureCode(
        Exception error,
        byte objectKind,
        FailureContext context = FailureContext.General
    ) => TargetFailureReply(error, objectKind, context).FailureCode;

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

    //  Terminal replies carry a fine failure code. A recognised code is the
    //  peer naming its own reason and keeps its kind, except a target-not-found
    //  code on a channel: a select-one call names no target, so "not found"
    //  there is the empty eligible set the coarse rule above already covers.
    public static Exception CreateChannelCompletionException(
        RequestResult result,
        int failureErrno,
        string operationName
    )
    {
        if (
            (ServiceWireConstants.FrameworkErrorCode)failureErrno
                != ServiceWireConstants.FrameworkErrorCode.RequestTargetNotFound
            && ClassifyFineFailure(failureErrno) is { } kind
        )
            return new ZLinkFrameworkException(
                kind,
                operationName,
                innerException: CreateRequestException(result)
            );
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
        );
    }

    //  Fine failure-code table shared with ZLinkBackendSpotNodeWrapper's
    //  join/create/destroy/close classification so both surfaces stay in lockstep
    //  (spec 32-framework-error-model). Returns null for None(0) or any code with
    //  no fine refinement, leaving the coarse terminal to classify.
    internal static ZLinkFrameworkErrorKind? ClassifyFineFailure(int failureErrno)
    {
        foreach (var row in WireFailureMappings)
            if (
                row.Receive
                && row.Context == FailureContext.General
                && (int)row.Code == failureErrno
            )
                return row.Kind;
        return null;
    }

    internal static ZLinkFrameworkErrorKind RelocationFailureKind()
    {
        // Existing relocation completion classifies all fine codes identically.
        foreach (var row in WireFailureMappings)
            if (row.Receive && row.Context == FailureContext.RelocationReceive)
                return row.Kind!.Value;
        throw new InvalidOperationException("The relocation receive mapping is missing.");
    }

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
        bool completionFailure = true
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
