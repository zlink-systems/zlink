using System.Net;
using System.Net.Sockets;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Compression;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public void ConnectorOptionsDiagnosticsLevel_DefaultsToErrors()
    {
        var options = new ZlinkStreamConnectorOptions { Endpoint = new Uri("tcp://127.0.0.1:1") };

        Assert.Equal(ZlinkStreamDiagnosticsLevel.Errors, options.DiagnosticsLevel);
    }

    [Fact]
    public async Task SetDiagnosticsLevel_RejectsUndefinedValue()
    {
        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://127.0.0.1:1")
        });

        var exception = Assert.Throws<ZlinkStreamException>(
            () => connector.SetDiagnosticsLevel((ZlinkStreamDiagnosticsLevel)999));

        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, exception.Error.Code);
        Assert.Equal(ZlinkStreamDiagnosticsLevel.Errors, connector.DiagnosticsLevel);
    }

    [Fact]
    public async Task SetDiagnosticsLevel_UpdatesConnectorAndOptionsTogether()
    {
        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://127.0.0.1:1")
        });

        Assert.Equal(ZlinkStreamDiagnosticsLevel.Errors, connector.DiagnosticsLevel);
        Assert.Equal(ZlinkStreamDiagnosticsLevel.Errors, connector.Options.DiagnosticsLevel);

        connector.SetDiagnosticsLevel(ZlinkStreamDiagnosticsLevel.Off);

        Assert.Equal(ZlinkStreamDiagnosticsLevel.Off, connector.DiagnosticsLevel);
        Assert.Equal(ZlinkStreamDiagnosticsLevel.Off, connector.Options.DiagnosticsLevel);
    }

    [Fact]
    public async Task LiveLevelChange_OnToOff_ClearsFlowFieldsOnTheNextOutboundFrame()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var firstReceived = new TaskCompletionSource<ZlinkStreamHeader>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var secondReceived = new TaskCompletionSource<ZlinkStreamHeader>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var first = await ReadPacketAsync(stream);
            firstReceived.SetResult(headerCodec.Decode(first.Header));
            var second = await ReadPacketAsync(stream);
            secondReceived.SetResult(headerCodec.Decode(second.Header));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat()
        });
        await connector.Connect.Async();

        Assert.Equal(ZlinkStreamDiagnosticsLevel.Errors, connector.DiagnosticsLevel);
        await connector.Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
            .PacketName("before-off")
            .Async();
        var before = await firstReceived.Task.WaitAsync(TimeSpan.FromSeconds(15));
        Assert.True(before.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));
        Assert.NotNull(before.FlowId);

        connector.SetDiagnosticsLevel(ZlinkStreamDiagnosticsLevel.Off);

        await connector.Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 2 }))
            .PacketName("after-off")
            .Async();
        var after = await secondReceived.Task.WaitAsync(TimeSpan.FromSeconds(15));
        Assert.False(after.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));
        Assert.Null(after.FlowId);

        await server;
    }

    [Fact]
    public async Task LiveLevelChange_OffToOn_CarriesFlowFieldsAgainOnTheNextOutboundFrame()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var firstReceived = new TaskCompletionSource<ZlinkStreamHeader>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var secondReceived = new TaskCompletionSource<ZlinkStreamHeader>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var first = await ReadPacketAsync(stream);
            firstReceived.SetResult(headerCodec.Decode(first.Header));
            var second = await ReadPacketAsync(stream);
            secondReceived.SetResult(headerCodec.Decode(second.Header));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DiagnosticsLevel = ZlinkStreamDiagnosticsLevel.Off
        });
        await connector.Connect.Async();

        await connector.Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
            .PacketName("before-on")
            .Async();
        var before = await firstReceived.Task.WaitAsync(TimeSpan.FromSeconds(15));
        Assert.False(before.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));

        connector.SetDiagnosticsLevel(ZlinkStreamDiagnosticsLevel.Normal);

        await connector.Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 2 }))
            .PacketName("after-on")
            .Async();
        var after = await secondReceived.Task.WaitAsync(TimeSpan.FromSeconds(15));
        Assert.True(after.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));
        Assert.NotNull(after.FlowId);

        await server;
    }

    [Fact]
    public void FrameSender_PicksUpALiveLevelChangeStartingWithTheNextBuiltFrame()
    {
        var options = new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://127.0.0.1:1")
        };
        var sender = new ZlinkStreamFrameSender(
            options,
            new ZlinkStreamHeaderCodec(),
            new ZlinkStreamLz4CompressionCodec(),
            new SemaphoreSlim(1, 1),
            static () => null);

        var onFrame = sender.BuildOutboundFrame(
            ZlinkStreamMessageKind.Send,
            "on",
            new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }),
            ZlinkStreamMetadata.Empty,
            compress: false,
            requestSeq: null);
        var onHeader = new ZlinkStreamHeaderCodec().Decode(onFrame.HeaderBytes);
        Assert.True(onHeader.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));

        // A decoded header is an immutable snapshot, so it obviously does not change
        // after the fact. What this exercises is that the live cell write below is
        // visible to the *next* BuildOutboundFrame call without recreating the sender.
        options.SetDiagnosticsLevelLive(ZlinkStreamDiagnosticsLevel.Off);
        Assert.True(onHeader.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));

        var offFrame = sender.BuildOutboundFrame(
            ZlinkStreamMessageKind.Send,
            "off",
            new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 2 }),
            ZlinkStreamMetadata.Empty,
            compress: false,
            requestSeq: null);
        var offHeader = new ZlinkStreamHeaderCodec().Decode(offFrame.HeaderBytes);
        Assert.False(offHeader.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));
    }

    [Fact]
    public async Task InboundDispatch_ReadsDiagnosticsLevelOnceForThePacket_NotOncePerHandler()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var flowId = ZlinkStreamFlowId.Create();
        var handled = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.HasFlowId,
                    null,
                    "toggling",
                    ZlinkStreamMetadata.Empty,
                    FlowId: flowId,
                    FlowOrigin: ZlinkStreamFlowOrigin.Application)).ToArray(),
                Array.Empty<byte>());
            await handled.Task.WaitAsync(TimeSpan.FromSeconds(15));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });

        // Two handlers on the same packet name run inline, in sequence, inside a
        // single DispatchPacketAsync call. Whichever runs first flips the level to
        // Off before the second one runs. If the dispatcher re-read the level per
        // handler (the bug), the second handler would see Off and find no flow
        // scope; with a single read at packet-dispatch entry, both see the level
        // captured when this packet's processing began (Errors) and both find a
        // flow scope installed.
        var flipped = 0;
        var flowSeenByFirst = false;
        var flowSeenBySecond = false;
        connector.On("toggling", (_, _) =>
        {
            var flowWasEntered = ZlinkStreamFlowContext.Current is not null;
            if (Interlocked.Exchange(ref flipped, 1) == 0)
            {
                flowSeenByFirst = flowWasEntered;
                connector.SetDiagnosticsLevel(ZlinkStreamDiagnosticsLevel.Off);
            }
            else
            {
                flowSeenBySecond = flowWasEntered;
                handled.SetResult(true);
            }

            return ValueTask.CompletedTask;
        });
        connector.On("toggling", (_, _) =>
        {
            var flowWasEntered = ZlinkStreamFlowContext.Current is not null;
            if (Interlocked.Exchange(ref flipped, 1) == 0)
            {
                flowSeenByFirst = flowWasEntered;
                connector.SetDiagnosticsLevel(ZlinkStreamDiagnosticsLevel.Off);
            }
            else
            {
                flowSeenBySecond = flowWasEntered;
                handled.SetResult(true);
            }

            return ValueTask.CompletedTask;
        });

        await connector.Connect.Async();
        Assert.True(await handled.Task.WaitAsync(TimeSpan.FromSeconds(15)));

        Assert.True(flowSeenByFirst);
        Assert.True(flowSeenBySecond);
        Assert.Equal(ZlinkStreamDiagnosticsLevel.Off, connector.DiagnosticsLevel);

        await server;
    }
}
