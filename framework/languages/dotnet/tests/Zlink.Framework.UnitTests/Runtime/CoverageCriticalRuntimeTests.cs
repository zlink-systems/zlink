using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using K4os.Compression.LZ4;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Codecs;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class CoverageCriticalRuntimeTests
{
    [Fact]
    public void StreamProtocolLz4DecompressRejectsDecodedPayloadAboveDefaultLimit()
    {
        var compressed = LZ4Pickler.Pickle(new byte[64 * 1024 + 1]);

        Assert.Throws<InvalidOperationException>(() =>
            ZLinkLz4StreamCompressionCodec.DecompressPayload(compressed, 64 * 1024));
    }

    [Fact]
    public void StreamSendBuilderUsesConfiguredCompressionCodec()
    {
        var compression = new PrefixCompressionCodec();
        var builder = new ZLinkStreamSendBuilder<CompressionProbe>(
            new CompressionProbe("hello"),
            new ZLinkCodecRegistryBuilder(),
            compression);
        ZlinkStreamHeader? capturedHeader = null;
        byte[]? capturedFrame = null;

        builder.EnableCompression();
        builder.Write(
            (codec, flags, name, metadata) =>
            {
                capturedHeader = new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    codec,
                    flags,
                    null,
                    name,
                    metadata);
                return capturedHeader;
            },
            message =>
            {
                capturedFrame = message.ToArray();
                return true;
            },
            "send failed");

        Assert.NotNull(capturedHeader);
        Assert.True(capturedHeader.Flags.HasFlag(ZlinkStreamHeaderFlags.PayloadCompressed));
        Assert.NotNull(capturedFrame);
        var headerLength = BinaryPrimitives.ReadUInt16BigEndian(capturedFrame.AsSpan(0, 2));
        Assert.Equal(PrefixCompressionCodec.Marker, capturedFrame[6 + headerLength]);
    }

    [Fact]
    public void StreamPayloadDecodeUsesConfiguredCompressionCodecAndRuntimeLimit()
    {
        var compression = new PrefixCompressionCodec();
        var payload = compression.Compress(
            ZLinkStreamPacketPayloadCodec.EncodeJson(new CompressionProbe("hello"), typeof(CompressionProbe)));
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.PayloadCompressed,
            null,
            nameof(CompressionProbe),
            ZlinkStreamMetadata.Empty);

        using var message = Message.From(payload.Span);
        var decoded = ZLinkStreamPacketPayloadCodec.DecodeMessage(
            header,
            message,
            new ZLinkCodecRegistryBuilder(),
            compression);

        Assert.Equal(new CompressionProbe("hello"), decoded.Decode<CompressionProbe>());

        using var oversized = Message.From([0x01]);
        Assert.Throws<InvalidOperationException>(() =>
            ZLinkStreamPacketPayloadCodec.DecodeMessage(
                header,
                oversized,
                new ZLinkCodecRegistryBuilder(),
                new OversizedCompressionCodec()));
    }

    [Fact]
    public async Task FrameworkHostStartupFailureDisposesCreatedStreamRuntime()
    {
        var firstEndpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddStreamNode("stream-a")
                .Bind(firstEndpoint)
                .AddSession<StartupFailureTestSession>();
            options.AddStreamNode("stream-b")
                .Bind("invalid://startup-failure")
                .AddSession<StartupFailureTestSession>();
        });

        using var host = builder.Build();
        var startTask = host.StartAsync();
        var completed = await Task.WhenAny(startTask, Task.Delay(TimeSpan.FromSeconds(5)));

        Assert.Same(startTask, completed);
        await Assert.ThrowsAnyAsync<Exception>(async () => await startTask);
    }

    private static int FindFreeTcpPort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        try
        {
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }
        finally
        {
            listener.Stop();
        }
    }

    private sealed record CompressionProbe(string Text);

    private sealed class StartupFailureTestSession(IZLinkSessionContext context) : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }

        public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
        {
            return ValueTask.CompletedTask;
        }
    }

    private sealed class PrefixCompressionCodec : IZlinkStreamCompressionCodec
    {
        public const byte Marker = 0x5A;

        public ReadOnlyMemory<byte> Compress(ReadOnlyMemory<byte> payload)
        {
            var compressed = new byte[payload.Length + 1];
            compressed[0] = Marker;
            payload.CopyTo(compressed.AsMemory(1));
            return compressed;
        }

        public ReadOnlyMemory<byte> Decompress(ReadOnlyMemory<byte> payload, int maxDecompressedPayloadSize)
        {
            if (payload.Length == 0 || payload.Span[0] != Marker)
                throw new InvalidOperationException("Unexpected custom compression marker.");

            return payload[1..].ToArray();
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
