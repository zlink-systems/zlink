using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task R3TerminalRequestDoesNotWriteItsQueuedFrame(bool cancel)
    {
        var connection = new TransportCloseEndsWriteConnection();
        var connector = CreateConnectorOn(connection, ZlinkStreamDispatchMode.Manual);
        using var cancellation = new CancellationTokenSource();
        try
        {
            await connector.Connect.Async();
            var first = connector
                .Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                .PacketName("first")
                .Async()
                .AsTask();
            await connection.WriteStarted.Task.WaitAsync(EndingWait);
            var request = connector
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 2 }))
                .PacketName("terminal")
                .Timeout(cancel ? EndingWait : TimeSpan.FromMilliseconds(1))
                .Async(cancellation.Token)
                .AsTask();
            if (cancel)
            {
                cancellation.Cancel();
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() => request);
            }
            else
            {
                var error = await Assert.ThrowsAsync<ZlinkStreamException>(() => request);
                Assert.Equal(ZlinkStreamErrorCode.RequestTimeout, error.Error.Code);
            }
            var last = connector
                .Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 3 }))
                .PacketName("last")
                .Async()
                .AsTask();
            connection.ReleaseWrite.TrySetResult();
            await first.WaitAsync(EndingWait);
            await last.WaitAsync(EndingWait);
            Assert.Equal(2, connection.WriteCount);
        }
        finally
        {
            connection.ReleaseWrite.TrySetResult();
            await connector.DisposeAsync();
        }
    }

    [Fact]
    public void R3WhitespaceNameIsRejectedByStructureOwner()
    {
        Assert.Throws<ZlinkStreamException>(() => ZlinkStreamConnector.ValidateName(" \t"));
    }

    [Fact]
    public async Task R3ActionFailurePreservesIdentity()
    {
        var failure = new IOException("action failure");
        var observed = await Assert.ThrowsAsync<IOException>(async () =>
            await ZlinkStreamAssert.ExpectFailureAsync(_ => ValueTask.FromException(failure))
        );
        Assert.Same(failure, observed);
    }

    [Fact]
    public async Task R3NonTimeoutWrapperIsNotClassifiedFromItsCause()
    {
        var failure = new InvalidOperationException("action wrapper", new TimeoutException());
        foreach (
            var action in new Func<Func<CancellationToken, ValueTask>, ValueTask>[]
            {
                async run =>
                {
                    await ZlinkStreamAssert.ExpectFailureAsync(run);
                },
                ZlinkStreamAssert.ExpectTimeoutAsync,
            }
        )
        {
            var observed = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
                await action(_ => ValueTask.FromException(failure))
            );
            Assert.Same(failure, observed);
        }
    }

    [Fact]
    public async Task R3CloseFailureIsDisconnectedEvent()
    {
        var failure = new InvalidOperationException("transport close failure");
        var connection = new FaultingCloseConnection(failure);
        var connector = new ZlinkStreamConnector(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            },
            _ => ValueTask.FromResult<IZlinkStreamConnection>(connection)
        );
        var errorReceived = new TaskCompletionSource<ZlinkStreamError>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var errorCount = 0;
        ZlinkStreamConnectionState? errorState = null;
        ZlinkStreamCloseReason? errorReason = null;
        connector.OnErrorReceived(
            (error, _) =>
            {
                errorState = connector.State;
                errorReason = connector.CloseReason;
                Interlocked.Increment(ref errorCount);
                errorReceived.TrySetResult(error);
                return ValueTask.CompletedTask;
            }
        );
        await connector.Connect.Async();
        await connector.Close.Async();
        var error = await errorReceived.Task.WaitAsync(EndingWait);
        Assert.Equal(ZlinkStreamErrorCode.Disconnected, error.Code);
        Assert.Same(failure, error.Exception);
        Assert.Equal(ZlinkStreamConnectionState.Closed, errorState);
        Assert.Equal(ZlinkStreamCloseReason.ClientClose, errorReason);
        Assert.Equal(ZlinkStreamCloseReason.ClientClose, connector.CloseReason);
        await connector.DisposeAsync();
        Assert.Equal(1, connection.CloseCount);
        Assert.Equal(1, errorCount);
    }
}
