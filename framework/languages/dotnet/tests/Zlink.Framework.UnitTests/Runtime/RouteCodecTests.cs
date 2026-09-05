using System.Diagnostics;
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
    public async Task ChannelBundleFactory_Applies_MaxMessageSize_To_BindingSocket()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var socket = context.CreateRouterSocket();
        var config = new ZLinkSocketConfig
        {
            MaxMessageSize = 4096,
            SendHighWaterMark = 12,
            ReceiveHighWaterMark = 34
        };

        ZLinkChannelBundleFactory.ApplySocketConfig(socket.Options, config);

        Assert.Equal(4096, socket.Options.MaxMessageSize);
        Assert.Equal(12UL, socket.Options.SendHighWaterMark);
        Assert.Equal(34UL, socket.Options.ReceiveHighWaterMark);
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
    public async Task RouteReplyWriter_Uses_Configured_Codec_Over_BindingSocket()
    {
        var codecs = new ZLinkCodecRegistryBuilder();
        codecs.AddSerializer("application/route-test", new RouteProbeSerializer());
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var dealer = context.CreateDealerSocket();
        await using var router = context.CreateRouterSocket();
        var endpoint = $"inproc://route-reply-codec-{Guid.NewGuid():N}";
        router.Bind(endpoint);
        dealer.Connect(endpoint);
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

        using var request = Message.From("request");
        var replyTask = dealer.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();
        using var received = await ReceiveAsync(router, TimeSpan.FromSeconds(2));
        ZLinkChannelReplyWriter.ReplyRequest(
            router,
            received,
            ZLinkChannelReplyWriter.CreateReplyHeader(
                ZLinkMessageKind.Response,
                "play",
                requestHeader),
            new RouteProbe("reply"),
            typeof(RouteProbe),
            codecs);

        var parts = await replyTask;
        try
        {
            Assert.Equal(
                "application/route-test",
                ZLinkEnvelopeCodec.DecodeHeader(parts).ContentType);
            Assert.Equal("ROUTE:reply", Encoding.UTF8.GetString(
                parts[1].AsReadOnlyMemory().Span));
            Assert.Equal(
                string.Empty,
                ZLinkEnvelopeCodec.DecodeHeader(parts).MessageName);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static async Task<Received> ReceiveAsync(
        IRouterSocket router,
        TimeSpan timeout)
    {
        var deadlineStarted = Stopwatch.GetTimestamp();
        var received = Received.Create();
        while (Stopwatch.GetElapsedTime(deadlineStarted) < timeout)
        {
            if (router.Recv(received, RecvFlags.DontWait))
                return received;
            await Task.Delay(10);
        }

        received.Dispose();
        throw new TimeoutException("Timed out waiting for route request.");
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
        var deadlineStarted = Stopwatch.GetTimestamp();
        using var received = Received.Create();
        while (Stopwatch.GetElapsedTime(deadlineStarted) < timeout)
        {
            using (var message = Message.From(payload))
            {
                try
                {
                    await sender.Send(targetRid)
                        .Message(message)
                        .Async(CancellationToken.None);
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


}
