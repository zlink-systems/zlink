using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using System.Threading.Channels;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Framing;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public async Task RequestsWithoutAdmissionReuseFrameBuffers()
    {
        const int requestCount = 1000;
        const int payloadSize = 32 * 1024;
        var payload = new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[payloadSize]);
        var clock = new AdmissionTestClock([]);
        var peer = new AdmissionTestConnection(clock);
        await using var connector = new ZlinkStreamConnector(
            AdmissionOptions(5000, null),
            _ => new ValueTask<IZlinkStreamConnection>(peer),
            clock.Clock
        );
        await connector.Connect.Async();

        await CompleteRequestAsync(connector.Request(payload).PacketName("echo").Async());
        long allocated = 0;
        for (var index = 0; index < requestCount; index++)
        {
            var request = connector.Request(payload).PacketName("echo");
            var before = GC.GetAllocatedBytesForCurrentThread();
            var pending = request.Async();
            allocated += GC.GetAllocatedBytesForCurrentThread() - before;
            await CompleteRequestAsync(pending);
        }

        // Allow request bookkeeping and pool misses while detecting a frame allocation per call.
        Assert.True(
            allocated < requestCount * payloadSize / 4,
            $"Submitting {requestCount} requests allocated {allocated:N0} bytes."
        );

        async Task CompleteRequestAsync(ValueTask<ZlinkStreamEncodedPayload> pending)
        {
            var write = await peer.NextWriteAsync();
            Assert.Equal(payloadSize, write.Payload.Length);
            peer.Reply(write.Header.RequestSeq!.Value);
            Assert.Equal(42, (await pending).Payload.Span[0]);
        }
    }

    [Fact]
    public async Task AdmissionRetryUsesTheSameConnectionAndLogicalRequest()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            }
        );
        await connector.Connect.Async();
        using var peer = await listener.AcceptTcpClientAsync();
        await using var stream = peer.GetStream();
        var codec = new ZlinkStreamHeaderCodec();
        byte[] callerPayload = [7];
        var result = connector
            .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, callerPayload))
            .PacketName("Login")
            .Async()
            .AsTask();
        var first = await ReadPacketAsync(stream);
        callerPayload[0] = 9;
        var header = codec.Decode(first.Header);
        var rejection = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Error,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.HasRequestSeq,
            header.RequestSeq,
            string.Empty,
            ZlinkStreamMetadata.Empty.With("$zlink.admission.retryAfterMs", "0")
        );
        await WritePacketAsync(
            stream,
            codec.Encode(rejection).ToArray(),
            """{"code":"unavailable","message":"Authentication admission is full."}"""u8.ToArray()
        );
        var next = ReadPacketAsync(stream);
        var winner = await Task.WhenAny(result, next).WaitAsync(TimeSpan.FromSeconds(5));
        if (winner == result)
            await result;
        var retry = await next.WaitAsync(TimeSpan.FromSeconds(5));
        var retryHeader = codec.Decode(retry.Header);
        Assert.NotEqual(header.RequestSeq, retryHeader.RequestSeq);
        Assert.Equal(header.Name, retryHeader.Name);
        Assert.Equal(header.CorrelationId, retryHeader.CorrelationId);
        Assert.Equal(first.Payload, retry.Payload);
        await WritePacketAsync(
            stream,
            codec
                .Encode(
                    rejection with
                    {
                        Kind = ZlinkStreamMessageKind.Response,
                        Codec = ZlinkStreamCodec.Raw,
                        RequestSeq = retryHeader.RequestSeq,
                        Metadata = ZlinkStreamMetadata.Empty,
                    }
                )
                .ToArray(),
            [8]
        );
        Assert.Equal((byte)8, (await result.WaitAsync(TimeSpan.FromSeconds(5))).Payload.Span[0]);
    }

    public static IEnumerable<object[]> AdmissionConnectorScenarios()
    {
        using var fixture = JsonDocument.Parse(
            File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "login-admission-v1.json"))
        );
        // v1 lists stage 1 only. Queue, position and ticket scenarios belong to stage 2.
        foreach (
            var scenario in fixture.RootElement.GetProperty("connectorScenarios").EnumerateArray()
        )
            yield return [scenario.GetProperty("name").GetString()!, scenario.GetRawText()];
    }

    [Theory]
    [MemberData(nameof(AdmissionConnectorScenarios))]
    public async Task AdmissionConnectorFixture(string name, string json)
    {
        using var scenario = JsonDocument.Parse(json);
        var given = scenario.RootElement.GetProperty("given");
        var expected = scenario.RootElement.GetProperty("expect");
        var responses = given.GetProperty("responses").EnumerateArray().ToArray();
        var clock = new AdmissionTestClock(
            responses
                .Where(response => response.TryGetProperty("jitterMs", out _))
                .Select(response => response.GetProperty("jitterMs").GetInt32())
        );
        var maxWait = given.GetProperty("maxAdmissionWaitMs");
        var peer = new AdmissionTestConnection(clock);
        await using var connector = new ZlinkStreamConnector(
            AdmissionOptions(
                given.GetProperty("requestTimeoutMs").GetInt32(),
                maxWait.ValueKind == JsonValueKind.Null ? null : maxWait.GetInt32()
            ),
            _ => new ValueTask<IZlinkStreamConnection>(peer),
            clock.Clock
        );
        var sendingHooks = 0;
        var replyHooks = 0;
        var callbacks = 0;
        var waiting = new List<ZlinkStreamAdmissionWaiting>();
        connector.OnRequestSending(_ => Interlocked.Increment(ref sendingHooks));
        connector.OnReplyReceived(
            (_, _) =>
            {
                Interlocked.Increment(ref replyHooks);
                return ValueTask.CompletedTask;
            }
        );
        connector.OnAdmissionWaiting(
            (notification, _) =>
            {
                waiting.Add(notification);
                return ValueTask.CompletedTask;
            }
        );
        await connector.Connect.Async();

        var payload = Convert.FromBase64String(given.GetProperty("payloadBase64").GetString()!);
        var packetName = given.GetProperty("packetName").GetString()!;
        var call = connector
            .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, payload))
            .PacketName(packetName);
        foreach (var metadata in given.GetProperty("applicationMetadata").EnumerateObject())
            call.Metadata(metadata.Name, metadata.Value.GetString()!);

        var completed = new TaskCompletionSource<ZlinkStreamResult<ZlinkStreamEncodedPayload>>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        call.Submit(result =>
        {
            Interlocked.Increment(ref callbacks);
            completed.TrySetResult(result);
        });

        var sendTimes = expected
            .GetProperty("sendAtElapsedMs")
            .EnumerateArray()
            .Select(value => value.GetInt32())
            .ToArray();
        var sequences = expected
            .GetProperty("requestSeqs")
            .EnumerateArray()
            .Select(value => ulong.Parse(value.GetString()!))
            .ToArray();
        var first = await peer.NextWriteAsync();
        Assert.False(string.IsNullOrEmpty(first.Header.CorrelationId), name);
        // The fixture's symbolic correlation maps to the first ID generated by the connector.
        AssertAttempt(first, 0);
        var attempt = 1;

        foreach (var response in responses)
        {
            clock.AdvanceTo(response.GetProperty("atElapsedMs").GetInt32());
            var sequence = new ZlinkStreamRequestSeq(
                ulong.Parse(response.GetProperty("requestSeq").GetString()!)
            );
            if (response.GetProperty("kind").GetString() == "Response")
            {
                peer.Reply(sequence);
                continue;
            }

            var code = response.GetProperty("remoteCode").GetString()!;
            var hint = response.GetProperty("retryAfterMs").GetString();
            peer.Reject(sequence, code, hint);
            if (
                attempt >= sendTimes.Length
                && !expected.TryGetProperty("terminalAtElapsedMs", out _)
            )
                continue;

            var wake =
                attempt < sendTimes.Length
                    ? sendTimes[attempt]
                    : expected.GetProperty("terminalAtElapsedMs").GetInt32();
            Assert.Equal(TimeSpan.FromMilliseconds(wake), await clock.NextTimerAsync());
            clock.AdvanceTo(wake - 1);
            Assert.False(peer.HasWrite);
            Assert.False(completed.Task.IsCompleted);
            clock.AdvanceTo(wake);

            if (attempt < sendTimes.Length)
            {
                AssertAttempt(await peer.NextWriteAsync(), attempt);
                attempt++;
            }
        }

        var result = await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(
            expected.GetProperty("result").GetString(),
            result.Error?.Code.ToString() ?? "success"
        );
        Assert.Equal(expected.GetProperty("waitingCallbacks").GetInt32(), waiting.Count);
        Assert.Equal(expected.GetProperty("requestSendingHooks").GetInt32(), sendingHooks);
        Assert.Equal(expected.GetProperty("replyReceivedHooks").GetInt32(), replyHooks);
        Assert.Equal(expected.GetProperty("requestCallbacks").GetInt32(), callbacks);
        Assert.All(
            waiting,
            notification => Assert.Equal(packetName, notification.RequestPacketName)
        );
        Assert.Equal(sendTimes.Length, attempt);
        Assert.False(peer.HasWrite);
        Assert.True(connector.IsConnected);

        if (result.Error?.Code == ZlinkStreamErrorCode.FrameDecodeFailed)
        {
            var next = connector
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, payload))
                .PacketName("FollowUp")
                .Async()
                .AsTask();
            peer.Reply((await peer.NextWriteAsync()).Header.RequestSeq!.Value);
            Assert.Equal((byte)42, (await next.WaitAsync(TimeSpan.FromSeconds(5))).Payload.Span[0]);
        }

        void AssertAttempt(AdmissionWrite write, int index)
        {
            Assert.Equal(TimeSpan.FromMilliseconds(sendTimes[index]), write.At);
            Assert.Equal(sequences[index], write.Header.RequestSeq!.Value.Value);
            Assert.Equal(packetName, write.Header.Name);
            Assert.Equal(first.Header.CorrelationId, write.Header.CorrelationId);
            Assert.Equal(payload, write.Payload);
            foreach (var metadata in given.GetProperty("applicationMetadata").EnumerateObject())
                Assert.Equal(metadata.Value.GetString(), write.Header.Metadata.Get(metadata.Name));
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task AdmissionWaitingEndsWithoutRetryOnCancellationOrDisconnect(bool disconnect)
    {
        var clock = new AdmissionTestClock([50]);
        var peer = new AdmissionTestConnection(clock);
        await using var connector = new ZlinkStreamConnector(
            AdmissionOptions(3000, null),
            _ => new ValueTask<IZlinkStreamConnection>(peer),
            clock.Clock
        );
        var waitingCount = 0;
        var replyHooks = 0;
        connector.OnAdmissionWaiting(
            (notification, _) =>
            {
                Assert.Equal(TimeSpan.FromMilliseconds(int.MaxValue), notification.RetryAfter);
                Interlocked.Increment(ref waitingCount);
                return ValueTask.CompletedTask;
            }
        );
        connector.OnReplyReceived(
            (_, _) =>
            {
                Interlocked.Increment(ref replyHooks);
                return ValueTask.CompletedTask;
            }
        );
        await connector.Connect.Async();
        using var caller = new CancellationTokenSource();
        var result = connector
            .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
            .PacketName("Login")
            .Async(caller.Token)
            .AsTask();
        var first = await peer.NextWriteAsync();
        clock.AdvanceTo(10);
        peer.Reject(first.Header.RequestSeq!.Value, "unavailable", "2147483647");
        Assert.Equal(TimeSpan.FromMilliseconds(3000), await clock.NextTimerAsync());
        if (disconnect)
        {
            peer.EndReads();
            var failure = await Assert.ThrowsAsync<ZlinkStreamException>(() =>
                result.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(ZlinkStreamErrorCode.Disconnected, failure.Error.Code);
        }
        else
        {
            caller.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                result.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.True(connector.IsConnected);
        }
        clock.AdvanceTo(3000);
        Assert.False(peer.HasWrite);
        Assert.Equal(1, waitingCount);
        Assert.Equal(disconnect ? 1 : 0, replyHooks);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(-1)]
    public void AdmissionWaitMustBePositive(int milliseconds)
    {
        var failure = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamConnectorFactory.Create(AdmissionOptions(3000, milliseconds))
        );
        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, failure.Error.Code);
    }

    private static ZlinkStreamConnectorOptions AdmissionOptions(int timeout, int? admissionWait) =>
        new()
        {
            Endpoint = new Uri("tcp://127.0.0.1:1"),
            Heartbeat = DisabledHeartbeat(),
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            RequestTimeout = TimeSpan.FromMilliseconds(timeout),
            MaxAdmissionWait = admissionWait is { } wait ? TimeSpan.FromMilliseconds(wait) : null,
        };

    private sealed class AdmissionTestClock(IEnumerable<int> jitter)
    {
        private readonly Queue<int> _jitter = new(jitter);
        private readonly Channel<TimeSpan> _scheduled = Channel.CreateUnbounded<TimeSpan>();
        private readonly List<(TimeSpan Due, TaskCompletionSource Signal)> _timers = [];
        private long _ticks;

        public TimeSpan Now => TimeSpan.FromTicks(Interlocked.Read(ref _ticks));

        public ZlinkStreamRequestClock Clock => new(() => Now, DelayAsync, () => _jitter.Dequeue());

        public Task<TimeSpan> NextTimerAsync() =>
            _scheduled.Reader.ReadAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));

        public void AdvanceTo(int milliseconds)
        {
            lock (_timers)
            {
                Interlocked.Exchange(ref _ticks, TimeSpan.FromMilliseconds(milliseconds).Ticks);
                foreach (var timer in _timers.Where(timer => timer.Due <= Now))
                    timer.Signal.TrySetResult();
                _timers.RemoveAll(timer => timer.Due <= Now);
            }
        }

        private Task DelayAsync(TimeSpan duration, CancellationToken token)
        {
            lock (_timers)
            {
                var due = Now + duration;
                var signal = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                _timers.Add((due, signal));
                _scheduled.Writer.TryWrite(due);
                return signal.Task.WaitAsync(token);
            }
        }
    }

    private sealed record AdmissionWrite(TimeSpan At, ZlinkStreamHeader Header, byte[] Payload);

    private sealed class AdmissionTestConnection(AdmissionTestClock clock) : IZlinkStreamConnection
    {
        private readonly ZlinkStreamHeaderCodec _codec = new();
        private readonly Channel<byte[]> _reads = Channel.CreateUnbounded<byte[]>();
        private readonly Channel<AdmissionWrite> _writes =
            Channel.CreateUnbounded<AdmissionWrite>();
        private ReadOnlyMemory<byte> _remaining;

        public bool HasWrite => _writes.Reader.TryPeek(out _);

        public Task<AdmissionWrite> NextWriteAsync() =>
            _writes.Reader.ReadAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));

        public void EndReads() => _reads.Writer.TryComplete();

        public void Reject(ZlinkStreamRequestSeq sequence, string code, string? delay)
        {
            var metadata = delay is null
                ? ZlinkStreamMetadata.Empty
                : ZlinkStreamMetadata.Empty.With("$zlink.admission.retryAfterMs", delay);
            Deliver(
                new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Error,
                    ZlinkStreamCodec.Json,
                    ZlinkStreamHeaderFlags.HasRequestSeq,
                    sequence,
                    "",
                    metadata
                ),
                JsonSerializer.SerializeToUtf8Bytes(new { code, message = "rejected" })
            );
        }

        public void Reply(ZlinkStreamRequestSeq sequence) =>
            Deliver(
                new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Response,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.HasRequestSeq,
                    sequence,
                    "",
                    ZlinkStreamMetadata.Empty
                ),
                [42]
            );

        private void Deliver(ZlinkStreamHeader header, byte[] payload) =>
            Assert.True(
                _reads.Writer.TryWrite(
                    ZlinkStreamFrameCodec.Encode(_codec.Encode(header).Span, payload)
                )
            );

        public async ValueTask<int> ReadAsync(
            Memory<byte> buffer,
            CancellationToken cancellationToken
        )
        {
            if (_remaining.IsEmpty)
            {
                if (!await _reads.Reader.WaitToReadAsync(cancellationToken))
                    return 0;
                _remaining = await _reads.Reader.ReadAsync(cancellationToken);
            }
            var count = Math.Min(buffer.Length, _remaining.Length);
            _remaining[..count].CopyTo(buffer);
            _remaining = _remaining[count..];
            return count;
        }

        public ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken
        )
        {
            Assert.True(
                ZlinkStreamFrameCodec.TryDecode(buffer.Span, out var header, out var payload)
            );
            Assert.True(
                _writes.Writer.TryWrite(
                    new AdmissionWrite(
                        clock.Now,
                        _codec.Decode(header.ToArray()),
                        payload.ToArray()
                    )
                )
            );
            return ValueTask.CompletedTask;
        }

        public ValueTask CloseAsync(CancellationToken cancellationToken)
        {
            EndReads();
            return ValueTask.CompletedTask;
        }
    }
}
