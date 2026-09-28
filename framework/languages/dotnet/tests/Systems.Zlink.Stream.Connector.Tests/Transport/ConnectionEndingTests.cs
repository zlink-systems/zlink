using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Threading.Channels;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Compression;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Framing;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Xunit;

public sealed partial class StreamConnectorTests
{
    private static readonly TimeSpan EndingWait = TimeSpan.FromSeconds(5);

    /// <summary>
    ///     Stream-connector spec §9, §12: a frame or header decode failure and a frame over the
    ///     receive limit end the connection with close reason ProtocolError, and a transport
    ///     read failure ends it with TransportError. The pending request fails with
    ///     Disconnected whatever ended the connection; the cause stays in the close reason.
    /// </summary>
    [Theory]
    [InlineData("decode", ZlinkStreamCloseReason.ProtocolError)]
    [InlineData("too-large", ZlinkStreamCloseReason.ProtocolError)]
    [InlineData("read-failure", ZlinkStreamCloseReason.TransportError)]
    public async Task AReceiveFailureEndsTheConnectionWithTheReasonDecidedWhereItOccurs(
        string failure,
        ZlinkStreamCloseReason expected
    )
    {
        var connection = new ScriptedConnection();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Immediate);
        var disconnected = new TaskCompletionSource<ZlinkStreamCloseReason>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.OnDisconnected(
            (ending, _) =>
            {
                disconnected.TrySetResult(ending.CloseReason);
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();
            var request = connector
                .Request(EndingPayload())
                .PacketName("pending")
                .Timeout(TimeSpan.FromSeconds(30))
                .Async()
                .AsTask();
            await connection.WaitForWritesAsync(1);

            switch (failure)
            {
                case "decode":
                    connection.Deliver(ZlinkStreamFrameCodec.Encode([0xFF, 0xFF, 0xFF, 0xFF], []));
                    break;
                case "too-large":
                    var prefix = new byte[ZlinkStreamFrameCodec.PrefixSize];
                    BinaryPrimitives.WriteUInt32BigEndian(
                        prefix.AsSpan(2, 4),
                        (uint)connector.Options.MaxReceivePayloadSize + 1
                    );
                    connection.Deliver(prefix);
                    break;
                default:
                    connection.FailRead(new IOException("The connection was reset."));
                    break;
            }

            var error = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                await request.WaitAsync(EndingWait)
            );
            Assert.Equal(ZlinkStreamErrorCode.Disconnected, error.Error.Code);
            Assert.Equal(expected, await disconnected.Task.WaitAsync(EndingWait));
            Assert.Equal(expected, connector.CloseReason);
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    /// <summary>
    ///     Stream-connector spec §6.2, §9: the heartbeat decides HeartbeatTimeout where it
    ///     detects the silence, and the pending request fails with Disconnected.
    /// </summary>
    [Fact]
    public async Task AHeartbeatTimeoutEndsTheConnectionWithHeartbeatTimeout()
    {
        var connection = new ScriptedConnection();
        var connector = new ZlinkStreamConnector(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions
                {
                    Enabled = true,
                    Interval = TimeSpan.FromMilliseconds(20),
                    Timeout = TimeSpan.FromMilliseconds(60),
                },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
            },
            _ => ValueTask.FromResult<IZlinkStreamConnection>(connection)
        );
        var disconnected = new TaskCompletionSource<ZlinkStreamCloseReason>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.OnDisconnected(
            (ending, _) =>
            {
                disconnected.TrySetResult(ending.CloseReason);
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();
            var request = connector
                .Request(EndingPayload())
                .PacketName("pending")
                .Timeout(TimeSpan.FromSeconds(30))
                .Async()
                .AsTask();

            Assert.Equal(
                ZlinkStreamCloseReason.HeartbeatTimeout,
                await disconnected.Task.WaitAsync(EndingWait)
            );
            var error = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                await request.WaitAsync(EndingWait)
            );
            Assert.Equal(ZlinkStreamErrorCode.Disconnected, error.Error.Code);
            Assert.Equal(ZlinkStreamCloseReason.HeartbeatTimeout, connector.CloseReason);
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    /// <summary>
    ///     Stream-connector spec §9: a transport write failure that ends the connection fails
    ///     the operation of that write with SendFailed. Every other operation in progress fails
    ///     with Disconnected, and the close reason is TransportError.
    /// </summary>
    [Fact]
    public async Task AWriteFailureFailsOnlyItsOperationWithSendFailed()
    {
        var connection = new ScriptedConnection { FailWriteNumber = 2 };
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Immediate);
        try
        {
            await connector.Connect.Async();
            var request = connector
                .Request(EndingPayload())
                .PacketName("pending")
                .Timeout(TimeSpan.FromSeconds(30))
                .Async()
                .AsTask();
            await connection.WaitForWritesAsync(1);

            var send = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                await connector
                    .Send(EndingPayload())
                    .PacketName("failing")
                    .Async()
                    .AsTask()
                    .WaitAsync(EndingWait)
            );
            var pending = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                await request.WaitAsync(EndingWait)
            );

            Assert.Equal(ZlinkStreamErrorCode.SendFailed, send.Error.Code);
            Assert.Equal(ZlinkStreamErrorCode.Disconnected, pending.Error.Code);
            Assert.Equal(ZlinkStreamCloseReason.TransportError, connector.CloseReason);
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    /// <summary>
    ///     Stream-connector spec §5.2, §5.7 and the .NET language spec §6: a Request cancelled
    ///     while it waits in the write queue ends with the OperationCanceledException of its
    ///     token, writes no frame and does not run the reply received hook.
    /// </summary>
    [Fact]
    public async Task ARequestCancelledWhileQueuedWritesNoFrameAndSkipsTheReplyHook()
    {
        var connection = new ScriptedConnection();
        connection.HoldWrites();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Manual);
        var hooks = new ConcurrentQueue<ZlinkStreamReplyReceivedContext>();
        connector.OnReplyReceived(
            (context, _) =>
            {
                hooks.Enqueue(context);
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();
            var first = connector.Send(EndingPayload()).PacketName("first").Async().AsTask();
            await connection.WaitForWritesAsync(1);
            using var cancellation = new CancellationTokenSource();
            var queued = connector
                .Request(EndingPayload())
                .PacketName("queued")
                .Timeout(TimeSpan.FromSeconds(30))
                .Async(cancellation.Token)
                .AsTask();

            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
                await queued.WaitAsync(EndingWait)
            );

            connection.ReleaseWrites();
            await first.WaitAsync(EndingWait);
            // The queue passes the canceled request before it writes this Send.
            await connector
                .Send(EndingPayload())
                .PacketName("last")
                .Async()
                .AsTask()
                .WaitAsync(EndingWait);
            await connector.Dispatch.Async();

            Assert.Equal(["first", "last"], connection.WrittenNames());
            Assert.Empty(hooks);
        }
        finally
        {
            connection.ReleaseWrites();
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    /// <summary>
    ///     Stream-connector spec §5.2, §5.7: cancelling a Request after its frame write started
    ///     does not stop the write, ends the Request as cancelled without the reply hook and
    ///     removes it from the pending map, so an Error reply with its sequence is a
    ///     stream-level error.
    /// </summary>
    [Fact]
    public async Task ARequestCancelledAfterItsWriteStartedLeavesThePendingMap()
    {
        var connection = new ScriptedConnection();
        connection.HoldWrites();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Immediate);
        var hooks = new ConcurrentQueue<ZlinkStreamReplyReceivedContext>();
        connector.OnReplyReceived(
            (context, _) =>
            {
                hooks.Enqueue(context);
                return ValueTask.CompletedTask;
            }
        );
        var streamError = new TaskCompletionSource<ZlinkStreamError>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.OnErrorReceived(
            (error, _) =>
            {
                streamError.TrySetResult(error);
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();
            using var cancellation = new CancellationTokenSource();
            var request = connector
                .Request(EndingPayload())
                .PacketName("writing")
                .Timeout(TimeSpan.FromSeconds(30))
                .Async(cancellation.Token)
                .AsTask();
            await connection.WaitForWritesAsync(1);

            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
                await request.WaitAsync(EndingWait)
            );
            connection.ReleaseWrites();

            var codec = new ZlinkStreamHeaderCodec();
            var sent = codec.Decode(connection.Written.Single().Header);
            var reply = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Error,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                sent.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty
            );
            connection.Deliver(
                ZlinkStreamFrameCodec.Encode(
                    codec.Encode(reply).Span,
                    """{"code":"late","message":"after cancel"}"""u8
                )
            );

            var error = await streamError.Task.WaitAsync(EndingWait);
            Assert.Equal(ZlinkStreamErrorCode.RemoteError, error.Code);
            Assert.Empty(hooks);
        }
        finally
        {
            connection.ReleaseWrites();
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    /// <summary>
    ///     Stream-connector spec §5.2: the write queue has no size bound. Operations beyond
    ///     any fixed count are accepted behind a write that does not finish, and are all
    ///     written in acceptance order once it does.
    /// </summary>
    [Fact]
    public async Task TheWriteQueueHasNoSizeBound()
    {
        const int count = 5000;
        var connection = new ScriptedConnection();
        connection.HoldWrites();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Manual);
        try
        {
            await connector.Connect.Async();
            var sends = new Task[count];
            for (var index = 0; index < count; index++)
                sends[index] = connector
                    .Send(EndingPayload())
                    .PacketName($"n{index}")
                    .Async()
                    .AsTask();
            await connection.WaitForWritesAsync(1);

            connection.ReleaseWrites();
            await Task.WhenAll(sends).WaitAsync(TimeSpan.FromSeconds(30));

            Assert.Equal(
                Enumerable.Range(0, count).Select(index => $"n{index}"),
                connection.WrittenNames()
            );
        }
        finally
        {
            connection.ReleaseWrites();
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    /// <summary>
    ///     Stream-connector spec §9: a push packet whose payload does not decompress fails
    ///     only that packet. The error event reports DecompressionFailed, the connection stays
    ///     and the next packet is delivered.
    /// </summary>
    [Fact]
    public async Task APushThatFailsToDecompressFailsOnlyThatPacket()
    {
        var connection = new ScriptedConnection();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Immediate);
        var errors = new ConcurrentQueue<ZlinkStreamErrorCode>();
        connector.OnErrorReceived(
            (error, _) =>
            {
                errors.Enqueue(error.Code);
                return ValueTask.CompletedTask;
            }
        );
        var delivered = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.On(
            "after",
            (message, _) =>
            {
                delivered.TrySetResult(message.Name);
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();

            connection.Deliver(PushFrame("broken", CorruptCompressedPayload(), compressed: true));
            connection.Deliver(PushFrame("after", new byte[] { 1 }, compressed: false));

            Assert.Equal("after", await delivered.Task.WaitAsync(EndingWait));
            Assert.Equal([ZlinkStreamErrorCode.DecompressionFailed], errors);
            Assert.Equal(ZlinkStreamConnectionState.Connected, connector.State);
            Assert.Null(connector.CloseReason);
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    /// <summary>
    ///     Stream-connector spec §9: a reply whose payload does not decompress fails only the
    ///     request it answers with DecompressionFailed, and the connection stays.
    /// </summary>
    [Fact]
    public async Task AReplyThatFailsToDecompressFailsOnlyItsRequest()
    {
        var connection = new ScriptedConnection();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Immediate);
        try
        {
            await connector.Connect.Async();
            var request = connector
                .Request(EndingPayload())
                .PacketName("compressed.reply")
                .Timeout(TimeSpan.FromSeconds(30))
                .Async()
                .AsTask();
            await connection.WaitForWritesAsync(1);
            var codec = new ZlinkStreamHeaderCodec();
            var sent = codec.Decode(connection.Written.Single().Header);
            var reply = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq | ZlinkStreamHeaderFlags.PayloadCompressed,
                sent.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty
            );
            connection.Deliver(
                ZlinkStreamFrameCodec.Encode(codec.Encode(reply).Span, CorruptCompressedPayload())
            );

            var error = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                await request.WaitAsync(EndingWait)
            );
            Assert.Equal(ZlinkStreamErrorCode.DecompressionFailed, error.Error.Code);
            Assert.Equal(ZlinkStreamConnectionState.Connected, connector.State);
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    /// <summary>
    ///     Stream-connector spec §4.7, §9: the decompressed payload is compared with the receive
    ///     limit too. Over it is FrameTooLarge, which ends the connection with ProtocolError, and
    ///     the pending request fails with Disconnected.
    /// </summary>
    [Fact]
    public async Task ADecompressedPayloadOverTheReceiveLimitEndsTheConnection()
    {
        var connection = new ScriptedConnection();
        var connector = new ZlinkStreamConnector(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                MaxReceivePayloadSize = 1024,
            },
            _ => ValueTask.FromResult<IZlinkStreamConnection>(connection)
        );
        var errors = new ConcurrentQueue<ZlinkStreamErrorCode>();
        connector.OnErrorReceived(
            (error, _) =>
            {
                errors.Enqueue(error.Code);
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();
            var request = connector
                .Request(EndingPayload())
                .PacketName("pending")
                .Timeout(TimeSpan.FromSeconds(30))
                .Async()
                .AsTask();
            await connection.WaitForWritesAsync(1);

            var compressed = ZlinkStreamLz4PayloadCodec.Compress(new byte[4096]).ToArray();
            Assert.True(compressed.Length <= 1024);
            connection.Deliver(PushFrame("too.large", compressed, compressed: true));

            var error = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                await request.WaitAsync(EndingWait)
            );
            Assert.Equal(ZlinkStreamErrorCode.Disconnected, error.Error.Code);
            Assert.Equal(ZlinkStreamCloseReason.ProtocolError, connector.CloseReason);
            Assert.Contains(ZlinkStreamErrorCode.FrameTooLarge, errors);
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    /// <summary>
    ///     Stream-connector spec §10: a packet stays in the receive queue until a handler or a
    ///     wait takes it, so a handler registered after the packet arrived receives it. The
    ///     handlers are decided when the packet is dispatched.
    /// </summary>
    [Theory]
    [InlineData(ZlinkStreamDispatchMode.Manual)]
    [InlineData(ZlinkStreamDispatchMode.Immediate)]
    public async Task AHandlerRegisteredAfterThePacketArrivedReceivesIt(
        ZlinkStreamDispatchMode dispatchMode
    )
    {
        var connection = new ScriptedConnection();
        var connector = CreateConnectorOn(connection, dispatchMode);
        try
        {
            await connector.Connect.Async();
            connection.Deliver(PushFrame("late", new byte[] { 7 }, compressed: false));
            await WaitUntilAsync(() => connector.ReceivedCount("late") == 1, EndingWait);

            var received = new TaskCompletionSource<byte>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            connector.On(
                "late",
                (message, _) =>
                {
                    received.TrySetResult(message.Payload.Payload.Span[0]);
                    return ValueTask.CompletedTask;
                }
            );
            await connector.Dispatch.Async();

            Assert.Equal(7, await received.Task.WaitAsync(EndingWait));
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    /// <summary>
    ///     Stream-connector spec §7, §10: a Manual dispatch runs the handlers registered at that
    ///     moment. A handler removed before the pump does not run, and the packet stays queued
    ///     for the handler registered after it.
    /// </summary>
    [Fact]
    public async Task AManualDispatchDeliversToTheHandlersRegisteredWhenItRuns()
    {
        var connection = new ScriptedConnection();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Manual);
        var calls = new ConcurrentQueue<string>();
        try
        {
            await connector.Connect.Async();
            var first = connector.On(
                "moved",
                (_, _) =>
                {
                    calls.Enqueue("first");
                    return ValueTask.CompletedTask;
                }
            );
            connection.Deliver(PushFrame("moved", new byte[] { 1 }, compressed: false));
            await WaitUntilAsync(() => connector.ReceivedCount("moved") == 1, EndingWait);

            first.Dispose();
            await connector.Dispatch.Async();
            Assert.Empty(calls);

            connector.On(
                "moved",
                (_, _) =>
                {
                    calls.Enqueue("second");
                    return ValueTask.CompletedTask;
                }
            );
            await connector.Dispatch.Async();

            Assert.Equal(["second"], calls);
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    /// <summary>
    ///     Stream-connector spec §5.6, §7, §10: an Actor handle receive registration follows
    ///     the connector rule. A packet no handler takes stays queued through a dispatch, and
    ///     a handle handler registered after that receives it.
    /// </summary>
    [Theory]
    [InlineData(ZlinkStreamDispatchMode.Manual)]
    [InlineData(ZlinkStreamDispatchMode.Immediate)]
    public async Task AnActorHandlerRegisteredAfterThePacketArrivedReceivesIt(
        ZlinkStreamDispatchMode dispatchMode
    )
    {
        var connection = new ScriptedConnection();
        var connector = CreateConnectorOn(connection, dispatchMode);
        try
        {
            await connector.Connect.Async();
            var codec = new ZlinkStreamHeaderCodec();
            var bound = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Control,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                "$zlink.actor.bound",
                ZlinkStreamMetadata.Empty
            );
            connection.Deliver(
                ZlinkStreamFrameCodec.Encode(
                    codec.Encode(bound).Span,
                    [1, 0, 7, 8, .. "player-a"u8]
                )
            );
            await WaitUntilAsync(() => connector.Actor("player-a") is not null, EndingWait);
            var actorPacket = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasActorSlot,
                null,
                "late",
                ZlinkStreamMetadata.Empty,
                ActorSlot: 7
            );
            connection.Deliver(ZlinkStreamFrameCodec.Encode(codec.Encode(actorPacket).Span, [9]));
            await WaitUntilAsync(() => connector.ReceivedCount("late") == 1, EndingWait);
            await connector.Dispatch.Async();

            var received = new TaskCompletionSource<string?>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            connector
                .Actor("player-a")!
                .On(
                    "late",
                    (message, _) =>
                    {
                        received.TrySetResult(message.ActorId);
                        return ValueTask.CompletedTask;
                    }
                );
            await connector.Dispatch.Async();

            Assert.Equal("player-a", await received.Task.WaitAsync(EndingWait));
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }
    }

    private static byte[] PushFrame(string name, byte[] payload, bool compressed)
    {
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Raw,
            compressed ? ZlinkStreamHeaderFlags.PayloadCompressed : ZlinkStreamHeaderFlags.None,
            null,
            name,
            ZlinkStreamMetadata.Empty
        );
        return ZlinkStreamFrameCodec.Encode(
            new ZlinkStreamHeaderCodec().Encode(header).Span,
            payload
        );
    }

    /// <summary>
    ///     An LZ4 pickle whose header announces a small payload but whose body is cut short,
    ///     so decompressing it fails.
    /// </summary>
    private static byte[] CorruptCompressedPayload()
    {
        var pickled = ZlinkStreamLz4PayloadCodec.Compress(new byte[200]).ToArray();
        return pickled[..^3];
    }

    private static ZlinkStreamEncodedPayload EndingPayload() =>
        new(ZlinkStreamCodec.Raw, new byte[] { 1 });

    /// <summary>
    ///     A transport the test drives: inbound bytes and read failures are delivered in order,
    ///     every written frame is recorded, writes can be held until released, and one write
    ///     can fail as a broken transport does.
    /// </summary>
    private sealed class ScriptedConnection : IZlinkStreamConnection
    {
        private readonly Channel<object> _inbound = Channel.CreateUnbounded<object>();
        private readonly ConcurrentQueue<ZlinkStreamFrame> _written = new();
        private readonly SemaphoreSlim _writeSignal = new(0);
        private TaskCompletionSource? _heldWrites;
        private byte[] _current = [];
        private int _offset;
        private int _writeCount;

        public int FailWriteNumber { get; init; }

        public IReadOnlyCollection<ZlinkStreamFrame> Written => _written;

        public bool CanWriteSegments => false;

        public void Deliver(byte[] bytes) => _inbound.Writer.TryWrite(bytes);

        public void FailRead(Exception failure) => _inbound.Writer.TryWrite(failure);

        public void HoldWrites() =>
            Volatile.Write(
                ref _heldWrites,
                new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously)
            );

        public void ReleaseWrites() => Volatile.Read(ref _heldWrites)?.TrySetResult();

        public async Task WaitForWritesAsync(int count)
        {
            for (var index = 0; index < count; index++)
                Assert.True(await _writeSignal.WaitAsync(EndingWait));
        }

        public IReadOnlyList<string> WrittenNames()
        {
            var codec = new ZlinkStreamHeaderCodec();
            return _written.Select(frame => codec.Decode(frame.Header).Name).ToList();
        }

        public async ValueTask<int> ReadAsync(
            Memory<byte> buffer,
            CancellationToken cancellationToken
        )
        {
            while (_offset == _current.Length)
            {
                var next = await _inbound.Reader.ReadAsync(cancellationToken);
                if (next is Exception failure)
                    throw failure;
                _current = (byte[])next;
                _offset = 0;
            }

            var count = Math.Min(buffer.Length, _current.Length - _offset);
            _current.AsMemory(_offset, count).CopyTo(buffer);
            _offset += count;
            return count;
        }

        public async ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken
        )
        {
            var number = Interlocked.Increment(ref _writeCount);
            if (number == FailWriteNumber)
                throw new IOException("The transport is broken.");

            Record(buffer);
            if (Volatile.Read(ref _heldWrites) is { } held)
                await held.Task.WaitAsync(cancellationToken);
        }

        private void Record(ReadOnlyMemory<byte> buffer)
        {
            Assert.True(
                ZlinkStreamFrameCodec.TryDecode(buffer.Span, out var header, out var payload)
            );
            _written.Enqueue(new ZlinkStreamFrame(header.ToArray(), payload.ToArray()));
            _writeSignal.Release();
        }

        public ValueTask CloseAsync(CancellationToken cancellationToken)
        {
            Volatile
                .Read(ref _heldWrites)
                ?.TrySetException(new IOException("The transport was closed."));
            return ValueTask.CompletedTask;
        }
    }
}
