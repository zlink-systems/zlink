using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Framing;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task StaleDisconnectDuringErrorCallbackDoesNotFailNewConnectionRequest(
        bool replyBeforeEnding
    )
    {
        var first = new ScriptedConnection();
        var second = new ScriptedConnection();
        var callbackEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseCallback = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var connector = CreateConnectorForStaleDisconnect(first, second);
        using var registration = connector.OnErrorReceived(
            (error, _) =>
            {
                if (error.Code == ZlinkStreamErrorCode.FrameDecodeFailed)
                {
                    callbackEntered.SetResult();
                    releaseCallback.Task.GetAwaiter().GetResult();
                }
                return ValueTask.CompletedTask;
            }
        );

        await AssertReconnectDuringEndingAsync(
            connector,
            first,
            second,
            callbackEntered.Task,
            () => releaseCallback.TrySetResult(),
            replyBeforeEnding
        );
    }

    [Theory]
    [InlineData(true, false)]
    [InlineData(false, false)]
    [InlineData(false, true)]
    public async Task StaleDisconnectDuringTransportCloseDoesNotFailNewConnectionRequest(
        bool replyBeforeEnding,
        bool retryAfterAdmissionRejection
    )
    {
        var first = new ScriptedConnection();
        var second = new ScriptedConnection();
        var slowClose = new SlowCloseConnection(first);
        var connector = CreateConnectorForStaleDisconnect(slowClose, second);

        await AssertReconnectDuringEndingAsync(
            connector,
            first,
            second,
            slowClose.CloseEntered.Task,
            () => slowClose.ReleaseClose.TrySetResult(),
            replyBeforeEnding,
            retryAfterAdmissionRejection
        );
    }

    private static async Task AssertReconnectDuringEndingAsync(
        ZlinkStreamConnector connector,
        ScriptedConnection first,
        ScriptedConnection second,
        Task endingHeld,
        Action releaseEnding,
        bool replyBeforeEnding,
        bool retryAfterAdmissionRejection = false
    )
    {
        var disconnected = new TaskCompletionSource<ZlinkStreamCloseReason>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var registration = connector.OnDisconnected(
            (ending, _) =>
            {
                disconnected.TrySetResult(ending.CloseReason);
                return ValueTask.CompletedTask;
            }
        );
        try
        {
            await connector.Connect.Async();
            var oldRequest = connector
                .Request(EndingPayload())
                .PacketName("old.connection")
                .Async()
                .AsTask();
            await first.WaitForWritesAsync(1);
            var codec = new ZlinkStreamHeaderCodec();
            if (retryAfterAdmissionRejection)
                await RetryAfterAdmissionRejectionAsync(first);

            first.Deliver(ZlinkStreamFrameCodec.Encode([0xFF, 0xFF, 0xFF, 0xFF], []));
            await endingHeld.WaitAsync(EndingWait);
            Assert.Equal(ZlinkStreamConnectionState.Disconnected, connector.State);

            await connector.Connect.Async().AsTask().WaitAsync(EndingWait);
            var newRequest = connector
                .Request(EndingPayload())
                .PacketName("new.connection")
                .Async()
                .AsTask();
            await second.WaitForWritesAsync(1);

            if (retryAfterAdmissionRejection)
                await RetryAfterAdmissionRejectionAsync(second);
            var sent = codec.Decode(
                (
                    retryAfterAdmissionRejection
                        ? second.Written.Last()
                        : Assert.Single(second.Written)
                ).Header
            );
            var reply = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                sent.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty
            );
            var replyBytes = ZlinkStreamFrameCodec.Encode(codec.Encode(reply).Span, [7]);
            if (replyBeforeEnding)
            {
                second.Deliver(replyBytes);
                Assert.Equal(
                    new byte[] { 7 },
                    (await newRequest.WaitAsync(EndingWait)).Payload.ToArray()
                );
            }

            releaseEnding();
            Assert.Equal(
                ZlinkStreamCloseReason.ProtocolError,
                await disconnected.Task.WaitAsync(EndingWait)
            );
            var oldFailure = await Assert.ThrowsAsync<ZlinkStreamException>(() =>
                oldRequest.WaitAsync(EndingWait)
            );
            Assert.Equal(ZlinkStreamErrorCode.Disconnected, oldFailure.Error.Code);

            if (!replyBeforeEnding)
            {
                second.Deliver(replyBytes);
                Assert.Equal(
                    new byte[] { 7 },
                    (await newRequest.WaitAsync(EndingWait)).Payload.ToArray()
                );
            }
            Assert.Equal(ZlinkStreamConnectionState.Connected, connector.State);
        }
        finally
        {
            releaseEnding();
            await connector.DisposeAsync().AsTask().WaitAsync(EndingWait);
        }

        static async Task RetryAfterAdmissionRejectionAsync(ScriptedConnection connection)
        {
            var codec = new ZlinkStreamHeaderCodec();
            var firstAttempt = codec.Decode(Assert.Single(connection.Written).Header);
            var rejection = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Error,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                firstAttempt.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty.With("$zlink.admission.retryAfterMs", "0")
            );
            connection.Deliver(
                ZlinkStreamFrameCodec.Encode(
                    codec.Encode(rejection).Span,
                    """{"code":"unavailable","message":"Authentication admission is full."}"""u8
                )
            );
            await connection.WaitForWritesAsync(1);
            Assert.Equal(2, connection.Written.Count);
            var retry = codec.Decode(connection.Written.Last().Header);
            Assert.NotEqual(firstAttempt.RequestSeq, retry.RequestSeq);
            Assert.Equal(firstAttempt.CorrelationId, retry.CorrelationId);
        }
    }

    private static ZlinkStreamConnector CreateConnectorForStaleDisconnect(
        IZlinkStreamConnection first,
        IZlinkStreamConnection second
    )
    {
        var connectCount = 0;
        return new ZlinkStreamConnector(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
            },
            _ =>
                ValueTask.FromResult(
                    Interlocked.Increment(ref connectCount) switch
                    {
                        1 => first,
                        2 => second,
                        _ => throw new InvalidOperationException("Unexpected transport open."),
                    }
                )
        );
    }

    private sealed class SlowCloseConnection(ScriptedConnection inner) : IZlinkStreamConnection
    {
        public TaskCompletionSource CloseEntered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource ReleaseClose { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken) =>
            inner.ReadAsync(buffer, cancellationToken);

        public ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken
        ) => inner.WriteAsync(buffer, cancellationToken);

        public async ValueTask CloseAsync(CancellationToken cancellationToken)
        {
            CloseEntered.SetResult();
            await ReleaseClose.Task;
            await inner.CloseAsync(cancellationToken);
        }
    }
}
