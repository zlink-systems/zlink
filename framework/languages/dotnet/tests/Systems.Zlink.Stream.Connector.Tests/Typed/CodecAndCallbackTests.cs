using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Compression;
using Xunit;
using Zlink.Framework.Codecs.MessagePack;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public void JsonExtensionBuildsEncodedPayload()
    {
        var payload = new Ping("hello").ToJson();

        Assert.Equal(ZlinkStreamCodec.Json, payload.Codec);
        Assert.Equal(typeof(Ping), payload.MessageType);
        Assert.Equal("hello", payload.FromJson<Ping>().Text);
    }

    [Fact]
    public void JsonCodecConfigurationUsesDefensiveSnapshots()
    {
        var original = ZlinkStreamJsonCodec.SerializerOptions;
        try
        {
            var configured = new JsonSerializerOptions(original);
            ZlinkStreamJsonCodec.Configure(configured);

            configured.MaxDepth = 1;
            Assert.NotEqual(1, ZlinkStreamJsonCodec.SerializerOptions.MaxDepth);

            var exposed = ZlinkStreamJsonCodec.SerializerOptions;
            exposed.MaxDepth = 2;
            Assert.NotEqual(2, ZlinkStreamJsonCodec.SerializerOptions.MaxDepth);

            var payload = new Ping("snapshot").ToJson();
            Assert.Equal("snapshot", payload.FromJson<Ping>().Text);
        }
        finally
        {
            ZlinkStreamJsonCodec.Configure(original);
        }
    }

    [Fact]
    public async Task TypedConnectorUsesMessagePackExtensionWhenConfigured()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var packet = await ReadPacketAsync(stream);
            var header = headerCodec.Decode(packet.Header);
            Assert.Equal(ZlinkStreamCodec.MessagePack, header.Codec);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            PayloadCodec = ZLinkMessagePackCodec.Default
        });
        await connector.Connect.Async();

        await connector.Send(new PackedPing { Text = "hello" })
            .PacketName("packed").Async();
        await server;
    }

    [Fact]
    public async Task TypedCallbackDecompressesServerPacket()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        var compressionCodec = new ZlinkStreamLz4CompressionCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var received = new TaskCompletionSource<Pong>(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.PayloadCompressed,
                null,
                "pong",
                ZlinkStreamMetadata.Empty);
            var payload = compressionCodec.Compress(JsonSerializer.SerializeToUtf8Bytes(new Pong("compressed")));
            await WritePacketAsync(stream, headerCodec.Encode(header).ToArray(), payload.ToArray());
            await received.Task.WaitAsync(TimeSpan.FromSeconds(5));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        using var subscription = connector.On<Pong>("pong", (message, _) =>
        {
            received.SetResult(message.Payload);
            return ValueTask.CompletedTask;
        });

        await connector.Connect.Async();

        var reply = await received.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal("compressed", reply.Text);
        await server;
    }

    [Fact]
    public async Task Callback_Outbound_Reuses_Inbound_Flow_And_Does_Not_Leak_To_The_Next_Callback()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        var inheritedFlow = ZlinkStreamFlowId.Create();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var inheritedHeader = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                "flow-trigger",
                ZlinkStreamMetadata.Empty,
                FlowId: inheritedFlow,
                FlowOrigin: ZlinkStreamFlowOrigin.Lifecycle);
            await WritePacketAsync(
                stream,
                headerCodec.Encode(inheritedHeader).ToArray(),
                Array.Empty<byte>());

            var inheritedReply = headerCodec.Decode((await ReadPacketAsync(stream)).Header);
            Assert.Equal(inheritedFlow, inheritedReply.FlowId);
            Assert.Equal(ZlinkStreamFlowOrigin.Lifecycle, inheritedReply.FlowOrigin);

            var unrelatedHeader = inheritedHeader with
            {
                FlowId = null,
                FlowOrigin = null
            };
            await WritePacketAsync(
                stream,
                headerCodec.Encode(unrelatedHeader).ToArray(),
                Array.Empty<byte>());

            var unrelatedReply = headerCodec.Decode((await ReadPacketAsync(stream)).Header);
            Assert.True(ZlinkStreamFlowId.IsValid(unrelatedReply.FlowId));
            Assert.NotEqual(inheritedFlow, unrelatedReply.FlowId);
            Assert.Equal(ZlinkStreamFlowOrigin.Application, unrelatedReply.FlowOrigin);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Compression = ZlinkStreamCompression.None
        });
        using var subscription = connector.On("flow-trigger", (_, _) =>
        {
            connector.Send(new ZlinkStreamEncodedPayload(
                    ZlinkStreamCodec.Raw,
                    ReadOnlyMemory<byte>.Empty))
                .PacketName("flow-followup")
                .Async();
            return ValueTask.CompletedTask;
        });

        await connector.Connect.Async();
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task Request_Callback_Reuses_Response_Flow_And_Expires_After_Dispatch()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        var responseFlow = ZlinkStreamFlowId.Create();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var request = headerCodec.Decode((await ReadPacketAsync(stream)).Header);
            var response = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                request.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty,
                request.CorrelationId,
                responseFlow,
                ZlinkStreamFlowOrigin.Timer);
            await WritePacketAsync(stream, headerCodec.Encode(response).ToArray(), Array.Empty<byte>());

            var followup = headerCodec.Decode((await ReadPacketAsync(stream)).Header);
            Assert.Equal(responseFlow, followup.FlowId);
            Assert.Equal(ZlinkStreamFlowOrigin.Timer, followup.FlowOrigin);

            var unrelated = headerCodec.Decode((await ReadPacketAsync(stream)).Header);
            Assert.True(ZlinkStreamFlowId.IsValid(unrelated.FlowId));
            Assert.NotEqual(responseFlow, unrelated.FlowId);
            Assert.Equal(ZlinkStreamFlowOrigin.Application, unrelated.FlowOrigin);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Manual,
            Compression = ZlinkStreamCompression.None
        });
        await connector.Connect.Async();

        var releaseDetached = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task<(string FlowId, ZlinkStreamFlowOrigin Origin)?> detached = null!;
        connector.Request(new ZlinkStreamEncodedPayload(
                ZlinkStreamCodec.Raw,
                ReadOnlyMemory<byte>.Empty))
            .PacketName("flow-request")
            .Submit((ZlinkStreamResult<ZlinkStreamEncodedPayload> result) =>
            {
                Assert.True(result.IsSuccess);
                Assert.Equal(
                    (responseFlow, ZlinkStreamFlowOrigin.Timer),
                    ZlinkStreamFlowContext.Current);
                detached = Task.Run(async () =>
                {
                    await releaseDetached.Task.ConfigureAwait(false);
                    return ZlinkStreamFlowContext.Current;
                });
                connector.Send(new ZlinkStreamEncodedPayload(
                        ZlinkStreamCodec.Raw,
                        ReadOnlyMemory<byte>.Empty))
                    .PacketName("flow-followup")
                    .Async();
            });

        await WaitUntilAsync(() => connector.PendingDispatchCount > 0, TimeSpan.FromSeconds(5));
        await connector.Dispatch.Async();
        releaseDetached.SetResult();
        Assert.Null(await detached.WaitAsync(TimeSpan.FromSeconds(5)));

        await connector.Send(new ZlinkStreamEncodedPayload(
                ZlinkStreamCodec.Raw,
                ReadOnlyMemory<byte>.Empty))
            .PacketName("unrelated")
            .Async();
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public void Lz4CodecUsesWirePayloadLimitInsteadOfDecodedSize()
    {
        var source = Encoding.UTF8.GetBytes(new string('A', 1024));
        var compressed = new ZlinkStreamLz4CompressionCodec().Compress(source);
        var codec = new ZlinkStreamLz4CompressionCodec();

        var decoded = codec.Decompress(compressed, int.MaxValue);

        Assert.Equal(source, decoded.ToArray());
    }

    [Fact]
    public async Task ConnectorUsesCustomCompressionCodecForOutboundFrame()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        var compressionCodec = new PrefixCompressionCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var packet = await ReadPacketAsync(stream);
            var header = headerCodec.Decode(packet.Header);
            Assert.True(header.Flags.HasFlag(ZlinkStreamHeaderFlags.PayloadCompressed));
            Assert.Equal(PrefixCompressionCodec.Marker, packet.Payload[0]);
            var restored = compressionCodec.Decompress(packet.Payload, 64 * 1024);
            var decoded = JsonSerializer.Deserialize<Ping>(
                restored.Span,
                new JsonSerializerOptions(JsonSerializerDefaults.Web));
            Assert.Equal("custom", decoded?.Text);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            CompressionCodec = compressionCodec
        });
        await connector.Connect.Async();

        await connector.Send(new Ping("custom"))
            .PacketName("custom")
            .Compress().Async();
        await server;
    }

    [Fact]
    public async Task SendSubmitCompressesOutboundFrameOnce()
    {
        var compressionCodec = new PrefixCompressionCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await ReadPacketAsync(stream);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            CompressionCodec = compressionCodec
        });
        await connector.Connect.Async();

        await connector.Send(new Ping("single-compress"))
            .PacketName("single-compress")
            .Compress().Async();

        await server;
        Assert.Equal(1, compressionCodec.CompressCount);
    }

    [Fact]
    public async Task ConnectorUsesCustomCompressionCodecForInboundFrame()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        var compressionCodec = new PrefixCompressionCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var received = new TaskCompletionSource<Pong>(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.PayloadCompressed,
                null,
                "custom-pong",
                ZlinkStreamMetadata.Empty);
            var payload = compressionCodec.Compress(JsonSerializer.SerializeToUtf8Bytes(new Pong("custom")));
            await WritePacketAsync(stream, headerCodec.Encode(header).ToArray(), payload.ToArray());
            await received.Task.WaitAsync(TimeSpan.FromSeconds(5));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            CompressionCodec = compressionCodec,
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        using var subscription = connector.On<Pong>("custom-pong", (message, _) =>
        {
            received.SetResult(message.Payload);
            return ValueTask.CompletedTask;
        });

        await connector.Connect.Async();

        var reply = await received.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal("custom", reply.Text);
        await server;
    }

    [Fact]
    public async Task ConnectorRejectsCompressedInboundFrameWhenCompressionDisabled()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        var compressionCodec = new ZlinkStreamLz4CompressionCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var error = new TaskCompletionSource<ZlinkStreamError>(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.PayloadCompressed,
                null,
                "disabled-pong",
                ZlinkStreamMetadata.Empty);
            var payload = compressionCodec.Compress(JsonSerializer.SerializeToUtf8Bytes(new Pong("disabled")));
            await WritePacketAsync(stream, headerCodec.Encode(header).ToArray(), payload.ToArray());
            await error.Task.WaitAsync(TimeSpan.FromSeconds(5));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            Compression = ZlinkStreamCompression.None,
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        connector.ErrorReceived += (receivedError, _) =>
        {
            error.TrySetResult(receivedError);
            return ValueTask.CompletedTask;
        };
        using var subscription = connector.On<Pong>("disabled-pong", (_, _) =>
            throw new InvalidOperationException(
                "Handler must not receive compressed payload when compression is disabled."));

        await connector.Connect.Async();

        var receivedError = await error.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ZlinkStreamErrorCode.DecompressionFailed, receivedError.Code);
        Assert.Contains("compression codec", receivedError.Message, StringComparison.OrdinalIgnoreCase);
        await server;
    }

    [Fact]
    public void ConnectorRejectsCustomCompressionCodecWhenCompressionIsDisabled()
    {
        var exception = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:12345"),
                Compression = ZlinkStreamCompression.None,
                CompressionCodec = new PrefixCompressionCodec()
            }));

        Assert.Equal(ZlinkStreamErrorCode.ConfigurationError, exception.Error.Code);
    }

    [Fact]
    public async Task ConnectorAppliesRuntimeReceiveLimitAfterCustomDecompression()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        var compressionCodec = new OversizedCompressionCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var error = new TaskCompletionSource<ZlinkStreamError>(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.PayloadCompressed,
                null,
                "oversized",
                ZlinkStreamMetadata.Empty);
            await WritePacketAsync(stream, headerCodec.Encode(header).ToArray(), [1]);
            await error.Task.WaitAsync(TimeSpan.FromSeconds(5));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            CompressionCodec = compressionCodec,
            MaxReceivePayloadSize = 8,
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        connector.ErrorReceived += (receivedError, _) =>
        {
            error.TrySetResult(receivedError);
            return ValueTask.CompletedTask;
        };

        await connector.Connect.Async();

        var receivedError = await error.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ZlinkStreamErrorCode.DecompressionFailed, receivedError.Code);
        await server;
    }

    private sealed class PrefixCompressionCodec : IZlinkStreamCompressionCodec
    {
        public const byte Marker = 0x7A;

        public int CompressCount { get; private set; }

        public ReadOnlyMemory<byte> Compress(ReadOnlyMemory<byte> payload)
        {
            CompressCount++;
            var compressed = new byte[payload.Length + 1];
            compressed[0] = Marker;
            payload.CopyTo(compressed.AsMemory(1));
            return compressed;
        }

        public ReadOnlyMemory<byte> Decompress(ReadOnlyMemory<byte> payload, int maxDecompressedPayloadSize)
        {
            if (payload.Length == 0 || payload.Span[0] != Marker)
                throw new InvalidOperationException("Unexpected custom compression marker.");

            var restored = payload[1..].ToArray();
            if (restored.Length > maxDecompressedPayloadSize)
                throw new InvalidOperationException("Custom decoded payload exceeds limit.");

            return restored;
        }
    }

    private sealed class OversizedCompressionCodec : IZlinkStreamCompressionCodec
    {
        public ReadOnlyMemory<byte> Compress(ReadOnlyMemory<byte> payload)
        {
            return payload;
        }

        public ReadOnlyMemory<byte> Decompress(ReadOnlyMemory<byte> payload, int maxDecompressedPayloadSize)
        {
            return new byte[maxDecompressedPayloadSize + 1];
        }
    }
}
