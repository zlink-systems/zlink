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

    [Fact]
    public async Task SubmitRequestAsync_Fails_NotConnected_Without_Timeout()
    {
        await using var submitter = new ZLinkAsyncSubmitter(
            _ => { },
            TimeSpan.FromSeconds(30),
            CancellationToken.None,
            failFastNotConnected: static () => true);

        var task = submitter.SubmitRequestAsync<string>(
            Message.From("payload"),
            (_, _, _) => throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected));

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () => await task.AsTask());
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.IsType<ZlinkSubmitException>(error.InnerException);
    }

    [Fact]
    public async Task SubmitRequestAsync_Maps_Backpressure_Timeout_To_DeadlineExceeded()
    {
        await using var submitter = new ZLinkAsyncSubmitter(
            _ => { },
            TimeSpan.FromMilliseconds(20),
            CancellationToken.None);

        var task = submitter.SubmitRequestAsync<string>(
            Message.From("payload"),
            (_, _, _) => throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.Backpressured));

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () => await task.AsTask());
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
        Assert.IsType<ZlinkSubmitException>(error.InnerException);
    }
}
