using System.Net;
using System.Net.Sockets;
using Systems.Zlink.Stream.Connector.Contracts;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public async Task ExpectNonePassesOnlyWhenTheWindowHasNoNamedPush()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var sendUnexpected = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await sendUnexpected.Task;
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "Notice",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                "unexpected"u8.ToArray());
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        await connector.Connect.Async();

        await connector.ExpectNone("Notice")
            .Within(TimeSpan.FromMilliseconds(25))
            .Async();

        var unexpected = connector.ExpectNone("Notice")
            .Within(TimeSpan.FromSeconds(1))
            .Async().AsTask();
        sendUnexpected.SetResult();

        await Assert.ThrowsAsync<InvalidOperationException>(() => unexpected);
        await server;
    }

    [Fact]
    public async Task WaitForSequenceChecksEachPushAgainstTheNextExpectation()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var sendOutOfOrder = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            foreach (var value in new[] { "first", "second" })
                await WritePacketAsync(
                    stream,
                    headerCodec.Encode(new ZlinkStreamHeader(
                        ZlinkStreamMessageKind.Send,
                        ZlinkStreamCodec.Raw,
                        ZlinkStreamHeaderFlags.None,
                        null,
                        "Notice",
                        ZlinkStreamMetadata.Empty)).ToArray(),
                    System.Text.Encoding.UTF8.GetBytes(value));

            await sendOutOfOrder.Task;
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "Notice",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                "second"u8.ToArray());
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        await connector.Connect.Async();

        var messages = await connector.WaitForSequence("Notice")
            .Expect(message => Text(message) == "first")
            .Expect(message => Text(message) == "second")
            .Timeout(TimeSpan.FromSeconds(1))
            .Async();
        Assert.Equal(new[] { "first", "second" }, messages.Select(Text));

        var outOfOrder = connector.WaitForSequence("Notice")
            .Expect(message => Text(message) == "first")
            .Expect(message => Text(message) == "second")
            .Timeout(TimeSpan.FromSeconds(1))
            .Async().AsTask();
        sendOutOfOrder.SetResult();

        await Assert.ThrowsAsync<InvalidOperationException>(() => outOfOrder);
        await server;
    }

    [Fact]
    public async Task StreamAssertionsExecuteActionsAndPreserveFailureMeaning()
    {
        ZlinkStreamAssert.Ensure(true, "condition should pass");
        Assert.Throws<InvalidOperationException>(
            () => ZlinkStreamAssert.Ensure(false, "condition failed"));
        Assert.Throws<ArgumentException>(() => ZlinkStreamAssert.Ensure(true, ""));

        var invoked = false;
        var failure = await ZlinkStreamAssert.ExpectFailureAsync(
            _ =>
            {
                invoked = true;
                throw new ZlinkStreamException(new ZlinkStreamError(
                    ZlinkStreamErrorCode.RequestTimeout,
                    "request timed out"));
            },
            nameof(ZlinkStreamErrorCode.RequestTimeout));
        Assert.True(invoked);
        Assert.Equal(ZlinkStreamErrorCode.RequestTimeout, failure.Code);
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            ZlinkStreamAssert.ExpectFailureAsync(
                _ => ValueTask.FromException(new TimeoutException("request timed out")),
                nameof(ZlinkStreamErrorCode.ConnectTimeout)).AsTask());
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            ZlinkStreamAssert.ExpectFailureAsync(_ => ValueTask.CompletedTask).AsTask());
        var programmingFailure = new InvalidOperationException("programming failure");
        var propagatedFailure = await Assert.ThrowsAsync<InvalidOperationException>(() =>
            ZlinkStreamAssert.ExpectFailureAsync(
                _ => ValueTask.FromException(programmingFailure),
                nameof(ZlinkStreamErrorCode.RemoteError)).AsTask());
        Assert.Same(programmingFailure, propagatedFailure);

        var wrappedTransportFailure = new InvalidOperationException(
            "request failed",
            new HttpRequestException("connection refused"));
        var disconnected = await ZlinkStreamAssert.ExpectFailureAsync(
            _ => ValueTask.FromException(wrappedTransportFailure),
            nameof(ZlinkStreamErrorCode.Disconnected));
        Assert.Equal(ZlinkStreamErrorCode.Disconnected, disconnected.Code);
        Assert.Same(wrappedTransportFailure, disconnected.Exception);

        await ZlinkStreamAssert.ExpectTimeoutAsync(
            _ => ValueTask.FromException(new TimeoutException("wait timed out")));
        await ZlinkStreamAssert.ExpectTimeoutAsync(
            _ => ValueTask.FromException(
                new InvalidOperationException(
                    "HTTP request exceeded timeout",
                    new TimeoutException("HTTP request exceeded timeout"))));
        using var callerCanceled = new CancellationTokenSource();
        callerCanceled.Cancel();
        var cancellation = new OperationCanceledException(callerCanceled.Token);
        var propagatedCancellation = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            ZlinkStreamAssert.ExpectTimeoutAsync(
                _ => ValueTask.FromException(cancellation)).AsTask());
        Assert.Same(cancellation, propagatedCancellation);
        var nonTimeout = new InvalidOperationException("not a timeout");
        var propagated = await Assert.ThrowsAsync<InvalidOperationException>(() =>
            ZlinkStreamAssert.ExpectTimeoutAsync(
                _ => ValueTask.FromException(nonTimeout)).AsTask());
        Assert.Same(nonTimeout, propagated);
    }

    [Fact]
    public async Task WaitForPreservesCallerCancellationBeforeAndDuringTheWait()
    {
        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://127.0.0.1:1"),
            Heartbeat = DisabledHeartbeat(),
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });

        using var alreadyCanceled = new CancellationTokenSource();
        alreadyCanceled.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            connector.WaitFor("never.pre-canceled")
                .Timeout(TimeSpan.FromSeconds(30))
                .Async(alreadyCanceled.Token).AsTask());

        using var canceledWhileWaiting = new CancellationTokenSource();
        var pending = connector.WaitFor("never.waiting")
            .Timeout(TimeSpan.FromSeconds(30))
            .Async(canceledWhileWaiting.Token).AsTask();
        Assert.False(pending.IsCompleted);
        await canceledWhileWaiting.CancelAsync();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => pending);
    }

    private static string Text(ZlinkStreamMessage<ZlinkStreamEncodedPayload> message)
    {
        return System.Text.Encoding.UTF8.GetString(message.Payload.Payload.Span);
    }
}
