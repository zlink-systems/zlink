using Google.Protobuf.WellKnownTypes;
using MessagePack;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Contracts.Calls;
using Xunit;
using Zlink.Framework.Codecs.MessagePack;
using Zlink.Framework.Codecs.Protobuf;
using Type = System.Type;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public async Task TypedConnectorUsesJsonByDefaultAndDecodeReply()
    {
        var connector = new RecordingConnector();

        await connector.Send(new JsonPayload("send"))
            .PacketName("json.send")
            .Metadata("k", "v")
            .Metadata(ZlinkStreamMetadata.Empty.With("m", "n"))
            .Compress().Async();

        Assert.Equal(ZlinkStreamCodec.Json, connector.SendCall.Payload.Codec);
        Assert.Equal("json.send", connector.SendCall.Name);
        Assert.True(connector.SendCall.Compressed);
        Assert.Equal("v", connector.SendCall.RecordedMetadata.Get("k"));
        Assert.Equal("n", connector.SendCall.RecordedMetadata.Get("m"));

        connector.NextReply = new JsonPayload("reply").ToJson();
        var reply = await connector.Request(new JsonPayload("request"))
            .PacketName("json.request")
            .Metadata("rk", "rv")
            .Timeout(TimeSpan.FromSeconds(3))
            .Compress()
            .Async<JsonPayload>();

        Assert.Equal("reply", reply.Text);
        Assert.Equal("json.request", connector.RequestCall.Name);
        Assert.Equal(TimeSpan.FromSeconds(3), connector.RequestCall.TimeoutValue);
        Assert.True(connector.RequestCall.Compressed);

        ZlinkStreamResult<JsonPayload>? callbackResult = null;
        connector.NextCallbackPayloadResult = ZlinkStreamResult<ZlinkStreamEncodedPayload>.Success(
            new JsonPayload("callback").ToJson());
        connector.Request(new JsonPayload("request"))
            .Submit<JsonPayload>(result => callbackResult = result);
        Assert.True(callbackResult?.IsSuccess);
        Assert.Equal("callback", callbackResult.GetValueOrDefault().Value!.Text);

        ZlinkStreamResult<JsonPayload>? failedDecode = null;
        connector.NextCallbackPayloadResult = ZlinkStreamResult<ZlinkStreamEncodedPayload>.Success(
            new ZlinkStreamEncodedPayload(
                ZlinkStreamCodec.Json,
                new byte[] { 0xFF }));
        connector.Request(new JsonPayload("request"))
            .Submit<JsonPayload>(result => failedDecode = result);
        Assert.False(failedDecode?.IsSuccess);
        Assert.Equal(ZlinkStreamErrorCode.UserCallbackFailed, failedDecode.GetValueOrDefault().Error!.Code);

        connector.RecordReceived("json.notify", new JsonPayload("notify").ToJson());
        var notify = await connector.WaitFor<JsonPayload>("json.notify")
            .Timeout(TimeSpan.FromSeconds(1))
            .Async();
        Assert.Equal("notify", notify.Payload.Text);

        connector.RecordReceived("json.filtered", new JsonPayload("first").ToJson());
        connector.RecordReceived("json.filtered", new JsonPayload("second").ToJson());
        var filtered = await connector.WaitFor<JsonPayload>("json.filtered")
            .Where(message => message.Payload.Text == "second")
            .Timeout(TimeSpan.FromSeconds(1))
            .Async();
        var remaining = await connector.WaitFor<JsonPayload>("json.filtered")
            .Timeout(TimeSpan.FromSeconds(1))
            .Async();
        Assert.Equal("second", filtered.Payload.Text);
        Assert.Equal("first", remaining.Payload.Text);

        connector.RecordReceived("json.sequence", new JsonPayload("first").ToJson());
        connector.RecordReceived("json.sequence", new JsonPayload("second").ToJson());
        var sequence = await connector.WaitForSequence<JsonPayload>("json.sequence")
            .Expect(message => message.Payload.Text == "first")
            .Expect(message => message.Payload.Text == "second")
            .Timeout(TimeSpan.FromSeconds(1))
            .Async();
        Assert.Equal(new[] { "first", "second" }, sequence.Select(message => message.Payload.Text));

        await connector.ExpectNone<JsonPayload>("json.missing")
            .Within(TimeSpan.FromMilliseconds(1))
            .Async();
    }

    [Fact]
    public void WithInboundObserverRegistersObserverAndReturnsConnector()
    {
        var connector = new RecordingConnector();

        var returned = connector.WithInboundObserver((_, _) => ValueTask.CompletedTask);

        Assert.Same(connector, returned);
        Assert.Equal(1, connector.InboundObserverCount);
    }

    [Fact]
    public async Task MessagePackConnectorExtensionsDelegateBuilderAndDecodeReply()
    {
        var connector = new RecordingConnector(ZLinkMessagePackCodec.Default);

        await connector.Send(new PackedConnectorPayload { Text = "send" })
            .PacketName("packed.send")
            .Metadata("k", "v")
            .Metadata(ZlinkStreamMetadata.Empty.With("m", "n"))
            .Compress().Async();

        Assert.Equal(ZlinkStreamCodec.MessagePack, connector.SendCall.Payload.Codec);
        Assert.Equal("packed.send", connector.SendCall.Name);
        Assert.True(connector.SendCall.Compressed);

        connector.NextReply = ZLinkMessagePackCodec.Default.Encode(new PackedConnectorPayload { Text = "reply" });
        var reply = await connector.Request(new PackedConnectorPayload { Text = "request" })
            .PacketName("packed.request")
            .Metadata("rk", "rv")
            .Timeout(TimeSpan.FromSeconds(2))
            .Compress()
            .Async<PackedConnectorPayload>();

        Assert.Equal("reply", reply.Text);
        Assert.Equal("packed.request", connector.RequestCall.Name);
        Assert.Equal(TimeSpan.FromSeconds(2), connector.RequestCall.TimeoutValue);

        ZlinkStreamResult<PackedConnectorPayload>? callbackResult = null;
        connector.NextCallbackPayloadResult = ZlinkStreamResult<ZlinkStreamEncodedPayload>.Success(
            ZLinkMessagePackCodec.Default.Encode(new PackedConnectorPayload { Text = "callback" }));
        connector.Request(new PackedConnectorPayload { Text = "request" })
            .Submit<PackedConnectorPayload>(result => callbackResult = result);
        Assert.True(callbackResult?.IsSuccess);
        Assert.Equal("callback", callbackResult.GetValueOrDefault().Value!.Text);
    }

    [Fact]
    public async Task ProtobufConnectorExtensionsDelegateBuilderAndDecodeReply()
    {
        var connector = new RecordingConnector(ZLinkProtobufCodec.Default);
        var payload = new StringValue { Value = "send" };

        await connector.Send(payload)
            .PacketName("proto.send")
            .Metadata("k", "v")
            .Metadata(ZlinkStreamMetadata.Empty.With("m", "n"))
            .Compress().Async();

        Assert.Equal(ZlinkStreamCodec.Protobuf, connector.SendCall.Payload.Codec);
        Assert.Equal("proto.send", connector.SendCall.Name);

        connector.NextReply = ZLinkProtobufCodec.Default.Encode(new StringValue { Value = "reply" });
        var reply = await connector.Request(new StringValue { Value = "request" })
            .PacketName("proto.request")
            .Metadata("rk", "rv")
            .Timeout(TimeSpan.FromSeconds(4))
            .Compress()
            .Async<StringValue>();

        Assert.Equal("reply", reply.Value);
        Assert.Equal("proto.request", connector.RequestCall.Name);

        ZlinkStreamResult<StringValue>? callbackResult = null;
        connector.NextCallbackPayloadResult = ZlinkStreamResult<ZlinkStreamEncodedPayload>.Success(
            ZLinkProtobufCodec.Default.Encode(new StringValue { Value = "callback" }));
        connector.Request(new StringValue { Value = "request" })
            .Submit<StringValue>(result => callbackResult = result);
        Assert.True(callbackResult?.IsSuccess);
        Assert.Equal("callback", callbackResult.GetValueOrDefault().Value!.Value);
    }

    [Fact]
    public async Task TypedConnectorUsesConfiguredCodecAndDecodeHandlers()
    {
        var connector = new RecordingConnector();

        await connector.Send(new JsonPayload("json"))
            .PacketName("auto.json").Async();
        Assert.Equal(ZlinkStreamCodec.Json, connector.SendCall.Payload.Codec);

        var packedConnector = new RecordingConnector(ZLinkMessagePackCodec.Default);
        await packedConnector.Send(new PackedConnectorPayload { Text = "packed" })
            .PacketName("auto.packed").Async();
        Assert.Equal(ZlinkStreamCodec.MessagePack, packedConnector.SendCall.Payload.Codec);

        var protobufConnector = new RecordingConnector(ZLinkProtobufCodec.Default);
        await protobufConnector.Send(new StringValue { Value = "proto" })
            .PacketName("auto.proto").Async();
        Assert.Equal(ZlinkStreamCodec.Protobuf, protobufConnector.SendCall.Payload.Codec);

        JsonPayload? namedPayload = null;
        using var named = connector.On<JsonPayload>(
            "json.name",
            (message, _) =>
            {
                namedPayload = message.Payload;
                return ValueTask.CompletedTask;
            });
        await connector.InvokeHandler("json.name", new JsonPayload("handler").ToJson());
        Assert.Equal("handler", namedPayload?.Text);

        PackedConnectorPayload? resolvedPayload = null;
        using var resolved = packedConnector.On<PackedConnectorPayload>((message, _) =>
        {
            resolvedPayload = message.Payload;
            return ValueTask.CompletedTask;
        });
        await packedConnector.InvokeHandler(
            nameof(PackedConnectorPayload),
            ZLinkMessagePackCodec.Default.Encode(new PackedConnectorPayload { Text = "resolved" }));
        Assert.Equal("resolved", resolvedPayload?.Text);
    }

    private sealed record JsonPayload(string Text);

    [MessagePackObject]
    public sealed class PackedConnectorPayload
    {
        [Key(0)] public string Text { get; set; } = string.Empty;
    }

    private sealed class RecordingConnector : IZlinkStreamConnector
    {
        private readonly HashSet<(string Name, int Index)> _consumed = [];

        private readonly
            Dictionary<string, Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask>>
            _handlers = new(StringComparer.Ordinal);

        private readonly Dictionary<string, List<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>> _received =
            new(StringComparer.Ordinal);

        public RecordingConnector(IZlinkStreamPayloadCodec? payloadCodec = null)
        {
            Options = new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                NameResolver = new TypeNameResolver(),
                PayloadCodec = payloadCodec
            };
        }

        public int InboundObserverCount { get; private set; }

        public RecordingSendCall SendCall { get; private set; } =
            new(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, Array.Empty<byte>()));

        public RecordingRequestCall RequestCall { get; private set; } =
            new(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, Array.Empty<byte>()));

        public ZlinkStreamEncodedPayload NextReply { get; set; } = new(ZlinkStreamCodec.Raw, Array.Empty<byte>());

        public ZlinkStreamResult<ZlinkStreamEncodedPayload> NextCallbackPayloadResult { get; set; } =
            ZlinkStreamResult<ZlinkStreamEncodedPayload>.Failure(new ZlinkStreamError(
                ZlinkStreamErrorCode.RemoteError,
                "missing callback result"));

        public event Func<ZlinkStreamError, CancellationToken, ValueTask>? ErrorReceived
        {
            add { }
            remove { }
        }

        public event Func<ZlinkStreamDisconnected, CancellationToken, ValueTask>? Disconnected
        {
            add { }
            remove { }
        }

        public event Func<ZlinkStreamConnectionStateChanged, CancellationToken, ValueTask>? ConnectionStateChanged
        {
            add { }
            remove { }
        }

        public bool IsConnected => true;

        public ZlinkStreamConnectionState State => ZlinkStreamConnectionState.Connected;

        public ZlinkStreamConnectorOptions Options { get; }

        public int PendingDispatchCount => 0;

        public int ReceivedCount(string name)
        {
            return _received.TryGetValue(name, out var messages) ? messages.Count : 0;
        }

        public IZlinkStreamLifecycleCall Connect { get; } = new RecordingLifecycleCall();

        public IZlinkStreamLifecycleCall Close { get; } = new RecordingLifecycleCall();

        public IZlinkStreamLifecycleCall Dispatch { get; } = new RecordingLifecycleCall();

        public IZlinkStreamSendCall Send(ZlinkStreamEncodedPayload payload)
        {
            return SendCall = new RecordingSendCall(payload);
        }

        public IZlinkStreamRequestCall Request(ZlinkStreamEncodedPayload payload)
        {
            return RequestCall = new RecordingRequestCall(payload)
            {
                Reply = NextReply,
                CallbackPayloadResult = NextCallbackPayloadResult
            };
        }

        public IDisposable ObserveInbound(
            Func<ZlinkStreamInboundObservation, CancellationToken, ValueTask> observer)
        {
            InboundObserverCount++;
            return new Subscription(() => InboundObserverCount--);
        }

        public IDisposable On(
            string name,
            Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, CancellationToken, ValueTask> handler)
        {
            _handlers.Add(name, handler);
            return new Subscription(() => _handlers.Remove(name));
        }

        public IZlinkStreamWaitCall WaitFor(string name)
        {
            return new RecordingWaitCall(name, this);
        }

        public IZlinkStreamExpectNoneCall ExpectNone(string name)
        {
            return new RecordingExpectNoneCall(name, this);
        }

        public IZlinkStreamSequenceCall WaitForSequence(string name)
        {
            return new RecordingSequenceCall(name, this);
        }

        public ValueTask DisposeAsync()
        {
            return ValueTask.CompletedTask;
        }

        private ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> WaitForRecordedAsync(
            string name,
            TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            return WaitForRecordedAsync(name, static _ => true, timeout, cancellationToken);
        }

        private ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> WaitForRecordedAsync(
            string name,
            Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> predicate,
            TimeSpan timeout,
            CancellationToken cancellationToken = default)
        {
            _ = timeout;
            cancellationToken.ThrowIfCancellationRequested();
            if (!_received.TryGetValue(name, out var messages))
                throw new TimeoutException($"Timed out waiting for {name}.");

            for (var i = 0; i < messages.Count; i++)
            {
                var message = messages[i];
                var key = (name, i);
                if (_consumed.Contains(key) || !predicate(message)) continue;

                _consumed.Add(key);
                return ValueTask.FromResult(message);
            }

            throw new TimeoutException($"Timed out waiting for {name}.");
        }

        public ValueTask InvokeHandler(string name, ZlinkStreamEncodedPayload payload)
        {
            return _handlers[name](new ZlinkStreamMessage<ZlinkStreamEncodedPayload>(
                name,
                ZlinkStreamMetadata.Empty,
                payload), CancellationToken.None);
        }

        public void RecordReceived(string name, ZlinkStreamEncodedPayload payload)
        {
            if (!_received.TryGetValue(name, out var messages))
            {
                messages = [];
                _received.Add(name, messages);
            }

            messages.Add(new ZlinkStreamMessage<ZlinkStreamEncodedPayload>(
                name,
                ZlinkStreamMetadata.Empty,
                payload));
        }

        private sealed class RecordingWaitCall : IZlinkStreamWaitCall
        {
            private readonly RecordingConnector _connector;
            private readonly string _name;
            private Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>? _predicate;
            private TimeSpan? _timeout;

            public RecordingWaitCall(string name, RecordingConnector connector)
            {
                _name = name;
                _connector = connector;
            }

            public IZlinkStreamWaitCall Timeout(TimeSpan timeout)
            {
                _timeout = timeout;
                return this;
            }

            public IZlinkStreamWaitCall Where(Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> predicate)
            {
                ArgumentNullException.ThrowIfNull(predicate);
                var previous = _predicate;
                _predicate = previous is null
                    ? predicate
                    : message => previous(message) && predicate(message);
                return this;
            }

            public ValueTask<ZlinkStreamMessage<ZlinkStreamEncodedPayload>> Async(
                CancellationToken cancellationToken = default)
            {
                return _connector.WaitForRecordedAsync(
                    _name,
                    _predicate ?? (static _ => true),
                    _timeout ?? _connector.Options.WaitTimeout,
                    cancellationToken);
            }
        }

        private sealed class RecordingExpectNoneCall : IZlinkStreamExpectNoneCall
        {
            private readonly RecordingConnector _connector;
            private readonly string _name;
            private TimeSpan? _window;

            public RecordingExpectNoneCall(string name, RecordingConnector connector)
            {
                _name = name;
                _connector = connector;
            }

            public IZlinkStreamExpectNoneCall Within(TimeSpan window)
            {
                _window = window;
                return this;
            }

            public async ValueTask Async(CancellationToken cancellationToken = default)
            {
                try
                {
                    await _connector.WaitForRecordedAsync(
                        _name,
                        _window ?? TimeSpan.Zero,
                        cancellationToken);
                }
                catch (TimeoutException)
                {
                    return;
                }

                throw new InvalidOperationException($"Unexpected {_name} message.");
            }
        }

        private sealed class RecordingSequenceCall : IZlinkStreamSequenceCall
        {
            private readonly RecordingConnector _connector;
            private readonly List<Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool>> _expectations = [];
            private readonly string _name;
            private TimeSpan? _timeout;

            public RecordingSequenceCall(string name, RecordingConnector connector)
            {
                _name = name;
                _connector = connector;
            }

            public IZlinkStreamSequenceCall Expect(
                Func<ZlinkStreamMessage<ZlinkStreamEncodedPayload>, bool> predicate)
            {
                _expectations.Add(predicate);
                return this;
            }

            public IZlinkStreamSequenceCall Timeout(TimeSpan timeout)
            {
                _timeout = timeout;
                return this;
            }

            public async ValueTask<IReadOnlyList<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>> Async(
                CancellationToken cancellationToken = default)
            {
                var messages = new List<ZlinkStreamMessage<ZlinkStreamEncodedPayload>>(_expectations.Count);
                foreach (var expectation in _expectations)
                {
                    var message = await _connector.WaitForRecordedAsync(
                        _name,
                        _timeout ?? _connector.Options.WaitTimeout,
                        cancellationToken);
                    if (!expectation(message)) throw new InvalidOperationException("Out of order.");
                    messages.Add(message);
                }

                return messages;
            }
        }

        private sealed class TypeNameResolver : IZlinkStreamPacketNameResolver
        {
            public string Resolve(Type payloadType)
            {
                return payloadType.Name;
            }
        }

        private sealed class Subscription : IDisposable
        {
            private readonly Action _dispose;

            public Subscription(Action dispose)
            {
                _dispose = dispose;
            }

            public void Dispose()
            {
                _dispose();
            }
        }

        private sealed class RecordingLifecycleCall : IZlinkStreamLifecycleCall
        {
            public ValueTask Async(CancellationToken cancellationToken = default)
            {
                return ValueTask.CompletedTask;
            }
        }
    }

    private sealed class RecordingSendCall : IZlinkStreamSendCall
    {
        public RecordingSendCall(ZlinkStreamEncodedPayload payload)
        {
            Payload = payload;
        }

        public ZlinkStreamEncodedPayload Payload { get; }

        public string? Name { get; private set; }

        public ZlinkStreamMetadata RecordedMetadata { get; private set; } = ZlinkStreamMetadata.Empty;

        public bool Compressed { get; private set; }

        public IZlinkStreamSendCall PacketName(string name)
        {
            Name = name;
            return this;
        }

        public IZlinkStreamSendCall Metadata(string key, string value)
        {
            RecordedMetadata = RecordedMetadata.With(key, value);
            return this;
        }

        public IZlinkStreamSendCall Metadata(ZlinkStreamMetadata metadata)
        {
            RecordedMetadata = RecordedMetadata.WithMany(metadata.Values);
            return this;
        }

        public IZlinkStreamSendCall Compress()
        {
            Compressed = true;
            return this;
        }

        public ValueTask Async(CancellationToken cancellationToken = default)
        {
            return ValueTask.CompletedTask;
        }
    }

    private sealed class RecordingRequestCall : IZlinkStreamRequestCall
    {
        public RecordingRequestCall(ZlinkStreamEncodedPayload payload)
        {
            Payload = payload;
        }

        public ZlinkStreamEncodedPayload Payload { get; }

        public string? Name { get; private set; }

        public ZlinkStreamMetadata RecordedMetadata { get; private set; } = ZlinkStreamMetadata.Empty;

        public TimeSpan? TimeoutValue { get; private set; }

        public bool Compressed { get; private set; }

        public ZlinkStreamEncodedPayload Reply { get; set; } = new(ZlinkStreamCodec.Raw, Array.Empty<byte>());

        public ZlinkStreamResult<ZlinkStreamEncodedPayload> CallbackPayloadResult { get; set; } =
            ZlinkStreamResult<ZlinkStreamEncodedPayload>.Failure(new ZlinkStreamError(
                ZlinkStreamErrorCode.RemoteError,
                "missing callback result"));

        public IZlinkStreamRequestCall PacketName(string name)
        {
            Name = name;
            return this;
        }

        public IZlinkStreamRequestCall Metadata(string key, string value)
        {
            RecordedMetadata = RecordedMetadata.With(key, value);
            return this;
        }

        public IZlinkStreamRequestCall Metadata(ZlinkStreamMetadata metadata)
        {
            RecordedMetadata = RecordedMetadata.WithMany(metadata.Values);
            return this;
        }

        public IZlinkStreamRequestCall Timeout(TimeSpan timeout)
        {
            TimeoutValue = timeout;
            return this;
        }

        public IZlinkStreamRequestCall Compress()
        {
            Compressed = true;
            return this;
        }

        public ValueTask<ZlinkStreamEncodedPayload> Async(CancellationToken cancellationToken = default)
        {
            return ValueTask.FromResult(Reply);
        }

        public void Submit(Action<ZlinkStreamResult> callback)
        {
            callback(CallbackPayloadResult.IsSuccess
                ? ZlinkStreamResult.Success()
                : ZlinkStreamResult.Failure(CallbackPayloadResult.Error!));
        }

        public void Submit(Action<ZlinkStreamResult<ZlinkStreamEncodedPayload>> callback)
        {
            callback(CallbackPayloadResult);
        }
    }
}
