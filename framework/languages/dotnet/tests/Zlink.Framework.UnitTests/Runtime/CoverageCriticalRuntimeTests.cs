using System.Buffers.Binary;
using K4os.Compression.LZ4;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class CoverageCriticalRuntimeTests
{
    [Fact]
    public void StreamProtocolLz4DecompressRejectsDecodedPayloadAboveDefaultLimit()
    {
        var compressed = LZ4Pickler.Pickle(new byte[64 * 1024 + 1]);

        Assert.Throws<InvalidOperationException>(() =>
            ZLinkLz4StreamCompressionCodec.DecompressPayload(compressed, 64 * 1024)
        );
    }

    [Fact]
    public void StreamSendBuilderUsesConfiguredCompressionCodec()
    {
        var compression = new PrefixCompressionCodec();
        var builder = new ZLinkStreamSendBuilder<CompressionProbe>(
            new CompressionProbe("hello"),
            new ZLinkCodecRegistryBuilder(),
            compression
        );
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
                    metadata
                );
                return capturedHeader;
            },
            message =>
            {
                capturedFrame = message.ToArray();
                return true;
            },
            "send failed"
        );

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
            ZLinkStreamPacketPayloadCodec.EncodeJson(
                new CompressionProbe("hello"),
                typeof(CompressionProbe)
            )
        );
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.PayloadCompressed,
            null,
            nameof(CompressionProbe),
            ZlinkStreamMetadata.Empty
        );

        using var message = Message.From(payload.Span);
        var decoded = ZLinkStreamPacketPayloadCodec.DecodeMessage(
            header,
            message,
            new ZLinkCodecRegistryBuilder(),
            compression
        );

        Assert.Equal(new CompressionProbe("hello"), decoded.Decode<CompressionProbe>());

        using var oversized = Message.From([0x01]);
        Assert.Throws<InvalidOperationException>(() =>
            ZLinkStreamPacketPayloadCodec.DecodeMessage(
                header,
                oversized,
                new ZLinkCodecRegistryBuilder(),
                new OversizedCompressionCodec()
            )
        );
    }

    [Fact]
    public async Task FrameworkHostStartupFailureDisposesCreatedStreamRuntime()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options
                .AddStreamNode("stream-a")
                .Bind("tcp://127.0.0.1:0")
                .AddSession<StartupFailureTestSession>();
            //  같은 host 안에서 같은 session type을 두 node에 등록하면 등록 검증이 먼저
            //  거부하므로(STREAM 서버 session §3.2), 두 번째 node는 다른 type을 쓴다.
            //  이 테스트가 확인하려는 것은 endpoint 실패로 startup이 깨질 때 이미 만든
            //  stream runtime을 dispose하는가이다.
            options
                .AddStreamNode("stream-b")
                .Bind("invalid://startup-failure")
                .AddSession<SecondStartupFailureTestSession>();
        });

        using var host = builder.Build();
        var startTask = host.StartAsync();
        var completed = await Task.WhenAny(startTask, Task.Delay(TimeSpan.FromSeconds(5)));

        Assert.Same(startTask, completed);
        await Assert.ThrowsAnyAsync<Exception>(async () => await startTask);
    }

    /// <summary>
    ///     A STREAM node whose port is already taken fails startup with the bind error, and
    ///     the startup rollback releases the socket it created, so the runtime context
    ///     terminates instead of waiting for a socket nobody owns.
    /// </summary>
    [Fact]
    public async Task StreamNodeBindOnBusyPortFailsStartupAndReleasesTheContext()
    {
        using var occupied = new System.Net.Sockets.TcpListener(System.Net.IPAddress.Loopback, 0);
        occupied.Start();
        var port = ((System.Net.IPEndPoint)occupied.LocalEndpoint).Port;
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
            options
                .AddStreamNode("stream-busy")
                .Bind($"tcp://127.0.0.1:{port}")
                .AddSession<StartupFailureTestSession>()
        );

        var host = builder.Build();
        var startTask = host.StartAsync();
        var completed = await Task.WhenAny(startTask, Task.Delay(TimeSpan.FromSeconds(10)));

        Assert.Same(startTask, completed);
        var failure = await Assert.ThrowsAnyAsync<Exception>(async () => await startTask);
        Assert.IsType<Systems.Zlink.ZlinkBindException>(failure);
        var dispose = Task.Run(() => host.Dispose());
        Assert.Same(dispose, await Task.WhenAny(dispose, Task.Delay(TimeSpan.FromSeconds(10))));
    }

    /// <summary>
    ///     Startup owns a STREAM socket until the node that takes it exists. When creating
    ///     the node fails, startup disposes the socket, so the runtime context does not wait
    ///     for a socket nobody owns.
    /// </summary>
    [Fact]
    public async Task StreamNodeCreationFailureDisposesTheSocketItWasGiven()
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.StreamNodes.Add(
            "stream-a",
            new ZLinkStreamNodeRegistration { StreamNodeName = "stream-a" }
        );
        var socket = new DisposalRecordingStreamSocket();
        // No ZLinkFrameworkRuntime is registered, so creating the node fails after the
        // socket exists.
        using var services = new ServiceCollection().BuildServiceProvider();
        using var errorSink = new ZLinkRuntimeErrorSink();
        await using var state = new ZLinkFrameworkComponentState(
            new StreamSocketRuntimeContext(socket),
            registration,
            services,
            errorSink,
            new object(),
            ZLinkApplicationJobQueueCapacityResolver.Resolve(
                ZLinkApplicationJobQueueProfile.Balanced,
                int.MaxValue,
                1
            ),
            new ZLinkListenerRecords()
        );
        var manager = new ZLinkStreamRuntimeManager(
            services,
            new MonitorlessBackendAdapterFactory(),
            registration
        );

        await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await manager.InitializeStreamNodesAsync(state)
        );

        Assert.Equal(1, socket.DisposeCount);
        Assert.Empty(state.StreamNodes);
    }

    private sealed class MonitorlessBackendAdapterFactory : IZLinkBackendAdapterFactory
    {
        public IZLinkBackendRuntimeContext CreateRuntimeContext() =>
            throw new NotSupportedException();

        public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter() => null!;
    }

    private sealed class StreamSocketRuntimeContext(IZLinkBackendStreamSocket socket)
        : IZLinkBackendRuntimeContext
    {
        public void ConfigureCoreHwm(
            AutoHwmProfile profile,
            ulong memoryLimitBytes,
            ulong budgetBytes
        ) { }

        public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot() =>
            throw new NotSupportedException();

        public void ResetCoreHwmBudgetMetrics() => throw new NotSupportedException();

        public void ConfigureApplicationJobQueue(ZLinkApplicationJobQueue applicationJobQueue) { }

        public IDealerSocket CreateDealerSocket() => throw new NotSupportedException();

        public IRouterSocket CreateRouterSocket() => throw new NotSupportedException();

        public IPubSocket CreatePublisherSocket() => throw new NotSupportedException();

        public ISubSocket CreateSubscriberSocket() => throw new NotSupportedException();

        public IZLinkBackendSpotNode CreateSpotNode(string meshName) =>
            throw new NotSupportedException();

        public IZLinkBackendStreamSocket CreateStreamSocket(
            string standaloneMeshName,
            IZLinkBackendSpotNode? actorDispatchNode = null
        ) => socket;

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    private sealed class DisposalRecordingStreamSocket : IZLinkBackendStreamSocket
    {
        public int DisposeCount { get; private set; }

        public void Bind(string endpoint) => throw new NotSupportedException();

        public void SetTlsServer(string certPath, string keyPath, bool requireClientCert) =>
            throw new NotSupportedException();

        public bool RecvPacket(
            out ZLinkBackendStreamReceive? received,
            RecvFlags flags = RecvFlags.None
        ) => throw new NotSupportedException();

        public Task SendAsync(
            RoutingId routingId,
            Message payload,
            CancellationToken cancellationToken = default
        ) => throw new NotSupportedException();

        public void DisconnectPeer(RoutingId routingId) => throw new NotSupportedException();

        public ValueTask BindActorAsync(
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            TimeSpan timeout,
            CancellationToken cancellationToken
        ) => throw new NotSupportedException();

        public ValueTask UnbindActorAsync(
            RoutingId sessionRid,
            string actorId,
            TimeSpan timeout,
            CancellationToken cancellationToken
        ) => throw new NotSupportedException();

        public bool SendBoundActor(
            RoutingId sessionRid,
            string actorId,
            IReadOnlyList<Message> parts,
            SendFlags flags
        ) => throw new NotSupportedException();

        public ValueTask DisposeAsync()
        {
            DisposeCount++;
            return ValueTask.CompletedTask;
        }
    }

    private sealed record CompressionProbe(string Text);

    private sealed class SecondStartupFailureTestSession(IZLinkSessionContext context)
        : IZLinkSession
    {
        public IZLinkSessionContext Context { get; } = context;

        public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask OnErrorAsync(
            ZLinkStreamError error,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;
    }

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

        public ReadOnlyMemory<byte> Decompress(
            ReadOnlyMemory<byte> payload,
            int maxDecompressedPayloadSize
        )
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

        public ReadOnlyMemory<byte> Decompress(
            ReadOnlyMemory<byte> payload,
            int maxDecompressedPayloadSize
        )
        {
            return new byte[maxDecompressedPayloadSize + 1];
        }
    }
}
