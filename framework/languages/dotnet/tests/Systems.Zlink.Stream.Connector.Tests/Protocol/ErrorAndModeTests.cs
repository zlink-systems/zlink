using System.Net;
using System.Net.Sockets;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Compression;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Framing;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public async Task PendingResponseIgnoresLegacyReplyPacketName()
    {
        var requests = new ZlinkStreamPendingRequests();
        var pending = requests.Create("expected.response");
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Response,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            pending.RequestSeq,
            "legacy.response.name",
            ZlinkStreamMetadata.Empty);

        Assert.True(requests.TryComplete(
            header,
            new ZlinkStreamFrame(ReadOnlyMemory<byte>.Empty, ReadOnlyMemory<byte>.Empty),
            _ => new ZlinkStreamError(ZlinkStreamErrorCode.RemoteError, "unused")));

        var completion = await requests.WaitAsync(pending, CancellationToken.None);
        Assert.Equal(pending.RequestSeq, completion.Header.RequestSeq);
        Assert.Equal("legacy.response.name", completion.Header.Name);
        Assert.Null(completion.Error);
    }

    [Theory]
    [InlineData(ZlinkStreamMessageKind.Response)]
    [InlineData(ZlinkStreamMessageKind.Error)]
    public void ReplyHeaderCodecRequiresAnEmptyPacketName(ZlinkStreamMessageKind kind)
    {
        var codec = new ZlinkStreamHeaderCodec();
        var header = new ZlinkStreamHeader(
            kind,
            kind == ZlinkStreamMessageKind.Error ? ZlinkStreamCodec.Json : ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            new ZlinkStreamRequestSeq(7),
            string.Empty,
            ZlinkStreamMetadata.Empty);

        var decoded = codec.Decode(codec.Encode(header));

        Assert.Equal(string.Empty, decoded.Name);
        Assert.Throws<ZlinkStreamException>(() => codec.Encode(header with { Name = "forbidden.reply.name" }));

        var encoded = codec.Encode(header).ToArray();
        var legacy = new byte[encoded.Length + 6];
        encoded.AsSpan(0, encoded.Length - 1).CopyTo(legacy);
        legacy[encoded.Length - 1] = 6;
        "legacy"u8.CopyTo(legacy.AsSpan(encoded.Length));

        Assert.Equal("legacy", codec.Decode(legacy).Name);
    }

    [Fact]
    public async Task RequestTimeoutRemovesPendingRequest()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            _ = await ReadPacketAsync(stream);
            await Task.Delay(TimeSpan.FromMilliseconds(200));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            RequestTimeout = TimeSpan.FromMilliseconds(50)
        });
        await connector.Connect.Async();

        var exception = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
            await connector.Request(new Ping("hello"))
                .PacketName("ping")
                .Async<Pong>());

        Assert.Equal(ZlinkStreamErrorCode.RequestTimeout, exception.Error.Code);
        await server;
    }

    [Fact]
    public async Task DisconnectedSendFailsBeforeTransportWrite()
    {
        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://127.0.0.1:1")
        });

        var exception = Assert.Throws<ZlinkStreamException>(() =>
            connector.Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, "b"u8.ToArray()))
                .PacketName("h")
                .Async());

        Assert.Equal(ZlinkStreamErrorCode.Disconnected, exception.Error.Code);
    }

    [Fact]
    public async Task SendPayloadLimitIsEnforcedBeforeTransportWrite()
    {
        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://127.0.0.1:1"),
            MaxSendPayloadSize = 1
        });

        var exception = Assert.Throws<ZlinkStreamException>(() =>
            connector.Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, "bb"u8.ToArray()))
                .PacketName("h")
                .Async());

        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, exception.Error.Code);
    }

    [Fact]
    public void CompressedSendPayloadLimit_UsesCompressedWireSize()
    {
        var options = new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://127.0.0.1:1"),
            MaxSendPayloadSize = 64
        };
        var sender = new ZlinkStreamFrameSender(
            options,
            new ZlinkStreamHeaderCodec(),
            new ZlinkStreamLz4CompressionCodec(),
            new SemaphoreSlim(1, 1),
            static () => null);

        var frame = sender.BuildOutboundFrame(
            ZlinkStreamMessageKind.Send,
            "compressed",
            new ZlinkStreamEncodedPayload(
                ZlinkStreamCodec.Raw,
                Enumerable.Repeat((byte)'A', 1024).ToArray()),
            ZlinkStreamMetadata.Empty,
            compress: true,
            requestSeq: null);

        Assert.True(frame.PayloadBytes.Length <= options.MaxSendPayloadSize);
        Assert.True(
            new ZlinkStreamHeaderCodec()
                .Decode(frame.HeaderBytes)
                .Flags
                .HasFlag(ZlinkStreamHeaderFlags.PayloadCompressed));
    }

    [Fact]
    public void CompressedSendPayloadLimit_RejectsExpandedWirePayload()
    {
        var options = new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://127.0.0.1:1"),
            MaxSendPayloadSize = 1,
            CompressionCodec = new ExpandingCompressionCodec()
        };
        var sender = new ZlinkStreamFrameSender(
            options,
            new ZlinkStreamHeaderCodec(),
            options.CompressionCodec,
            new SemaphoreSlim(1, 1),
            static () => null);

        var exception = Assert.Throws<ZlinkStreamException>(() => sender.BuildOutboundFrame(
            ZlinkStreamMessageKind.Send,
            "expanded",
            new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }),
            ZlinkStreamMetadata.Empty,
            compress: true,
            requestSeq: null));

        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, exception.Error.Code);
    }

    [Fact]
    public async Task RequestPayloadLimitIsEnforcedBeforeTransportWrite()
    {
        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://127.0.0.1:1"),
            MaxSendPayloadSize = 1
        });

        var exception = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
            await connector.Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, "bb"u8.ToArray()))
                .PacketName("h")
                .Async());

        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, exception.Error.Code);
    }

    [Fact]
    public async Task ReceivePayloadLimitMustBePositive()
    {
        var exception = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                MaxReceivePayloadSize = 0
            }));

        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, exception.Error.Code);
        await Task.CompletedTask;
    }

    private sealed class ExpandingCompressionCodec : IZlinkStreamCompressionCodec
    {
        public ReadOnlyMemory<byte> Compress(ReadOnlyMemory<byte> payload)
        {
            return new byte[payload.Length + 1];
        }

        public ReadOnlyMemory<byte> Decompress(
            ReadOnlyMemory<byte> payload,
            int maxDecompressedPayloadSize)
        {
            return payload;
        }
    }

    [Theory]
    [InlineData("MaxReceivedMessages")]
    [InlineData("MaxPendingDispatchCallbacks")]
    [InlineData("MaxInboundObserverNotifications")]
    public async Task QueueLimitsMustBePositive(string optionName)
    {
        var options = optionName switch
        {
            "MaxReceivedMessages" => new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                MaxReceivedMessages = 0
            },
            "MaxPendingDispatchCallbacks" => new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                MaxPendingDispatchCallbacks = 0
            },
            "MaxInboundObserverNotifications" => new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                MaxInboundObserverNotifications = 0
            },
            _ => throw new ArgumentOutOfRangeException(nameof(optionName), optionName, null)
        };

        var exception = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamConnectorFactory.Create(options));

        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, exception.Error.Code);
        await Task.CompletedTask;
    }

    [Fact]
    public async Task InboundObserverPayloadPreviewLimitMustNotBeNegative()
    {
        var exception = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                MaxInboundObserverPayloadPreviewBytes = -1
            }));

        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, exception.Error.Code);
        await Task.CompletedTask;
    }

    [Fact]
    public async Task InvalidHeaderFramePublishesDecodeError()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(stream, "invalid-header"u8.ToArray(), "payload"u8.ToArray());
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}")
        });
        var errorReceived =
            new TaskCompletionSource<ZlinkStreamError>(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ErrorReceived += (error, _) =>
        {
            if (error.Code == ZlinkStreamErrorCode.FrameDecodeFailed) errorReceived.TrySetResult(error);

            return ValueTask.CompletedTask;
        };

        await connector.Connect.Async();
        await DispatchUntilAsync(
            connector,
            () => errorReceived.Task.IsCompleted,
            TimeSpan.FromSeconds(5));
        var error = await errorReceived.Task;

        Assert.Equal(ZlinkStreamErrorCode.FrameDecodeFailed, error.Code);
        await server;
    }
}
