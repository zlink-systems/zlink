using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed partial class StreamSessionForcedCleanupTests
{
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task Stream_reader_applies_only_complete_ping_before_expiring(bool complete)
    {
        const string nodeName = "reader-liveness";
        var registration = new ZLinkFrameworkRegistration();
        var lifetime = new SessionOrderingLifetime();
        ZLinkFrameworkRuntime runtime = null!;
        await using var provider = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifetime)
            .AddSingleton(_ => runtime)
            .BuildServiceProvider();
        runtime = CreateRuntime(provider, registration);
        var secondFrame = new TaskCompletionSource<byte[]>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var socket = new TestStreamSocket();
        socket.BeforePoll = () =>
        {
            if (socket.SentFrames.Count >= 2)
                secondFrame.TrySetResult(socket.SentFrames.Last());
        };
        var monitor = new TestSocketMonitor();
        var time = new ControllableTimeProvider();
        var runner = new ZLinkRuntimeTaskRunner(
            runtime.ErrorSink,
            CancellationToken.None,
            runtime.ExecutionOwner
        );
        await using var node = new ZLinkStreamNodeRuntime(
            nodeName,
            provider,
            socket,
            monitor,
            typeof(SessionOrderingSession),
            runner,
            "test",
            time
        );
        var routingId = RoutingId.From("reader-liveness-peer");
        var header = ZLinkStreamProtocolDefaults.EncodeHeader(
            new(
                ZlinkStreamMessageKind.Control,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                ZlinkStreamControlProtocol.HeartbeatPingName,
                ZlinkStreamMetadata.Empty
            )
        );
        var ping = ZLinkStreamFrameCodec.Encode(header.Span, []);
        var wireBytes = complete ? ping.AsMemory() : ping.AsMemory(0, ping.Length - 1);
        var readerEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var releaseReader = new ManualResetEventSlim();
        try
        {
            node.Start();
            monitor.Emit(
                new ZLinkBackendSocketMonitorEvent(
                    ZLinkSocketNativeEventType.ConnectionReady,
                    routingId,
                    "local",
                    "remote",
                    1,
                    MonitorEventFlags.ConnectionReadyEdge
                )
            );
            await lifetime.WaitConnectedAsync(routingId);
            using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            while (!time.HasTimerDueIn(TimeSpan.FromSeconds(1)))
                await Task.Delay(1, deadline.Token);

            socket.BeforeRecvPacket = () =>
            {
                readerEntered.TrySetResult();
                releaseReader.Wait();
                return true;
            };
            // The PACKET socket exposes a record only when the entire frame has arrived.
            EnqueueWireBytes(socket, routingId, wireBytes);
            if (complete)
                await readerEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));

            time.Advance(TimeSpan.FromSeconds(5));
            releaseReader.Set();

            var firstFrame = await socket.SentFrame.Task.WaitAsync(TimeSpan.FromSeconds(5));
            if (complete)
            {
                var name = ReadFrameName(firstFrame);
                if (name == ZlinkStreamSessionClosingCodec.ControlName)
                    AssertHeartbeatTimeoutFrame(firstFrame);
                Assert.Equal(ZlinkStreamControlProtocol.HeartbeatPongName, name);
                var nextFrame = await secondFrame.Task.WaitAsync(TimeSpan.FromSeconds(5));
                Assert.Equal(
                    ZlinkStreamControlProtocol.HeartbeatPingName,
                    ReadFrameName(nextFrame)
                );
                Assert.Equal(0, socket.DisconnectCount);
            }
            else
            {
                await socket.DisconnectStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
                AssertHeartbeatTimeoutFrame(firstFrame);
            }
        }
        finally
        {
            releaseReader.Set();
        }
    }

    private static void EnqueueWireBytes(
        TestStreamSocket socket,
        RoutingId routingId,
        ReadOnlyMemory<byte> wireBytes
    )
    {
        if (ZLinkStreamFrameCodec.TryDecode(wireBytes.Span, out var header, out var payload))
            socket.EnqueuePacket(routingId, header, payload);
    }

    private static string ReadFrameName(byte[] frame)
    {
        Assert.True(ZLinkStreamFrameCodec.TryDecode(frame, out var header, out _));
        return ZLinkStreamProtocolDefaults.DecodeHeader(header.ToArray()).Name;
    }
}
