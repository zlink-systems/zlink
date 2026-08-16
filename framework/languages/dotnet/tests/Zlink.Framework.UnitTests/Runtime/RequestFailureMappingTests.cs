namespace Zlink.Framework.UnitTests;

public sealed class RequestFailureMappingTests
{
    [Fact]
    public void Malformed_Envelope_Header_Is_A_Protocol_Error()
    {
        using var header = Message.From("{");

        Assert.Throws<ZLinkEnvelopeProtocolException>(() => ZLinkEnvelopeCodec.DecodeHeader(header));
    }

    [Fact]
    public void Undefined_Envelope_Message_Kind_Is_A_Protocol_Error()
    {
        var invalid = new ZLinkEnvelopeHeader(
            (ZLinkMessageKind)99,
            "route",
            "Lookup",
            ZLinkEnvelopeCodec.DefaultContentType,
            null,
            null,
            null,
            null,
            null)
        {
            FormatMarker = 0xF2
        };
        using var encoded = ZLinkEnvelopeCodec.EncodePart(invalid);

        Assert.Throws<ZLinkEnvelopeProtocolException>(() => ZLinkEnvelopeCodec.DecodeHeader(encoded));
    }

    [Fact]
    public void Spot_Error_Envelope_Preserves_Framework_Error_Kind()
    {
        var parts = ZLinkSpotReplyEnvelope.EncodeErrorParts(
            "spot",
            "Lookup",
            "correlation",
            new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Rejected, "draining"));
        try
        {
            var reply = ZLinkEnvelopeCodec.DecodeHeader(parts);
            Assert.Equal(nameof(ZLinkFrameworkErrorKind.Rejected), reply.ErrorCode);
            var error = Assert.IsType<ZLinkFrameworkException>(
                ZLinkEnvelopeErrorMapper.CreateException(reply, "fallback"));
            Assert.Equal(ZLinkFrameworkErrorKind.Rejected, error.Kind);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    [Fact]
    public void ErrorEnvelope_Preserves_Framework_Error_Kind()
    {
        var request = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "route",
            "Lookup",
            ZLinkEnvelopeCodec.DefaultContentType,
            "correlation",
            null,
            null,
            null,
            null);
        var reply = ZLinkChannelReplyWriter.CreateErrorHeader(
            "route",
            request,
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "draining"));

        Assert.Equal(nameof(ZLinkFrameworkErrorKind.Rejected), reply.ErrorCode);
        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkEnvelopeErrorMapper.CreateException(reply, "fallback"));
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, error.Kind);
        Assert.Equal("draining", error.Message);
    }

    [Fact]
    public void EnvelopeCompletion_Maps_NotConnected_To_RouteNotConnected()
    {
        Exception? observed = null;
        var reply = Array.Empty<Message>();

        ZLinkEnvelopeReplyCompletion.Complete<string>(
            RequestResult.NotConnected,
            reply,
            _ => throw new InvalidOperationException("Completion should not succeed."),
            error => observed = error,
            "test request");

        var error = Assert.IsType<ZLinkFrameworkException>(observed);
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.IsType<ZlinkRequestException>(error.InnerException);
    }

    [Fact]
    public void ChannelCompletion_Preserves_Native_NotFound()
    {
        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateChannelCompletionException(
                RequestResult.NotFound,
                "channel request"));

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        Assert.IsType<ZlinkRequestException>(error.InnerException);
    }

    [Fact]
    public void ChannelSubmit_Preserves_Native_NotFound()
    {
        var error = ZLinkSubmitFailureMapper.CreateChannelException(
            SubmitResult.NotFound,
            "channel 'game.api'");

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        Assert.Equal(ZLinkRetryAdvice.DoNotRetry, error.RetryAdvice);
    }

    [Fact]
    public void ChannelSubmit_Maps_Native_NotConnected_To_Unavailable_Route()
    {
        var error = ZLinkSubmitFailureMapper.CreateChannelException(
            SubmitResult.NotConnected,
            "channel 'game.api'");

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.Equal(ZLinkRetryAdvice.RetryAfterBackoff, error.RetryAdvice);
    }

    [Fact]
    public void Completion_Backpressure_Maps_To_CapacityExceeded()
    {
        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateCompletionException(
                RequestResult.Backpressured,
                "completion"));

        Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, error.Kind);
        Assert.Equal(ZLinkRetryAdvice.RetryAfterBackoff, error.RetryAdvice);
    }

    [Fact]
    public void RawCompletion_Maps_NotFound_To_RequestTargetNotFound()
    {
        Exception? observed = null;
        var reply = Array.Empty<Message>();

        ZLinkRawReplyCompletion.Complete(
            RequestResult.NotFound,
            reply,
            _ => throw new InvalidOperationException("Completion should not succeed."),
            error => observed = error,
            "raw request");

        var error = Assert.IsType<ZLinkFrameworkException>(observed);
        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        Assert.IsType<ZlinkRequestException>(error.InnerException);
    }

    [Fact]
    public void RawCompletion_Maps_TimedOut_To_DeadlineExceeded()
    {
        Exception? observed = null;
        var reply = Array.Empty<Message>();

        ZLinkRawReplyCompletion.Complete(
            RequestResult.TimedOut,
            reply,
            _ => throw new InvalidOperationException("Completion should not succeed."),
            error => observed = error,
            "raw request");

        var error = Assert.IsType<ZLinkFrameworkException>(observed);
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
    }

    [Theory]
    [InlineData(RequestResult.Terminated, ZLinkFrameworkErrorKind.ShuttingDown)]
    [InlineData(RequestResult.NotFound, ZLinkFrameworkErrorKind.NotFound)]
    [InlineData(RequestResult.TimedOut, ZLinkFrameworkErrorKind.DeadlineExceeded)]
    [InlineData(RequestResult.ProtocolError, ZLinkFrameworkErrorKind.ProtocolError)]
    [InlineData(RequestResult.Rejected, ZLinkFrameworkErrorKind.Rejected)]
    [InlineData(RequestResult.NotConnected, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData(RequestResult.Backpressured, ZLinkFrameworkErrorKind.CapacityExceeded)]
    [InlineData(RequestResult.InvalidArgument, ZLinkFrameworkErrorKind.InvalidOperation)]
    [InlineData(RequestResult.InvalidState, ZLinkFrameworkErrorKind.InvalidOperation)]
    [InlineData(RequestResult.NotSupported, ZLinkFrameworkErrorKind.InternalFailure)]
    [InlineData(RequestResult.InternalError, ZLinkFrameworkErrorKind.InternalFailure)]
    [InlineData(RequestResult.Conflict, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData(RequestResult.Busy, ZLinkFrameworkErrorKind.Unavailable)]
    public void Completion_Maps_Native_Result_To_Framework_Error(
        RequestResult result,
        ZLinkFrameworkErrorKind expected)
    {
        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateCompletionException(
                result,
                "request"));

        Assert.Equal(expected, error.Kind);
    }

    //  Ownership-aware remote-reply classification: a fine failure code refines
    //  the coarse terminal (spec 32-framework-error-model:81-118). These rows are
    //  the discriminators that encode the ownership ruling — a source-owned bound
    //  would classify differently, but every caller of this overload is a remote
    //  reply header.
    [Theory]
    //  actorAlreadyExists(3) -> AlreadyExists (fine code wins over the terminal).
    [InlineData(RequestResult.Busy, 3, ZLinkFrameworkErrorKind.AlreadyExists)]
    [InlineData(RequestResult.Busy, 4, ZLinkFrameworkErrorKind.TypeMismatch)]
    [InlineData(RequestResult.Busy, 7, ZLinkFrameworkErrorKind.TypeMismatch)]
    //  actorSessionNotBound(8) -> InvalidOperation (the row the private table omitted).
    [InlineData(RequestResult.Busy, 8, ZLinkFrameworkErrorKind.InvalidOperation)]
    [InlineData(RequestResult.NotFound, 9, ZLinkFrameworkErrorKind.NotFound)]
    [InlineData(RequestResult.Busy, 13, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData(RequestResult.Busy, 16, ZLinkFrameworkErrorKind.ProtocolError)]
    //  workerQueueFull(18) on a remote reply is the target's queue -> Unavailable,
    //  NOT CapacityExceeded (which is reserved for placement/admission capacity).
    [InlineData(RequestResult.Busy, 18, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData(RequestResult.Busy, 19, ZLinkFrameworkErrorKind.DeadlineExceeded)]
    [InlineData(RequestResult.Busy, 33, ZLinkFrameworkErrorKind.InvalidOperation)]
    [InlineData(RequestResult.Busy, 35, ZLinkFrameworkErrorKind.DataLost)]
    public void Completion_FineFailureCode_Refines_Terminal(
        RequestResult result,
        int failureErrno,
        ZLinkFrameworkErrorKind expected)
    {
        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateCompletionException(
                result,
                failureErrno,
                "request"));

        Assert.Equal(expected, error.Kind);
        Assert.IsType<ZlinkRequestException>(error.InnerException);
    }

    [Theory]
    //  None(0): placement Backpressured(113) with no fine code stays
    //  CapacityExceeded — the fine table must not swallow it.
    [InlineData(RequestResult.Backpressured, 0, ZLinkFrameworkErrorKind.CapacityExceeded)]
    //  None(0): a remote Conflict/Busy with no fine code stays the coarse
    //  Unavailable (remote owner/queue state).
    [InlineData(RequestResult.Conflict, 0, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData(RequestResult.Busy, 0, ZLinkFrameworkErrorKind.Unavailable)]
    //  An unrecognised fine code falls through to the coarse terminal.
    [InlineData(RequestResult.Backpressured, 9999, ZLinkFrameworkErrorKind.CapacityExceeded)]
    public void Completion_FineFailureCode_FallsThrough_To_Coarse_Terminal(
        RequestResult result,
        int failureErrno,
        ZLinkFrameworkErrorKind expected)
    {
        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateCompletionException(
                result,
                failureErrno,
                "request"));

        Assert.Equal(expected, error.Kind);
    }

    [Theory]
    [InlineData(SubmitResult.NotAdmitted, ZLinkFrameworkErrorKind.Rejected)]
    [InlineData(SubmitResult.Terminated, ZLinkFrameworkErrorKind.ShuttingDown)]
    [InlineData(SubmitResult.NotFound, ZLinkFrameworkErrorKind.NotFound)]
    public void Submit_Maps_Native_Result_To_Framework_Error(
        SubmitResult result,
        ZLinkFrameworkErrorKind expected)
    {
        var error = ZLinkSubmitFailureMapper.CreateException(result, "request");

        Assert.Equal(expected, error.Kind);
    }

    [Theory]
    [InlineData(ZlinkSubmitException.ErrorCode.NotAdmitted, ZLinkFrameworkErrorKind.Rejected)]
    [InlineData(ZlinkSubmitException.ErrorCode.InvalidState, ZLinkFrameworkErrorKind.InvalidOperation)]
    [InlineData(ZlinkSubmitException.ErrorCode.InvalidArgument, ZLinkFrameworkErrorKind.InvalidOperation)]
    [InlineData(ZlinkSubmitException.ErrorCode.InvalidHandle, ZLinkFrameworkErrorKind.InvalidOperation)]
    [InlineData(ZlinkSubmitException.ErrorCode.ThreadViolation, ZLinkFrameworkErrorKind.InvalidOperation)]
    [InlineData(ZlinkSubmitException.ErrorCode.NotSupported, ZLinkFrameworkErrorKind.InternalFailure)]
    [InlineData(ZlinkSubmitException.ErrorCode.InternalError, ZLinkFrameworkErrorKind.InternalFailure)]
    public void SubmitException_Maps_Native_ErrorCode_To_Framework_Error(
        ZlinkSubmitException.ErrorCode code,
        ZLinkFrameworkErrorKind expected)
    {
        var error = Assert.IsType<ZLinkFrameworkException>(
            ZLinkRequestFailureMapper.CreateSubmitException(
                new ZlinkSubmitException(code),
                "request"));

        Assert.Equal(expected, error.Kind);
    }

    [Theory]
    [InlineData(true, SubmitResult.NotConnected)]
    [InlineData(false, SubmitResult.Terminated)]
    public void Native_Terminated_Submit_Is_Unavailable_Only_While_Source_Is_Serving(
        bool sourceAcceptsApplicationOperations,
        SubmitResult expected)
    {
        var result = ZLinkManagedMeshNode.NormalizeNativeSubmitFailure(
            ZlinkSubmitException.ErrorCode.Terminated,
            sourceAcceptsApplicationOperations);

        Assert.Equal(expected, result);
    }

    [Theory]
    [InlineData(true, RequestResult.NotConnected)]
    [InlineData(false, RequestResult.Terminated)]
    public void Native_Terminated_Request_Is_Unavailable_Only_While_Source_Is_Serving(
        bool sourceAcceptsApplicationOperations,
        RequestResult expected)
    {
        var result = ZLinkManagedMeshNode.NormalizeNativeRequestFailure(
            ZlinkRequestException.ErrorCode.Terminated,
            sourceAcceptsApplicationOperations);

        Assert.Equal(expected, result);
    }

    [Fact]
    public async Task SpotRouteNativeReply_CancellationDisposesLateOkReply()
    {
        using var cancellation = new CancellationTokenSource();
        using var completion = new ZLinkNativeReplyCompletion<RequestResult>(cancellation.Token);

        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => completion.Task);

        using var lateReply = Message.From("late-route-cancellation-reply");
        completion.Complete(RequestResult.Ok, [lateReply]);

        Assert.Throws<ObjectDisposedException>(() => lateReply.AsReadOnlySpan());
    }

    [Fact]
    public async Task SpotRouteNativeReply_TimeoutDisposesLateOkReply()
    {
        using var completion = new ZLinkNativeReplyCompletion<RequestResult>(
            CancellationToken.None,
            TimeSpan.Zero,
            "route request timed out");

        await Assert.ThrowsAsync<TimeoutException>(() => completion.Task);

        using var lateReply = Message.From("late-route-timeout-reply");
        completion.Complete(RequestResult.Ok, [lateReply]);

        Assert.Throws<ObjectDisposedException>(() => lateReply.AsReadOnlySpan());
    }

    //  Spec 32-framework-error-model:91-92 — an Ok lifecycle record whose reply
    //  lacks the operation-specific completion cannot be processed:
    //  ProtocolError, never InternalFailure via the coarse terminal map.
    //  Non-OK records keep the ownership-aware terminal/fine classification
    //  (codex round-11 findings 3 and 6).
    [Theory]
    [InlineData((int)RequestResult.Ok, 0, ZLinkFrameworkErrorKind.ProtocolError)]
    [InlineData((int)RequestResult.Ok, 18, ZLinkFrameworkErrorKind.ProtocolError)]
    [InlineData((int)RequestResult.Busy, 0, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData((int)RequestResult.TimedOut, 0, ZLinkFrameworkErrorKind.DeadlineExceeded)]
    [InlineData((int)RequestResult.Terminated, 0, ZLinkFrameworkErrorKind.ShuttingDown)]
    [InlineData((int)RequestResult.Backpressured, 0, ZLinkFrameworkErrorKind.CapacityExceeded)]
    [InlineData((int)RequestResult.Conflict, 18, ZLinkFrameworkErrorKind.Unavailable)]
    [InlineData((int)RequestResult.Conflict, 19, ZLinkFrameworkErrorKind.DeadlineExceeded)]
    public void Lifecycle_Failure_Classifies_Ok_Missing_Completion_As_Protocol_Error(
        int terminalResult, int failureErrno, ZLinkFrameworkErrorKind expected)
    {
        Assert.Equal(
            expected,
            ZLinkBackendSpotNodeWrapper.MapLifecycleFailure(
                terminalResult, failureErrno));
    }

    [Fact]
    public async Task SpotRouteNativeReply_NormalWinnerTransfersOwnershipAndDisposesDuplicateReply()
    {
        using var completion = new ZLinkNativeReplyCompletion<RequestResult>(CancellationToken.None);
        using var winner = Message.From("route-winner");
        using var duplicate = Message.From("route-duplicate");

        completion.Complete(RequestResult.Ok, [winner]);
        var result = await completion.Task;
        completion.Complete(RequestResult.Ok, [duplicate]);

        Assert.Equal(RequestResult.Ok, result.Result);
        Assert.Same(winner, Assert.Single(result.Reply));
        Assert.Equal("route-winner", winner.GetString());
        Assert.Throws<ObjectDisposedException>(() => duplicate.AsReadOnlySpan());
    }

    //  Pins the generated schema terminal-failure-integrity predicate
    //  (service-wire-v1.schema.json; spec 51-internal-service-wire-protocol:
    //  43-47): success is ok+none, boundary terminals carry none, typed
    //  failures must match their exact schema terminal, unknown or reserved
    //  failure codes are invalid. NOTE: the predicate is not yet wired into
    //  TryDecodeReply — live .NET producers still emit the schema-illegal
    //  TimedOut(101)+WorkerTimedOut(19) pair (see the 101+19 row below), which
    //  must be fixed across languages before decode-side enforcement lands.
    [Theory]
    [InlineData(0u, 0u, true)]          // ok + none
    [InlineData(102u, 9u, true)]        // notFound + handlerNotFound
    [InlineData(105u, 17u, true)]       // internalError + requestFailed
    [InlineData(105u, 19u, true)]       // internalError + workerTimedOut
    [InlineData(106u, 18u, true)]       // rejected + workerQueueFull
    [InlineData(104u, 16u, true)]       // protocolError + requestProtocolError
    [InlineData(107u, 33u, true)]       // conflict + spotGenerationStale
    [InlineData(108u, 0u, true)]        // busy boundary + none
    [InlineData(113u, 0u, true)]        // backpressured boundary + none
    [InlineData(104u, 3u, false)]       // protocolError + actorAlreadyExists
    [InlineData(102u, 18u, false)]      // notFound + workerQueueFull
    [InlineData(108u, 5u, false)]       // boundary busy + spotCreateFailed
    [InlineData(0u, 9u, false)]         // ok + non-none
    [InlineData(101u, 19u, false)]      // boundary timedOut + workerTimedOut
    [InlineData(105u, 23u, false)]      // reserved failure code
    [InlineData(105u, 26u, false)]      // reserved failure code
    [InlineData(105u, 99u, false)]      // unknown failure code
    public void Schema_Terminal_Failure_Integrity_Predicate_Matches_The_Schema(
        uint terminal, uint failureCode, bool valid)
    {
        Assert.Equal(
            valid,
            Systems.Zlink.Framework.Runtime.Protocol.ServiceWireConstants
                .ValidTerminalFailure(terminal, failureCode));
    }
}
