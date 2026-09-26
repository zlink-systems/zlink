using System.Net.Sockets;
using System.Net.WebSockets;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Xunit;

public sealed partial class StreamConnectorTests
{
    /// <summary>Stream-connector spec §5.2: a Send completes once its frame is written.</summary>
    [Fact]
    public async Task SendCompletesOnlyAfterItsFrameIsWrittenToTheTransport()
    {
        var connection = new TransportCloseEndsWriteConnection();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Manual);
        try
        {
            await connector.Connect.Async();
            var send = connector
                .Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                .PacketName("written")
                .Async()
                .AsTask();
            await connection.WriteStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

            Assert.False(send.IsCompleted);
            connection.ReleaseWrite.TrySetResult();
            await send.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            connection.ReleaseWrite.TrySetResult();
            await connector.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    /// <summary>
    ///     Stream-connector spec §7, §9: close does not write frames not yet written to the
    ///     transport, fails their operations with Disconnected (close reason ClientClose) and
    ///     does not wait for the peer to read. The peer here never reads, so the write in
    ///     progress ends only when close closes the transport.
    /// </summary>
    [Fact]
    public async Task CloseFailsUnwrittenFramesWithDisconnectedWithoutWaitingForThePeer()
    {
        var connection = new TransportCloseEndsWriteConnection();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Manual);
        try
        {
            await connector.Connect.Async();
            var inProgress = connector
                .Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                .PacketName("in.progress")
                .Async()
                .AsTask();
            await connection.WriteStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var queuedSend = connector
                .Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 2 }))
                .PacketName("queued.send")
                .Async()
                .AsTask();
            var queuedRequest = connector
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 3 }))
                .PacketName("queued.request")
                .Timeout(TimeSpan.FromSeconds(30))
                .Async()
                .AsTask();

            await connector.Close.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));

            Assert.Equal(1, connection.WriteCount);
            foreach (var operation in new Task[] { inProgress, queuedSend, queuedRequest })
            {
                var error = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                    await operation.WaitAsync(TimeSpan.FromSeconds(5))
                );
                Assert.Equal(ZlinkStreamErrorCode.Disconnected, error.Error.Code);
            }
            Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
            Assert.Equal(ZlinkStreamCloseReason.ClientClose, connector.CloseReason);
        }
        finally
        {
            connection.ReleaseWrite.TrySetResult();
            await connector.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    /// <summary>
    ///     Stream-connector spec §7: the connection state and disconnect callbacks that result
    ///     from close follow the dispatch mode. Immediate runs them in the close work, before a
    ///     close called outside a callback returns; Manual runs them at the next pump.
    /// </summary>
    [Theory]
    [InlineData(ZlinkStreamDispatchMode.Immediate)]
    [InlineData(ZlinkStreamDispatchMode.Manual)]
    public async Task CloseCallbacksFollowTheDispatchMode(ZlinkStreamDispatchMode dispatchMode)
    {
        var connection = new TransportCloseEndsWriteConnection();
        var connector = CreateConnectorOn(connection, dispatchMode);
        var events = new List<string>();
        connector.OnConnectionStateChanged(
            (change, _) =>
            {
                lock (events)
                    events.Add($"state:{change.Current}");
                return ValueTask.CompletedTask;
            }
        );
        connector.OnDisconnected(
            (disconnected, _) =>
            {
                lock (events)
                    events.Add($"disconnected:{disconnected.CloseReason}");
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();
            await connector.Dispatch.Async();
            lock (events)
                events.Clear();

            await connector.Close.Async();

            string[] expected = ["state:Closed", "disconnected:ClientClose"];
            if (dispatchMode == ZlinkStreamDispatchMode.Manual)
            {
                lock (events)
                    Assert.Empty(events);
                await connector.Dispatch.Async();
            }
            lock (events)
                Assert.Equal(expected, events);
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    /// <summary>
    ///     Stream-connector spec §7: closing the transport does not wait for the peer to
    ///     respond. The peer here never answers the close frame.
    /// </summary>
    [Fact]
    public async Task WebSocketCloseDoesNotWaitForThePeerCloseFrame()
    {
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        await using var app = builder.Build();
        app.UseWebSockets();
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        app.Map(
            "/silent-close",
            async (HttpContext context) =>
            {
                using var socket = await context.WebSockets.AcceptWebSocketAsync();
                await release.Task;
            }
        );
        await app.StartAsync();
        var address = new Uri(app.Urls.Single());
        using var client = new ClientWebSocket();
        await client.ConnectAsync(
            new Uri($"ws://127.0.0.1:{address.Port}/silent-close/"),
            CancellationToken.None
        );
        var connection = new WebSocketConnection(client, 1024);
        try
        {
            await connection
                .CloseAsync(CancellationToken.None)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(WebSocketState.Closed, client.State);
        }
        finally
        {
            release.TrySetResult();
        }
    }

    /// <summary>
    ///     Stream-connector spec §7, §9: ending a WebSocket connection ends a frame being
    ///     written by closing the transport and does not wait for the peer to read, whether
    ///     close ends it or a transport error the receive loop reads. The WebSocket here runs
    ///     over a stream whose writes stop, the way a socket does once a peer that stops
    ///     reading has filled its buffers, and end only when the stream is disposed.
    /// </summary>
    /// <remarks>
    ///     A real peer does not make the write stop reliably: Windows loopback accepts far
    ///     more than a test can send before a peer that does not read holds the writer back.
    ///     The transport error case ends the connection from the receive loop, which has no
    ///     receive in progress that canceling could abort.
    /// </remarks>
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task WebSocketConnectionEndEndsAWriteThePeerDoesNotRead(bool endByTransportError)
    {
        var builder = WebApplication.CreateBuilder();
        builder.WebHost.UseUrls("http://127.0.0.1:0");
        await using var app = builder.Build();
        app.UseWebSockets();
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var sendText = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        app.Map(
            "/stalled",
            async (HttpContext context) =>
            {
                using var socket = await context.WebSockets.AcceptWebSocketAsync();
                await Task.WhenAny(sendText.Task, release.Task);
                // A text message is a frame decode failure for the connector (spec §9).
                if (sendText.Task.IsCompleted)
                    await socket.SendAsync(
                        "text"u8.ToArray(),
                        WebSocketMessageType.Text,
                        true,
                        CancellationToken.None
                    );
                await release.Task;
            }
        );
        await app.StartAsync();
        var address = new Uri(app.Urls.Single());
        StallingStream? transport = null;
        using var handler = new SocketsHttpHandler
        {
            ConnectCallback = async (context, cancellationToken) =>
            {
                var socket = new Socket(SocketType.Stream, ProtocolType.Tcp);
                await socket.ConnectAsync(context.DnsEndPoint, cancellationToken);
                transport = new StallingStream(new NetworkStream(socket, ownsSocket: true));
                return transport;
            },
        };
        var endpoint = new Uri($"ws://127.0.0.1:{address.Port}/stalled/");
        var client = new ClientWebSocket();
        await client.ConnectAsync(
            endpoint,
            new HttpMessageInvoker(handler),
            CancellationToken.None
        );
        var connector = new ZlinkStreamConnector(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = endpoint,
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            },
            _ => ValueTask.FromResult<IZlinkStreamConnection>(new WebSocketConnection(client, 1024))
        );
        try
        {
            await connector.Connect.Async();
            transport!.StallWrites();
            var send = connector
                .Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                .PacketName("stalled.write")
                .Async()
                .AsTask();
            await transport.WriteStalled.Task.WaitAsync(TimeSpan.FromSeconds(5));

            if (endByTransportError)
                sendText.TrySetResult();
            else
                await connector.Close.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));

            var error = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                await send.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(ZlinkStreamErrorCode.Disconnected, error.Error.Code);
            Assert.Equal(
                endByTransportError
                    ? ZlinkStreamConnectionState.Disconnected
                    : ZlinkStreamConnectionState.Closed,
                connector.State
            );
        }
        finally
        {
            release.TrySetResult();
            await connector.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
            client.Dispose();
        }
    }

    /// <summary>
    ///     Stream-connector spec §7: a close called in a callback that a Manual dispatch pump
    ///     runs starts the close work and returns at once. The transport close here does not
    ///     finish until the test releases it, so a close that waited for the close work could
    ///     not have returned.
    /// </summary>
    [Fact]
    public async Task CloseInAManualPumpCallbackReturnsWithoutWaitingForTheCloseWork()
    {
        var connection = new HeldCloseConnection();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Manual);
        Task? closeInCallback = null;
        connector.OnConnectionStateChanged(
            (change, _) =>
            {
                if (change.Current == ZlinkStreamConnectionState.Connected)
                    closeInCallback = connector.Close.Async().AsTask();
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();
            await connector.Dispatch.Async();

            Assert.NotNull(closeInCallback);
            Assert.True(closeInCallback.IsCompletedSuccessfully);
            await connection.CloseStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var closeOutside = connector.Close.Async().AsTask();
            Assert.False(closeOutside.IsCompleted);

            connection.ReleaseClose.TrySetResult();
            await closeOutside.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            connection.ReleaseClose.TrySetResult();
            await connector.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    /// <summary>
    ///     Stream-connector spec §7: the error handler that a failed frame write publishes is a
    ///     connector callback too, so a close called in it returns without waiting for the
    ///     close work.
    /// </summary>
    [Fact]
    public async Task CloseInTheWriteFailureErrorHandlerReturnsWithoutWaitingForTheCloseWork()
    {
        var connection = new HeldCloseConnection(failWrites: true);
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Immediate);
        var closeInCallback = new TaskCompletionSource<Task>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.OnErrorReceived(
            (_, _) =>
            {
                closeInCallback.TrySetResult(connector.Close.Async().AsTask());
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();
            var send = connector
                .Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                .PacketName("failing.write")
                .Async()
                .AsTask();

            var close = await closeInCallback.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.True(close.IsCompletedSuccessfully);
            connection.ReleaseClose.TrySetResult();
            await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                await send.WaitAsync(TimeSpan.FromSeconds(5))
            );
        }
        finally
        {
            connection.ReleaseClose.TrySetResult();
            await connector.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    /// <summary>
    ///     Stream-connector spec §9: an operation that reaches the write queue after close has
    ///     finished finds no connection, so it fails with Disconnected like one submitted
    ///     while disconnected. The request sending hook, which runs before acceptance, closes
    ///     the connector and waits for the close to finish.
    /// </summary>
    [Fact]
    public async Task OperationReachingTheQueueAfterCloseFailsWithDisconnected()
    {
        var connection = new TransportCloseEndsWriteConnection();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Manual);
        connector.OnRequestSending(_ => connector.Close.Async().AsTask().GetAwaiter().GetResult());
        try
        {
            await connector.Connect.Async();
            var payload = new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 });

            var request = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
                await connector
                    .Request(payload)
                    .PacketName("after.close")
                    .Async()
                    .AsTask()
                    .WaitAsync(TimeSpan.FromSeconds(5))
            );

            Assert.Equal(ZlinkStreamErrorCode.Disconnected, request.Error.Code);
            Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
            Assert.Equal(0, connection.WriteCount);
        }
        finally
        {
            await connector.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        }
    }

    private static ZlinkStreamConnector CreateConnectorOn(
        IZlinkStreamConnection connection,
        ZlinkStreamDispatchMode dispatchMode
    ) =>
        new(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                DispatchMode = dispatchMode,
            },
            _ => ValueTask.FromResult(connection)
        );

    /// <summary>
    ///     A transport whose peer does not read: a write stays in progress until the test
    ///     releases it or the connector closes the transport, which ends the write the way a
    ///     closed socket does.
    /// </summary>
    private sealed class TransportCloseEndsWriteConnection : IZlinkStreamConnection
    {
        private int _writeCount;

        public TaskCompletionSource WriteStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseWrite { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int WriteCount => Volatile.Read(ref _writeCount);
        public bool CanWriteSegments => false;

        public async ValueTask<int> ReadAsync(
            Memory<byte> buffer,
            CancellationToken cancellationToken
        )
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            return 0;
        }

        public async ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken
        )
        {
            Interlocked.Increment(ref _writeCount);
            WriteStarted.TrySetResult();
            await ReleaseWrite.Task.WaitAsync(cancellationToken);
        }

        public ValueTask CloseAsync(CancellationToken cancellationToken)
        {
            ReleaseWrite.TrySetException(new IOException("The transport was closed."));
            return ValueTask.CompletedTask;
        }
    }

    /// <summary>
    ///     A transport whose close does not finish until the test releases it. With
    ///     <c>failWrites</c> every frame write fails as a broken transport does.
    /// </summary>
    private sealed class HeldCloseConnection(bool failWrites = false) : IZlinkStreamConnection
    {
        public TaskCompletionSource CloseStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseClose { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool CanWriteSegments => false;

        public async ValueTask<int> ReadAsync(
            Memory<byte> buffer,
            CancellationToken cancellationToken
        )
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            return 0;
        }

        public ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken
        ) =>
            failWrites
                ? ValueTask.FromException(new IOException("The transport is broken."))
                : ValueTask.CompletedTask;

        public async ValueTask CloseAsync(CancellationToken cancellationToken)
        {
            CloseStarted.TrySetResult();
            await ReleaseClose.Task;
        }
    }

    /// <summary>
    ///     A connection stream whose writes, once stalled, do not finish until the stream is
    ///     disposed, as a socket write does when the peer stops reading.
    /// </summary>
    private sealed class StallingStream(Stream inner) : Stream
    {
        private readonly TaskCompletionSource _disposed = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private volatile bool _stalled;

        public TaskCompletionSource WriteStalled { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => true;
        public override long Length => throw new NotSupportedException();

        public override long Position
        {
            get => throw new NotSupportedException();
            set => throw new NotSupportedException();
        }

        public void StallWrites() => _stalled = true;

        public override async ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken = default
        )
        {
            if (!_stalled)
            {
                await inner.WriteAsync(buffer, cancellationToken);
                return;
            }

            WriteStalled.TrySetResult();
            await _disposed.Task.WaitAsync(cancellationToken);
            throw new IOException("The connection was closed.");
        }

        public override Task WriteAsync(
            byte[] buffer,
            int offset,
            int count,
            CancellationToken cancellationToken
        ) => WriteAsync(buffer.AsMemory(offset, count), cancellationToken).AsTask();

        public override void Write(byte[] buffer, int offset, int count) =>
            WriteAsync(buffer, offset, count, CancellationToken.None).GetAwaiter().GetResult();

        public override ValueTask<int> ReadAsync(
            Memory<byte> buffer,
            CancellationToken cancellationToken = default
        ) => inner.ReadAsync(buffer, cancellationToken);

        public override Task<int> ReadAsync(
            byte[] buffer,
            int offset,
            int count,
            CancellationToken cancellationToken
        ) => inner.ReadAsync(buffer, offset, count, cancellationToken);

        public override int Read(byte[] buffer, int offset, int count) =>
            inner.Read(buffer, offset, count);

        public override void Flush() => inner.Flush();

        public override Task FlushAsync(CancellationToken cancellationToken) =>
            inner.FlushAsync(cancellationToken);

        public override long Seek(long offset, SeekOrigin origin) =>
            throw new NotSupportedException();

        public override void SetLength(long value) => throw new NotSupportedException();

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                _disposed.TrySetResult();
                inner.Dispose();
            }
            base.Dispose(disposing);
        }

        public override async ValueTask DisposeAsync()
        {
            _disposed.TrySetResult();
            await inner.DisposeAsync();
            await base.DisposeAsync();
        }
    }
}
