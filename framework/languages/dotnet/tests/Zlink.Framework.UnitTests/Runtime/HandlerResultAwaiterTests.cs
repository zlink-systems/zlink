using Zlink.Framework.Runtime.Codecs;

namespace Zlink.Framework.UnitTests;

public sealed class HandlerResultAwaiterTests
{
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
        var preparedHeaderCount = ZLinkEnvelopeCodec.SnapshotCacheCounts().SimpleHeaders;
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
        Assert.Equal(preparedHeaderCount, ZLinkEnvelopeCodec.SnapshotCacheCounts().SimpleHeaders);
    }

    public ValueTask<int> PreparedReply() => new(42);

    [Fact]
    public async Task InvocationPreservesCancelableTokensAndReusesTheUncancelableArgument()
    {
        object? Invoke(CancellationToken token) =>
            ZLinkHandlerInvocationEngine
                .InvokeAsync(
                    this,
                    static (_, argument, _, _, _, _) => argument,
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
    public async Task AwaitAsync_Returns_ValueTaskOfT_Result()
    {
        object result = new ValueTask<int>(42);

        var awaited = await ZLinkHandlerResultAwaiter.AwaitAsync(result);

        Assert.Equal(42, awaited);
    }
}
