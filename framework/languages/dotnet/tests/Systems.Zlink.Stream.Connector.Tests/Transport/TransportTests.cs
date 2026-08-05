using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public async Task CanceledWebSocketCloseStillDisposesTransport()
    {
        using var listener = new HttpListener();
        var port = GetFreeTcpPort();
        listener.Prefixes.Add($"http://127.0.0.1:{port}/canceled-close/");
        listener.Start();
        var server = Task.Run(async () =>
        {
            var context = await listener.GetContextAsync();
            var accepted = await context.AcceptWebSocketAsync(null);
            using var socket = accepted.WebSocket;
            var buffer = new byte[1];
            try
            {
                await socket.ReceiveAsync(buffer, CancellationToken.None);
            }
            catch (WebSocketException)
            {
            }
        });
        using var client = new ClientWebSocket();
        await client.ConnectAsync(new Uri($"ws://127.0.0.1:{port}/canceled-close/"), CancellationToken.None);
        var connection = new WebSocketConnection(client, 1024);
        using var canceled = new CancellationTokenSource();
        canceled.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await connection.CloseAsync(canceled.Token));

        Assert.Equal(WebSocketState.Closed, client.State);
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task StreamCloseFailureStillDisposesTcpClient()
    {
        using var tcp = new TcpClient();
        var failure = new InvalidOperationException("expected stream dispose failure");
        var connection = new StreamConnection(tcp, new FaultingDisposeStream(failure));

        var observed = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await connection.CloseAsync(CancellationToken.None));

        Assert.Same(failure, observed);
        Assert.Null(tcp.Client);
    }

    [Fact]
    public async Task TcpSendUsesHeaderPayloadFrame()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var packet = await ReadPacketAsync(stream);
            var header = headerCodec.Decode(packet.Header);
            Assert.Equal("h", header.Name);
            Assert.Equal(ZlinkStreamCodec.Raw, header.Codec);
            Assert.Equal("b", Encoding.UTF8.GetString(packet.Payload));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        await connector.Connect.Async();

        await connector.Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, "b"u8.ToArray()))
            .PacketName("h").Async();

        await server;
    }

    [Fact]
    public async Task TcpReceiveDispatchesMultipleHeaderPacketsInOrder()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var receivedAll = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "h1",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                "b1"u8.ToArray());
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "h2",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                "b2"u8.ToArray());
            await receivedAll.Task.WaitAsync(TimeSpan.FromSeconds(5));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        var received = new List<string>();
        var first = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var second = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.On("h1", (message, _) =>
        {
            received.Add($"{message.Name}:{Encoding.UTF8.GetString(message.Payload.Payload.Span)}");
            first.SetResult();
            return ValueTask.CompletedTask;
        });
        connector.On("h2", (message, _) =>
        {
            received.Add($"{message.Name}:{Encoding.UTF8.GetString(message.Payload.Payload.Span)}");
            second.SetResult();
            receivedAll.TrySetResult();
            return ValueTask.CompletedTask;
        });

        await connector.Connect.Async();
        await first.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await second.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(["h1:b1", "h2:b2"], received);
        await server;
    }

    [Fact]
    public async Task TcpReceivePayloadLimitDisconnectsBeforePayloadAllocation()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePrefixAsync(stream, 0, 2);
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            MaxReceivePayloadSize = 1,
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        await connector.Connect.Async();

        await WaitUntilAsync(
            () => connector.State == ZlinkStreamConnectionState.Disconnected,
            TimeSpan.FromSeconds(5));
        await server;
    }

    [Fact]
    public async Task WebSocketSendUsesBinaryFrames()
    {
        using var listener = new HttpListener();
        var port = GetFreeTcpPort();
        listener.Prefixes.Add($"http://127.0.0.1:{port}/ws/");
        listener.Start();
        var headerCodec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            var context = await listener.GetContextAsync();
            var webSocketContext = await context.AcceptWebSocketAsync(null);
            using var webSocket = webSocketContext.WebSocket;
            var received = await ReceiveWebSocketMessageAsync(webSocket);
            var packet = DecodePacket(received);
            var header = headerCodec.Decode(packet.Header);
            Assert.Equal("wh", header.Name);
            Assert.Equal(ZlinkStreamCodec.Raw, header.Codec);
            Assert.Equal("wb", Encoding.UTF8.GetString(packet.Payload));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"ws://127.0.0.1:{port}/ws/"),
            Heartbeat = DisabledHeartbeat()
        });
        await connector.Connect.Async();

        await connector.Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, "wb"u8.ToArray()))
            .PacketName("wh").Async();

        await server;
    }

    [Fact]
    public async Task WebSocketReceivePayloadLimitDisconnectsBeforeExtraCopy()
    {
        using var listener = new HttpListener();
        var port = GetFreeTcpPort();
        listener.Prefixes.Add($"http://127.0.0.1:{port}/ws/");
        listener.Start();
        var accepting = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            accepting.SetResult();
            var context = await listener.GetContextAsync();
            var webSocketContext = await context.AcceptWebSocketAsync(null);
            using var webSocket = webSocketContext.WebSocket;
            await webSocket.SendAsync(
                new byte[70_000],
                WebSocketMessageType.Binary,
                true,
                CancellationToken.None);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"ws://127.0.0.1:{port}/ws/"),
            Heartbeat = DisabledHeartbeat(),
            ConnectTimeout = TimeSpan.FromSeconds(15),
            MaxReceivePayloadSize = 1,
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        await accepting.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await connector.Connect.Async();

        await WaitUntilAsync(
            () => connector.State == ZlinkStreamConnectionState.Disconnected,
            TimeSpan.FromSeconds(5));
        await server;
    }

    [Fact]
    public async Task TlsSendWorksWithSkippedCertificateValidation()
    {
        using var certificate = CreateSelfSignedCertificate();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var ssl = new SslStream(tcp.GetStream(), false);
            await ssl.AuthenticateAsServerAsync(certificate);
            var packet = await ReadPacketAsync(ssl);
            var header = headerCodec.Decode(packet.Header);
            Assert.Equal("th", header.Name);
            Assert.Equal(ZlinkStreamCodec.Raw, header.Codec);
            Assert.Equal("tb", Encoding.UTF8.GetString(packet.Payload));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tls://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            SkipServerCertificateValidation = true
        });
        await connector.Connect.Async();

        await connector.Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, "tb"u8.ToArray()))
            .PacketName("th").Async();

        await server;
    }

    [Fact]
    public async Task TlsConnectTimeoutDisposesAcceptedSocket()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var observedClientClose = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var buffer = new byte[1024];
            while (await stream.ReadAsync(buffer).AsTask().WaitAsync(TimeSpan.FromSeconds(5)) > 0)
            {
            }

            observedClientClose.SetResult();
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tls://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            ConnectTimeout = TimeSpan.FromMilliseconds(100),
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            SkipServerCertificateValidation = true
        });

        var exception = await Assert.ThrowsAsync<ZlinkStreamException>(async () => await connector.Connect.Async());

        Assert.Equal(ZlinkStreamErrorCode.ConnectTimeout, exception.Error.Code);
        await observedClientClose.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server;
    }

    [Fact]
    public async Task TlsReceivePayloadLimitDisconnectsBeforePayloadAllocation()
    {
        using var certificate = CreateSelfSignedCertificate();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var ssl = new SslStream(tcp.GetStream(), false);
            await ssl.AuthenticateAsServerAsync(certificate);
            await WritePrefixAsync(ssl, 0, 2);
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tls://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            MaxReceivePayloadSize = 1,
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            SkipServerCertificateValidation = true
        });
        await connector.Connect.Async();

        await WaitUntilAsync(
            () => connector.State == ZlinkStreamConnectionState.Disconnected,
            TimeSpan.FromSeconds(5));
        await server;
    }

    private sealed class FaultingDisposeStream(Exception failure) : MemoryStream
    {
        public override ValueTask DisposeAsync() => ValueTask.FromException(failure);
    }
}
