using System.Diagnostics;
using System.Collections.Concurrent;
using System.Diagnostics.Metrics;
using System.Net;
using System.Net.Sockets;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Handlers;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests;

[Collection(RuntimeMetricsCollection.Name)]
public sealed partial class UnhandledDispatchPolicyTests
{
    [Fact]
    public async Task Dynamic_Spot_Subscription_Retries_Transient_Native_Attachment_Failures()
    {
        var registry = new ZLinkSpotSubscriptionRegistry();
        var nativeSpot = new CapturingSpot { SubscriptionFailuresRemaining = 2 };
        registry.Add("events", "events", typeof(TestSubscriptionHandler));

        await registry.BindAsync(
            new TestSubscriptionSpot(),
            nativeSpot,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        Assert.Equal(3, nativeSpot.SubscriptionAttempts);
    }

    [Fact]
    public async Task SpotSubscription_MalformedUnknownTopic_DoesNotEmitPublishFlowMonitoring()
    {
        var result = await ObserveUnknownSpotSubscriptionAsync(malformed: true);

        Assert.Null(result.Received);
        Assert.Null(result.Terminal);
        Assert.Equal(0, result.DispatchCount);
        Assert.Empty(result.LogMessages);
    }

    [Fact]
    public async Task SpotSubscription_OffModeUnknownTopic_DoesNotEmitPublishFlowMonitoring()
    {
        var result = await ObserveUnknownSpotSubscriptionAsync(malformed: false);

        Assert.Null(result.Received);
        Assert.Null(result.Terminal);
        Assert.Equal(0, result.DispatchCount);
        Assert.Empty(result.LogMessages);
    }

    [Fact]
    public async Task SpotSubscription_Fanout_Instances_Keep_One_Flow_And_Owner_Skip_Uses_The_Same_Identity()
    {
        var observer = new CapturingMessageFlowObserver();
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
        var services = new ServiceCollection().BuildServiceProvider();
        var runner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), CancellationToken.None);
        var reporter = new ZLinkDispatchErrorReporter(options);
        var logger = new CapturingLogger<ZLinkSpotSubscriptionRegistry>();
        var registryA = new ZLinkSpotSubscriptionRegistry();
        var registryB = new ZLinkSpotSubscriptionRegistry();
        var spotA = new CapturingSpot();
        var spotB = new CapturingSpot();
        var applicationProbe = new TestSubscriptionProbe();
        var owner = new TestSubscriptionSpot(true, applicationProbe);
        var nonOwner = new TestSubscriptionSpot(false, applicationProbe);
        registryA.Add("events", "events.child", typeof(TestSubscriptionHandler));
        registryB.Add("events", "events.child", typeof(TestSubscriptionHandler));
        registryA.Bind(owner, spotA);
        registryB.Bind(nonOwner, spotB);

        var backendFactory = new ZLinkDotNetBackendAdapterFactory();
        var channelAdapter = backendFactory.CreateChannelAdapter();
        await using var context = channelAdapter.CreateContext();
        await using var publisher = channelAdapter.CreatePublisherSocket(context);
        await using var subscriberA = channelAdapter.CreateSubscriberSocket(context);
        await using var subscriberB = channelAdapter.CreateSubscriberSocket(context);
        var endpoint = GetTcpEndpoint();
        publisher.Bind(endpoint);
        subscriberA.Connect(endpoint);
        subscriberB.Connect(endpoint);
        subscriberA.SetSubscription("events.child");
        subscriberB.SetSubscription("events.child");
        spotA.SubscribeHandler = subscriberA.Subscribe;
        spotB.SubscribeHandler = subscriberB.Subscribe;

        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Publish,
            "play",
            nameof(TestSubscriptionEvent),
            ZLinkEnvelopeCodec.DefaultContentType,
            "fanout-correlation",
            null,
            "events.child",
            null,
            null,
            "source-rid")
        {
            FlowId = TestFlowId,
            FlowOrigin = ZLinkFlowOrigin.Application
        };
        var dispatchA = 0;
        var dispatchB = 0;
        try
        {
            for (var attempt = 0; attempt < 100 && (dispatchA == 0 || dispatchB == 0); attempt++)
            {
                var parts = ZLinkEnvelopeCodec.EncodeParts(
                    header,
                    new TestSubscriptionEvent("payload"),
                    typeof(TestSubscriptionEvent),
                    null);
                try
                {
                    publisher.Publish("events.child", parts, SendFlags.None);
                }
                finally
                {
                    ZLinkMessageParts.DisposeAll(parts);
                }

                await Task.Delay(5);
                await registryA.DrainAsync(
                    spotA,
                    null,
                    reporter,
                    logger,
                    async (_, body, context, cancellationToken) =>
                    {
                        dispatchA++;
                        await new TestSubscriptionHandler().HandleAsync(
                            owner,
                            Assert.IsType<TestSubscriptionEvent>(body),
                            context,
                            cancellationToken);
                    },
                    CancellationToken.None);
                await registryB.DrainAsync(
                    spotB,
                    null,
                    reporter,
                    logger,
                    async (_, body, context, cancellationToken) =>
                    {
                        dispatchB++;
                        await new TestSubscriptionHandler().HandleAsync(
                            nonOwner,
                            Assert.IsType<TestSubscriptionEvent>(body),
                            context,
                            cancellationToken);
                    },
                    CancellationToken.None);
            }

            Assert.True(dispatchA > 0);
            Assert.True(dispatchB > 0);
            Assert.True(applicationProbe.OwnerHandled > 0);
            Assert.True(applicationProbe.NonOwnerSkipped > 0);
            Assert.Empty(observer.Events);
        }
        finally
        {
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task ActorJoinMissingHandler_OffMode_KeepsTelemetryButSuppressesLog()
    {
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) => ActivitySamplingResult.AllDataAndRecorded
        };
        var activities = new List<Activity>();
        listener.ActivityStopped = activities.Add;
        ActivitySource.AddActivityListener(listener);
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Off);
        using var runtimeServices = CreateDispatchRuntime(out var runtime);
        var nativeSpot = new CapturingSpot();
        var logger = new CapturingLogger<ZLinkSpotActorJoinDispatcher>();
        var dispatcher = new ZLinkSpotActorJoinDispatcher(
            runtime,
            nativeSpot,
            "entry",
            new ZLinkSpotActorJoinRegistry(),
            new ZLinkSpotActorMembership(),
            static () => throw new InvalidOperationException("Handler invoker should not be used."),
            logger,
            dispatchErrors: new ZLinkDispatchErrorReporter(options));
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Request,
                "entry",
                "JoinReq",
                ZLinkEnvelopeCodec.DefaultContentType,
                "join-request-1",
                null,
                null,
                null,
                null),
            new { Value = "join" },
            typeof(object),
            null);
        var request = new ZLinkBackendActorJoinRequest(
            new ZLinkBackendActorRef(RoutingId.From("source-node"), "source-actor", 1),
            new ZLinkBackendActorRef(RoutingId.From("target-node"), "target-actor", 1),
            RoutingId.From("source-node"),
            "target-spot",
            1,
            parts[0],
            parts);

        try
        {
            await dispatcher.DispatchAsync(request, CancellationToken.None);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }

        Assert.Equal(1, nativeSpot.LastJoinResultCode);
        Assert.Empty(logger.Messages);
        Assert.Empty(activities);
    }

    [Fact]
    public async Task ActorJoinMissingHandler_RepliesRejected_AndLogsReason()
    {
        using var runtimeServices = CreateDispatchRuntime(out var runtime);
        var nativeSpot = new CapturingSpot();
        var logger = new CapturingLogger<ZLinkSpotActorJoinDispatcher>();
        var dispatcher = new ZLinkSpotActorJoinDispatcher(
            runtime,
            nativeSpot,
            "entry",
            new ZLinkSpotActorJoinRegistry(),
            new ZLinkSpotActorMembership(),
            static () => throw new InvalidOperationException("Handler invoker should not be used."),
            logger);
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Request,
                "entry",
                "JoinReq",
                ZLinkEnvelopeCodec.DefaultContentType,
                "join-request-2",
                null,
                null,
                null,
                null),
            new { Value = "join" },
            typeof(object),
            null);
        var request = new ZLinkBackendActorJoinRequest(
            new ZLinkBackendActorRef(RoutingId.From("source-node"), "source-actor", 1),
            new ZLinkBackendActorRef(RoutingId.From("target-node"), "target-actor", 1),
            RoutingId.From("source-node"),
            "target-spot",
            1,
            parts[0],
            parts);

        try
        {
            await dispatcher.DispatchAsync(request, CancellationToken.None);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }

        Assert.Equal(1, nativeSpot.LastJoinResultCode);
        Assert.Contains(logger.Messages, message => message.Contains("no-join-handler", StringComparison.Ordinal));
    }

    [Fact]
    public async Task HandlerMissing_EmitsTrace()
    {
        ZLinkTelemetry.SetDiagnosticsLevel(ZLinkDiagnosticsLevel.Errors);
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) => ActivitySamplingResult.AllDataAndRecorded
        };
        var activities = new List<Activity>();
        listener.ActivityStopped = activities.Add;
        ActivitySource.AddActivityListener(listener);
        using var runtimeServices = CreateDispatchRuntime(out var runtime);
        var nativeSpot = new CapturingSpot();
        var dispatcher = new ZLinkSpotActorJoinDispatcher(
            runtime,
            nativeSpot,
            "entry",
            new ZLinkSpotActorJoinRegistry(),
            new ZLinkSpotActorMembership(),
            static () => throw new InvalidOperationException("Handler invoker should not be used."),
            new CapturingLogger<ZLinkSpotActorJoinDispatcher>());
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Request,
                "entry",
                "JoinReq",
                ZLinkEnvelopeCodec.DefaultContentType,
                "join-request-3",
                null,
                null,
                null,
                null),
            new { Value = "join" },
            typeof(object),
            null);
        var request = new ZLinkBackendActorJoinRequest(
            new ZLinkBackendActorRef(RoutingId.From("source-node"), "source-actor", 1),
            new ZLinkBackendActorRef(RoutingId.From("target-node"), "target-actor", 1),
            RoutingId.From("source-node"),
            "target-spot",
            1,
            parts[0],
            parts);

        try
        {
            await dispatcher.DispatchAsync(request, CancellationToken.None);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }

        Assert.Contains(activities, activity =>
            activity.OperationName == "zlink.actor.dispatch"
            && activity.Tags.Any(tag =>
                tag.Key == "zlink.reason" && tag.Value == "no_handler"));
    }

    [Fact]
    public async Task ChannelPublishDecodeFailure_DoesNotStopOtherEndpointTypes()
    {
        var dropReasons = new ConcurrentQueue<string>();
        using var metricListener = new MeterListener
        {
            InstrumentPublished = (instrument, listener) =>
            {
                if (instrument.Meter.Name == ZLinkMeters.Framework
                    && instrument.Name == "zlink.mesh_node.messages.dropped")
                    listener.EnableMeasurementEvents(instrument);
            }
        };
        metricListener.SetMeasurementEventCallback<long>((_, _, tags, _) =>
        {
            foreach (var tag in tags)
                if (tag.Key == "reason" && tag.Value is string reason)
                    dropReasons.Enqueue(reason);
        });
        metricListener.Start();

        var probe = new PublishProbe();
        var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddTransient<CapturingPublishHandler>()
            .BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        registration.Filters.Add(typeof(CapturingFanoutFilter));
        var observer = new CapturingMessageFlowObserver();
        registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
        var runner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), CancellationToken.None);
        var registry = new ZLinkHandlerRegistry([
            new ZLinkHandlerEndpointDescriptor(
                ZLinkMessageKind.Publish,
                "SharedEvent",
                typeof(NeverInvokedHandler),
                static (_, _, _, _, _, _) => null,
                [ZLinkHandlerArgumentKind.Message],
                typeof(int),
                null,
                null,
                false,
                new HashSet<string>(StringComparer.Ordinal),
                "play"),
            new ZLinkHandlerEndpointDescriptor(
                ZLinkMessageKind.Publish,
                "SharedEvent",
                typeof(CapturingPublishHandler),
                static (target, message, _, _, _, _) =>
                    ((CapturingPublishHandler)target).Handle((TestPublishedEvent)message!),
                [ZLinkHandlerArgumentKind.Message],
                typeof(TestPublishedEvent),
                null,
                null,
                false,
                new HashSet<string>(StringComparer.Ordinal),
                "play")
        ]);
        var pipeline = new ZLinkChannelPublishDispatchPipeline(
            registry,
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration),
            static _ => new HashSet<string>(StringComparer.Ordinal),
            LogLevel.Warning,
            new ZLinkDispatchErrorReporter(registration.DispatchOptions),
            registration.Codecs,
            new CapturingLogger<ZLinkChannelPublishDispatchPipeline>());
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Publish,
            "play",
            "SharedEvent",
            ZLinkEnvelopeCodec.DefaultContentType,
            null,
            null,
            "events",
            null,
            null);
        var backendFactory = new ZLinkDotNetBackendAdapterFactory();
        var channelAdapter = backendFactory.CreateChannelAdapter();
        await using var context = channelAdapter.CreateContext();
        await using var publisher = channelAdapter.CreatePublisherSocket(context);
        await using var subscriber = channelAdapter.CreateSubscriberSocket(context);
        var endpoint = GetTcpEndpoint();
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription(string.Empty);
        using var topicMessage = new TopicMessage();

        var timeout = Stopwatch.StartNew();
        var received = false;
        while (timeout.Elapsed < TimeSpan.FromSeconds(2))
        {
            var parts = ZLinkEnvelopeCodec.EncodeParts(
                header,
                new TestPublishedEvent("delivered"),
                typeof(TestPublishedEvent),
                null);
            try
            {
                publisher.Publish("events", parts, SendFlags.None);
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(parts);
            }

            if (subscriber.Subscribe(topicMessage, RecvFlags.DontWait))
            {
                received = true;
                break;
            }

            await Task.Delay(10);
        }

        Assert.True(received, "The publish test message was not received.");
        await pipeline.DispatchAsync("play", topicMessage, header, CancellationToken.None);

        Assert.Equal("delivered", probe.Value);
        Assert.Equal(
            ZLinkHandlerDispatchKind.ClassicFanout,
            probe.FilterDispatchKind);
        Assert.Null(probe.FilterMeshName);
        Assert.Empty(observer.Events);
        Assert.Empty(dropReasons);
        await runner.StopAsync();
    }

    [Fact]
    public async Task DispatchErrorReporter_DeliversMessageFlowErrorSnapshot()
    {
        var observer = new CapturingMessageFlowObserver();
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
        var services = new ServiceCollection().BuildServiceProvider();
        var runner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), CancellationToken.None);
        var reporter = new ZLinkDispatchErrorReporter(
            options);
        var error = new ZLinkDispatchFailure(
            ZLinkDispatchErrorSurface.Channel,
            ZLinkDispatchMessageKind.Request,
            ZLinkDispatchErrorReason.HandlerMissing,
            ZLinkDispatchErrorAction.ReplyError,
            "MissingReq",
            "api",
            CorrelationId: "corr-1");

        reporter.Report(error);

        var observed = await observer.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("failed", observed.Outcome);
        Assert.Equal("channel", observed.Surface);
        Assert.Equal("request", observed.MessageKind);
        Assert.Equal("no_handler", observed.Reason);
        Assert.Equal("reply_error", observed.Action);
        Assert.Equal("MissingReq", observed.PacketName);
        Assert.Equal("api", observed.ChannelName);
        Assert.Equal("corr-1", observed.CorrelationId);
        await runner.StopAsync();
    }

    [Fact]
    public async Task ReplyPathMissing_ReportsFailCallerMessageFlowError()
    {
        var observer = new CapturingMessageFlowObserver();
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
        var services = new ServiceCollection().BuildServiceProvider();
        var runner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), CancellationToken.None);
        var reporter = new ZLinkDispatchErrorReporter(options);
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.SpotActor,
            "SpotActor",
            ZLinkDispatchMessageKind.ActorRequest,
            "ActorRequest",
            "MissingReply",
            actorId: "actor-1");

        scope.ReplyPathMissing(
            NullLogger.Instance,
            reporter,
            new InvalidOperationException("The handler returned no reply."));

        var observed = await observer.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("failed", observed.Outcome);
        Assert.Equal("reply_path_missing", observed.Reason);
        Assert.Equal("fail_caller", observed.Action);
        Assert.Equal("MissingReply", observed.PacketName);
        Assert.Equal("actor-1", observed.ActorId);
        await runner.StopAsync();
    }

    [Fact]
    public async Task SpotActorSendMissingHandler_LogsAndReportsMessageFlowDroppedEvent()
    {
        var observer = new CapturingMessageFlowObserver();
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
        var services = new ServiceCollection().BuildServiceProvider();
        var runner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), CancellationToken.None);
        var logger = new CapturingLogger<ZLinkSpotActorPacketDispatcher>();
        var dispatcher = new ZLinkSpotActorPacketDispatcher(
            static () => new ZLinkSpotActorHandlerRegistry(ZLinkSpotActorHandlerSurface.UserSpot),
            static () => throw new InvalidOperationException("Handler invoker should not be used."),
            new ZLinkDispatchErrorReporter(
                options,
                logger),
            logger);
        var actor = new TestActor("actor-1");
        var runtimeState = new ZLinkActorRuntimeState(actor.ActorId);
        runtimeState.BindActorInstance(actor);
        using var body = Message.From("payload");
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            "missing-actor-send",
            ZlinkStreamMetadata.Empty);

        await dispatcher.DispatchAsync(actor, runtimeState, header, body, CancellationToken.None);

        var observed = await observer.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("dropped", observed.Phase);
        Assert.Equal("actor", observed.Surface);
        Assert.Equal("send", observed.MessageKind);
        Assert.Equal("no_handler", observed.Reason);
        Assert.Equal("drop", observed.Action);
        Assert.Equal("missing-actor-send", observed.PacketName);
        Assert.Equal("actor-1", observed.ActorId);
        Assert.Contains(logger.Messages, message => message.Contains("phase=dropped", StringComparison.Ordinal)
                                                    && message.Contains("errorReason=HandlerMissing", StringComparison.Ordinal));
        await runner.StopAsync();
    }

    [Fact]
    public async Task SpotActorSendMalformedPayload_ReportsPayloadDecodeFailureBeforeHandlerInvocation()
    {
        var observer = new CapturingMessageFlowObserver();
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
        var probe = new TestActorSendProbe();
        using var services = new ServiceCollection()
            .AddSingleton(probe)
            .BuildServiceProvider();
        var runner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), CancellationToken.None);
        var logger = new CapturingLogger<ZLinkSpotActorPacketDispatcher>();
        var registry = new ZLinkSpotActorHandlerRegistry(
            ZLinkSpotActorHandlerSurface.UserSpot,
            typeof(TestActorSpot));
        registry.AddPacket(
            typeof(TestActorSendHandler),
            typeof(TestActor),
            "malformed-actor-send");
        registry.Bind();
        await using var handlerInstances = new ZLinkScopedHandlerInstanceOwner(services);
        var invoker = new ZLinkSpotHandlerInvoker(
            handlerInstances,
            new TestActorSpot(),
            new ZLinkCodecRegistryBuilder(),
            null);
        var dispatcher = new ZLinkSpotActorPacketDispatcher(
            () => registry,
            () => invoker,
            new ZLinkDispatchErrorReporter(
                options,
                logger),
            logger);
        var actor = new TestActor("actor-1");
        var runtimeState = new ZLinkActorRuntimeState(actor.ActorId);
        runtimeState.BindActorInstance(actor);
        using var body = Message.From("{");
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "malformed-actor-send",
            ZlinkStreamMetadata.Empty);

        await dispatcher.DispatchAsync(actor, runtimeState, header, body, CancellationToken.None);

        var observed = await observer.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal("dropped", observed.Phase);
        Assert.Equal("actor", observed.Surface);
        Assert.Equal("send", observed.MessageKind);
        Assert.Equal("decode_error", observed.Reason);
        Assert.Equal("drop", observed.Action);
        Assert.Equal("malformed-actor-send", observed.PacketName);
        Assert.Equal("actor-1", observed.ActorId);
        Assert.Equal(0, probe.InvocationCount);
        await runner.StopAsync();
    }

    private static async Task<Received> ReceiveAsync(
        ZLinkBackendRouterSocketWrapper router,
        TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            var received = router.Recv(RecvFlags.DontWait);
            if (received is not null) return received;

            await Task.Yield();
        }

        throw new TimeoutException("Timed out waiting for router request.");
    }

    private const string TestFlowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d";

    private static async Task<SpotSubscriptionObservation> ObserveUnknownSpotSubscriptionAsync(
        bool malformed)
    {
        var observer = new CapturingMessageFlowObserver();
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
        var services = new ServiceCollection().BuildServiceProvider();
        var runner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), CancellationToken.None);
        var reporter = new ZLinkDispatchErrorReporter(options);
        var logger = new CapturingLogger<ZLinkSpotSubscriptionRegistry>();
        var registry = new ZLinkSpotSubscriptionRegistry();
        var nativeSpot = new CapturingSpot();
        registry.Add("events", "events", typeof(TestSubscriptionHandler));
        await registry.BindAsync(
            new TestSubscriptionSpot(),
            nativeSpot,
            TimeSpan.FromSeconds(1),
            CancellationToken.None);

        var backendFactory = new ZLinkDotNetBackendAdapterFactory();
        var channelAdapter = backendFactory.CreateChannelAdapter();
        await using var context = channelAdapter.CreateContext();
        await using var publisher = channelAdapter.CreatePublisherSocket(context);
        await using var subscriber = channelAdapter.CreateSubscriberSocket(context);
        var endpoint = GetTcpEndpoint();
        publisher.Bind(endpoint);
        subscriber.Connect(endpoint);
        subscriber.SetSubscription("events");
        nativeSpot.SubscribeHandler = subscriber.Subscribe;

        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Publish,
            "play",
            nameof(TestSubscriptionEvent),
            ZLinkEnvelopeCodec.DefaultContentType,
            "subscription-correlation",
            null,
            "events.child",
            null,
            null,
            "source-rid")
        {
            FlowId = TestFlowId,
            FlowOrigin = ZLinkFlowOrigin.Application
        };
        IReadOnlyList<Message> CreateParts()
        {
            if (!malformed)
                return ZLinkEnvelopeCodec.EncodeParts(
                    header,
                    new TestSubscriptionEvent("payload"),
                    typeof(TestSubscriptionEvent),
                    null);

            return
            [
                Message.From(System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(
                    header,
                    ZLinkJsonSerializerOptions.Default)),
                Message.From("{}")
            ];
        }

        var dispatchCount = 0;
        try
        {
            for (var attempt = 0; attempt < 100 && !observer.HasTerminal; attempt++)
            {
                var parts = CreateParts();
                try
                {
                    publisher.Publish("events.child", parts, SendFlags.None);
                }
                finally
                {
                    ZLinkMessageParts.DisposeAll(parts);
                }

                await Task.Delay(5);
                await registry.DrainAsync(
                    nativeSpot,
                    null,
                    reporter,
                    logger,
                    (_, _, _, _) =>
                    {
                        dispatchCount++;
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None);
            }

            var error = observer.Events.FirstOrDefault();
            var received = observer.Events.Skip(1).FirstOrDefault();
            return new SpotSubscriptionObservation(
                received,
                error,
                dispatchCount,
                logger.Messages.ToArray());
        }
        finally
        {
            await runner.StopAsync();
        }
    }

    private static string GetTcpEndpoint()
    {
        using var listener = new TcpListener(
            IPAddress.Loopback,
            0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        return $"tcp://127.0.0.1:{port}";
    }

    private sealed class NeverInvokedHandler;

    private sealed record TestRequest(string Value);

    private sealed record TestReply(string Value);

    private sealed record TestPublishedEvent(string Value);

    private sealed record TestSubscriptionEvent(string Value);

    private sealed class TestSubscriptionSpot(
        bool ownsMessage = true,
        TestSubscriptionProbe? probe = null) : IZLinkSpot
    {
        public bool OwnsMessage { get; } = ownsMessage;

        public TestSubscriptionProbe? Probe { get; } = probe;

        public IZLinkSpotContext Context => null!;
    }

    private sealed class TestSubscriptionProbe
    {
        public int OwnerHandled;

        public int NonOwnerSkipped;
    }

    private sealed class TestSubscriptionHandler
        : IZLinkSpotSubscriptionHandler<TestSubscriptionSpot, TestSubscriptionEvent>
    {
        public ValueTask HandleAsync(
            TestSubscriptionSpot spot,
            TestSubscriptionEvent message,
            ZLinkPublishMessageContext context,
            CancellationToken cancellationToken)
        {
            _ = message;
            cancellationToken.ThrowIfCancellationRequested();
            if (!spot.OwnsMessage)
            {
                Interlocked.Increment(ref spot.Probe!.NonOwnerSkipped);
                return ValueTask.CompletedTask;
            }

            if (spot.Probe is not null)
                Interlocked.Increment(ref spot.Probe.OwnerHandled);
            return ValueTask.CompletedTask;
        }
    }

    private sealed record SpotSubscriptionObservation(
        ObservedMessageFlow? Received,
        ObservedMessageFlow? Terminal,
        int DispatchCount,
        IReadOnlyList<string> LogMessages);

    private sealed class PublishProbe
    {
        public string? Value { get; set; }

        public ZLinkHandlerDispatchKind? FilterDispatchKind { get; set; }

        public string? FilterMeshName { get; set; }
    }

    private sealed class CapturingFanoutFilter(PublishProbe probe)
        : IZLinkHandlerFilter
    {
        public async ValueTask InvokeAsync(
            IZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext next,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            probe.FilterDispatchKind = context.DispatchKind;
            probe.FilterMeshName = context.MeshName;
            await next();
        }
    }

    private sealed class CapturingPublishHandler(PublishProbe probe)
    {
        public ValueTask Handle(TestPublishedEvent message)
        {
            probe.Value = message.Value;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class TestActor(string actorId) : IZLinkActor
    {
        public string ActorId { get; } = actorId;

        public IZLinkActorContext Context { get; } = new TestActorContext(actorId);
    }

    private sealed class TestActorSpot;

    private sealed class TestActorSendProbe
    {
        public int InvocationCount;
    }

    private sealed class TestActorSendHandler(TestActorSendProbe probe)
        : IZLinkSpotActorSendHandler<TestActorSpot, TestActor, TestRequest>
    {
        public ValueTask HandleAsync(
            TestActorSpot spot,
            TestActor actor,
            IZLinkMessageContext context,
            TestRequest message,
            CancellationToken cancellationToken)
        {
            Interlocked.Increment(ref probe.InvocationCount);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class CapturingMessageFlowObserver : IDisposable
    {
        private readonly ActivityListener _listener;
        private readonly ConcurrentQueue<ObservedMessageFlow> _events = new();
        private readonly TaskCompletionSource<ObservedMessageFlow> _observed =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public CapturingMessageFlowObserver()
        {
            _listener = new ActivityListener
            {
                ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
                Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                    ActivitySamplingResult.AllDataAndRecorded,
                ActivityStopped = Capture
            };
            ActivitySource.AddActivityListener(_listener);
        }

        public IReadOnlyCollection<ObservedMessageFlow> Events => _events.ToArray();

        public bool HasTerminal => _observed.Task.IsCompleted;

        public async Task<ObservedMessageFlow> WaitAsync(TimeSpan timeout)
        {
            return await _observed.Task.WaitAsync(timeout);
        }

        public void Dispose() => _listener.Dispose();

        private void Capture(Activity activity)
        {
            string? Tag(string name) =>
                activity.TagObjects.FirstOrDefault(tag => tag.Key == name).Value?.ToString();
            var phase = Tag("phase") ?? activity.Events.FirstOrDefault().Name;
            var messageKind = Normalize(Tag("message_kind") ?? Tag("zlink.kind"));
            if (messageKind == "publish")
                return;
            var flow = new ObservedMessageFlow(
                phase,
                Tag("event_id") == "zlink.dispatch_error" ? "failed" : phase,
                Normalize(Tag("surface") ?? Tag("zlink.surface")),
                messageKind,
                Tag("packet_name") ?? Tag("zlink.packet.name"),
                Tag("channel_name") ?? Tag("zlink.channel.name"),
                Tag("correlation_id"),
                Tag("actor_id") ?? Tag("zlink.actor.id"),
                Normalize(Tag("reason") ?? Tag("zlink.reason")),
                Normalize(Tag("action") ?? Tag("zlink.action")));
            _events.Enqueue(flow);
            if (flow.Phase == "dropped"
                || flow.Outcome == "failed" && flow.Action != "drop")
                _observed.TrySetResult(flow);
        }

        private static string? Normalize(string? value) =>
            value switch
            {
                "SpotActor" => "actor",
                "SpotSubscription" => "subscription",
                "Channel" => "channel",
                "HandlerMissing" => "no_handler",
                "PayloadDecodeFailed" => "decode_error",
                "ReplyPathMissing" => "reply_path_missing",
                "ReplyError" => "reply_error",
                "FailCaller" => "fail_caller",
                "ActorSend" => "send",
                "ActorRequest" => "request",
                _ => value?.ToLowerInvariant()
            };
    }

    private sealed record ObservedMessageFlow(
        string? Phase,
        string? Outcome,
        string? Surface,
        string? MessageKind,
        string? PacketName,
        string? ChannelName,
        string? CorrelationId,
        string? ActorId,
        string? Reason,
        string? Action);

    private sealed class CapturingLogger<T> : ILogger<T>
    {
        public List<string> Messages { get; } = [];

        public IDisposable? BeginScope<TState>(TState state) where TState : notnull
        {
            return null;
        }

        public bool IsEnabled(LogLevel logLevel)
        {
            return true;
        }

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter)
        {
            Messages.Add(formatter(state, exception));
        }
    }

    private static ServiceProvider CreateDispatchRuntime(out ZLinkFrameworkRuntime runtime)
    {
        var registration = new ZLinkFrameworkRegistration();
        var services = new ServiceCollection();
        services.AddSingleton(registration);
        var provider = services.BuildServiceProvider();
        runtime = new ZLinkFrameworkRuntime(
            provider,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                provider.GetRequiredService<IServiceScopeFactory>(),
                registration));
        return provider;
    }

    private sealed class CapturingSpot : IZLinkBackendSpot
    {
        public ulong LifecycleGeneration => 1;

        public int SubscriptionAttempts { get; private set; }

        public int SubscriptionFailuresRemaining { get; init; }

        public Func<TopicMessage, RecvFlags, bool>? SubscribeHandler { get; set; }

        public int? LastJoinResultCode { get; private set; }

        public RoutingId RoutingId => RoutingId.From("spot");

        public ValueTask DisposeAsync()
        {
            return ValueTask.CompletedTask;
        }

        public void SetRoutingId(RoutingId routingId)
        {
        }

        public void SetSubscription(string channelName, string topic)
        {
            SubscriptionAttempts++;
            if (SubscriptionAttempts <= SubscriptionFailuresRemaining)
                throw new ZlinkConfigException(ZlinkConfigException.ErrorCode.InternalError);
        }

        public ZLinkBackendSubscribeMessage? Subscribe(RecvFlags flags)
        {
            if (SubscribeHandler is null) return null;
            var topicMessage = new TopicMessage();
            try
            {
                if (!SubscribeHandler(topicMessage, flags)) return null;
                var parts = topicMessage.Parts
                    .Select(static part => Message.From(part.AsReadOnlySpan()))
                    .ToArray();
                return new ZLinkBackendSubscribeMessage("events", topicMessage.Topic, parts);
            }
            finally
            {
                topicMessage.Dispose();
            }
        }

        public ZLinkBackendRouteReceived? RecvRoute(RecvFlags flags) => null;

        public void OnDispatchEvent(Action<ZLinkBackendSpotDispatchInfo> handler)
        {
        }

        public void OnSendReady(Action handler)
        {
        }

        public bool RequestToChannel(
            string channelName,
            Message message,
            RequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout,
            ReadOnlyMemory<byte> metadata)
        {
            return false;
        }

        public bool RequestToChannel(
            string channelName,
            IReadOnlyList<Message> parts,
            RequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout,
            ReadOnlyMemory<byte> metadata)
        {
            return false;
        }

        public SubmitResult SendToChannel(
            string channelName, Message message, SendFlags flags,
            ReadOnlyMemory<byte> metadata)
        {
            return SubmitResult.Backpressured;
        }

        public SubmitResult SendToChannel(
            string channelName, IReadOnlyList<Message> parts, SendFlags flags,
            ReadOnlyMemory<byte> metadata)
        {
            return SubmitResult.Backpressured;
        }

        public void Publish(
            string channelName, string topic, Message message, SendFlags flags,
            ReadOnlyMemory<byte> metadata)
        {
        }

        public void Publish(
            string channelName, string topic, IReadOnlyList<Message> parts, SendFlags flags,
            ReadOnlyMemory<byte> metadata)
        {
        }

        public SubmitResult SendToSpot(
            RoutingId targetRid, string targetSpotId, ulong spotGeneration,
            Message message, SendFlags flags, ReadOnlyMemory<byte> metadata)
        {
            return SubmitResult.Backpressured;
        }

        public SubmitResult SendToSpot(
            RoutingId targetRid,
            string targetSpotId,
            ulong spotGeneration,
            IReadOnlyList<Message> parts,
            SendFlags flags,
            ReadOnlyMemory<byte> metadata)
        {
            return SubmitResult.Backpressured;
        }

        public bool RequestToSpot(
            RoutingId targetRid,
            string targetSpotId,
            ulong spotGeneration,
            Message message,
            RequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout,
            ReadOnlyMemory<byte> metadata)
        {
            return false;
        }

        public bool RequestToSpot(
            RoutingId targetRid,
            string targetSpotId,
            ulong spotGeneration,
            IReadOnlyList<Message> parts,
            RequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout,
            ReadOnlyMemory<byte> metadata)
        {
            return false;
        }

        public ZLinkBackendActorJoinRequest? RecvActorJoin(RecvFlags flags)
        {
            return null;
        }

        public ZLinkBackendSpotActorLifecycleEvent? RecvActorLifecycle(RecvFlags flags)
        {
            return null;
        }

        public void ReplyActorJoin(
            ZLinkBackendActorJoinRequest request,
            int joinResultCode,
            Message reply)
        {
            LastJoinResultCode = joinResultCode;
        }

        public void ReplyActorJoin(
            ZLinkBackendActorJoinRequest request,
            int joinResultCode,
            IReadOnlyList<Message> parts)
        {
            LastJoinResultCode = joinResultCode;
        }

        public void OnActorLifecycle(
            Action<ZLinkBackendSpotActorLifecycleInfo>? onJoin,
            Action<ZLinkBackendSpotActorLifecycleInfo>? onLeave)
        {
        }
    }
}
