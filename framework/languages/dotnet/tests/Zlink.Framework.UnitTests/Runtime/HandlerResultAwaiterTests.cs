using Zlink.Framework.Runtime.Codecs;

namespace Zlink.Framework.UnitTests;

public sealed class HandlerResultAwaiterTests(Xunit.Abstractions.ITestOutputHelper output)
{
    private static readonly object ReferenceReply = new();
    private static readonly Task<object> CompletedReferenceReply = Task.FromResult(ReferenceReply);
    private object? _lastReply;

    [Theory]
    [InlineData(nameof(SynchronousReferenceReply))]
    [InlineData(nameof(CompletedTaskReply))]
    [InlineData(nameof(CompletedValueTaskReply))]
    [InlineData(nameof(VoidReply))]
    [InlineData(nameof(CompletedValueTask))]
    public void PreparedInvocation_DoesNotBoxAwaitableResults(string methodName)
    {
        var method = GetType().GetMethod(methodName)!;
        var invoker = ZLinkHandlerMethodInvokerFactory.Create(method);
        void Invoke() =>
            _lastReply = ZLinkHandlerInvocationEngine
                .InvokeAsync(this, invoker)
                .GetAwaiter()
                .GetResult();
        var bytes = PerCallAllocation.MinWindow(Invoke, 1024, warmup: 256);
        output.WriteLine($"{methodName}: {bytes / 1024.0} B/invocation");
        Assert.Equal(
            methodName is nameof(VoidReply) or nameof(CompletedValueTask) ? null : ReferenceReply,
            _lastReply
        );
        Assert.Equal(0, bytes);
    }

    public object SynchronousReferenceReply() => ReferenceReply;

    public Task<object> CompletedTaskReply() => CompletedReferenceReply;

    public ValueTask<object> CompletedValueTaskReply() => new(ReferenceReply);

    public void VoidReply() { }

    public ValueTask CompletedValueTask() => ValueTask.CompletedTask;

    private ValueTask<object> _pendingReply;

    public ValueTask<object> PendingReferenceReply() => _pendingReply;

    public Task? NullTaskReply() => null;

    public object BoxedAwaitableReply() => CompletedReferenceReply;

    [Theory]
    [InlineData(false, "result")]
    [InlineData(true, "result")]
    [InlineData(false, "error")]
    [InlineData(true, "error")]
    [InlineData(false, "cancel")]
    [InlineData(true, "cancel")]
    public async Task PreparedInvocation_ConsumesValueTaskSourceExactlyOnce(
        bool completed,
        string terminal
    )
    {
        using var cancellation = new CancellationTokenSource();
        await cancellation.CancelAsync();
        var source = new ReplySource();
        _pendingReply = source.Reply;
        var failure = new InvalidOperationException("handler failure");
        void Complete()
        {
            if (terminal == "result")
                source.Complete(ReferenceReply);
            else
                source.Fail(
                    terminal == "cancel"
                        ? new OperationCanceledException(cancellation.Token)
                        : failure
                );
        }
        if (completed)
            Complete();
        var invoker = ZLinkHandlerMethodInvokerFactory.Create(
            GetType().GetMethod(nameof(PendingReferenceReply))!
        );
        var invocation = ZLinkHandlerInvocationEngine.InvokeAsync(this, invoker).AsTask();
        if (!completed)
        {
            Assert.False(invocation.IsCompleted);
            Complete();
        }
        if (terminal == "result")
            Assert.Same(ReferenceReply, await invocation);
        else if (terminal == "error")
            Assert.Same(
                failure,
                await Assert.ThrowsAsync<InvalidOperationException>(() => invocation)
            );
        else
        {
            var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => invocation);
            Assert.Equal(cancellation.Token, error.CancellationToken);
        }
        Assert.Equal(1, source.ResultsRead);
    }

    [Theory]
    [InlineData(nameof(NullTaskReply))]
    [InlineData(nameof(BoxedAwaitableReply))]
    public async Task PreparedInvocation_UsesTheDeclaredResultShape(string name)
    {
        var invoker = ZLinkHandlerMethodInvokerFactory.Create(GetType().GetMethod(name)!);
        var result = await ZLinkHandlerInvocationEngine.InvokeAsync(this, invoker);
        if (name == nameof(NullTaskReply))
            Assert.Null(result);
        else
            Assert.Same(CompletedReferenceReply, result);
    }

    private sealed class ReplySource : System.Threading.Tasks.Sources.IValueTaskSource<object>
    {
        private System.Threading.Tasks.Sources.ManualResetValueTaskSourceCore<object> _source =
            new() { RunContinuationsAsynchronously = true };
        internal int ResultsRead { get; private set; }
        internal ValueTask<object> Reply => new(this, _source.Version);

        internal void Complete(object value) => _source.SetResult(value);

        internal void Fail(Exception error) => _source.SetException(error);

        public object GetResult(short token)
        {
            ResultsRead++;
            return _source.GetResult(token);
        }

        public System.Threading.Tasks.Sources.ValueTaskSourceStatus GetStatus(short token) =>
            _source.GetStatus(token);

        public void OnCompleted(
            Action<object?> continuation,
            object? state,
            short token,
            System.Threading.Tasks.Sources.ValueTaskSourceOnCompletedFlags flags
        ) => _source.OnCompleted(continuation, state, token, flags);
    }

    [Fact]
    public async Task RegisteredInvoker_UsesPreparedResultWithoutDescriptorReinterpretation()
    {
        var method = typeof(HandlerResultAwaiterTests).GetMethod(nameof(PreparedReply))!;
        var invoker = ZLinkHandlerMethodInvokerFactory.Create(method);
        for (var index = 0; index < 4; index++)
        {
            var result = invoker(this, null, null, null, null, null);
            var prepared = Assert.IsType<ValueTask<object?>>(result);
            Assert.Equal(42, await prepared);
        }
    }

    [Fact]
    public void BoundReply_UsesPreparedCodecAndInvariantHeader()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        var selections = 0;
        codecs.AddSerializer(
            "application/prepared",
            ZLinkFrameworkJsonPayloadCodec.ReceiveSerializer,
            type =>
            {
                selections++;
                return type == typeof(int);
            }
        );
        var prepared = ZLinkEnvelopeCodec.PrepareResponse("prepared", typeof(int), codecs);
        Assert.Equal(1, selections);
        codecs.Freeze();
        for (var index = 0; index < 4; index++)
        {
            var request = new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Request,
                "prepared",
                "query",
                ZLinkEnvelopeCodec.DefaultContentType,
                index.ToString(),
                null,
                null,
                null,
                null
            );
            var response = ZLinkChannelReplyWriter.CreateReplyHeader(
                ZLinkMessageKind.Response,
                "prepared",
                request,
                prepared
            );
            var parts = ZLinkEnvelopeCodec.EncodeParts(response, 42, typeof(int), codecs);
            try
            {
                var decoded = ZLinkEnvelopeCodec.DecodeHeader(parts, false);
                Assert.Equal(index.ToString(), decoded.CorrelationId);
                Assert.Equal("application/prepared", decoded.ContentType);
                Assert.Equal(
                    42,
                    ZLinkEnvelopeCodec.DecodeBody(parts, typeof(int), decoded.ContentType, codecs)
                );
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(parts);
            }
        }
        Assert.Equal(1, selections);
    }

    public ValueTask<int> PreparedReply() => new(42);

    [Fact]
    public async Task InvocationPreservesCancelableTokensAndReusesTheUncancelableArgument()
    {
        object? Invoke(CancellationToken token) =>
            ZLinkHandlerInvocationEngine
                .InvokeAsync(
                    this,
                    static (_, argument, _, _, _, _) => new ValueTask<object?>(argument),
                    new[] { ZLinkHandlerArgumentKind.CancellationToken },
                    null,
                    null!,
                    token
                )
                .GetAwaiter()
                .GetResult();

        var first = Invoke(CancellationToken.None);
        var second = Invoke(default);
        Assert.Same(first, second);
        Assert.Equal(CancellationToken.None, Assert.IsType<CancellationToken>(first));
        using var cancellation = new CancellationTokenSource();
        var captured = Assert.IsType<CancellationToken>(Invoke(cancellation.Token));
        Assert.Equal(cancellation.Token, captured);
        await cancellation.CancelAsync();
        Assert.True(captured.IsCancellationRequested);
        Assert.True(
            Assert.IsType<CancellationToken>(Invoke(cancellation.Token)).IsCancellationRequested
        );
    }

    [Fact]
    public async Task PreparedInvoker_Returns_ValueTaskOfT_Result()
    {
        var invoker = ZLinkHandlerMethodInvokerFactory.Create(
            GetType().GetMethod(nameof(PreparedReply))!
        );
        Assert.Equal(42, await invoker(this, null, null, null, null, null));
    }
}
