using System.Diagnostics;
using Google.Protobuf;
using Google.Protobuf.WellKnownTypes;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Codecs.Protobuf;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class TargetNodeRouteCostTests(Xunit.Abstractions.ITestOutputHelper output)
{
    [Fact]
    public async Task MalformedNodeHeaderAllocatesOnlyOneDecodeFailure()
    {
        await using var fixture = new TargetFixture();
        using var malformed = Message.From("{");
        const int calls = 128;
        var singleDecode = PerCallAllocation.MinWindow(
            () =>
            {
                using var record = Record(malformed);
                try
                {
                    ZLinkEnvelopeCodec.DecodeHeader(record.Parts, false);
                }
                catch (ZLinkEnvelopeProtocolException) { }
            },
            calls,
            warmup: 64
        );
        var dispatch = PerCallAllocation.MinWindow(
            () => fixture.Dispatch([Record(malformed)]),
            calls,
            warmup: 64
        );
        output.WriteLine(
            $"malformed: single decode={singleDecode / calls} B, dispatch={dispatch / calls} B"
        );
        // The extra allowance covers the batch container, not a second parser/exception graph.
        Assert.True(
            dispatch <= singleDecode + calls * 64L,
            $"Malformed dispatch allocated {dispatch / calls} B, single decode {singleDecode / calls} B."
        );
    }

    [Fact]
    public async Task ReplySubmissionReturnsFailureAndDisposesBothParts()
    {
        var failure = new InvalidOperationException("reply failure");
        using var received = new ZLinkBackendRouteReceived(
            Array.Empty<Message>(),
            null,
            null,
            1,
            _ => throw failure
        );
        var header = Message.From("header");
        var body = Message.From("body");
        var completion = ZLinkSpotReplySubmitter.SubmitDirectAsync(received, [header, body]);
        Assert.True(completion.IsFaulted);
        Assert.Same(
            failure,
            await Assert.ThrowsAsync<InvalidOperationException>(() => completion.AsTask())
        );
        Assert.Throws<ObjectDisposedException>(() => header.Size);
        Assert.Throws<ObjectDisposedException>(() => body.Size);
    }

    [Theory]
    [InlineData(1024)]
    [InlineData(4096)]
    public async Task SynchronousTargetRequestCost(int size)
    {
        await using var fixture = new TargetFixture();
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "mesh",
            "Echo",
            "application/x-protobuf",
            "request-1",
            null,
            null,
            null,
            null
        );
        var payload = new BytesValue { Value = ByteString.CopyFrom(new byte[size]) };
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            header,
            payload,
            typeof(BytesValue),
            fixture.Registration.Codecs
        );
        try
        {
            using var frame = ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipartMessage(
                parts
            );
            Assert.True(
                ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipartView(
                    frame,
                    out var view
                )
            );
            const int headerCalls = 8192;
            for (var i = 0; i < 256; i++)
                _ = ZLinkEnvelopeCodec.DecodeRequestHeader(parts[0].AsReadOnlySpan(), false);
            var headerStart = Stopwatch.GetTimestamp();
            for (var i = 0; i < headerCalls; i++)
                _ = ZLinkEnvelopeCodec.DecodeRequestHeader(parts[0].AsReadOnlySpan(), false);
            output.WriteLine(
                $"request header {size}: {Stopwatch.GetElapsedTime(headerStart).TotalMicroseconds / headerCalls:F3} us/msg"
            );
            var replies = 0;
            Func<IReadOnlyList<Message>, SubmitResult> reply = _ =>
            {
                replies++;
                return SubmitResult.Ok;
            };
            void Dispatch() =>
                fixture.Dispatch([
                    new ZLinkBackendRouteReceived(
                        Array.Empty<Message>(),
                        default(RoutingId),
                        null,
                        1,
                        reply,
                        applicationPayloadView: view
                    ),
                ]);
            for (var i = 0; i < 256; i++)
                Dispatch();
            const int calls = 4096;
            var times = new List<double>();
            for (var window = 0; window < 5; window++)
            {
                var start = Stopwatch.GetTimestamp();
                for (var i = 0; i < calls; i++)
                    Dispatch();
                times.Add(Stopwatch.GetElapsedTime(start).TotalMicroseconds / calls);
            }
            const int allocationCalls = 128;
            var bytes = PerCallAllocation.MinWindow(Dispatch, allocationCalls);
            times.Sort();
            using var replyRecord = new ZLinkBackendRouteReceived(
                Array.Empty<Message>(),
                default(RoutingId),
                null,
                1,
                static _ => SubmitResult.Ok
            );
            var replyStart = Stopwatch.GetTimestamp();
            for (var i = 0; i < calls; i++)
            {
                var replyHeader = ZLinkChannelReplyWriter.CreateReplyHeader(
                    ZLinkMessageKind.Response,
                    "mesh",
                    header
                );
                var replyParts = ZLinkEnvelopeCodec.EncodeParts(
                    replyHeader,
                    payload,
                    typeof(BytesValue),
                    fixture.Registration.Codecs
                );
                ZLinkSpotReplySubmitter
                    .SubmitDirectAsync(replyRecord, replyParts)
                    .GetAwaiter()
                    .GetResult();
            }
            output.WriteLine(
                $"reply encode+submit {size}: {Stopwatch.GetElapsedTime(replyStart).TotalMicroseconds / calls:F3} us/msg"
            );
            output.WriteLine(
                $"target {size}: {times[times.Count / 2]:F3} us/msg, {bytes / allocationCalls} B/msg"
            );
            Assert.Equal(256 + calls * 5 + allocationCalls * PerCallAllocation.Windows, replies);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static ZLinkBackendRouteReceived Record(Message header) =>
        new([Message.From(header)], default(RoutingId), null, null, null);

    private sealed class Echo : IZLinkRouteRequestHandler<BytesValue, BytesValue>
    {
        public ValueTask<BytesValue> HandleAsync(
            BytesValue request,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(request);
    }

    private sealed class TargetFixture : IAsyncDisposable
    {
        private readonly ServiceProvider _services = new ServiceCollection()
            .AddTransient<Echo>()
            .BuildServiceProvider();
        private readonly ZLinkRuntimeTaskRunner _runner = new(
            new FailureSink(),
            CancellationToken.None
        );
        internal ZLinkFrameworkRegistration Registration { get; } = new();
        internal ZLinkMeshNodeRouteDispatcher Dispatcher { get; }
        private readonly List<Task> _pending = new();

        internal void Dispatch(IReadOnlyList<ZLinkBackendRouteReceived> batch)
        {
            Dispatcher.DispatchBatch(batch, _pending, CancellationToken.None);
            if (_pending.Count != 0)
                throw new InvalidOperationException("The synchronous fixture suspended.");
        }

        internal TargetFixture()
        {
            Registration.Codecs.Use(ZLinkProtobufCodec.Default);
            var node = new ZLinkSpotNodeRegistration
            {
                SpotNodeName = "mesh",
                RoutingId = RoutingId.From("target"),
            };
            node.RouteRequestHandlers.Add(
                new ZLinkRouteHandlerRegistration(
                    typeof(Echo),
                    typeof(BytesValue),
                    typeof(BytesValue),
                    "Echo"
                )
            );
            var runtime = new ZLinkFrameworkRuntime(
                _services,
                null!,
                Registration,
                new ZLinkHandlerRegistry([]),
                new ZLinkHandlerDispatcher(
                    _services.GetRequiredService<IServiceScopeFactory>(),
                    Registration
                )
            );
            Dispatcher = ZLinkMeshNodeRouteDispatcher.Create(
                _services,
                Registration,
                node,
                runtime,
                _runner
            )!;
        }

        public async ValueTask DisposeAsync()
        {
            await Dispatcher.DisposeAsync();
            await _runner.StopAsync();
            await _services.DisposeAsync();
        }
    }

    private sealed class FailureSink
        : Zlink.Framework.Runtime.Diagnostics.IZLinkRuntimeFailureReporter
    {
        public void ReportHandlerException(Exception exception) => throw exception;

        public void ReportUnhandledCallbackException(Exception exception) => throw exception;

        public void ReportRuntimeTaskException(string name, Exception exception) => throw exception;
    }
}
