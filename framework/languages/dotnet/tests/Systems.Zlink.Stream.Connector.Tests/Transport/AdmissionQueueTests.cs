using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Xunit;

public sealed partial class StreamConnectorTests
{
    private const string AdmissionQueueTestTicket =
        "AQAAAYvP5d0wAAAAAAAAACpkd-u3yih1dIog4S6zkov_tIe5kI5BZpnfdXmWl3x7kQ";

    [Fact]
    public async Task AdmissionQueueRequirementSendsAnInternalEnqueue()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{port}"),
                Heartbeat = DisabledHeartbeat(),
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                RequestTimeout = TimeSpan.FromSeconds(5),
            }
        );
        await connector.Connect.Async();
        using var peer = await listener.AcceptTcpClientAsync();
        await using var stream = peer.GetStream();
        var codec = new ZlinkStreamHeaderCodec();
        var request = connector
            .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 7 }))
            .PacketName("Login")
            .Async()
            .AsTask();
        var first = codec.Decode((await ReadPacketAsync(stream)).Header);
        await WritePacketAsync(
            stream,
            codec
                .Encode(
                    new ZlinkStreamHeader(
                        ZlinkStreamMessageKind.Error,
                        ZlinkStreamCodec.Json,
                        ZlinkStreamHeaderFlags.HasRequestSeq,
                        first.RequestSeq,
                        string.Empty,
                        ZlinkStreamMetadata.Empty.With("$zlink.admission.queueRequired", "1")
                    )
                )
                .ToArray(),
            """{"code":"unavailable","message":"queue required"}"""u8.ToArray()
        );
        var enqueue = ReadPacketAsync(stream);
        if (await Task.WhenAny(enqueue, request).WaitAsync(TimeSpan.FromSeconds(5)) == request)
            await request;
        var packet = await enqueue.WaitAsync(TimeSpan.FromSeconds(5));
        var registration = codec.Decode(packet.Header);
        Assert.Equal("$zlink.admission.enqueue", registration.Name);
        Assert.Equal("{}"u8.ToArray(), packet.Payload);

        await WritePacketAsync(
            stream,
            codec
                .Encode(
                    new ZlinkStreamHeader(
                        ZlinkStreamMessageKind.Response,
                        ZlinkStreamCodec.Json,
                        ZlinkStreamHeaderFlags.HasRequestSeq,
                        registration.RequestSeq,
                        string.Empty,
                        ZlinkStreamMetadata.Empty
                    )
                )
                .ToArray(),
            JsonSerializer.SerializeToUtf8Bytes(
                new { ticket = AdmissionQueueTestTicket, expiresInMs = 30000 }
            )
        );
        var retry = await ReadPacketAsync(stream).WaitAsync(TimeSpan.FromSeconds(5));
        var retried = codec.Decode(retry.Header);
        Assert.Equal(first.Name, retried.Name);
        Assert.Equal(first.CorrelationId, retried.CorrelationId);
        Assert.NotEqual(first.RequestSeq, retried.RequestSeq);
        Assert.Equal(AdmissionQueueTestTicket, retried.Metadata.Get("$zlink.admission.ticket"));
        Assert.Equal(new byte[] { 7 }, retry.Payload);
        await WritePacketAsync(
            stream,
            codec
                .Encode(
                    new ZlinkStreamHeader(
                        ZlinkStreamMessageKind.Response,
                        ZlinkStreamCodec.Raw,
                        ZlinkStreamHeaderFlags.HasRequestSeq,
                        retried.RequestSeq,
                        string.Empty,
                        ZlinkStreamMetadata.Empty
                    )
                )
                .ToArray(),
            [42]
        );
        Assert.Equal(
            new byte[] { 42 },
            (await request.WaitAsync(TimeSpan.FromSeconds(5))).Payload.ToArray()
        );
    }

    public static IEnumerable<object[]> AdmissionQueueConnectorScenarios()
    {
        using var fixture = JsonDocument.Parse(
            File.ReadAllText(
                Path.Combine(AppContext.BaseDirectory, "login-admission-queue-v1.json")
            )
        );
        foreach (
            var scenario in fixture.RootElement.GetProperty("connectorScenarios").EnumerateArray()
        )
            yield return [scenario.GetProperty("name").GetString()!, scenario.GetRawText()];
    }

    [Theory]
    [MemberData(nameof(AdmissionQueueConnectorScenarios))]
    public async Task AdmissionQueueConnectorFixture(string name, string json)
    {
        using var scenario = JsonDocument.Parse(json);
        var given = scenario.RootElement.GetProperty("given");
        var firstRejection = ReadNumber(given, "firstRejectionMs", 0);
        var timeout = ReadNumber(given, "originalDeadlineMs", 60000);
        int? maxWait = given.TryGetProperty("maxAdmissionWaitMs", out var maximum)
            ? maximum.GetInt32()
            : null;
        var clock = new AdmissionTestClock([0]);
        var peer = new ScriptedConnection();
        await using var connector = new ZlinkStreamConnector(
            AdmissionOptions(timeout, maxWait),
            _ => new ValueTask<IZlinkStreamConnection>(peer),
            clock.Clock
        );

        var callbacks = new ConcurrentQueue<string>();
        var applicationErrors = new ConcurrentQueue<ZlinkStreamError>();
        using var errors = connector.OnErrorReceived(
            (error, _) =>
            {
                applicationErrors.Enqueue(error);
                return ValueTask.CompletedTask;
            }
        );
        var actualPositions = new ConcurrentQueue<ZlinkStreamAdmissionPosition>();
        using var queued = connector.OnAdmissionQueued(_ => Record("OnAdmissionQueued"));
        using var position = connector.OnAdmissionPosition(
            (value, _) =>
            {
                actualPositions.Enqueue(value);
                return Record("OnAdmissionPosition");
            }
        );
        using var waiting = connector.OnAdmissionWaiting((_, _) => Record("OnAdmissionWaiting"));
        await connector.Connect.Async();
        var codec = new ZlinkStreamHeaderCodec();
        var requests = new Dictionary<string, Task<ZlinkStreamEncodedPayload>>();
        var attempts = new Dictionary<string, ZlinkStreamHeader>();
        var originals = new Dictionary<string, ZlinkStreamHeader>();
        var queuedRequests = new HashSet<string>();
        var givenRequests = given.TryGetProperty("requests", out var array)
            ? array.EnumerateArray().Select(value => value.GetString()!).ToArray()
            : ["a"];
        var consumedWrites = 0;
        var callbackCount = 0;
        ZlinkStreamHeader? enqueue = null;
        const string enqueueName = "$zlink.admission.enqueue";
        const string positionName = "$zlink.admission.position";
        const string ticketName = "$zlink.admission.ticket";

        var initialEnqueues = 0;
        if (given.TryGetProperty("cachedTicket", out var initialTicket))
        {
            Assert.Equal("old", initialTicket.GetString());
            var oldTicket = AdmissionQueueTestTicket.Replace("pkd-", "pkd_");
            const string priming = "priming";
            await StartRequestAsync(priming);
            await JoinAsync(priming);
            Deliver(
                ZlinkStreamMessageKind.Response,
                enqueue!.RequestSeq,
                "",
                JsonSerializer.SerializeToUtf8Bytes(new { ticket = oldTicket, expiresInMs = 30000 })
            );
            Assert.Equal(priming, await ReadRetryAsync(oldTicket));
            Deliver(
                ZlinkStreamMessageKind.Response,
                attempts[priming].RequestSeq,
                "",
                [42],
                ZlinkStreamCodec.Raw
            );
            await requests[priming].WaitAsync(TimeSpan.FromSeconds(5));
            requests.Remove(priming);
            queuedRequests.Clear();
            callbacks.Clear();
            enqueue = null;
            initialEnqueues = 1;
        }

        foreach (var request in givenRequests)
            await StartRequestAsync(request);

        foreach (var step in scenario.RootElement.GetProperty("steps").EnumerateArray())
        {
            var expected = step.GetProperty("expect");
            var request = step.TryGetProperty("request", out var id) ? id.GetString()! : "a";
            var action = step.GetProperty("action").GetString();
            switch (action)
            {
                case "queueRequired":
                    Assert.Equal(
                        TimeSpan.FromMilliseconds(queuedRequests.Count == 0 ? 15000 : timeout),
                        await JoinAsync(request)
                    );
                    break;
                case "positionResponse":
                case "positionSend":
                    var marker = action == "positionResponse" ? enqueue : null;
                    Deliver(
                        marker is null
                            ? ZlinkStreamMessageKind.Send
                            : ZlinkStreamMessageKind.Response,
                        marker?.RequestSeq,
                        marker is null ? positionName : string.Empty,
                        JsonSerializer.SerializeToUtf8Bytes(
                            new
                            {
                                position = step.GetProperty("position").GetInt32(),
                                estimatedWaitMs = step.GetProperty("estimatedWaitMs").GetInt32(),
                            }
                        )
                    );
                    await FlushAsync();
                    if (expected.TryGetProperty("callbacks", out _))
                    {
                        Assert.True(actualPositions.TryDequeue(out var observed), name);
                        Assert.Equal(step.GetProperty("position").GetInt32(), observed!.Position);
                        Assert.Equal(
                            step.GetProperty("estimatedWaitMs").GetInt32(),
                            observed.EstimatedWaitMs
                        );
                    }
                    break;
                case "ticketSend":
                    Assert.Equal("T", step.GetProperty("ticket").GetString());
                    Deliver(
                        ZlinkStreamMessageKind.Send,
                        null,
                        ticketName,
                        JsonSerializer.SerializeToUtf8Bytes(
                            new
                            {
                                ticket = AdmissionQueueTestTicket,
                                expiresInMs = step.GetProperty("expiresInMs").GetInt32(),
                            }
                        )
                    );
                    var retried = new List<string>();
                    foreach (var _ in expected.GetProperty("retriedRequests").EnumerateArray())
                        retried.Add(await ReadRetryAsync());
                    Assert.Equal(
                        expected
                            .GetProperty("retriedRequests")
                            .EnumerateArray()
                            .Select(value => value.GetString())
                            .Order(),
                        retried.Order()
                    );
                    break;
                case "stageOneReject":
                    Reject(
                        request,
                        ZlinkStreamMetadata.Empty.With(
                            "$zlink.admission.retryAfterMs",
                            step.GetProperty("retryAfterMs").GetString()!
                        )
                    );
                    await WaitUntilAsync(
                        () => callbacks.Count > callbackCount,
                        TimeSpan.FromSeconds(5)
                    );
                    clock.AdvanceTo(int.Parse(step.GetProperty("retryAfterMs").GetString()!));
                    Assert.Equal(request, await ReadRetryAsync());
                    break;
                case "complete":
                    Deliver(
                        ZlinkStreamMessageKind.Response,
                        attempts[request].RequestSeq,
                        "",
                        [42],
                        ZlinkStreamCodec.Raw
                    );
                    Assert.Equal(
                        new byte[] { 42 },
                        (
                            await requests[request].WaitAsync(TimeSpan.FromSeconds(5))
                        ).Payload.ToArray()
                    );
                    await Record("requestComplete");
                    break;
                case "silence":
                    clock.AdvanceTo(step.GetProperty("atMs").GetInt32());
                    enqueue = await ReadEnqueueAsync();
                    break;
                case "error":
                    var metadata = ZlinkStreamMetadata.Empty;
                    foreach (var property in step.GetProperty("metadata").EnumerateObject())
                        metadata = metadata.With(property.Name, property.Value.GetString()!);
                    Reject(request, metadata, step.GetProperty("code").GetString()!);
                    await AssertFailureAsync(request, expected.GetProperty(request).GetString()!);
                    break;
                case "enqueueResponse":
                    await JoinAsync(request);
                    Deliver(
                        ZlinkStreamMessageKind.Response,
                        enqueue!.RequestSeq,
                        "",
                        JsonSerializer.SerializeToUtf8Bytes(step.GetProperty("payload"))
                    );
                    await AssertFailureAsync(request, expected.GetProperty(request).GetString()!);
                    break;
                case "send":
                    foreach (var pending in requests.Keys)
                        await JoinAsync(pending);
                    Deliver(
                        ZlinkStreamMessageKind.Send,
                        null,
                        step.GetProperty("packet").GetString()!,
                        JsonSerializer.SerializeToUtf8Bytes(step.GetProperty("payload"))
                    );
                    if (
                        expected.TryGetProperty("discarded", out var discarded)
                        && discarded.GetBoolean()
                    )
                    {
                        await FlushAsync();
                        Assert.Empty(applicationErrors);
                    }
                    else
                    {
                        foreach (var pending in requests.Keys)
                            await AssertFailureAsync(
                                pending,
                                expected.GetProperty(pending).GetString()!
                            );
                        await WaitUntilAsync(() => !connector.IsConnected, TimeSpan.FromSeconds(5));
                    }
                    break;
                case "wait":
                    clock.AdvanceTo(firstRejection);
                    Assert.Equal(
                        TimeSpan.FromMilliseconds(expected.GetProperty("deadlineMs").GetInt32()),
                        await JoinAsync(request)
                    );
                    clock.AdvanceTo(step.GetProperty("atMs").GetInt32());
                    await AssertFailureAsync(request, expected.GetProperty("result").GetString()!);
                    Assert.Equal(
                        expected.GetProperty("sendAfterDeadline").GetBoolean(),
                        peer.WriteAttempts > consumedWrites
                    );
                    break;
                default:
                    throw new InvalidOperationException($"Unknown queue fixture action {action}.");
            }

            if (expected.TryGetProperty("callbacks", out var names))
            {
                Assert.Equal(
                    names.EnumerateArray().Select(value => value.GetString()),
                    callbacks.Skip(callbackCount)
                );
                callbackCount = callbacks.Count;
            }
            if (expected.TryGetProperty("enqueueCount", out var count))
                Assert.Equal(
                    count.GetInt32(),
                    peer.Written.Count(frame => codec.Decode(frame.Header).Name == enqueueName)
                        - initialEnqueues
                );
            if (expected.TryGetProperty("queuedCallbacks", out var queuedCount))
                Assert.Equal(
                    queuedCount.GetInt32(),
                    callbacks.Count(value => value == "OnAdmissionQueued")
                );
            if (expected.TryGetProperty("positionCallbacks", out var positionCount))
                Assert.Equal(positionCount.GetInt32(), actualPositions.Count);
            if (
                expected.TryGetProperty("cachedTicket", out var cachedTicket)
                && cachedTicket.ValueKind == JsonValueKind.Null
            )
                Assert.False(enqueue!.Metadata.TryGet(ticketName, out _));
            if (expected.TryGetProperty("connectionClosed", out var closed))
                Assert.Equal(closed.GetBoolean(), !connector.IsConnected);
            if (
                expected.TryGetProperty("b", out var pendingResult)
                && pendingResult.GetString() == "pending"
            )
                Assert.False(requests["b"].IsCompleted, name);
        }

        ValueTask Record(string callback)
        {
            callbacks.Enqueue(callback);
            return ValueTask.CompletedTask;
        }

        async Task StartRequestAsync(string request)
        {
            requests[request] = connector
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 7 }))
                .PacketName("Login")
                .Metadata("request", request)
                .Async()
                .AsTask();
            attempts[request] = originals[request] = (await NextWriteAsync()).Header;
        }

        async Task<TimeSpan> JoinAsync(string request)
        {
            Assert.True(queuedRequests.Add(request), name);
            Reject(request, ZlinkStreamMetadata.Empty.With("$zlink.admission.queueRequired", "1"));
            enqueue ??= await ReadEnqueueAsync();
            return await clock.NextTimerAsync();
        }

        async Task<ZlinkStreamHeader> ReadEnqueueAsync()
        {
            var write = await NextWriteAsync();
            Assert.Equal(enqueueName, write.Header.Name);
            Assert.Equal(ZlinkStreamCodec.Json, write.Header.Codec);
            Assert.Equal("{}"u8.ToArray(), write.Payload);
            return write.Header;
        }

        async Task<string> ReadRetryAsync(string expectedTicket = AdmissionQueueTestTicket)
        {
            var retry = await NextWriteAsync();
            var request = retry.Header.Metadata.Get("request")!;
            Assert.Equal("Login", retry.Header.Name);
            Assert.Equal(originals[request].CorrelationId, retry.Header.CorrelationId);
            Assert.NotEqual(attempts[request].RequestSeq, retry.Header.RequestSeq);
            Assert.Equal(expectedTicket, retry.Header.Metadata.Get(ticketName));
            Assert.Equal(new byte[] { 7 }, retry.Payload);
            attempts[request] = retry.Header;
            return request;
        }

        async Task<(ZlinkStreamHeader Header, byte[] Payload)> NextWriteAsync()
        {
            await peer.WaitForWritesAsync(1);
            var write = peer.Written.ElementAt(consumedWrites++);
            return (codec.Decode(write.Header), write.Payload.ToArray());
        }

        void Reject(string request, ZlinkStreamMetadata metadata, string code = "unavailable") =>
            Deliver(
                ZlinkStreamMessageKind.Error,
                attempts[request].RequestSeq,
                "",
                JsonSerializer.SerializeToUtf8Bytes(new { code, message = code }),
                metadata: metadata
            );

        void Deliver(
            ZlinkStreamMessageKind kind,
            ZlinkStreamRequestSeq? sequence,
            string packet,
            byte[] payload,
            ZlinkStreamCodec payloadCodec = ZlinkStreamCodec.Json,
            ZlinkStreamMetadata? metadata = null
        ) =>
            peer.Deliver(
                EncodePacket(
                    codec
                        .Encode(
                            new ZlinkStreamHeader(
                                kind,
                                payloadCodec,
                                sequence is null
                                    ? ZlinkStreamHeaderFlags.None
                                    : ZlinkStreamHeaderFlags.HasRequestSeq,
                                sequence,
                                packet,
                                metadata ?? ZlinkStreamMetadata.Empty
                            )
                        )
                        .ToArray(),
                    payload
                )
            );

        async Task FlushAsync()
        {
            var after = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            using var marker = connector.On(
                "Marker",
                (_, _) =>
                {
                    after.TrySetResult();
                    return ValueTask.CompletedTask;
                }
            );
            Deliver(ZlinkStreamMessageKind.Send, null, "Marker", []);
            await after.Task.WaitAsync(TimeSpan.FromSeconds(5));
        }

        async Task AssertFailureAsync(string request, string code)
        {
            var failure = await Assert.ThrowsAsync<ZlinkStreamException>(() =>
                requests[request].WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(code, failure.Error.Code.ToString());
        }

        static int ReadNumber(JsonElement given, string name, int fallback) =>
            given.TryGetProperty(name, out var value) ? value.GetInt32() : fallback;
    }
}
