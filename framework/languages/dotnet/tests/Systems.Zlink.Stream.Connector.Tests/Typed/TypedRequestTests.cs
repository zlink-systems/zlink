using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Compression;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public async Task TcpTypedRequestCorrelatesResponse()
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
            var requestHeader = headerCodec.Decode(packet.Header);

            Assert.Equal(ZlinkStreamMessageKind.Request, requestHeader.Kind);
            Assert.NotNull(requestHeader.RequestSeq);
            Assert.Equal("ping", requestHeader.Name);

            var responseHeader = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                requestHeader.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty);
            var responsePayload = JsonSerializer.SerializeToUtf8Bytes(new Pong("pong"));
            await WritePacketAsync(stream, headerCodec.Encode(responseHeader).ToArray(), responsePayload);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        await connector.Connect.Async();

        var reply = await connector
            .Request(new Ping("hello"))
            .PacketName("ping")
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<Pong>();

        Assert.Equal("pong", reply.Text);
        await server;
    }

    [Fact]
    public async Task CallbackRequestReturnsTypedResult()
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
            var requestHeader = headerCodec.Decode(packet.Header);
            var responseHeader = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                requestHeader.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty);
            await WritePacketAsync(
                stream,
                headerCodec.Encode(responseHeader).ToArray(),
                JsonSerializer.SerializeToUtf8Bytes(new Pong("callback")));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        await connector.Connect.Async();

        var completed =
            new TaskCompletionSource<ZlinkStreamResult<Pong>>(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.Request(new Ping("hello"))
            .PacketName("ping")
            .Submit<Pong>(result => completed.SetResult(result));

        var result = await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(result.IsSuccess);
        Assert.Equal("callback", result.Value?.Text);
        await server;
    }

    [Fact]
    public async Task PacketNameAttributeIsUsedByDefault()
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
            Assert.Equal("custom.packet", header.Name);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat()
        });
        await connector.Connect.Async();

        await connector.Send(new NamedPacket("name")).Async();
        await server;
    }

    [Fact]
    public async Task MetadataSendLimitIsEnforced()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat()
        });
        await connector.Connect.Async();

        var exception = Assert.Throws<ZlinkStreamException>(() =>
            connector.Send(new Ping("hello"))
                .PacketName("ping")
                .Metadata("traceId", new string('x', 1014))
                .Async());

        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, exception.Error.Code);
        await server;
    }

    [Fact]
    public async Task ClientToServerCompressionIsExplicit()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        var compressionCodec = new ZlinkStreamLz4CompressionCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var plainPacket = await ReadPacketAsync(stream);
            var plainHeader = headerCodec.Decode(plainPacket.Header);
            Assert.False(plainHeader.Flags.HasFlag(ZlinkStreamHeaderFlags.PayloadCompressed));

            var compressedPacket = await ReadPacketAsync(stream);
            var compressedHeader = headerCodec.Decode(compressedPacket.Header);
            Assert.True(compressedHeader.Flags.HasFlag(ZlinkStreamHeaderFlags.PayloadCompressed));
            var payload = compressionCodec.Decompress(compressedPacket.Payload, 64 * 1024);
            var decoded =
                JsonSerializer.Deserialize<Ping>(payload.Span, new JsonSerializerOptions(JsonSerializerDefaults.Web));
            Assert.Equal("compressed", decoded?.Text);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat()
        });
        await connector.Connect.Async();

        await connector.Send(new Ping("plain"))
            .PacketName("plain").Async();
        await connector.Send(new Ping("compressed"))
            .PacketName("compressed")
            .Compress().Async();
        await server;
    }
}
