using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class StreamSessionForcedCleanupTests
{
    [Fact]
    public async Task Stream_Node_Concurrent_Dispose_Callers_Share_Socket_Cleanup()
    {
        var registration = new ZLinkFrameworkRegistration();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(_ => runtime);
        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var failure = new InvalidOperationException("stream socket cleanup failed");
        var socket = new TestStreamSocket { BlockDispose = true, DisposeFailure = failure };
        var node = new ZLinkStreamNodeRuntime(
            "dispose-node",
            provider,
            socket,
            new TestSocketMonitor(),
            null,
            new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), CancellationToken.None, runtime.ExecutionOwner),
            "test");

        var first = node.DisposeAsync().AsTask();
        await socket.DisposeStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var second = node.DisposeAsync().AsTask();

        Assert.Same(first, second);
        Assert.False(second.IsCompleted);
        socket.AllowDispose.TrySetResult();
        var firstFailure = await Assert.ThrowsAsync<InvalidOperationException>(
            () => first.WaitAsync(TimeSpan.FromSeconds(5)));
        var secondFailure = await Assert.ThrowsAsync<InvalidOperationException>(
            () => second.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Same(failure, firstFailure);
        Assert.Same(firstFailure, secondFailure);
        Assert.Equal(1, socket.DisposeCount);
    }

    [Fact]
    public async Task Stream_Request_Emits_Received_Then_Replied_With_Wire_Correlation()
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var lifetime = new StreamFlowLifetime();
        var loggerFactory = new StreamFlowLoggerFactory();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton<ILoggerFactory>(loggerFactory)
            .AddSingleton(_ => runtime);

        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var session = await ZLinkStreamSessionRuntime.CreateAsync(
            provider,
            socket,
            RoutingId.From("stream-flow-client"),
            typeof(StreamFlowSession),
            static _ => { },
            "test",
            TimeProvider.System);
        const string flowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d";
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(17),
            nameof(StreamFlowRequest),
            ZlinkStreamMetadata.Empty,
            "stream-corr-17",
            flowId,
            ZlinkStreamFlowOrigin.Application);

        try
        {
            session.EnqueuePacket(
                Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span),
                Message.From(ZLinkStreamPacketPayloadCodec.EncodeJson(
                    new StreamFlowRequest("request"),
                    typeof(StreamFlowRequest))));

            await lifetime.ReplyCompleted.Task.WaitAsync(TimeSpan.FromSeconds(2));
            var frame = await socket.SentFrame.Task.WaitAsync(TimeSpan.FromSeconds(2));
            var response = DecodeStreamHeader(frame);
            Assert.Equal(ZlinkStreamMessageKind.Response, response.Kind);
            Assert.Equal("stream-corr-17", response.CorrelationId);
            Assert.Equal(flowId, response.FlowId);

            var lines = loggerFactory.Messages.ToArray();
            Assert.Equal(2, lines.Length);
            Assert.Contains(lines, line => line.Contains("phase=received", StringComparison.Ordinal)
                                          && line.Contains("corr=stream-corr-17", StringComparison.Ordinal));
            Assert.Contains(lines, line => line.Contains("phase=replied", StringComparison.Ordinal)
                                          && line.Contains("corr=stream-corr-17", StringComparison.Ordinal)
                                          && line.Contains("src=stream-flow-client", StringComparison.Ordinal));
            Assert.DoesNotContain(lines, line => line.Contains("phase=dispatched", StringComparison.Ordinal));
            Assert.Contains(ZLinkMessageFlowTracer.LoggerCategory, loggerFactory.Categories);
        }
        finally
        {
            await session.DisposeAsync();
        }
    }

    [Fact]
    public async Task Stream_Request_Without_Wire_Correlation_DoesNotInvent_One_From_RequestSeq()
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var lifetime = new StreamFlowLifetime();
        var loggerFactory = new StreamFlowLoggerFactory();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton<ILoggerFactory>(loggerFactory)
            .AddSingleton(_ => runtime);

        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var session = await ZLinkStreamSessionRuntime.CreateAsync(
            provider,
            socket,
            RoutingId.From("stream-no-corr-client"),
            typeof(StreamFlowSession),
            static _ => { },
            "test",
            TimeProvider.System);
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Request,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(19),
            nameof(StreamFlowRequest),
            ZlinkStreamMetadata.Empty,
            null,
            null,
            null);

        try
        {
            session.EnqueuePacket(
                Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span),
                Message.From(ZLinkStreamPacketPayloadCodec.EncodeJson(
                    new StreamFlowRequest("request"),
                    typeof(StreamFlowRequest))));

            await lifetime.ReplyCompleted.Task.WaitAsync(TimeSpan.FromSeconds(2));
            var frame = await socket.SentFrame.Task.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.Null(DecodeStreamHeader(frame).CorrelationId);

            var lines = loggerFactory.Messages.ToArray();
            Assert.Equal(2, lines.Length);
            Assert.All(lines, line => Assert.Contains("corr=(null)", line, StringComparison.Ordinal));
            Assert.All(lines, line => Assert.DoesNotContain("corr=19", line, StringComparison.Ordinal));
        }
        finally
        {
            await session.DisposeAsync();
        }
    }

    [Fact]
    public async Task Native_Callbacks_Are_Offloaded_Serialized_Per_Session_And_Parallel_Across_Sessions()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new SessionOrderingLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton(_ => runtime);
        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "session-ordering",
            provider,
            socket,
            monitor,
            typeof(SessionOrderingSession),
            runner,
            "test");
        var first = RoutingId.From("session-a");
        var second = RoutingId.From("session-b");
        try
        {
            node.Start();
            for (var attempt = 0; attempt < 200 && monitor.WaitCount == 0; attempt++)
                await Task.Delay(5);
            Assert.True(monitor.WaitCount > 0);
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                first,
                "local-a",
                "remote-a",
                0));
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                second,
                "local-b",
                "remote-b",
                0));
            await lifetime.WaitConnectedAsync(first);
            await lifetime.WaitConnectedAsync(second);

            // The native callback returns even though this session's dispatch
            // remains blocked on its own serial executor.
            EmitJson(socket, first, new SessionOrderingMessage());
            await lifetime.WaitDispatchStartedAsync(first);
            Assert.False(lifetime.IsDispatchCompleted(first));

            // A separate session has a separate serial executor and therefore
            // completes while the first session remains blocked.
            EmitJson(socket, second, new SessionOrderingMessage());
            await lifetime.WaitDispatchCompletedAsync(second);
            Assert.False(lifetime.IsDispatchCompleted(first));

            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.Disconnected,
                first,
                "local-a",
                "remote-a",
                54));
            await Task.Delay(30);
            Assert.DoesNotContain("error", lifetime.Events(first));
            Assert.DoesNotContain("disconnected", lifetime.Events(first));

            lifetime.ReleaseFirst.TrySetResult();
            await lifetime.WaitDisconnectedAsync(first);

            Assert.Equal(
                new[]
                {
                    "connected",
                    "dispatch-start",
                    "dispatch-end",
                    "error",
                    "disconnected"
                },
                lifetime.Events(first));
            Assert.Equal(
                new[] { "connected", "dispatch-start", "dispatch-end" },
                lifetime.Events(second));
        }
        finally
        {
            lifetime.ReleaseFirst.TrySetResult();
            await node.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task Packet_before_connection_ready_waits_for_connected_lifecycle()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new SessionOrderingLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton(_ => runtime);
        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "packet-before-ready",
            provider,
            socket,
            monitor,
            typeof(SessionOrderingSession),
            runner,
            "test");
        var session = RoutingId.From("session-c");
        try
        {
            node.Start();
            EmitJson(socket, session, new SessionOrderingMessage());
            await Task.Delay(50);
            Assert.Empty(lifetime.Events(session));

            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                session,
                "local-c",
                "remote-c",
                0));

            await lifetime.WaitDispatchCompletedAsync(session);
            Assert.Equal(
                new[] { "connected", "dispatch-start", "dispatch-end" },
                lifetime.Events(session));
        }
        finally
        {
            await node.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task Malformed_stream_frame_disconnects_only_the_offending_peer()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new SessionOrderingLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton(_ => runtime);
        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "malformed-peer",
            provider,
            socket,
            monitor,
            typeof(SessionOrderingSession),
            runner,
            "test");
        var badPeer = RoutingId.From("malformed-peer");
        var goodPeer = RoutingId.From("good-peer");
        try
        {
            node.Start();
            socket.EnqueueRawPart(
                badPeer,
                [0, 0, 0xff, 0xff, 0xff, 0xff]);
            await socket.DisconnectStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));

            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                goodPeer,
                "local-good",
                "remote-good",
                0));
            EmitJson(socket, goodPeer, new SessionOrderingMessage());

            await lifetime.WaitDispatchCompletedAsync(goodPeer);
            Assert.Equal(1, socket.DisconnectCount);
            Assert.Equal(
                new[] { "connected", "dispatch-start", "dispatch-end" },
                lifetime.Events(goodPeer));
        }
        finally
        {
            await node.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task Unidentified_stream_part_does_not_stop_ingress_for_other_peers()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new SessionOrderingLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton(_ => runtime);
        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "unidentified-part",
            provider,
            socket,
            monitor,
            typeof(SessionOrderingSession),
            runner,
            "test");
        var goodPeer = RoutingId.From("good-peer-after-unidentified-part");
        try
        {
            node.Start();
            socket.EnqueueUnidentifiedRawPart([0, 0, 0xff]);
            await socket.UnidentifiedPartConsumed.Task.WaitAsync(
                TimeSpan.FromSeconds(2));

            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                goodPeer,
                "local-good",
                "remote-good",
                0));
            EmitJson(socket, goodPeer, new SessionOrderingMessage());

            await lifetime.WaitDispatchCompletedAsync(goodPeer);
            Assert.Equal(0, socket.DisconnectCount);
        }
        finally
        {
            await node.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task Stream_receive_reassembles_a_frame_across_multipart_parts()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new SessionOrderingLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton(_ => runtime);
        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "multipart-stream",
            provider,
            socket,
            monitor,
            typeof(SessionOrderingSession),
            runner,
            "test");
        var routingId = RoutingId.From("multipart-peer");
        try
        {
            node.Start();
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                routingId,
                "local",
                "remote",
                0));
            var encoded = ZLinkStreamFrameCodec.Encode(
                ZLinkStreamProtocolDefaults.EncodeHeader(
                    new ZlinkStreamHeader(
                        ZlinkStreamMessageKind.Send,
                        ZlinkStreamCodec.Json,
                        ZlinkStreamHeaderFlags.None,
                        null,
                        nameof(SessionOrderingMessage),
                        ZlinkStreamMetadata.Empty)).Span,
                ZLinkStreamPacketPayloadCodec.EncodeJson(
                    new SessionOrderingMessage(),
                    typeof(SessionOrderingMessage)));
            socket.EnqueueRawPart(routingId, encoded.AsSpan(0, 3), hasMore: true);
            socket.EnqueueRawPart(routingId, encoded.AsSpan(3), hasMore: false);

            await lifetime.WaitDispatchCompletedAsync(routingId);
            Assert.Equal(0, socket.DisconnectCount);
        }
        finally
        {
            await node.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task Stream_receive_stops_before_next_recv_when_application_hwm_is_paused()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new SessionOrderingLifetime();
        // This path cannot classify an application frame before RecvPart. A
        // single bounded receive reservation preserves the HWM test's
        // one-message overshoot while preventing a second raw receive.
        var budget = new ZLinkInboundDispatchBudget(
            1,
            maximumConcurrentReceives: 1);
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton(_ => runtime);
        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "stream-hwm-recv",
            provider,
            socket,
            monitor,
            typeof(SessionOrderingSession),
            runner,
            "test",
            inboundDispatchBudget: budget);
        var routingId = RoutingId.From("session-a");
        try
        {
            node.Start();
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                routingId,
                "local",
                "remote",
                0));
            EmitJson(socket, routingId, new SessionOrderingMessage());
            // Queue a second raw part before the first handler releases. The
            // receive batch must stop at the HWM instead of pulling it too.
            EmitJson(socket, routingId, new SessionOrderingMessage());
            await lifetime.WaitDispatchStartedAsync(routingId);
            Assert.False(budget.CanStartApplicationReceive);

            var receivedBeforePausedSend = socket.RecvPartCount;
            await Task.Delay(100);
            Assert.Equal(receivedBeforePausedSend, socket.RecvPartCount);

            lifetime.ReleaseFirst.TrySetResult();
            await WaitUntilAsync(() => socket.RecvPartCount > receivedBeforePausedSend);
            await WaitUntilAsync(
                () => lifetime.Events(routingId)
                    .Count(static value => value == "dispatch-end") >= 2);
        }
        finally
        {
            lifetime.ReleaseFirst.TrySetResult();
            await node.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task Stream_receive_finishes_active_multipart_after_hwm_fills_at_batch_boundary()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new SessionOrderingLifetime();
        // Keep the raw receive reservation explicit so this test verifies the
        // configured overshoot bound at the multipart boundary.
        var budget = new ZLinkInboundDispatchBudget(
            1,
            maximumConcurrentReceives: 1);
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton(_ => runtime);
        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "stream-hwm-multipart-batch",
            provider,
            socket,
            monitor,
            typeof(SessionOrderingSession),
            runner,
            "test",
            inboundDispatchBudget: budget);
        var routingId = RoutingId.From("session-a");
        try
        {
            var firstFrame = EncodeJsonFrame(new SessionOrderingMessage());
            var secondFrame = EncodeJsonFrame(new SessionOrderingMessage());
            var thirdFrame = EncodeJsonFrame(new SessionOrderingMessage());
            var firstPart = new byte[firstFrame.Length + 1];
            firstFrame.CopyTo(firstPart, 0);
            secondFrame.AsSpan(0, 1).CopyTo(firstPart.AsSpan(firstFrame.Length));

            node.Start();
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                routingId,
                "local",
                "remote",
                0));
            socket.EnqueueRawPart(routingId, firstPart, hasMore: true);
            for (var part = 1; part < 64; part++)
                socket.EnqueueRawPart(routingId, [], hasMore: true);

            await WaitUntilAsync(() => socket.DequeuedPartCount >= 1);
            await lifetime.WaitDispatchStartedAsync(routingId);
            await WaitUntilAsync(() => socket.DequeuedPartCount >= 64);
            Assert.Equal(64, socket.DequeuedPartCount);
            Assert.False(budget.CanStartApplicationReceive);

            socket.EnqueueRawPart(routingId, secondFrame.AsSpan(1), hasMore: false);
            socket.EnqueueRawPart(routingId, thirdFrame, hasMore: false);
            await WaitUntilAsync(() => socket.DequeuedPartCount >= 65);
            Assert.Equal(65, socket.DequeuedPartCount);
            var dequeuedBeforePausedSend = socket.DequeuedPartCount;
            await Task.Delay(100);
            Assert.Equal(dequeuedBeforePausedSend, socket.DequeuedPartCount);
            Assert.Equal(
                1,
                lifetime.Events(routingId).Count(static value => value == "dispatch-start"));

            lifetime.ReleaseFirst.TrySetResult();
            await WaitUntilAsync(
                () => lifetime.Events(routingId)
                    .Count(static value => value == "dispatch-end") >= 2);
        }
        finally
        {
            lifetime.ReleaseFirst.TrySetResult();
            await node.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task Unidentified_monitor_lifecycle_does_not_attach_to_a_later_session()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new SessionOrderingLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton(_ => runtime);
        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "unidentified-monitor",
            provider,
            socket,
            monitor,
            typeof(SessionOrderingSession),
            runner,
            "test");
        var session = RoutingId.From("session-b");
        try
        {
            node.Start();
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                null,
                "stale-local",
                "stale-remote",
                0));
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.Disconnected,
                null,
                "stale-local",
                "stale-remote",
                0));
            await monitor.WaitReceivedAsync(2);

            EmitJson(socket, session, new SessionOrderingMessage());
            await Task.Delay(50);
            Assert.Empty(lifetime.Events(session));

            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                session,
                "local-b",
                "remote-b",
                0));

            await lifetime.WaitDispatchCompletedAsync(session);
            Assert.Equal(
                ("local-b", "remote-b"),
                lifetime.ConnectedAddresses(session));
        }
        finally
        {
            await node.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task Concurrent_sessions_use_only_exact_monitor_identity()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new SessionOrderingLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton(_ => runtime);
        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "concurrent-monitor-identity",
            provider,
            socket,
            monitor,
            typeof(SessionOrderingSession),
            runner,
            "test");
        var first = RoutingId.From("session-a");
        var second = RoutingId.From("session-b");
        try
        {
            node.Start();
            EmitJson(socket, first, new SessionOrderingMessage());
            EmitJson(socket, second, new SessionOrderingMessage());
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                null,
                "ambiguous-local-a",
                "ambiguous-remote-a",
                0));
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                null,
                "ambiguous-local-b",
                "ambiguous-remote-b",
                0));
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                second,
                "local-b",
                "remote-b",
                0));
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                first,
                "local-a",
                "remote-a",
                0));

            await lifetime.WaitDispatchCompletedAsync(second);
            await lifetime.WaitDispatchStartedAsync(first);
            Assert.Equal(("local-a", "remote-a"), lifetime.ConnectedAddresses(first));
            Assert.Equal(("local-b", "remote-b"), lifetime.ConnectedAddresses(second));
        }
        finally
        {
            lifetime.ReleaseFirst.TrySetResult();
            await node.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task Stream_node_preserves_typed_routing_id_from_backend_callback()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new RoutingIdentityLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton(_ => runtime);

        await using var provider = services.BuildServiceProvider();
        runtime = new ZLinkFrameworkRuntime(
            provider,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                provider.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "typed-routing-id",
            provider,
            socket,
            monitor,
            typeof(RoutingIdentitySession),
            runner,
            "test");
        try
        {
            node.Start();
            var expected = RoutingId.From([0x00, 0x7f, 0x80, 0xff]);
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                expected,
                "local",
                "remote",
                0));
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.None,
                null,
                nameof(RoutingIdentityMessage),
                ZlinkStreamMetadata.Empty);

            socket.Emit(
                expected,
                Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span),
                Message.From(ZLinkStreamPacketPayloadCodec.EncodeJson(
                    new RoutingIdentityMessage(),
                    typeof(RoutingIdentityMessage))));

            Assert.Equal(expected, await lifetime.Observed.Task.WaitAsync(TimeSpan.FromSeconds(2)));
        }
        finally
        {
            await node.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task Stream_node_shutdown_upper_bound_does_not_wait_for_cancellation_callback()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new CancellationAwareLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddScoped<CancellationAwareDependency>()
            .AddSingleton(_ => runtime);

        await using var provider = services.BuildServiceProvider();
        runtime = new ZLinkFrameworkRuntime(
            provider,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                provider.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "forced-cancellation",
            provider,
            socket,
            monitor,
            typeof(CancellationAwareSession),
            runner,
            "test");
        try
        {
            node.Start();
            var routingId = RoutingId.From("node-force");
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                routingId,
                "local",
                "remote",
                0));
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.None,
                null,
                nameof(CancellationAwareMessage),
                ZlinkStreamMetadata.Empty);
            socket.Emit(
                routingId,
                Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span),
                Message.From(ZLinkStreamPacketPayloadCodec.EncodeJson(
                    new CancellationAwareMessage(),
                    typeof(CancellationAwareMessage))));
            await lifetime.Entered.Task.WaitAsync(TimeSpan.FromSeconds(2));

            var shutdown = node.DisposeAsync().AsTask();
            await lifetime.CancellationCallbackStarted.Task.WaitAsync(TimeSpan.FromSeconds(10));
            await shutdown.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.Equal(0, lifetime.HandlerDisposeCount);
            Assert.Equal(0, lifetime.DependencyDisposeCount);
        }
        finally
        {
            lifetime.AllowCancellationCallback.TrySetResult();
            await lifetime.CleanupCompleted.Task.WaitAsync(TimeSpan.FromSeconds(2));
            await runner.StopAsync();
        }

        Assert.Equal(1, lifetime.HandlerDisposeCount);
        Assert.Equal(1, lifetime.DependencyDisposeCount);
    }

    [Fact]
    public async Task Stream_node_shutdown_upper_bound_does_not_wait_for_terminal_cancellation_callback()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new TerminalCancellationLifetime { CloseFromDispatch = true };
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddScoped<TerminalCancellationDependency>()
            .AddSingleton(_ => runtime);

        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var monitor = new TestSocketMonitor();
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None,
            runtime.ExecutionOwner);
        var node = new ZLinkStreamNodeRuntime(
            "terminal-cancellation",
            provider,
            socket,
            monitor,
            typeof(TerminalCancellationSession),
            runner,
            "test");
        try
        {
            node.Start();
            var routingId = RoutingId.From("terminal-force");
            monitor.Emit(new ZLinkBackendSocketMonitorEvent(
                ZLinkSocketNativeEventType.ConnectionReady,
                routingId,
                "local",
                "remote",
                0));
            EmitJson(socket, routingId, new TerminalCancellationMessage());
            await lifetime.DisconnectedStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));

            var shutdown = node.DisposeAsync().AsTask();
            await lifetime.CancellationCallbackStarted.Task.WaitAsync(TimeSpan.FromSeconds(10));
            await shutdown.WaitAsync(TimeSpan.FromSeconds(2));
            Assert.Equal(0, lifetime.DependencyDisposeCount);
        }
        finally
        {
            lifetime.AllowCancellationCallback.TrySetResult();
            await lifetime.CleanupCompleted.Task.WaitAsync(TimeSpan.FromSeconds(2));
            await runner.StopAsync();
        }

        Assert.Equal(1, lifetime.DependencyDisposeCount);
        Assert.True(lifetime.CancellationObserved.Task.IsCompletedSuccessfully);
    }

    [Fact]
    public async Task Terminal_cancellation_callback_failure_is_reported_after_scope_cleanup()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new TerminalCancellationLifetime { ThrowFromCancellationCallback = true };
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddScoped<TerminalCancellationDependency>()
            .AddSingleton(_ => runtime);

        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var session = await ZLinkStreamSessionRuntime.CreateAsync(
            provider,
            new TestStreamSocket(),
            RoutingId.From("terminal-failure"),
            typeof(TerminalCancellationSession),
            static _ => { },
            "test",
            TimeProvider.System);
        var disposeTask = session.DisposeAsync().AsTask();
        await lifetime.DisconnectedStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));

        await session.ForceCloseForShutdownAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(1));
        await lifetime.CancellationCallbackStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        lifetime.AllowCancellationCallback.TrySetResult();

        var failure = await Assert.ThrowsAsync<AggregateException>(
            () => disposeTask.WaitAsync(TimeSpan.FromSeconds(2)));
        Assert.Equal(
            "terminal cancellation callback failed",
            Assert.IsType<InvalidOperationException>(Assert.Single(failure.InnerExceptions)).Message);
        await lifetime.CleanupCompleted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.Equal(1, lifetime.DependencyDisposeCount);
    }

    [Fact]
    public async Task Rejected_terminal_work_starts_disposal_and_releases_the_session_scope()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new RejectedTerminalLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddScoped<RejectedTerminalDependency>()
            .AddSingleton(_ => runtime);

        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket();
        var session = await ZLinkStreamSessionRuntime.CreateAsync(
            provider,
            socket,
            RoutingId.From("rejected-terminal"),
            typeof(RejectedTerminalSession),
            _ => lifetime.RemoveCount++,
            "test",
            TimeProvider.System);

        session.RequestStop();
        session.EnqueueDisconnected(new ZLinkStreamError(
            ZLinkStreamSessionError.TransportError,
            "transport closed"));

        await lifetime.CleanupCompleted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await session.DisposeAsync();

        Assert.Equal(1, socket.DisconnectCount);
        Assert.Equal(1, lifetime.DisconnectedCount);
        Assert.Equal(1, lifetime.DependencyDisposeCount);
        Assert.Equal(1, lifetime.RemoveCount);
    }

    [Fact]
    public async Task Rejected_terminal_disposal_and_force_close_share_one_transport_close()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new RejectedTerminalLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddScoped<RejectedTerminalDependency>()
            .AddSingleton(_ => runtime);

        await using var provider = services.BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var socket = new TestStreamSocket { BlockDisconnect = true };
        var session = await ZLinkStreamSessionRuntime.CreateAsync(
            provider,
            socket,
            RoutingId.From("rejected-terminal-force-race"),
            typeof(RejectedTerminalSession),
            _ => lifetime.RemoveCount++,
            "test",
            TimeProvider.System);

        session.RequestStop();
        var rejectedTerminal = Task.Run(() => session.EnqueueDisconnected(new ZLinkStreamError(
            ZLinkStreamSessionError.TransportError,
            "transport closed")));
        await socket.DisconnectStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));

        var forcedClose = session.ForceCloseForShutdownAsync().AsTask();
        await AssertStillRunningAsync(forcedClose);
        socket.AllowDisconnect.TrySetResult();

        await rejectedTerminal.WaitAsync(TimeSpan.FromSeconds(2));
        await forcedClose.WaitAsync(TimeSpan.FromSeconds(2));
        await lifetime.CleanupCompleted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await session.DisposeAsync();

        Assert.Equal(1, socket.DisconnectCount);
        Assert.Equal(1, lifetime.DependencyDisposeCount);
        Assert.Equal(1, lifetime.RemoveCount);
    }

    [Fact]
    public async Task Forced_shutdown_cancels_cooperative_handler_before_late_cleanup()
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new CancellationAwareLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddScoped<CancellationAwareDependency>()
            .AddSingleton(_ => runtime);

        await using var provider = services.BuildServiceProvider();
        runtime = new ZLinkFrameworkRuntime(
            provider,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                provider.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var session = await ZLinkStreamSessionRuntime.CreateAsync(
            provider,
            new TestStreamSocket(),
            RoutingId.From("forced-cancellation"),
            typeof(CancellationAwareSession),
            static _ => { },
            "test",
            TimeProvider.System);

        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            nameof(CancellationAwareMessage),
            ZlinkStreamMetadata.Empty);
        session.EnqueuePacket(
            Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span),
            Message.From(ZLinkStreamPacketPayloadCodec.EncodeJson(
                new CancellationAwareMessage(),
                typeof(CancellationAwareMessage))));
        await lifetime.Entered.Task.WaitAsync(TimeSpan.FromSeconds(2));

        var disposeTask = session.DisposeAsync().AsTask();
        await AssertStillRunningAsync(disposeTask);
        await session.ForceCloseForShutdownAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(1));

        await lifetime.CancellationCallbackStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await AssertStillRunningAsync(disposeTask);
        Assert.Equal(0, lifetime.HandlerDisposeCount);
        Assert.Equal(0, lifetime.DependencyDisposeCount);
        lifetime.AllowCancellationCallback.TrySetResult();
        await lifetime.CancellationCallbackCompleted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await lifetime.CancellationObserved.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await disposeTask.WaitAsync(TimeSpan.FromSeconds(2));
        await session.DisposeAsync();

        Assert.Equal(1, lifetime.HandlerDisposeCount);
        Assert.Equal(1, lifetime.DependencyDisposeCount);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Forced_shutdown_disposes_handler_and_scoped_dependency_after_blocked_work_drains(
        bool registerHandler)
    {
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new HandlerLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddScoped<ScopedDependency>()
            .AddSingleton(_ => runtime);
        if (registerHandler) services.AddScoped<BlockingSessionHandler>();

        await using var provider = services.BuildServiceProvider();
        runtime = new ZLinkFrameworkRuntime(
            provider,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                provider.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var socket = new TestStreamSocket();
        var session = await ZLinkStreamSessionRuntime.CreateAsync(
            provider,
            socket,
            RoutingId.From("forced-cleanup"),
            typeof(BlockingSession),
            static _ => { },
            "test",
            TimeProvider.System);

        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            nameof(BlockingMessage),
            ZlinkStreamMetadata.Empty);
        session.EnqueuePacket(
            Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span),
            Message.From(ZLinkStreamPacketPayloadCodec.EncodeJson(
                new BlockingMessage(),
                typeof(BlockingMessage))));
        await lifetime.Entered.Task.WaitAsync(TimeSpan.FromSeconds(2));

        var disposeTask = session.DisposeAsync().AsTask();
        await AssertStillRunningAsync(disposeTask);
        await session.ForceCloseForShutdownAsync();
        await AssertStillRunningAsync(disposeTask);

        lifetime.Release.TrySetResult();
        await lifetime.HandlerDisposeStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await AssertStillRunningAsync(disposeTask);
        lifetime.AllowHandlerDispose.TrySetResult();
        await lifetime.DependencyDisposeStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        await AssertStillRunningAsync(disposeTask);
        lifetime.AllowDependencyDispose.TrySetResult();
        await disposeTask.WaitAsync(TimeSpan.FromSeconds(2));
        await session.DisposeAsync();

        Assert.Equal(1, lifetime.HandlerDisposeCount);
        Assert.Equal(1, lifetime.DependencyDisposeCount);
        Assert.Equal(0, lifetime.DisconnectedCount);
    }

    private static async Task AssertStillRunningAsync(Task task)
    {
        var marker = Task.Delay(TimeSpan.FromMilliseconds(50));
        Assert.Same(marker, await Task.WhenAny(task, marker));
    }

    private static async Task WaitUntilAsync(Func<bool> predicate)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
        while (!predicate())
        {
            Assert.True(DateTime.UtcNow < deadline);
            await Task.Delay(5);
        }
    }

    private static ZlinkStreamHeader DecodeStreamHeader(byte[] frame)
    {
        Assert.True(ZLinkStreamFrameCodec.TryDecode(frame, out var headerBytes, out _));
        return ZLinkStreamProtocolDefaults.DecodeHeader(headerBytes.ToArray());
    }

    private static ZLinkFrameworkRuntime CreateRuntime(
        IServiceProvider provider,
        ZLinkFrameworkRegistration registration)
    {
        var runtime = new ZLinkFrameworkRuntime(
            provider,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                provider.GetRequiredService<IServiceScopeFactory>(),
                registration));
        return runtime;
    }

    private sealed class StreamFlowLoggerFactory : ILoggerFactory
    {
        public List<string> Categories { get; } = [];

        public List<string> Messages { get; } = [];

        public void AddProvider(ILoggerProvider provider) => _ = provider;

        public ILogger CreateLogger(string categoryName)
        {
            Categories.Add(categoryName);
            return new StreamFlowLogger(Messages);
        }

        public void Dispose() { }
    }

    private sealed class StreamFlowLogger(List<string> messages) : ILogger
    {
        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter) => messages.Add(formatter(state, exception));
    }

    private static void EmitJson<TMessage>(
        TestStreamSocket socket,
        RoutingId routingId,
        TMessage message)
    {
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            typeof(TMessage).Name,
            ZlinkStreamMetadata.Empty);
        socket.Emit(
            routingId,
            Message.From(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span),
            Message.From(ZLinkStreamPacketPayloadCodec.EncodeJson(message, typeof(TMessage))));
    }

    private static byte[] EncodeJsonFrame<TMessage>(TMessage message)
    {
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            typeof(TMessage).Name,
            ZlinkStreamMetadata.Empty);
        return ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            ZLinkStreamPacketPayloadCodec.EncodeJson(message, typeof(TMessage)));
    }

    private sealed class BlockingSession(
        IZLinkSessionContext context,
        HandlerLifetime lifetime) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddHandler<BlockingSessionHandler>();
        }

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
        {
            lifetime.DisconnectedCount++;
            return ValueTask.CompletedTask;
        }

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken)
        {
            _ = await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken);
        }
    }

    private sealed class StreamFlowSession(
        IZLinkSessionContext context,
        StreamFlowLifetime lifetime) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken)
        {
            await Context.Client.Reply(new StreamFlowReply("reply"))
                .Async(cancellationToken);
            lifetime.ReplyCompleted.TrySetResult();
        }
    }

    private sealed record StreamFlowRequest(string Value);

    private sealed record StreamFlowReply(string Value);

    private sealed class StreamFlowLifetime
    {
        public TaskCompletionSource ReplyCompleted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed record BlockingMessage;

    private sealed class RejectedTerminalSession(
        IZLinkSessionContext context,
        RejectedTerminalLifetime lifetime,
        RejectedTerminalDependency dependency) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
        {
            _ = dependency;
            lifetime.DisconnectedCount++;
            return ValueTask.CompletedTask;
        }

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;
    }

    private sealed class RejectedTerminalDependency(RejectedTerminalLifetime lifetime) : IAsyncDisposable
    {
        public ValueTask DisposeAsync()
        {
            lifetime.DependencyDisposeCount++;
            lifetime.CleanupCompleted.TrySetResult();
            return ValueTask.CompletedTask;
        }
    }

    private sealed class RejectedTerminalLifetime
    {
        public TaskCompletionSource CleanupCompleted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int DisconnectedCount { get; set; }

        public int DependencyDisposeCount { get; set; }

        public int RemoveCount { get; set; }
    }

    private sealed class TerminalCancellationSession(
        IZLinkSessionContext context,
        TerminalCancellationLifetime lifetime,
        TerminalCancellationDependency dependency) : IZLinkSession
    {
        private CancellationTokenRegistration _registration;

        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public async ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
        {
            _ = dependency;
            _registration = cancellationToken.Register(() =>
            {
                lifetime.CancellationCallbackStarted.TrySetResult();
                lifetime.AllowCancellationCallback.Task.GetAwaiter().GetResult();
                if (lifetime.ThrowFromCancellationCallback)
                    throw new InvalidOperationException("terminal cancellation callback failed");
            });
            lifetime.DisconnectedStarted.TrySetResult();
            try
            {
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                lifetime.CancellationObserved.TrySetResult();
            }
            finally
            {
                _registration.Dispose();
            }
        }

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken)
        {
            lifetime.DispatchObserved.TrySetResult();
            if (lifetime.CloseFromDispatch)
                await Context.CloseAsync();
        }
    }

    private sealed record TerminalCancellationMessage;

    private sealed class TerminalCancellationDependency(TerminalCancellationLifetime lifetime) : IAsyncDisposable
    {
        public ValueTask DisposeAsync()
        {
            lifetime.DependencyDisposeCount++;
            lifetime.CleanupCompleted.TrySetResult();
            return ValueTask.CompletedTask;
        }
    }

    private sealed class TerminalCancellationLifetime
    {
        public TaskCompletionSource DispatchObserved { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource DisconnectedStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource CancellationCallbackStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource AllowCancellationCallback { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource CancellationObserved { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource CleanupCompleted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public bool ThrowFromCancellationCallback { get; init; }

        public bool CloseFromDispatch { get; init; }

        public int DependencyDisposeCount { get; set; }
    }

    private sealed class RoutingIdentitySession(
        IZLinkSessionContext context,
        RoutingIdentityLifetime lifetime) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken)
        {
            lifetime.Observed.TrySetResult(Context.RoutingId!.Value);
            return ValueTask.CompletedTask;
        }
    }

    private sealed record RoutingIdentityMessage;

    private sealed class RoutingIdentityLifetime
    {
        public TaskCompletionSource<RoutingId> Observed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class CancellationAwareSession(IZLinkSessionContext context) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public void Configure()
        {
            Context.Handlers.AddHandler<CancellationAwareHandler>();
        }

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken)
        {
            _ = await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken);
        }
    }

    private sealed record CancellationAwareMessage;

    private sealed class CancellationAwareHandler(
        CancellationAwareLifetime lifetime,
        CancellationAwareDependency dependency)
        : IZLinkSessionPacketHandler<IZLinkSessionContext, CancellationAwareMessage>, IAsyncDisposable
    {
        private CancellationTokenRegistration _registration;

        public async ValueTask HandleAsync(
            IZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            CancellationAwareMessage message,
            CancellationToken cancellationToken)
        {
            _ = context;
            _ = dispatch;
            _ = message;
            _ = dependency;
            _registration = cancellationToken.Register(() =>
            {
                lifetime.CancellationCallbackStarted.TrySetResult();
                lifetime.AllowCancellationCallback.Task.GetAwaiter().GetResult();
                lifetime.CancellationCallbackCompleted.TrySetResult();
            });
            lifetime.Entered.TrySetResult();
            try
            {
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                lifetime.CancellationObserved.TrySetResult();
                throw;
            }
        }

        public async ValueTask DisposeAsync()
        {
            _registration.Dispose();
            await Task.Yield();
            lifetime.HandlerDisposeCount++;
        }
    }

    private sealed class CancellationAwareDependency(CancellationAwareLifetime lifetime) : IAsyncDisposable
    {
        public async ValueTask DisposeAsync()
        {
            await Task.Yield();
            lifetime.DependencyDisposeCount++;
            lifetime.CleanupCompleted.TrySetResult();
        }
    }

    private sealed class CancellationAwareLifetime
    {
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource CancellationObserved { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource CancellationCallbackStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource AllowCancellationCallback { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource CancellationCallbackCompleted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource CleanupCompleted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int HandlerDisposeCount { get; set; }

        public int DependencyDisposeCount { get; set; }
    }

    private sealed class BlockingSessionHandler(
        HandlerLifetime lifetime,
        ScopedDependency dependency)
        : IZLinkSessionPacketHandler<IZLinkSessionContext, BlockingMessage>, IAsyncDisposable
    {
        public async ValueTask HandleAsync(
            IZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            BlockingMessage message,
            CancellationToken cancellationToken)
        {
            _ = context;
            _ = dispatch;
            _ = message;
            _ = cancellationToken;
            _ = dependency;
            lifetime.Entered.TrySetResult();
            await lifetime.Release.Task;
        }

        public async ValueTask DisposeAsync()
        {
            lifetime.HandlerDisposeStarted.TrySetResult();
            await lifetime.AllowHandlerDispose.Task;
            lifetime.HandlerDisposeCount++;
        }
    }

    private sealed class ScopedDependency(HandlerLifetime lifetime) : IAsyncDisposable
    {
        public async ValueTask DisposeAsync()
        {
            lifetime.DependencyDisposeStarted.TrySetResult();
            await lifetime.AllowDependencyDispose.Task;
            lifetime.DependencyDisposeCount++;
        }
    }

    private sealed class HandlerLifetime
    {
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource HandlerDisposeStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource AllowHandlerDispose { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource DependencyDisposeStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource AllowDependencyDispose { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int HandlerDisposeCount { get; set; }

        public int DependencyDisposeCount { get; set; }

        public int DisconnectedCount { get; set; }
    }

    private sealed record SessionOrderingMessage;

    private sealed class SessionOrderingSession(
        IZLinkSessionContext context,
        SessionOrderingLifetime lifetime) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lifetime.RecordConnected(
                Context.RoutingId!.Value,
                Context.LocalAddr!,
                Context.RemoteAddr!);
            lifetime.Record(Context.RoutingId!.Value, "connected");
            lifetime.SignalConnected(Context.RoutingId.Value);
            return ValueTask.CompletedTask;
        }

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken)
        {
            _ = error;
            cancellationToken.ThrowIfCancellationRequested();
            lifetime.Record(Context.RoutingId!.Value, "error");
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lifetime.Record(Context.RoutingId!.Value, "disconnected");
            lifetime.SignalDisconnected(Context.RoutingId.Value);
            return ValueTask.CompletedTask;
        }

        public async ValueTask OnDispatchAsync(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload,
            CancellationToken cancellationToken)
        {
            _ = dispatch;
            _ = payload;
            var routingId = Context.RoutingId!.Value;
            lifetime.Record(routingId, "dispatch-start");
            lifetime.SignalDispatchStarted(routingId);
            if (routingId == RoutingId.From("session-a"))
                await lifetime.ReleaseFirst.Task.WaitAsync(cancellationToken);
            lifetime.Record(routingId, "dispatch-end");
            lifetime.SignalDispatchCompleted(routingId);
        }
    }

    private sealed class SessionOrderingLifetime
    {
        private readonly object _gate = new();
        private readonly Dictionary<string, List<string>> _events = new(StringComparer.Ordinal);
        private readonly Dictionary<string, TaskCompletionSource> _connected = new(StringComparer.Ordinal);
        private readonly Dictionary<string, TaskCompletionSource> _dispatchStarted = new(StringComparer.Ordinal);
        private readonly Dictionary<string, TaskCompletionSource> _dispatchCompleted = new(StringComparer.Ordinal);
        private readonly Dictionary<string, TaskCompletionSource> _disconnected = new(StringComparer.Ordinal);
        private readonly Dictionary<string, (string LocalAddr, string RemoteAddr)>
            _connectedAddresses = new(StringComparer.Ordinal);

        public TaskCompletionSource ReleaseFirst { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public void Record(RoutingId routingId, string value)
        {
            lock (_gate)
            {
                var key = routingId.ToHex();
                if (!_events.TryGetValue(key, out var events))
                    _events.Add(key, events = []);
                events.Add(value);
            }
        }

        public string[] Events(RoutingId routingId)
        {
            lock (_gate)
                return _events.TryGetValue(routingId.ToHex(), out var events)
                    ? events.ToArray()
                    : [];
        }

        public void RecordConnected(
            RoutingId routingId,
            string localAddr,
            string remoteAddr)
        {
            lock (_gate)
                _connectedAddresses[routingId.ToHex()] = (localAddr, remoteAddr);
        }

        public (string LocalAddr, string RemoteAddr) ConnectedAddresses(
            RoutingId routingId)
        {
            lock (_gate) return _connectedAddresses[routingId.ToHex()];
        }

        public bool IsDispatchCompleted(RoutingId routingId)
        {
            lock (_gate)
                return Signal(_dispatchCompleted, routingId).Task.IsCompleted;
        }

        public void SignalConnected(RoutingId routingId) => Signal(_connected, routingId).TrySetResult();

        public void SignalDispatchStarted(RoutingId routingId) =>
            Signal(_dispatchStarted, routingId).TrySetResult();

        public void SignalDispatchCompleted(RoutingId routingId) =>
            Signal(_dispatchCompleted, routingId).TrySetResult();

        public void SignalDisconnected(RoutingId routingId) =>
            Signal(_disconnected, routingId).TrySetResult();

        public Task WaitConnectedAsync(RoutingId routingId) =>
            Signal(_connected, routingId).Task.WaitAsync(TimeSpan.FromSeconds(2));

        public Task WaitDispatchStartedAsync(RoutingId routingId) =>
            Signal(_dispatchStarted, routingId).Task.WaitAsync(TimeSpan.FromSeconds(2));

        public Task WaitDispatchCompletedAsync(RoutingId routingId) =>
            Signal(_dispatchCompleted, routingId).Task.WaitAsync(TimeSpan.FromSeconds(2));

        public Task WaitDisconnectedAsync(RoutingId routingId) =>
            Signal(_disconnected, routingId).Task.WaitAsync(TimeSpan.FromSeconds(2));

        private TaskCompletionSource Signal(
            Dictionary<string, TaskCompletionSource> signals,
            RoutingId routingId)
        {
            lock (_gate)
            {
                var key = routingId.ToHex();
                if (!signals.TryGetValue(key, out var signal))
                    signals.Add(key, signal = new TaskCompletionSource(
                        TaskCreationOptions.RunContinuationsAsynchronously));
                return signal;
            }
        }
    }

    private sealed class TestStreamSocket : IZLinkBackendStreamSocket
    {
        private readonly System.Collections.Concurrent.ConcurrentQueue<(
            RoutingId? RoutingId,
            Message Part,
            bool HasMore)>
            _receivedParts = new();
        private readonly AutoResetEvent _receiveSignal = new(false);
        private int _recvPartCount;
        private int _dequeuedPartCount;
        public bool BlockDisconnect { get; init; }
        public bool BlockDispose { get; init; }
        public Exception? DisposeFailure { get; init; }
        private int _disposeCount;
        public int DisposeCount => Volatile.Read(ref _disposeCount);
        public TaskCompletionSource DisposeStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource AllowDispose { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource DisconnectStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource AllowDisconnect { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource<byte[]> SentFrame { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource UnidentifiedPartConsumed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int RecvPartCount => Volatile.Read(ref _recvPartCount);

        public int DequeuedPartCount => Volatile.Read(ref _dequeuedPartCount);

        public void Bind(string endpoint) { }

        public void SetChannelName(string channelName) { }

        public void SetTlsServer(string certPath, string keyPath, bool requireClientCert) { }

        public void OnSendReady(Action handler) { }

        public IZLinkBackendSocketPoller CreateReceivePoller() =>
            new TestStreamSocketPoller(
                () => !_receivedParts.IsEmpty,
                _receiveSignal);

        public bool RecvPart(
            out RoutingId? sourceRoutingId,
            out Message? part,
            out bool hasMore,
            RecvFlags flags = RecvFlags.None)
        {
            Interlocked.Increment(ref _recvPartCount);
            if (_receivedParts.TryDequeue(out var received))
            {
                Interlocked.Increment(ref _dequeuedPartCount);
                if (_receivedParts.IsEmpty)
                    _receiveSignal.Reset();
                sourceRoutingId = received.RoutingId;
                if (received.RoutingId is null)
                    UnidentifiedPartConsumed.TrySetResult();
                part = received.Part;
                hasMore = received.HasMore;
                return true;
            }

            sourceRoutingId = null;
            part = null;
            hasMore = false;
            return false;
        }

        public void EnqueueRawPart(
            RoutingId routingId,
            ReadOnlySpan<byte> bytes,
            bool hasMore = false) =>
            EnqueuePart(routingId, Message.From(bytes), hasMore);

        public void EnqueueUnidentifiedRawPart(ReadOnlySpan<byte> bytes) =>
            EnqueuePart(null, Message.From(bytes), false);

        private void EnqueuePart(
            RoutingId? routingId,
            Message part,
            bool hasMore)
        {
            _receivedParts.Enqueue((routingId, part, hasMore));
            _receiveSignal.Set();
        }

        public void Emit(RoutingId routingId, Message header, Message payload)
        {
            try
            {
                var frame = ZLinkStreamFrameCodec.Encode(
                    header.AsReadOnlySpan(), payload.AsReadOnlySpan());
                EnqueuePart(routingId, Message.From(frame), false);
            }
            finally
            {
                header.Dispose();
                payload.Dispose();
            }
        }

        public bool Send(RoutingId routingId, Message payload, SendFlags flags)
        {
            SentFrame.TrySetResult(payload.ToArray());
            return true;
        }

        public bool Send(RoutingId routingId, IReadOnlyList<Message> parts, SendFlags flags) => true;

        public int DisconnectCount { get; private set; }

        public void DisconnectPeer(RoutingId routingId)
        {
            DisconnectCount++;
            DisconnectStarted.TrySetResult();
            if (BlockDisconnect) AllowDisconnect.Task.GetAwaiter().GetResult();
        }

        public ValueTask BindActorAsync(
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            TimeSpan timeout,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public ValueTask UnbindActorAsync(
            RoutingId sessionRid,
            string actorId,
            TimeSpan timeout,
            CancellationToken cancellationToken) => ValueTask.CompletedTask;

        public bool SendBoundActor(
            RoutingId sessionRid,
            string actorId,
            IReadOnlyList<Message> parts,
            SendFlags flags) => true;

        public async ValueTask DisposeAsync()
        {
            Interlocked.Increment(ref _disposeCount);
            DisposeStarted.TrySetResult();
            if (BlockDispose) await AllowDispose.Task.ConfigureAwait(false);
            while (_receivedParts.TryDequeue(out var received))
                received.Part.Dispose();
            _receiveSignal.Set();
            if (DisposeFailure is not null)
                System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(DisposeFailure).Throw();
        }
    }

    private sealed class TestStreamSocketPoller(
        Func<bool> isReadable,
        AutoResetEvent signal) : IZLinkBackendSocketPoller
    {
        public PollEventFlags Wait(TimeSpan timeout)
        {
            if (!isReadable() && timeout > TimeSpan.Zero)
                signal.WaitOne(timeout);
            return isReadable() ? PollEventFlags.PollIn : PollEventFlags.None;
        }

        public void Dispose() => signal.Set();
    }

    private sealed class TestSocketMonitor : IZLinkBackendSocketMonitor
    {
        private readonly System.Collections.Concurrent.ConcurrentQueue<ZLinkBackendSocketMonitorEvent> _events = new();
        private readonly AutoResetEvent _eventSignal = new(false);
        private int _waitCount;
        private int _emptyPollCount;
        private int _receivedCount;

        public int WaitCount => Volatile.Read(ref _waitCount);

        public int EmptyPollCount => Volatile.Read(ref _emptyPollCount);

        public bool Wait(TimeSpan timeout)
        {
            Interlocked.Increment(ref _waitCount);
            if (_events.IsEmpty && timeout > TimeSpan.Zero)
                _eventSignal.WaitOne(timeout);
            return !_events.IsEmpty;
        }

        public void OnEvent(Action<ZLinkBackendSocketMonitorEvent> handler) { }

        public void Emit(ZLinkBackendSocketMonitorEvent monitorEvent)
        {
            _events.Enqueue(monitorEvent);
            _eventSignal.Set();
        }

        public async Task WaitReceivedAsync(int count)
        {
            using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(2));
            while (Volatile.Read(ref _receivedCount) < count)
                await Task.Delay(5, timeout.Token);
        }

        public bool TryRecv(out ZLinkBackendSocketMonitorEvent monitorEvent)
        {
            // The production monitor loop uses non-blocking receive with a
            // bounded backoff; count each receive attempt for the startup
            // assertion below.
            Interlocked.Increment(ref _waitCount);
            if (_events.TryDequeue(out monitorEvent))
            {
                if (_events.IsEmpty)
                    _eventSignal.Reset();
                Interlocked.Increment(ref _receivedCount);
                return true;
            }
            Interlocked.Increment(ref _emptyPollCount);
            return false;
        }

        public ValueTask DisposeAsync()
        {
            _eventSignal.Set();
            return ValueTask.CompletedTask;
        }
    }
}
