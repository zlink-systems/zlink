using System.Text;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Systems.Zlink;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class ChannelEnvelopeHandoffTests
{
    [Fact]
    public async Task ClientServerReplyAddsNoCallbackAllocationToTheRequestPipeline()
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Off);
        var errors = new ZLinkDispatchErrorReporter(
            registration.DispatchOptions,
            Microsoft.Extensions.Logging.Abstractions.NullLogger.Instance
        );
        var groups = new HashSet<string>(StringComparer.Ordinal);
        var pipeline = new ZLinkChannelRequestDispatchPipeline(
            null,
            new ZLinkHandlerRegistry([]),
            null!,
            _ => groups,
            registration.Codecs,
            errors
        );
        var dispatcher = new ZLinkClientServerDispatcher(
            null!,
            pipeline,
            registration.Codecs,
            static () => false,
            errors
        );
        using var received = Received.Create();
        var gate = new ZLinkChannelReplyGate();
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "work",
            "missing",
            "application/json",
            "correlation",
            null,
            null,
            null,
            null
        );
        Task Baseline() =>
            pipeline.DispatchAsync(
                "work",
                received.Parts,
                header,
                gate,
                static (_, _, _, _) => throw new InvalidOperationException(),
                static (gate, _) =>
                {
                    gate.TryInvoke(static () => { });
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            );
        ValueTask Dispatch() =>
            dispatcher.DispatchAsync(
                "work",
                null!,
                received,
                gate,
                uint.MaxValue,
                CancellationToken.None,
                (header, null)
            );
        for (var i = 0; i < 1000; i++)
        {
            await Baseline();
            await Dispatch();
        }
        // Both paths finish synchronously here. Measure the same pipeline and reply gate;
        // the dispatcher must add no per-record callback storage before the absent-token check.
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var i = 0; i < 1000; i++)
            await Baseline();
        var pipelineStorage = GC.GetAllocatedBytesForCurrentThread() - before;
        before = GC.GetAllocatedBytesForCurrentThread();
        for (var i = 0; i < 1000; i++)
            await Dispatch();
        Assert.Equal(pipelineStorage, GC.GetAllocatedBytesForCurrentThread() - before);
    }

    [Theory]
    [InlineData(false, false, false)]
    [InlineData(true, false, false)]
    [InlineData(false, true, false)]
    [InlineData(true, true, false)]
    [InlineData(false, false, true)]
    [InlineData(true, false, true)]
    public async Task ClientServer_dispatch_uses_the_ingress_header_or_error_after_frame_release(
        bool captureFlow,
        bool malformed,
        bool invalidFlow
    )
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.DispatchOptions.Diagnostics.SetLevel(
            captureFlow ? ZLinkDiagnosticsLevel.Normal : ZLinkDiagnosticsLevel.Off
        );
        var logger = new DispatchLogger();
        var errors = new ZLinkDispatchErrorReporter(registration.DispatchOptions, logger);
        var requestPipeline = new ZLinkChannelRequestDispatchPipeline(
            null,
            new ZLinkHandlerRegistry([]),
            null!,
            static _ => new HashSet<string>(StringComparer.Ordinal),
            registration.Codecs,
            errors
        );
        var dispatcher = new ZLinkClientServerDispatcher(
            null!,
            requestPipeline,
            registration.Codecs,
            () => captureFlow,
            errors
        );
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        poller.Add(dealer, PollEventFlags.PollCompletion, 1);
        var endpoint = $"inproc://header-handoff-{Guid.NewGuid():N}";
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        var flowId = invalidFlow ? "invalid-flow" : "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d";
        var metadata = malformed ? "{\"\":\"invalid\"}" : "{}";
        // Mixed-case field names exercise the codec's existing ordinal ignore-case contract.
        var json =
            "{\"FORMATMARKER\":242,\"KiNd\":1,\"CHANNELNAME\":\"work\","
            + "\"MESSAGENAME\":\"missing-handler\",\"CONTENTTYPE\":\"application/json\","
            + "\"CORRELATIONID\":\"handoff-request\",\"FLOWID\":\""
            + flowId
            + "\",\"FLOWORIGIN\":1,\"METADATA\":"
            + metadata
            + "}";
        using var headerFrame = Message.From(Encoding.UTF8.GetBytes(json));
        var replyTask = dealer.Request().Message(headerFrame).Async(CancellationToken.None).Reply;
        using var received = Received.Create();
        Assert.True(router.Recv(received));
        var envelope = dispatcher.ReadHeader(received.Parts);
        var rejected = malformed || (captureFlow && invalidFlow);
        if (rejected)
        {
            Assert.Null(envelope.Header);
            Assert.NotNull(envelope.Error);
            Assert.Equal("handoff-request", envelope.Error.Header.CorrelationId);
        }
        else
        {
            Assert.Null(envelope.Error);
            Assert.Equal("missing-handler", envelope.Header!.MessageName);
            Assert.Equal(captureFlow ? flowId : null, envelope.Header.FlowId);
        }
        // Releasing the frame makes any attempt to parse it again observable.
        received.Parts[0].Dispose();
        await dispatcher.DispatchAsync(
            "work",
            router,
            received,
            new ZLinkChannelReplyGate(),
            uint.MaxValue,
            CancellationToken.None,
            envelope
        );
        Assert.Equal(captureFlow ? (rejected ? 1 : 2) : 0, logger.Messages.Count);
        if (captureFlow && !rejected)
        {
            // Spec 26 §2.1: a missing handler produces an error reply terminal.
            Assert.Contains("phase=replied", logger.Messages[^1], StringComparison.Ordinal);
        }
        var events = new PollEvent[1];
        Assert.True(poller.Wait(events, TimeSpan.FromSeconds(3)) > 0);
        var reply = await replyTask;
        try
        {
            var error = ZLinkEnvelopeCodec.DecodeHeader(reply);
            Assert.Equal(ZLinkMessageKind.Error, error.Kind);
            Assert.Equal("handoff-request", error.CorrelationId);
            Assert.Equal(
                ZLinkErrorWireNames.Name(
                    rejected
                        ? ZLinkFrameworkErrorKind.ProtocolError
                        : ZLinkFrameworkErrorKind.NotFound
                ),
                error.ErrorCode
            );
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(reply);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Fanout_dispatch_uses_the_ingress_header_or_error_after_frame_release(
        bool malformed
    )
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var probe = new PublishProbe();
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .BuildServiceProvider();
        var registry = new ZLinkHandlerRegistry([
            new ZLinkHandlerEndpointDescriptor(
                ZLinkMessageKind.Publish,
                "handoff-event",
                typeof(PublishHandler),
                static (target, message, _, _, _, _) =>
                {
                    var handler = (PublishHandler)target;
                    handler.Probe.Calls++;
                    handler.Probe.Value = ((PublishPayload)message!).Value;
                    return null;
                },
                [ZLinkHandlerArgumentKind.Message],
                typeof(PublishPayload),
                null,
                null,
                false,
                new HashSet<string>(StringComparer.Ordinal),
                "work"
            ),
        ]);
        var logger = new DispatchLogger();
        var dispatcher = new ZLinkFanoutPacketDispatcher(
            registry,
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration
            ),
            registration,
            null,
            logger
        );
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var publisher = context.CreateXPubSocket();
        using var subscriber = context.CreateSubSocket();
        publisher.Options.ReceiveTimeout = TimeSpan.FromSeconds(3);
        subscriber.Options.ReceiveTimeout = TimeSpan.FromSeconds(3);
        var endpoint = $"inproc://fanout-header-handoff-{Guid.NewGuid():N}";
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription("events");
        Assert.True(publisher.ReceiveSubscriptionEvent(new SubscriptionEvent()));
        var messageName = malformed ? "" : "handoff-event";
        var json =
            "{\"formatMarker\":242,\"kind\":4,\"channelName\":\"work\","
            + "\"messageName\":\""
            + messageName
            + "\",\"contentType\":\"application/json\",\"topic\":\"events\"}";
        using var headerFrame = Message.From(Encoding.UTF8.GetBytes(json));
        using var bodyFrame = Message.From("{\"value\":42}"u8);
        publisher.Publish("events").Messages([headerFrame, bodyFrame]).Submit();
        using var received = new TopicMessage();
        Assert.True(subscriber.Subscribe(received));
        var envelope = dispatcher.ReadHeader(received.Parts);
        Assert.Equal(malformed, envelope.Error is not null);
        received.Parts[0].Dispose();
        await dispatcher.DispatchEventMessageAsync(
            "work",
            received,
            CancellationToken.None,
            envelope
        );
        Assert.True(probe.Calls == (malformed ? 0 : 1), string.Join("; ", logger.Messages));
        if (malformed)
            Assert.Single(logger.Messages);
        else
        {
            Assert.Equal(42, probe.Value);
            Assert.Empty(logger.Messages);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Empty_fanout_reports_missing_header_once_without_throwing(bool readAtIngress)
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var logger = new DispatchLogger();
        var dispatcher = new ZLinkFanoutPacketDispatcher(
            new ZLinkHandlerRegistry([]),
            null!,
            registration,
            null,
            logger
        );
        using var received = new TopicMessage();
        Assert.Empty(received.Parts);
        if (readAtIngress)
        {
            // The receive loop reads the header and passes the result to dispatch.
            var envelope = dispatcher.ReadHeader(received.Parts);
            Assert.Null(envelope.Header);
            Assert.NotNull(envelope.Error);
            Assert.Equal(ZLinkEnvelopeCodec.MissingHeader().Message, envelope.Error.Message);
            Assert.Empty(logger.Messages);
            await dispatcher.DispatchEventMessageAsync(
                "work",
                received,
                CancellationToken.None,
                envelope
            );
        }
        else
        {
            await dispatcher.DispatchEventMessageAsync("work", received, CancellationToken.None);
        }
        var report = Assert.Single(logger.Messages);
        Assert.Contains(ZLinkEnvelopeCodec.MissingHeader().Message, report);
        Assert.Contains(nameof(ZLinkEnvelopeProtocolException), report);
        Assert.Contains("invalid_frame", report);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Empty_ClientServer_preserves_the_original_missing_part_exception(
        bool readAtIngress
    )
    {
        var registration = new ZLinkFrameworkRegistration();
        var dispatcher = new ZLinkClientServerDispatcher(
            null!,
            null!,
            registration.Codecs,
            static () => false
        );
        using var received = Received.Create();
        Assert.Empty(received.Parts);
        var error = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
        {
            if (readAtIngress)
                dispatcher.ReadHeader(received.Parts);
            else
                await dispatcher.DispatchAsync(
                    "work",
                    null!,
                    received,
                    new ZLinkChannelReplyGate(),
                    uint.MaxValue,
                    CancellationToken.None
                );
        });
        Assert.Equal("ZLink envelope header part is missing.", error.Message);
    }

    private sealed record PublishPayload(int Value);

    private sealed class PublishHandler(PublishProbe probe)
    {
        public PublishProbe Probe { get; } = probe;
    }

    private sealed class PublishProbe
    {
        public int Calls;
        public int Value;
    }

    private sealed class DispatchLogger : ILogger<ZLinkFanoutPacketDispatcher>
    {
        public List<string> Messages { get; } = [];

        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter
        ) => Messages.Add(formatter(state, exception));
    }
}
