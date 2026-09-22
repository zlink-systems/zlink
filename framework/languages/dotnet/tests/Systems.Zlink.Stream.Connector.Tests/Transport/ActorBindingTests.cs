using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Text;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;

public sealed partial class StreamConnectorTests
{
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
