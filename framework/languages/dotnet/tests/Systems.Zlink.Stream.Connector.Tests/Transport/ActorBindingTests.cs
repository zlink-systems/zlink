using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public async Task ActorHandlersIsolateMatchingNamesAndRequestHooksReportActorId()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var codec = new ZlinkStreamHeaderCodec();
        var releaseServer = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releasePushes = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                ControlHeader(codec, "$zlink.actor.bound"),
                BoundPayload(1, "actor-a")
            );
            await WritePacketAsync(
                stream,
                ControlHeader(codec, "$zlink.actor.bound"),
                BoundPayload(2, "actor-b")
            );
            await releasePushes.Task;
            foreach (
                var (slot, name, value) in new[]
                {
                    ((ushort)2, "shared", "b"),
                    ((ushort)1, "shared", "a"),
                    ((ushort)1, nameof(Pong), "derived"),
                }
            )
            {
                var header = new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Json,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    name,
                    ZlinkStreamMetadata.Empty,
                    ActorSlot: slot
                );
                await WritePacketAsync(
                    stream,
                    codec.Encode(header).ToArray(),
                    new Pong(value).ToJson().Payload.ToArray()
                );
            }
            var request = codec.Decode((await ReadPacketAsync(stream)).Header);
            Assert.Equal((ushort)1, request.ActorSlot);
            Assert.Equal("actor-a", request.Metadata.Get("actor"));
            var response = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                request.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty
            );
            await WritePacketAsync(stream, codec.Encode(response).ToArray(), [9]);
            await releaseServer.Task;
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            }
        );
        var hooks = new List<string>();
        var received = new System.Collections.Concurrent.ConcurrentQueue<string>();
        connector.OnRequestSending(context =>
        {
            hooks.Add($"sending:{context.ActorId}");
            context.SetMetadata("actor", context.ActorId!);
        });
        connector.OnReplyReceived(
            (context, _) =>
            {
                hooks.Add($"reply:{context.ActorId}");
                return ValueTask.CompletedTask;
            }
        );
        await connector.Connect.Async();
        await WaitUntilAsync(() => connector.Actors.Count == 2, TimeSpan.FromSeconds(5));
        var actorA = connector.Actor("actor-a")!;
        var actorB = connector.Actor("actor-b")!;
        using var onA = actorA.On<Pong>(
            "shared",
            (message, _) =>
            {
                received.Enqueue($"a:{message.Payload.Text}");
                return ValueTask.CompletedTask;
            }
        );
        using var onB = actorB.On<Pong>(
            "shared",
            (message, _) =>
            {
                received.Enqueue($"b:{message.Payload.Text}");
                return ValueTask.CompletedTask;
            }
        );
        using var onDerived = actorA.On<Pong>(
            (message, _) =>
            {
                received.Enqueue($"a:{message.Payload.Text}");
                return ValueTask.CompletedTask;
            }
        );
        releasePushes.SetResult();
        await WaitUntilAsync(() => received.Count == 3, TimeSpan.FromSeconds(5));
        Assert.Equal(new[] { "b:b", "a:a", "a:derived" }, received.ToArray());
        var reply = await actorA
            .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
            .PacketName("actor.query")
            .Async();
        Assert.Equal((byte)9, reply.Payload.Span[0]);
        Assert.Equal(new[] { "sending:actor-a", "reply:actor-a" }, hooks);
        releaseServer.SetResult();
        await server;
    }

    [Fact]
    public async Task ActorBindingPublishesHandleRoutesMessagesAndCarriesSlotOutbound()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var codec = new ZlinkStreamHeaderCodec();
        var outboundSlot = new TaskCompletionSource<ushort?>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                ControlHeader(codec, "$zlink.actor.bound"),
                BoundPayload(23, "actor-a")
            );
            await WritePacketAsync(
                stream,
                codec
                    .Encode(
                        new ZlinkStreamHeader(
                            ZlinkStreamMessageKind.Send,
                            ZlinkStreamCodec.Raw,
                            ZlinkStreamHeaderFlags.None,
                            null,
                            "actor.push",
                            ZlinkStreamMetadata.Empty,
                            ActorSlot: 23
                        )
                    )
                    .ToArray(),
                [1]
            );
            var outbound = await ReadPacketAsync(stream);
            outboundSlot.SetResult(codec.Decode(outbound.Header).ActorSlot);
            await WritePacketAsync(
                stream,
                ControlHeader(codec, "$zlink.actor.unbound"),
                [1, 0, 23]
            );
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
            }
        );
        var events = new List<string>();
        var received = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var unbound = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        IZlinkStreamActor? handle = null;
        connector.OnActorBound(
            (actor, _) =>
            {
                handle = actor;
                events.Add("bound");
                actor.On(
                    "actor.push",
                    (message, _) =>
                    {
                        events.Add($"message:{message.ActorId}");
                        received.SetResult();
                        return ValueTask.CompletedTask;
                    }
                );
                return ValueTask.CompletedTask;
            }
        );
        connector.OnActorUnbound(
            (actor, _) =>
            {
                events.Add("unbound");
                unbound.SetResult();
                return ValueTask.CompletedTask;
            }
        );

        await connector.Connect.Async();
        await received.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.NotNull(handle);
        Assert.Same(handle, connector.Actor("actor-a"));
        Assert.Single(connector.Actors);
        await handle!
            .Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 2 }))
            .PacketName("actor.send")
            .Async();
        Assert.Equal((ushort)23, await outboundSlot.Task.WaitAsync(TimeSpan.FromSeconds(5)));
        await unbound.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server;

        Assert.Equal(new[] { "bound", "message:actor-a", "unbound" }, events);
        Assert.False(handle.IsBound);
        Assert.Null(connector.Actor("actor-a"));
        var closed = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
            await handle
                .Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 3 }))
                .PacketName("actor.closed")
                .Async()
        );
        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, closed.Error.Code);
        var closedRequest = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
            await handle
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 4 }))
                .PacketName("actor.closed.request")
                .Async()
        );
        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, closedRequest.Error.Code);
    }

    [Fact]
    public async Task UnknownActorSlotEndsConnectionAsFrameDecodeFailed()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var codec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                codec
                    .Encode(
                        new ZlinkStreamHeader(
                            ZlinkStreamMessageKind.Send,
                            ZlinkStreamCodec.Raw,
                            ZlinkStreamHeaderFlags.None,
                            null,
                            "actor.unknown",
                            ZlinkStreamMetadata.Empty,
                            ActorSlot: 99
                        )
                    )
                    .ToArray(),
                [1]
            );
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
            }
        );
        var error = new TaskCompletionSource<ZlinkStreamError>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.OnErrorReceived(
            (received, _) =>
            {
                error.TrySetResult(received);
                return ValueTask.CompletedTask;
            }
        );

        await connector.Connect.Async();
        var observed = await error.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server;

        Assert.Equal(ZlinkStreamErrorCode.FrameDecodeFailed, observed.Code);
        Assert.Equal(ZlinkStreamCloseReason.ProtocolError, connector.CloseReason);
    }

    [Fact]
    public async Task DuplicateActorBoundEndsConnectionAsFrameDecodeFailed()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var codec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var header = ControlHeader(codec, "$zlink.actor.bound");
            await WritePacketAsync(stream, header, BoundPayload(1, "actor-a"));
            await WritePacketAsync(stream, header, BoundPayload(1, "actor-b"));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
            }
        );
        var error = new TaskCompletionSource<ZlinkStreamError>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.OnErrorReceived(
            (received, _) =>
            {
                error.TrySetResult(received);
                return ValueTask.CompletedTask;
            }
        );

        await connector.Connect.Async();
        var observed = await error.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server;

        Assert.Equal(ZlinkStreamErrorCode.FrameDecodeFailed, observed.Code);
        Assert.Equal(ZlinkStreamCloseReason.ProtocolError, connector.CloseReason);
    }

    [Fact]
    public async Task DuplicateActorIdOnDifferentSlotEndsConnectionAsFrameDecodeFailed()
    {
        await AssertActorProtocolErrorAsync(
            async (stream, codec) =>
            {
                var header = ControlHeader(codec, "$zlink.actor.bound");
                await WritePacketAsync(stream, header, BoundPayload(1, "actor-a"));
                await WritePacketAsync(stream, header, BoundPayload(2, "actor-a"));
            }
        );
    }

    [Fact]
    public async Task UnknownActorUnboundEndsConnectionAsFrameDecodeFailed()
    {
        await AssertActorProtocolErrorAsync(
            (stream, codec) =>
                WritePacketAsync(stream, ControlHeader(codec, "$zlink.actor.unbound"), [1, 0, 9])
        );
    }

    public static TheoryData<string, byte[]> InvalidActorControlPayloads =>
        new()
        {
            { "$zlink.actor.bound", new byte[] { 2, 0, 1, 1, (byte)'a' } },
            { "$zlink.actor.bound", new byte[] { 1, 0, 1, 2, (byte)'a' } },
            { "$zlink.actor.unbound", new byte[] { 2, 0, 1 } },
            { "$zlink.actor.unbound", new byte[] { 1, 0 } },
        };

    [Theory]
    [MemberData(nameof(InvalidActorControlPayloads))]
    public async Task InvalidActorControlPayloadEndsConnectionAsFrameDecodeFailed(
        string name,
        byte[] payload
    )
    {
        await AssertActorProtocolErrorAsync(
            (stream, codec) => WritePacketAsync(stream, ControlHeader(codec, name), payload)
        );
    }

    [Fact]
    public async Task ControlHeaderWithActorSlotEndsConnectionAsFrameDecodeFailed()
    {
        await AssertActorProtocolErrorAsync(
            async (stream, codec) =>
            {
                var valid = ControlHeader(codec, "$zlink.actor.bound");
                valid[3] |= (byte)ZlinkStreamHeaderFlags.HasActorSlot;
                var malformed = new byte[valid.Length + 2];
                valid.CopyTo(malformed, 0);
                malformed[^1] = 1;
                await WritePacketAsync(stream, malformed, BoundPayload(1, "actor-a"));
            }
        );
    }

    [Fact]
    public async Task DisconnectClosesActorsInIssueOrderBeforeStateAndDisconnectedCallbacks()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var codec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var header = ControlHeader(codec, "$zlink.actor.bound");
            await WritePacketAsync(stream, header, BoundPayload(1, "actor-a"));
            await WritePacketAsync(stream, header, BoundPayload(2, "actor-b"));
        });

        await using var connector = CreateActorConnector(
            endpoint,
            ZlinkStreamDispatchMode.Immediate
        );
        var events = new List<string>();
        var disconnected = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.OnActorUnbound(
            (actor, _) =>
            {
                events.Add($"unbound:{actor.ActorId}");
                return ValueTask.CompletedTask;
            }
        );
        connector.OnConnectionStateChanged(
            (change, _) =>
            {
                if (change.Current == ZlinkStreamConnectionState.Disconnected)
                    events.Add("state:disconnected");
                return ValueTask.CompletedTask;
            }
        );
        connector.OnDisconnected(
            (_, _) =>
            {
                events.Add("disconnected");
                disconnected.TrySetResult();
                return ValueTask.CompletedTask;
            }
        );

        await connector.Connect.Async();
        await server;
        await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(
            ["unbound:actor-a", "unbound:actor-b", "state:disconnected", "disconnected"],
            events
        );
    }

    [Fact]
    public async Task ManualQueueKeepsBoundBeforeFirstActorPacketWhenCapacityIsFull()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var codec = new ZlinkStreamHeaderCodec();
        var releaseServer = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                ControlHeader(codec, "$zlink.actor.bound"),
                BoundPayload(7, "actor-a")
            );
            await WritePacketAsync(
                stream,
                codec
                    .Encode(
                        new ZlinkStreamHeader(
                            ZlinkStreamMessageKind.Send,
                            ZlinkStreamCodec.Raw,
                            ZlinkStreamHeaderFlags.None,
                            null,
                            "actor.packet",
                            ZlinkStreamMetadata.Empty,
                            ActorSlot: 7
                        )
                    )
                    .ToArray(),
                [1]
            );
            await releaseServer.Task;
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                DispatchMode = ZlinkStreamDispatchMode.Manual,
                MaxPendingDispatchCallbacks = 1,
            }
        );
        var events = new List<string>();
        connector.OnActorBound(
            (actor, _) =>
            {
                events.Add("bound");
                actor.On(
                    "actor.packet",
                    (_, _) =>
                    {
                        events.Add("packet");
                        return ValueTask.CompletedTask;
                    }
                );
                return ValueTask.CompletedTask;
            }
        );

        await connector.Connect.Async();
        await WaitUntilAsync(() => connector.PendingDispatchCount == 1, TimeSpan.FromSeconds(5));
        await connector.Dispatch.Async();
        if (events.Count < 2)
        {
            await WaitUntilAsync(
                () => connector.PendingDispatchCount == 1,
                TimeSpan.FromSeconds(5)
            );
            await connector.Dispatch.Async();
        }

        Assert.Equal(["bound", "packet"], events);
        releaseServer.TrySetResult();
        await connector.Close.Async();
        await server;
    }

    [Fact]
    public async Task ActorRequestPreservesConnectorRequestCancellation()
    {
        var connection = new BlockingWriteConnection();
        await using var connector = new ZlinkStreamConnector(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = DisabledHeartbeat(),
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            },
            _ => ValueTask.FromResult<IZlinkStreamConnection>(connection)
        );
        await connector.Connect.Async();
        var actor = new ZlinkStreamActor(connector, "actor-a", 1);
        using var canceled = new CancellationTokenSource();
        canceled.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            connector
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                .PacketName("connector.request")
                .Async(canceled.Token)
                .AsTask()
        );
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            actor
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                .PacketName("actor.request")
                .Async(canceled.Token)
                .AsTask()
        );
    }

    [Fact]
    public async Task ActorRequestPreservesConnectorRequestBackpressure()
    {
        var connection = new BlockingWriteConnection();
        await using var connector = new ZlinkStreamConnector(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = DisabledHeartbeat(),
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                RequestTimeout = TimeSpan.FromMinutes(1),
            },
            _ => ValueTask.FromResult<IZlinkStreamConnection>(connection)
        );
        await connector.Connect.Async();
        var actor = new ZlinkStreamActor(connector, "actor-a", 1);
        using var cancelQueued = new CancellationTokenSource();
        var pending = new List<Task>();
        pending.Add(
            connector
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                .PacketName("connector.blocking")
                .Async(cancelQueued.Token)
                .AsTask()
        );
        await connection.WriteStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        for (var index = 0; index < 4096; index++)
            pending.Add(
                connector
                    .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                    .PacketName("connector.queued")
                    .Async(cancelQueued.Token)
                    .AsTask()
            );

        var connectorFailure = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
            await connector
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 2 }))
                .PacketName("connector.full")
                .Async()
        );
        var actorFailure = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
            await actor
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 2 }))
                .PacketName("actor.full")
                .Async()
        );

        Assert.Equal(ZlinkStreamErrorCode.SendFailed, connectorFailure.Error.Code);
        Assert.Equal(connectorFailure.Error.Code, actorFailure.Error.Code);
        Assert.Equal(connectorFailure.Error.Message, actorFailure.Error.Message);

        cancelQueued.Cancel();
        connection.ReleaseWrite.TrySetResult();
        await connector.Close.Async();
        foreach (var operation in pending)
            try
            {
                await operation;
            }
            catch (OperationCanceledException) { }
            catch (ZlinkStreamException error)
                when (error.Error.Code == ZlinkStreamErrorCode.Disconnected) { }
    }

    private static async Task AssertActorProtocolErrorAsync(
        Func<NetworkStream, ZlinkStreamHeaderCodec, Task> writeInvalidAsync
    )
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await writeInvalidAsync(stream, new ZlinkStreamHeaderCodec());
        });
        await using var connector = CreateActorConnector(
            endpoint,
            ZlinkStreamDispatchMode.Immediate
        );
        var error = new TaskCompletionSource<ZlinkStreamError>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.OnErrorReceived(
            (received, _) =>
            {
                error.TrySetResult(received);
                return ValueTask.CompletedTask;
            }
        );

        await connector.Connect.Async();
        var observed = await error.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server;

        Assert.Equal(ZlinkStreamErrorCode.FrameDecodeFailed, observed.Code);
        Assert.Equal(ZlinkStreamCloseReason.ProtocolError, connector.CloseReason);
    }

    private static IZlinkStreamConnector CreateActorConnector(
        IPEndPoint endpoint,
        ZlinkStreamDispatchMode dispatchMode
    ) =>
        ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                DispatchMode = dispatchMode,
            }
        );

    private static byte[] ControlHeader(ZlinkStreamHeaderCodec codec, string name) =>
        codec
            .Encode(
                new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Control,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    name,
                    ZlinkStreamMetadata.Empty
                )
            )
            .ToArray();

    private static byte[] BoundPayload(ushort slot, string actorId)
    {
        var id = Encoding.UTF8.GetBytes(actorId);
        var payload = new byte[4 + id.Length];
        payload[0] = 1;
        BinaryPrimitives.WriteUInt16BigEndian(payload.AsSpan(1, 2), slot);
        payload[3] = checked((byte)id.Length);
        id.CopyTo(payload.AsSpan(4));
        return payload;
    }
}
