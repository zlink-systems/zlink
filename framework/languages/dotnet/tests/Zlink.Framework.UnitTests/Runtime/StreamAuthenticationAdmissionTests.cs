using System.Collections.Concurrent;
using System.Diagnostics.Metrics;
using System.Globalization;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Configuration.Builders;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed partial class StreamSessionForcedCleanupTests
{
    public static IEnumerable<object[]> AuthenticationAdmissionScenarios()
    {
        var path = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework",
            "runtime",
            "conformance",
            "login-admission-v1.json"
        );
        using var fixture = JsonDocument.Parse(File.ReadAllText(path));
        // This fixture covers stage 1. Queue, ticket and position scenarios belong to stage 2.
        foreach (
            var scenario in fixture.RootElement.GetProperty("serverScenarios").EnumerateArray()
        )
            yield return [scenario.GetProperty("name").GetString()!, scenario.GetRawText()];
    }

    [Theory]
    [MemberData(nameof(AuthenticationAdmissionScenarios))]
    public async Task AuthenticationAdmissionServerFixture(string name, string json)
    {
        using var document = JsonDocument.Parse(json);
        var scenario = document.RootElement;
        var given = scenario.GetProperty("given");
        var configuredLimit = given.GetProperty("maxConcurrentAuthentications");
        var settings = new ZLinkFrameworkRegistration();
        settings.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var builder = new ZLinkFrameworkOptionsBuilder(settings);
        var stream = builder.AddStreamNode("authentication");
        stream.Bind(0).AddSession<AuthenticationSession>();
        stream.AuthenticationPacketName = given.GetProperty("authenticationPacketName").GetString();
        stream.AuthenticationRetryDelay = TimeSpan.FromMilliseconds(
            given.GetProperty("authenticationRetryDelayMs").GetInt32()
        );
        if (configuredLimit.ValueKind != JsonValueKind.Null)
            stream.MaxConcurrentAuthentications = configuredLimit.GetInt32();
        var fixtureLimit = scenario.GetProperty("expectedEffectiveLimit").GetInt32();
        if (configuredLimit.ValueKind == JsonValueKind.Null)
        {
            Assert.Equal(given.GetProperty("effectiveCpuCores").GetInt32() * 8, fixtureLimit);
            Assert.Equal(
                (int)Math.Min(8L * Environment.ProcessorCount, int.MaxValue),
                stream.MaxConcurrentAuthentications
            );
        }
        else
            Assert.Equal(fixtureLimit, stream.MaxConcurrentAuthentications);
        ZLinkFrameworkRegistrationValidator.Validate(settings);

        var lifetime = new AuthenticationHandlerLifetime();
        var logger = new StreamFlowLoggerFactory();
        ZLinkFrameworkRuntime runtime = null!;
        await using var services = new ServiceCollection()
            .AddSingleton(settings)
            .AddSingleton(lifetime)
            .AddSingleton<ILoggerFactory>(logger)
            .AddSingleton(_ => runtime)
            .BuildServiceProvider();
        runtime = CreateRuntime(services, settings);
        var runner = new ZLinkRuntimeTaskRunner(
            runtime.ErrorSink,
            CancellationToken.None,
            runtime.ExecutionOwner
        );
        var time = new ControllableTimeProvider();
        using var jobs = new ZLinkApplicationJobQueue(
            ZLinkApplicationJobQueueCapacityResolver.Resolve(
                ZLinkApplicationJobQueueProfile.Balanced,
                128,
                1
            )
        );
        var admission = new ZLinkAuthenticationAdmission(
            settings.StreamNodes["authentication"].AuthenticationAdmission
        );
        using var metricRegistration = ZLinkRuntimeMetrics.RegisterAuthenticationAdmission(() =>
            admission.InFlight
        );
        var gauge = new List<int>();
        long rejections = 0;
        using var meterListener = new MeterListener();
        meterListener.InstrumentPublished = (instrument, listener) =>
        {
            if (
                instrument.Name.StartsWith(
                    "zlink.host.authentication_admission.",
                    StringComparison.Ordinal
                )
            )
                listener.EnableMeasurementEvents(instrument);
        };
        meterListener.SetMeasurementEventCallback<int>(
            (instrument, value, tags, _) =>
            {
                Assert.Equal("zlink.host.authentication_admission.in_flight", instrument.Name);
                Assert.Empty(tags.ToArray());
                gauge.Add(value);
            }
        );
        meterListener.SetMeasurementEventCallback<long>(
            (instrument, value, tags, _) =>
            {
                Assert.Equal("zlink.host.authentication_admission.rejections", instrument.Name);
                Assert.Empty(tags.ToArray());
                Interlocked.Add(ref rejections, value);
            }
        );
        meterListener.Start();

        var socket = CreateSocket();
        var monitor = new TestSocketMonitor();
        var nodes = new List<ZLinkStreamNodeRuntime>();
        var node = StartNode();
        var requests = new Dictionary<string, (RoutingId RoutingId, ZlinkStreamHeader Header)>();
        var handlerCalls = 0;
        var expectedRejections = 0;
        var terminalRequests = new HashSet<string>(StringComparer.Ordinal);
        try
        {
            foreach (var step in scenario.GetProperty("steps").EnumerateArray())
            {
                var action = step.GetProperty("action").GetString();
                var id = step.GetProperty("requestId").GetString()!;
                var expected = step.GetProperty("expect");
                var expectedInFlight = expected.GetProperty("inFlight").GetInt32();
                var expectedPermits = expected.GetProperty("heldPermits").GetUInt64();
                handlerCalls = expected.GetProperty("handlerCalls").GetInt32();
                switch (action)
                {
                    case "receive":
                        var routingId = RoutingId.From(step.GetProperty("sessionId").GetString()!);
                        var kindName = step.GetProperty("kind").GetString();
                        var kind =
                            kindName == "Heartbeat"
                                ? ZlinkStreamMessageKind.Control
                                : Enum.Parse<ZlinkStreamMessageKind>(kindName!);
                        var requestSeq = step.GetProperty("requestSeq").GetString() is { } seq
                            ? new ZlinkStreamRequestSeq(
                                ulong.Parse(seq, CultureInfo.InvariantCulture)
                            )
                            : (ZlinkStreamRequestSeq?)null;
                        var header = new ZlinkStreamHeader(
                            kind,
                            kind == ZlinkStreamMessageKind.Control
                                ? ZlinkStreamCodec.Raw
                                : ZlinkStreamCodec.Json,
                            requestSeq is null
                                ? ZlinkStreamHeaderFlags.None
                                : ZlinkStreamHeaderFlags.HasRequestSeq,
                            requestSeq,
                            kind == ZlinkStreamMessageKind.Control
                                ? "$zlink.heartbeat.ping"
                                : step.GetProperty("packetName").GetString()!,
                            ZlinkStreamMetadata.Empty,
                            requestSeq is null ? null : $"admission-{id}"
                        );
                        requests.Add(id, (routingId, header));
                        var dequeued = socket.DequeuedPacketCount;
                        SendPacket(socket, routingId, header);
                        switch (expected.GetProperty("decision").GetString())
                        {
                            case "rejected":
                                expectedRejections++;
                                AssertAdmissionRejection(
                                    await socket.SentFrame.Task.WaitAsync(TimeSpan.FromSeconds(2)),
                                    expected.GetProperty("error"),
                                    header
                                );
                                Assert.Equal(0, socket.DisconnectCount);
                                Assert.Equal(
                                    expected.GetProperty("handlerCallsForRequest").GetInt32(),
                                    lifetime.Peer(routingId).Started.Task.IsCompleted ? 1 : 0
                                );
                                // A rejected frame never creates a session for the peer, which
                                // places it before Actor dispatch and access to the Location Store.
                                Assert.Equal(handlerCalls, node.SessionCount);
                                Assert.Equal(
                                    expected.GetProperty("applicationQueuedJobs").GetUInt64(),
                                    jobs.GetStatus().QueuedApplicationJobs
                                );
                                break;
                            case "infrastructure":
                                await socket.SentFrame.Task.WaitAsync(TimeSpan.FromSeconds(2));
                                Assert.Equal(
                                    "$zlink.heartbeat.pong",
                                    DecodeStreamHeader(socket.SentFrames.Last()).Name
                                );
                                break;
                        }
                        await WaitUntilAsync(() => socket.DequeuedPacketCount == dequeued + 1);
                        break;
                    case "handlerStart":
                        var peer = requests[id].RoutingId;
                        monitor.Emit(
                            new ZLinkBackendSocketMonitorEvent(
                                ZLinkSocketNativeEventType.ConnectionReady,
                                peer,
                                "local",
                                "remote",
                                0,
                                MonitorEventFlags.ConnectionReadyEdge
                            )
                        );
                        await lifetime.Peer(peer).Started.Task.WaitAsync(TimeSpan.FromSeconds(2));
                        break;
                    case "clientTimeout":
                        // The peer dropping its pending request produces no server frame or cancellation.
                        time.AdvanceMonotonicWithoutTimers(TimeSpan.FromMilliseconds(25));
                        break;
                    case "await":
                        Assert.False(
                            lifetime.Peer(requests[id].RoutingId).Finished.Task.IsCompleted
                        );
                        break;
                    case "terminal":
                        var request = requests[id];
                        var handler = lifetime.Peer(request.RoutingId);
                        var reason = step.GetProperty("reason").GetString();
                        if (terminalRequests.Add(id))
                        {
                            switch (reason)
                            {
                                case "cancellation":
                                    await ForceStopPeerAsync(handler);
                                    await handler.Finished.Task.WaitAsync(TimeSpan.FromSeconds(2));
                                    break;
                                case "shutdown":
                                    await node.DisposeAsync(new CancellationToken(canceled: true));
                                    await handler.Finished.Task.WaitAsync(TimeSpan.FromSeconds(2));
                                    // A closed listener cannot consume the fixture's next packet.
                                    // Use a second listener with the same host admission owner.
                                    socket = CreateSocket();
                                    monitor = new TestSocketMonitor();
                                    node = StartNode();
                                    break;
                                case "enqueueRejected":
                                    await ForceStopPeerAsync(handler);
                                    await WaitUntilAsync(() => admission.InFlight == 0);
                                    // Force a real enqueue rejection on the closed session.
                                    var pulled = socket.DequeuedPacketCount;
                                    SendPacket(socket, request.RoutingId, request.Header);
                                    // With the peer's application queue closed, a subsequent
                                    // control frame proves ingress advanced past the rejection.
                                    SendPacket(
                                        socket,
                                        RoutingId.From("enqueue-fence"),
                                        new ZlinkStreamHeader(
                                            ZlinkStreamMessageKind.Control,
                                            ZlinkStreamCodec.Raw,
                                            ZlinkStreamHeaderFlags.None,
                                            null,
                                            "$zlink.heartbeat.ping",
                                            ZlinkStreamMetadata.Empty
                                        )
                                    );
                                    await WaitUntilAsync(() =>
                                        socket.DequeuedPacketCount == pulled + 2
                                        && socket.SentFrames.Any(frame =>
                                            DecodeStreamHeader(frame).Name
                                            == "$zlink.heartbeat.pong"
                                        )
                                    );
                                    break;
                                default:
                                    handler.Complete.TrySetResult(reason!);
                                    if (reason is "replySuccess" or "replyFailure")
                                    {
                                        await handler.ReplySubmitted.Task.WaitAsync(
                                            TimeSpan.FromSeconds(2)
                                        );
                                        Assert.False(handler.Finished.Task.IsCompleted);
                                        var reply = DecodeStreamHeader(socket.SentFrames.Last());
                                        Assert.Equal(
                                            reason == "replySuccess"
                                                ? ZlinkStreamMessageKind.Response
                                                : ZlinkStreamMessageKind.Error,
                                            reply.Kind
                                        );
                                        Assert.Equal(request.Header.RequestSeq, reply.RequestSeq);
                                    }
                                    else
                                        await handler.Finished.Task.WaitAsync(
                                            TimeSpan.FromSeconds(2)
                                        );
                                    break;
                            }
                        }
                        else if (reason is "replySuccess" or "replyFailure")
                        {
                            handler.ReleaseAfterReply.TrySetResult();
                            await handler.Finished.Task.WaitAsync(TimeSpan.FromSeconds(2));
                        }

                        break;
                }

                await WaitForAdmissionStateAsync(
                    () =>
                        jobs.GetStatus().QueuedApplicationJobs == expectedPermits
                        && jobs.GetStatus().ReservedSupplyPermits == 0,
                    () =>
                        $"{name}/{id}/{action}: expected {expectedPermits} held permits, queue {jobs.GetStatus()}"
                );
                await WaitUntilAsync(() => admission.InFlight == expectedInFlight);
                Assert.Equal(handlerCalls, lifetime.HandlerCalls);
                Assert.True(socket.AllPullsHadPermit);
                Assert.Equal(expectedRejections, Interlocked.Read(ref rejections));
                gauge.Clear();
                meterListener.RecordObservableInstruments();
                Assert.Equal(expectedInFlight, Assert.Single(gauge));
            }
        }
        finally
        {
            foreach (var peer in lifetime.Peers)
            {
                peer.Complete.TrySetResult("callbackWithoutReply");
                peer.ReleaseAfterReply.TrySetResult();
            }
            try
            {
                foreach (var started in nodes)
                    await started.DisposeAsync(new CancellationToken(canceled: true));
            }
            finally
            {
                await runner.StopAsync();
            }
        }
        await WaitUntilAsync(() => admission.InFlight == 0);

        static async Task ForceStopPeerAsync(AuthenticationHandlerBarrier handler)
        {
            var session = await handler.Session.Task.WaitAsync(TimeSpan.FromSeconds(2));
            await session.RequestForceStopForDrainAsync(CancellationToken.None);
        }

        TestStreamSocket CreateSocket() =>
            new() { BeforeRecvPacket = () => jobs.GetStatus().ReservedSupplyPermits == 1 };

        ZLinkStreamNodeRuntime StartNode()
        {
            var listener = new ZLinkStreamNodeRuntime(
                "authentication",
                services,
                socket,
                monitor,
                typeof(AuthenticationSession),
                runner,
                "test",
                timeProvider: time,
                applicationJobQueue: jobs,
                authenticationAdmission: admission
            );
            nodes.Add(listener);
            listener.Start();
            return listener;
        }
    }

    private static async Task WaitForAdmissionStateAsync(Func<bool> ready, Func<string> message)
    {
        var started = System.Diagnostics.Stopwatch.GetTimestamp();
        while (!ready())
        {
            if (System.Diagnostics.Stopwatch.GetElapsedTime(started) >= TimeSpan.FromSeconds(2))
                Assert.Fail(message());
            await Task.Delay(5);
        }
    }

    [Theory]
    [InlineData(0, 500)]
    [InlineData(-1, 500)]
    [InlineData(1, 0)]
    [InlineData(1, -1)]
    [InlineData(1, 0.5)]
    [InlineData(1, 2147483648)]
    public void AuthenticationAdmissionRejectsInvalidSettings(int limit, double milliseconds)
    {
        var registration = new ZLinkFrameworkRegistration();
        var stream = new ZLinkFrameworkOptionsBuilder(registration).AddStreamNode("authentication");
        stream.Bind(0).AddSession<AuthenticationSession>();
        stream.MaxConcurrentAuthentications = limit;
        stream.AuthenticationRetryDelay = TimeSpan.FromMilliseconds(milliseconds);
        Assert.Throws<ZLinkConfigurationException>(() =>
            ZLinkFrameworkRegistrationValidator.Validate(registration)
        );
    }

    [Theory]
    [InlineData("name")]
    [InlineData("limit")]
    [InlineData("delay")]
    public void AuthenticationAdmissionRejectsDisagreementBetweenStreamRegistrations(
        string different
    )
    {
        var registration = new ZLinkFrameworkRegistration();
        var builder = new ZLinkFrameworkOptionsBuilder(registration);
        var primary = builder.AddStreamNode("primary");
        var extra = builder.AddStreamNode("extra");
        primary.Bind(0).AddSession<AuthenticationSession>();
        extra.Bind(0).AddSession<DrainRaceSession>();
        foreach (var stream in new[] { primary, extra })
        {
            stream.AuthenticationPacketName = "Login";
            stream.MaxConcurrentAuthentications = int.MaxValue;
            stream.AuthenticationRetryDelay = TimeSpan.FromMilliseconds(int.MaxValue);
        }
        ZLinkFrameworkRegistrationValidator.Validate(registration);
        switch (different)
        {
            case "name":
                extra.AuthenticationPacketName = null;
                break;
            case "limit":
                extra.MaxConcurrentAuthentications = 1;
                break;
            case "delay":
                extra.AuthenticationRetryDelay = TimeSpan.FromMilliseconds(1);
                break;
        }
        Assert.Throws<ZLinkConfigurationException>(() =>
            ZLinkFrameworkRegistrationValidator.Validate(registration)
        );
    }

    private static void SendPacket(
        TestStreamSocket socket,
        RoutingId routingId,
        ZlinkStreamHeader header
    ) =>
        socket.EnqueuePacket(
            routingId,
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            header.Kind == ZlinkStreamMessageKind.Control ? [] : "{}"u8
        );

    private static void AssertAdmissionRejection(
        byte[] frame,
        JsonElement expected,
        ZlinkStreamHeader request
    )
    {
        Assert.True(ZLinkStreamFrameCodec.TryDecode(frame, out var bytes, out var payload));
        var header = ZLinkStreamProtocolDefaults.DecodeHeader(bytes.ToArray());
        Assert.Equal(expected.GetProperty("kind").GetString(), header.Kind.ToString());
        Assert.Equal(expected.GetProperty("packetName").GetString(), header.Name);
        Assert.Equal(
            expected.GetProperty("requestSeq").GetString(),
            header.RequestSeq?.Value.ToString(CultureInfo.InvariantCulture)
        );
        Assert.Equal(request.CorrelationId, header.CorrelationId);
        using var body = JsonDocument.Parse(payload.ToArray());
        var expectedBody = expected.GetProperty("body");
        Assert.Equal(
            expectedBody.GetProperty("code").GetString(),
            body.RootElement.GetProperty("code").GetString()
        );
        Assert.Equal(
            expectedBody.GetProperty("message").GetString(),
            body.RootElement.GetProperty("message").GetString()
        );
        Assert.Equal(2, body.RootElement.EnumerateObject().Count());
        var metadata = expected.GetProperty("metadata").EnumerateObject().ToArray();
        Assert.Equal(metadata.Length, header.Metadata.Count);
        foreach (var pair in metadata)
            Assert.Equal(pair.Value.GetString(), header.Metadata.Get(pair.Name));
    }

    private sealed class AuthenticationHandlerLifetime
    {
        private readonly ConcurrentDictionary<RoutingId, AuthenticationHandlerBarrier> _peers =
            new();
        private int _handlerCalls;

        public int HandlerCalls => Volatile.Read(ref _handlerCalls);
        public ICollection<AuthenticationHandlerBarrier> Peers => _peers.Values;

        public AuthenticationHandlerBarrier Peer(RoutingId routingId) =>
            _peers.GetOrAdd(routingId, static _ => new());

        public void Start(AuthenticationHandlerBarrier peer)
        {
            Interlocked.Increment(ref _handlerCalls);
            peer.Started.TrySetResult();
        }
    }

    private sealed class AuthenticationHandlerBarrier
    {
        public TaskCompletionSource<ZLinkStreamSessionRuntime> Session { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource<string> Complete { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReplySubmitted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseAfterReply { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Finished { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class AuthenticationSession(
        IZLinkSessionContext context,
        AuthenticationHandlerLifetime lifetime
    ) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public void Configure() =>
            lifetime
                .Peer(Context.RoutingId!.Value)
                .Session.TrySetResult(((ZLinkSessionContext)Context).SessionRuntime!);

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken
        )
        {
            var peer = lifetime.Peer(Context.RoutingId!.Value);
            lifetime.Start(peer);
            try
            {
                switch (await peer.Complete.Task.WaitAsync(cancellationToken))
                {
                    case "replySuccess":
                        await Context.Client.Reply(new AuthenticationResponse()).Async();
                        break;
                    case "replyFailure":
                        Assert.True(dispatch.TryClaimReply());
                        await ((ZLinkSessionContext)Context).ReplyErrorAsync(
                            Assert.IsType<ZlinkStreamHeader>(dispatch.RuntimeState),
                            new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.Rejected,
                                "Login denied."
                            ),
                            cancellationToken
                        );
                        break;
                    case "handlerException":
                        throw new InvalidOperationException("Authentication handler failed.");
                    case "callbackWithoutReply":
                        return;
                    default:
                        throw new InvalidOperationException("Unexpected test terminal.");
                }
                peer.ReplySubmitted.TrySetResult();
                await peer.ReleaseAfterReply.Task.WaitAsync(cancellationToken);
            }
            finally
            {
                peer.Finished.TrySetResult();
            }
        }

        private sealed record AuthenticationResponse;
    }
}
