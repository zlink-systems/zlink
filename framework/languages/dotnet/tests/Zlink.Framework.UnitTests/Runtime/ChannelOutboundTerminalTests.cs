using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests.Runtime;

//  Pins the spec 26 §2.1/§2.2 channel outbound terminals added for parity with
//  Java (ZLinkChannelDirectCalls/ZLinkChannelRouteCalls) and Node
//  (channel-outbound-operations): every channel/node request records `sent` on
//  local-transport acceptance and exactly one `reply_received` terminal, and the
//  spot rejection replies honour the live spec 27 §4 flow gate.
public sealed class ChannelOutboundTerminalTests
{
    private const string ValidFlowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d";
    private const string MalformedFlowId = "zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz";

    [Fact]
    public void ChannelRequestTerminalEmitsExactlyOnceWithClientServerAttributes()
    {
        var activities = CaptureActivities(out var listener);
        using (listener)
        {
            var options = new ZLinkDispatchOptionsModel();
            options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            var tracer = new ZLinkMessageFlowTracer(options);
            var terminal = new ZLinkChannelRequestTerminalTrace(
                tracer,
                ZLinkDispatchErrorSurface.Channel,
                "TerminalPinPacket",
                "term-channel");
            terminal.SetCorrelation("corr-terminal");
            terminal.Succeeded();
            terminal.Dispose();
            terminal.Dispose();
        }

        var activity = Assert.Single(FlowActivities(activities, "TerminalPinPacket"));
        Assert.Equal("reply_received", activity.GetTagItem("phase"));
        Assert.Equal("channel", activity.GetTagItem("surface"));
        Assert.Equal("request", activity.GetTagItem("message_kind"));
        Assert.Equal("succeeded", activity.GetTagItem("outcome"));
        Assert.Equal("client_server", activity.GetTagItem("channel_route_kind"));
        Assert.Equal("term-channel", activity.GetTagItem("channel_name"));
        Assert.Equal("corr-terminal", activity.GetTagItem("correlation_id"));
        Assert.Null(activity.GetTagItem("duration_seconds"));
    }

    [Theory]
    [InlineData(ZLinkFrameworkErrorKind.ShuttingDown, "shutdown")]
    [InlineData(ZLinkFrameworkErrorKind.CapacityExceeded, "backpressured")]
    [InlineData(ZLinkFrameworkErrorKind.NotFound, "failed")]
    public void ChannelRequestTerminalClassifiesFailuresAndPassesAtErrorsLevel(
        ZLinkFrameworkErrorKind kind,
        string expectedOutcome)
    {
        var activities = CaptureActivities(out var listener);
        using (listener)
        {
            var options = new ZLinkDispatchOptionsModel();
            options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Errors);
            var tracer = new ZLinkMessageFlowTracer(options);

            //  Succeeded terminals are Normal-phase records: suppressed at Errors.
            var succeeded = new ZLinkChannelRequestTerminalTrace(
                tracer,
                ZLinkDispatchErrorSurface.Channel,
                "TerminalClassifyPacket",
                "term-channel");
            succeeded.Succeeded();
            succeeded.Dispose();

            var failed = new ZLinkChannelRequestTerminalTrace(
                tracer,
                ZLinkDispatchErrorSurface.Channel,
                "TerminalClassifyPacket",
                "term-channel");
            failed.Failed(new ZLinkFrameworkException(kind, "terminal failure"));
            failed.Dispose();
        }

        var activity = Assert.Single(FlowActivities(activities, "TerminalClassifyPacket"));
        Assert.Equal("reply_received", activity.GetTagItem("phase"));
        Assert.Equal(expectedOutcome, activity.GetTagItem("outcome"));
    }

    [Fact]
    public void ChannelRequestTerminalClassifiesCancellationAndAddsDurationAtDetailed()
    {
        var activities = CaptureActivities(out var listener);
        using (listener)
        {
            var options = new ZLinkDispatchOptionsModel();
            options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
            var tracer = new ZLinkMessageFlowTracer(options);
            var terminal = new ZLinkChannelRequestTerminalTrace(
                tracer,
                ZLinkDispatchErrorSurface.RouteMeshChannel,
                "TerminalRouteMeshPacket",
                meshName: "mesh-a",
                targetRid: "node-b");
            terminal.Failed(new OperationCanceledException());
            terminal.Dispose();
        }

        var activity = Assert.Single(FlowActivities(activities, "TerminalRouteMeshPacket"));
        Assert.Equal("cancelled", activity.GetTagItem("outcome"));
        Assert.Equal("route_mesh", activity.GetTagItem("channel_route_kind"));
        Assert.Equal("mesh-a", activity.GetTagItem("mesh_name"));
        Assert.Equal("node-b", activity.GetTagItem("target_rid"));
        Assert.NotNull(activity.GetTagItem("duration_seconds"));
    }

    [Fact]
    public async Task ClientServerOutboundEmitsSentOnceAndOneRequestTerminal()
    {
        var port = ReservePort();
        var activities = CaptureActivities(out var listener);
        using (listener)
        {
            await using var server = CreateTerminalServer(port);
            await using var client = CreateTerminalClient(port);
            var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
            var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
            client.GetRequiredService<ZLinkFrameworkRegistration>()
                .DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);

            await serverRuntime.StartAsync(CancellationToken.None);
            await clientRuntime.StartAsync(CancellationToken.None);
            try
            {
                await WaitUntilAsync(
                    () => clientRuntime.GetClientServerClientRuntime("term-work").ReadyCount == 1,
                    TimeSpan.FromSeconds(10));

                await client.GetRequiredService<IZLinkRouteClient>()
                    .SendToChannel("term-work", new TerminalEchoSend("one-way"))
                    .Async();
                Assert.Equal(
                    "one-way",
                    await server.GetRequiredService<TerminalProbe>().Received.Task
                        .WaitAsync(TimeSpan.FromSeconds(5)));

                var reply = await client.GetRequiredService<IZLinkRouteClient>()
                    .RequestToChannel("term-work", new TerminalEchoRequest("ready"))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<TerminalEchoReply>();
                Assert.Equal("ready", reply.Value);
            }
            finally
            {
                await clientRuntime.StopAsync(CancellationToken.None);
                await serverRuntime.StopAsync(CancellationToken.None);
            }
        }

        //  One-way send: one `sent`, no correlation, ClientServer target recorded.
        var sendSent = Assert.Single(FlowActivities(activities, nameof(TerminalEchoSend))
            .Where(activity => Equals(activity.GetTagItem("phase"), "sent")));
        Assert.Equal("channel", sendSent.GetTagItem("surface"));
        Assert.Equal("send", sendSent.GetTagItem("message_kind"));
        Assert.Equal("client_server", sendSent.GetTagItem("channel_route_kind"));
        Assert.Equal("term-work", sendSent.GetTagItem("channel_name"));
        Assert.NotNull(sendSent.GetTagItem("server_rid"));
        Assert.Null(sendSent.GetTagItem("correlation_id"));

        //  Request: one `sent` and exactly one `reply_received` terminal, both
        //  carrying the same correlation id (spec 26 §2.2 exactly-once).
        var requestFlows = FlowActivities(activities, nameof(TerminalEchoRequest)).ToArray();
        var requestSent = Assert.Single(requestFlows
            .Where(activity => Equals(activity.GetTagItem("phase"), "sent")));
        var terminal = Assert.Single(requestFlows
            .Where(activity => Equals(activity.GetTagItem("phase"), "reply_received")));
        Assert.Equal("request", requestSent.GetTagItem("message_kind"));
        Assert.Equal("client_server", requestSent.GetTagItem("channel_route_kind"));
        Assert.NotNull(requestSent.GetTagItem("server_rid"));
        Assert.NotNull(requestSent.GetTagItem("correlation_id"));
        Assert.Equal("request", terminal.GetTagItem("message_kind"));
        Assert.Equal("succeeded", terminal.GetTagItem("outcome"));
        Assert.Equal("client_server", terminal.GetTagItem("channel_route_kind"));
        Assert.Equal("term-work", terminal.GetTagItem("channel_name"));
        Assert.Equal(
            requestSent.GetTagItem("correlation_id"),
            terminal.GetTagItem("correlation_id"));
    }

    [Fact]
    public async Task MalformedClientServerEnvelopeRecordsInvalidFrameDispatchError()
    {
        var port = ReservePort();
        var activities = CaptureActivities(out var listener);
        using (listener)
        {
            await using var server = CreateTerminalServer(port, "mal-work");
            var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
            await serverRuntime.StartAsync(CancellationToken.None);
            try
            {
                await using var context = Systems.Zlink.Zlink.CreateContext();
                await using var dealer = context.CreateDealerSocket();
                dealer.SetRoutingId(RoutingId.From("mal-client"));
                dealer.Connect($"tcp://127.0.0.1:{port}");

                //  The raw dealer may not have finished connecting yet; retry the
                //  admission hello until the transport accepts it.
                var deadlineTimeout = TimeSpan.FromSeconds(5);
                var deadlineStarted = Stopwatch.GetTimestamp();
                while (true)
                {
                    var hello = ZLinkClientServerControlProtocol.EncodeHello(
                        new ZLinkClientServerControlProtocol.Hello(
                            "mal-work",
                            "plaintext",
                            1024 * 1024));
                    try
                    {
                        ZLinkMessageParts.DisposeAll(await dealer.Request()
                            .Message(hello)
                            .Timeout(TimeSpan.FromSeconds(5))
                            .Async(CancellationToken.None));
                        break;
                    }
                    catch (ZlinkSubmitException) when (Stopwatch.GetElapsedTime(deadlineStarted) < deadlineTimeout)
                    {
                        await Task.Delay(20);
                    }
                }

                //  A malformed envelope keeps producing the protocol error path,
                //  and now additionally records zlink.dispatch_error(invalid_frame).
                await dealer.Send()
                    .Message(Message.From("{"))
                    .Async(CancellationToken.None);

                await WaitUntilAsync(
                    () => DispatchErrors(activities, "mal-work").Any(),
                    TimeSpan.FromSeconds(5));
            }
            finally
            {
                await serverRuntime.StopAsync(CancellationToken.None);
            }
        }

        var failure = DispatchErrors(activities, "mal-work").First();
        Assert.Equal("zlink.dispatch_error", failure.GetTagItem("event_id"));
        Assert.Equal("invalid_frame", failure.GetTagItem("reason"));
        Assert.Equal("channel", failure.GetTagItem("surface"));
        Assert.Equal("send", failure.GetTagItem("message_kind"));
        Assert.Equal("drop", failure.GetTagItem("action"));
        Assert.Equal("failed", failure.GetTagItem("outcome"));
    }

    [Fact]
    public void RejectionRepliesClassifyMalformedFlowByLiveGate()
    {
        //  Gate off (tracing Off): flow fields are framing only, so the drain
        //  rejection reply keeps its rejection kind.
        var offReply = RejectRelocationReply(MalformedFlowId, validateFlow: false);
        Assert.Equal(ZLinkMessageKind.Error, offReply.Kind);
        Assert.Equal("unavailable", offReply.ErrorCode);
        Assert.Equal("corr-reject", offReply.CorrelationId);

        //  Gate on: spec 27 §3 classifies the malformed flow pair as a protocol
        //  error and the operation completes as ProtocolError.
        var onReply = RejectRelocationReply(MalformedFlowId, validateFlow: true);
        Assert.Equal(ZLinkMessageKind.Error, onReply.Kind);
        Assert.Equal("protocol_error", onReply.ErrorCode);
        Assert.Equal("corr-reject", onReply.CorrelationId);

        //  Gate on with a well-formed flow: the rejection kind is preserved.
        var validReply = RejectRelocationReply(ValidFlowId, validateFlow: true);
        Assert.Equal("unavailable", validReply.ErrorCode);
    }

    private static ZLinkEnvelopeHeader RejectRelocationReply(
        string flowId,
        bool validateFlow)
    {
        var parts = EncodeRoutedRequestParts(flowId);
        var replies = new List<Message[]>();
        var received = new ZLinkBackendRouteReceived(
            parts,
            sourceNodeRid: RoutingId.From("reject-source"),
            spotId: "reject-spot",
            requestSeq: 1UL,
            reply: (replyParts, _) =>
            {
                replies.Add(replyParts
                    .Select(static part => Message.From(part.AsReadOnlySpan()))
                    .ToArray());
                return SubmitResult.Ok;
            });

        ZLinkSpotActivationDispatcher.RejectApplicationRouteForRelocation(
            received,
            "reject-route",
            validateFlow);

        var reply = Assert.Single(replies);
        try
        {
            return ZLinkEnvelopeCodec.DecodeHeader(reply, validateFlow: false);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(reply);
        }
    }

    private static IReadOnlyList<Message> EncodeRoutedRequestParts(string flowId)
    {
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "reject-route",
            "RejectPacket",
            ZLinkEnvelopeCodec.DefaultContentType,
            "corr-reject",
            null,
            null,
            null,
            null)
        {
            FlowId = ValidFlowId,
            FlowOrigin = ZLinkFlowOrigin.Application
        };
        var encoded = ZLinkEnvelopeCodec.EncodeParts(header, null, null, null);
        try
        {
            //  Encoding validates the flow pair, so a malformed id is produced by
            //  mutating the already-encoded bytes (same length keeps JSON valid).
            var json = Encoding.UTF8.GetString(encoded[0].AsReadOnlySpan())
                .Replace(ValidFlowId, flowId, StringComparison.Ordinal);
            return [Message.From(json)];
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(encoded);
        }
    }

    private static IEnumerable<Activity> FlowActivities(
        IEnumerable<Activity> activities,
        string packetName) =>
        activities.Where(activity =>
            activity.OperationName == "zlink.message_flow"
            && Equals(activity.GetTagItem("packet_name")?.ToString(), packetName));

    private static IEnumerable<Activity> DispatchErrors(
        IEnumerable<Activity> activities,
        string channelName) =>
        activities.Where(activity =>
            activity.OperationName == "zlink.dispatch_error"
            && Equals(activity.GetTagItem("channel_name")?.ToString(), channelName));

    private static ConcurrentBag<Activity> CaptureActivities(out ActivityListener listener)
    {
        var activities = new ConcurrentBag<Activity>();
        listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = activities.Add
        };
        ActivitySource.AddActivityListener(listener);
        return activities;
    }

    private static ServiceProvider CreateTerminalServer(
        int port,
        string channelName = "term-work")
    {
        var services = new ServiceCollection();
        services.AddSingleton<TerminalProbe>();
        services.AddZLinkFramework(options =>
        {
            options.AddClientServerChannel(channelName)
                .Server()
                .Listen(port)
                .AddSendHandler<TerminalEchoSendHandler, TerminalEchoSend>()
                .AddRequestHandler<TerminalEchoHandler, TerminalEchoRequest, TerminalEchoReply>();
        });
        return services.BuildServiceProvider();
    }

    private static ServiceProvider CreateTerminalClient(int port)
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddClientServerChannel("term-work")
                .Client()
                .Connect($"tcp://127.0.0.1:{port}");
        });
        return services.BuildServiceProvider();
    }

    private static int ReservePort()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return ((IPEndPoint)listener.LocalEndpoint).Port;
    }

    private static async Task WaitUntilAsync(Func<bool> condition, TimeSpan timeout)
    {
        var deadlineStarted = Stopwatch.GetTimestamp();
        while (!condition())
        {
            if (Stopwatch.GetElapsedTime(deadlineStarted) >= timeout)
                throw new TimeoutException("Channel outbound condition was not reached.");
            await Task.Delay(10);
        }
    }

    private sealed record TerminalEchoRequest(string Value);

    private sealed record TerminalEchoReply(string Value);

    private sealed record TerminalEchoSend(string Value);

    private sealed class TerminalProbe
    {
        public TaskCompletionSource<string> Received { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class TerminalEchoSendHandler(TerminalProbe probe)
        : IZLinkSendHandler<TerminalEchoSend>
    {
        public ValueTask HandleAsync(
            TerminalEchoSend message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            probe.Received.TrySetResult(message.Value);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class TerminalEchoHandler
        : IZLinkRequestHandler<TerminalEchoRequest, TerminalEchoReply>
    {
        public ValueTask<TerminalEchoReply> HandleAsync(
            TerminalEchoRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(new TerminalEchoReply(request.Value));
        }
    }
}
