using System.Text;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Handlers;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RouteCodecTests
{
    [Fact]
    public void PublisherBackend_DoesNotExposeUnsupportedRoutingIdOption()
    {
        Assert.DoesNotContain(
            typeof(IZLinkBackendPublisherSocket).GetMethods(),
            static method => method.Name == "SetRoutingId");
    }

    [Fact]
    public void MeshMetadataCodec_RoundTrips_The_Last_Value_Snapshot()
    {
        var callMetadata = new ZLinkCallMetadata();
        callMetadata.Set("trace-id", "first");
        callMetadata.Set("trace-id", "last");
        callMetadata.Set("tenant", "sample");

        var encoded = callMetadata.Encode();

        Assert.True(ZLinkMeshMetadataCodec.TryDecode(encoded.Span, out var decoded));
        Assert.Equal("last", decoded.Find("trace-id"));
        Assert.Equal("sample", decoded.Find("tenant"));
        Assert.Equal(2, decoded.Values.Count);
    }

    [Fact]
    public void MeshMetadataCodec_Accepts_Exactly_1024_Bytes_And_Rejects_The_Next_Byte()
    {
        var atLimit = new ZLinkCallMetadata();
        atLimit.Set("k", new string('v', 1018));
        Assert.Equal(1024, atLimit.Encode().Length);

        var overLimit = new ZLinkCallMetadata();
        overLimit.Set("k", new string('v', 1019));
        var failure = Assert.Throws<ArgumentException>(() => overLimit.Encode());

        Assert.Contains("1024-byte limit", failure.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void MeshMetadataCodec_Rejects_A_Malformed_Ingress_Frame()
    {
        var malformed = new byte[]
        {
            1, 1,
            3, (byte)'k'
        };

        Assert.False(ZLinkMeshMetadataCodec.TryDecode(malformed, out _));
    }

    [Fact]
    public void SocketConfig_Uses_The_Framework_Default_PeerWeight()
    {
        var config = new ZLinkSocketConfig();

        Assert.Equal(ZLinkSocketConfig.DefaultPeerWeight, config.Weight);
    }

    [Fact]
    public void SocketConfig_SendTimeout_Ceils_Positive_SubMillisecond_Value()
    {
        var config = new ZLinkSocketConfig { SendTimeout = TimeSpan.FromTicks(1) };

        Assert.Equal(TimeSpan.FromMilliseconds(1), config.SendTimeout);
    }

    [Fact]
    public void SocketConfig_SendTimeout_Preserves_IntMax_Milliseconds()
    {
        var config = new ZLinkSocketConfig
        {
            SendTimeout = TimeSpan.FromMilliseconds(int.MaxValue)
        };

        Assert.Equal(TimeSpan.FromMilliseconds(int.MaxValue), config.SendTimeout);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(-1)]
    public void SocketConfig_SendTimeout_Rejects_NonPositive_Value(long ticks)
    {
        var config = new ZLinkSocketConfig();

        Assert.Throws<ZLinkConfigurationException>(() => config.SendTimeout = TimeSpan.FromTicks(ticks));
    }

    [Fact]
    public void SocketConfig_SendTimeout_Rejects_Value_Above_IntMax_Milliseconds()
    {
        var config = new ZLinkSocketConfig();
        var timeout = TimeSpan.FromMilliseconds((long)int.MaxValue + 1);

        Assert.Throws<ZLinkConfigurationException>(() => config.SendTimeout = timeout);
    }

    [Fact]
    public void ChannelBundleFactory_Applies_MaxMessageSize_To_BackendSocket()
    {
        var socket = new RecordingSocketOptions();
        var config = new ZLinkSocketConfig
        {
            MaxMessageSize = 4096,
            SendHighWaterMark = 12,
            ReceiveHighWaterMark = 34
        };

        ZLinkChannelBundleFactory.ApplySocketConfig(socket, config);

        Assert.Equal(4096, socket.MaxMessageSize);
        Assert.Equal(12UL, socket.SendHighWaterMark);
        Assert.Equal(34UL, socket.ReceiveHighWaterMark);
    }

    [Fact]
    public async Task RouteHandlerInvoker_Uses_Configured_Codec()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/route-test", new RouteProbeSerializer());
        var filterProbe = new RouteFilterProbe();
        var services = new ServiceCollection()
            .AddSingleton<RouteProbeHandler>()
            .AddSingleton(filterProbe)
            .BuildServiceProvider();
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "play",
            "Probe",
            ZLinkEnvelopeCodec.DefaultContentType,
            "route-request-1",
            null,
            null,
            null,
            null);
        var parts = ZLinkEnvelopeCodec.EncodeParts(header, new RouteProbe("hello"), typeof(RouteProbe), codecs);
        var descriptor = new ZLinkRouteHandlerDescriptor(
            ZLinkMessageKind.Request,
            "play",
            "Probe",
            typeof(RouteProbeHandler),
            typeof(RouteProbe),
            typeof(RouteProbeReply),
            ZLinkHandlerMethodInvokerFactory.Create(
                typeof(RouteProbeHandler).GetMethod(nameof(RouteProbeHandler.HandleAsync))!));

        var registration = new ZLinkFrameworkRegistration();
        registration.Filters.Add(typeof(RouteProbeFilter));
        var dispatcher = new ZLinkHandlerDispatcher(
            services.GetRequiredService<IServiceScopeFactory>(),
            registration);
        var invoker = new ZLinkRouteHandlerInvoker(dispatcher, codecs);

        var reply = await invoker.InvokeRequestAsync(
            descriptor,
            "play",
            RoutingId.From("source-node"),
            ZLinkEnvelopeCodec.DecodeHeader(parts),
            parts,
            CancellationToken.None);

        Assert.Equal("hello", RouteProbeHandler.LastRequest?.Text);
        Assert.Equal("application/route-test", ZLinkEnvelopeCodec.DecodeHeader(parts).ContentType);
        Assert.Equal(new RouteProbeReply("HELLO"), reply.Message);
        Assert.Equal(
            ZLinkHandlerDispatchKind.NodeDirectRequest,
            filterProbe.DispatchKind);
        Assert.Equal("play", filterProbe.MeshName);
    }

    [Fact]
    public void RouteReplyWriter_Uses_Configured_Codec()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/route-test", new RouteProbeSerializer());
        var router = new RecordingRouter();
        var requestHeader = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "play",
            "Probe",
            ZLinkEnvelopeCodec.DefaultContentType,
            "corr-1",
            null,
            null,
            null,
            null);

        ZLinkChannelReplyWriter.ReplyEnvelope(
            router,
            RoutingId.From("source-node"),
            7,
            ZLinkChannelReplyWriter.CreateReplyHeader(
                ZLinkMessageKind.Response,
                "play",
                requestHeader),
            new RouteProbe("reply"),
            typeof(RouteProbe),
            codecs);

        Assert.Equal("application/route-test", router.ReplyContentType);
        Assert.Equal("ROUTE:reply", router.ReplyBody);
        Assert.Equal(string.Empty, router.SentHeader?.MessageName);
    }

    [Fact]
    public async Task RouterProbe_AllowsNonInitiatorRidAddressedSendOverInboundIdentity()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var initiator = context.CreateRouterSocket();
        await using var nonInitiator = context.CreateRouterSocket();
        var initiatorRid = RoutingId.From("route-a-initiator");
        var nonInitiatorRid = RoutingId.From("route-z-non-initiator");
        var endpoint = $"inproc://route-inbound-identity-{Guid.NewGuid():N}";

        initiator.SetRoutingId(initiatorRid);
        nonInitiator.SetRoutingId(nonInitiatorRid);
        initiator.Options.Linger = TimeSpan.Zero;
        nonInitiator.Options.Linger = TimeSpan.Zero;
        nonInitiator.Options.Mandatory = true;
        nonInitiator.Bind(endpoint);
        initiator.Options.SetConnectRoutingId(nonInitiatorRid);
        initiator.Options.Probe = true;
        initiator.Connect(endpoint);

        await SendUntilReceivedAsync(
            nonInitiator,
            initiator,
            initiatorRid,
            "reply-from-non-initiator",
            TimeSpan.FromSeconds(3));
    }

    private static async Task SendUntilReceivedAsync(
        IRouterSocket sender,
        IRouterSocket receiver,
        RoutingId targetRid,
        string payload,
        TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        using var received = Received.Create();
        while (DateTime.UtcNow < deadline)
        {
            using (var message = Message.From(payload))
            {
                try
                {
                    _ = sender.Send(targetRid).Message(message).Submit();
                }
                catch (ZlinkException)
                {
                }
            }

            if (receiver.Recv(received, RecvFlags.DontWait))
            {
                Assert.Equal(payload, received.SinglePartOrThrow().GetString());
                return;
            }

            await Task.Delay(10);
        }

        throw new TimeoutException("Timed out waiting for inbound identity routed send.");
    }

    private sealed record RouteProbe(string Text);

    private sealed record RouteProbeReply(string Text);

    private sealed class RouteProbeHandler : IZLinkRouteRequestHandler<RouteProbe, RouteProbeReply>
    {
        public static RouteProbe? LastRequest { get; private set; }

        public ValueTask<RouteProbeReply> HandleAsync(
            RouteProbe request,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken)
        {
            _ = context;
            _ = cancellationToken;
            LastRequest = request;
            return ValueTask.FromResult(new RouteProbeReply(request.Text.ToUpperInvariant()));
        }
    }

    private sealed class RouteFilterProbe
    {
        public ZLinkHandlerDispatchKind? DispatchKind { get; set; }

        public string? MeshName { get; set; }
    }

    private sealed class RouteProbeFilter(RouteFilterProbe probe)
        : IZLinkHandlerFilter
    {
        public async ValueTask InvokeAsync(
            IZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext next,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            probe.DispatchKind = context.DispatchKind;
            probe.MeshName = context.MeshName;
            await next();
        }
    }

    private sealed class RouteProbeSerializer : IZLinkMessageSerializer
    {
        public ZLinkEncodedPayload Serialize(object value, Type type)
        {
            var text = value switch
            {
                RouteProbe probe => probe.Text,
                RouteProbeReply reply => reply.Text,
                _ => throw new NotSupportedException(type.FullName)
            };
            return ZLinkEncodedPayload.From(Encoding.UTF8.GetBytes("ROUTE:" + text));
        }

        public object? Deserialize(ZLinkEncodedPayload payload, Type type)
        {
            var text = Encoding.UTF8.GetString(payload.Bytes.Span);
            var value = text.StartsWith("ROUTE:", StringComparison.Ordinal)
                ? text["ROUTE:".Length..]
                : text;
            if (type == typeof(RouteProbe)) return new RouteProbe(value);
            if (type == typeof(RouteProbeReply)) return new RouteProbeReply(value);
            throw new NotSupportedException(type.FullName);
        }
    }

    private sealed class RecordingSocketOptions : IZLinkBackendSocketOptions
    {
        public long MaxMessageSize { get; private set; }

        public ulong SendHighWaterMark { get; private set; }

        public ulong ReceiveHighWaterMark { get; private set; }

        public void ApplySocketConfig(IZLinkSocketConfig config)
        {
            if (config.MaxMessageSize > 0) SetMaxMessageSize(config.MaxMessageSize);
            if (config.SendHighWaterMark > 0) SetSendHighWaterMark(config.SendHighWaterMark);
            if (config.ReceiveHighWaterMark > 0) SetReceiveHighWaterMark(config.ReceiveHighWaterMark);
        }

        public ValueTask DisposeAsync()
        {
            return ValueTask.CompletedTask;
        }

        public void Bind(string endpoint)
        {
            throw new NotSupportedException();
        }

        public void SetChannelName(string channelName)
        {
            throw new NotSupportedException();
        }

        public void SetMaxMessageSize(long value)
        {
            MaxMessageSize = value;
        }

        public void SetSendHighWaterMark(ulong value)
        {
            SendHighWaterMark = value;
        }

        public void SetReceiveHighWaterMark(ulong value)
        {
            ReceiveHighWaterMark = value;
        }
    }

    private sealed class RecordingRoutingDealer : IZLinkBackendDealerSocket
    {
        public bool ProbeRouterOnConnect { get; private set; }

        public void ApplySocketConfig(IZLinkSocketConfig config) => throw new NotSupportedException();

        public void SetProbe(bool enabled)
        {
            ProbeRouterOnConnect = enabled;
        }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;

        public void Bind(string endpoint) => throw new NotSupportedException();

        public void SetChannelName(string channelName) => throw new NotSupportedException();

        public void SetMaxMessageSize(long value) => throw new NotSupportedException();

        public void SetSendHighWaterMark(ulong value) => throw new NotSupportedException();

        public void SetReceiveHighWaterMark(ulong value) => throw new NotSupportedException();

        public void Connect(string endpoint) => throw new NotSupportedException();

        public void Disconnect(string endpoint) => throw new NotSupportedException();

        public void SetPeerWeight(int weight) => throw new NotSupportedException();

        public int GetPeerWeight() => throw new NotSupportedException();

        public void SetRoutingId(RoutingId routingId) => throw new NotSupportedException();

        public void OnSendReady(Action handler) => throw new NotSupportedException();

        public bool Send(Message message, SendFlags flags) => throw new NotSupportedException();

        public bool Send(IReadOnlyList<Message> parts, SendFlags flags) => throw new NotSupportedException();

        public bool Request(
            Message message,
            RequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout) => throw new NotSupportedException();

        public bool Request(
            IReadOnlyList<Message> parts,
            RequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout) => throw new NotSupportedException();

        public Task<IReadOnlyList<Message>> RequestAsync(
            Message message,
            TimeSpan timeout,
            CancellationToken cancellationToken) =>
            throw new NotSupportedException();

        public Received? Recv(RecvFlags flags = RecvFlags.None) => throw new NotSupportedException();

        public bool Reply(Received received, Message message) =>
            throw new NotSupportedException();
    }

    private sealed class RecordingRouter : IZLinkBackendRouterSocket
    {
        private int _disposeCount;
        public bool BlockDispose { get; init; }
        public Exception? DisposeFailure { get; init; }
        public TaskCompletionSource DisposeStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource AllowDispose { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int DisposeCount => Volatile.Read(ref _disposeCount);
        public ZLinkEnvelopeHeader? SentHeader { get; private set; }

        public string? ReplyContentType { get; private set; }

        public string? ReplyBody { get; private set; }

        public bool Mandatory { get; private set; }

        public bool Handover { get; private set; }

        public bool Probe { get; private set; }

        public RoutingId ConnectRoutingId { get; private set; }

        public void ApplySocketConfig(IZLinkSocketConfig config) => throw new NotSupportedException();

        public async ValueTask DisposeAsync()
        {
            Interlocked.Increment(ref _disposeCount);
            DisposeStarted.TrySetResult();
            if (BlockDispose) await AllowDispose.Task.ConfigureAwait(false);
            if (DisposeFailure is not null)
                System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(DisposeFailure).Throw();
        }

        public void Bind(string endpoint)
        {
            throw new NotSupportedException();
        }

        public void SetChannelName(string channelName)
        {
            throw new NotSupportedException();
        }

        public void SetMaxMessageSize(long value)
        {
            throw new NotSupportedException();
        }

        public void Connect(string endpoint)
        {
            throw new NotSupportedException();
        }

        public void Disconnect(string endpoint)
        {
            throw new NotSupportedException();
        }

        public void SetPeerWeight(int weight)
        {
            throw new NotSupportedException();
        }

        public int GetPeerWeight()
        {
            throw new NotSupportedException();
        }

        public void OnSendReady(Action handler)
        {
            _ = handler;
        }

        public void SetSendHighWaterMark(ulong value)
        {
            throw new NotSupportedException();
        }

        public void SetReceiveHighWaterMark(ulong value)
        {
            throw new NotSupportedException();
        }

        public void SetRoutingId(RoutingId routingId)
        {
            throw new NotSupportedException();
        }

        public void SetConnectRoutingId(RoutingId routingId)
        {
            ConnectRoutingId = routingId;
        }

        public void SetProbe(bool enabled)
        {
            Probe = enabled;
        }

        public void SetMandatory(bool mandatory)
        {
            Mandatory = mandatory;
        }

        public void SetHandover(bool enabled)
        {
            Handover = enabled;
        }

        public void DisconnectPeer(RoutingId routingId)
        {
            throw new NotSupportedException();
        }

        public Received? Recv(RecvFlags flags = RecvFlags.None)
        {
            throw new NotSupportedException();
        }

        public bool Send(RoutingId routingId, Message message, SendFlags flags)
        {
            _ = routingId;
            _ = flags;
            SentHeader = ZLinkEnvelopeCodec.DecodeHeader(message);
            return true;
        }

        public bool Send(RoutingId routingId, IReadOnlyList<Message> parts, SendFlags flags)
        {
            _ = routingId;
            _ = flags;
            SentHeader = ZLinkEnvelopeCodec.DecodeHeader(parts);
            return true;
        }

        public bool Request(
            RoutingId routingId,
            Message message,
            RequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout)
        {
            throw new NotSupportedException();
        }

        public bool Request(
            RoutingId routingId,
            IReadOnlyList<Message> parts,
            RequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout)
        {
            throw new NotSupportedException();
        }

        public bool SendToSpot(
            RoutingId targetNodeRid,
            string targetSpotId,
            IReadOnlyList<Message> parts,
            SendFlags flags)
        {
            throw new NotSupportedException();
        }

        public bool RequestToSpot(
            RoutingId targetNodeRid,
            string targetSpotId,
            IReadOnlyList<Message> parts,
            RequestCallback callback,
            SendFlags flags,
            TimeSpan? timeout)
        {
            throw new NotSupportedException();
        }

        public void Reply(RoutingId routingId, ulong requestSeq, Message message)
        {
            throw new NotSupportedException();
        }

        public void Reply(RoutingId routingId, ulong requestSeq, IReadOnlyList<Message> parts)
        {
            _ = routingId;
            _ = requestSeq;
            SentHeader = ZLinkEnvelopeCodec.DecodeHeader(parts);
            ReplyContentType = SentHeader.ContentType;
            ReplyBody = parts[1].GetString();
        }
    }

}
