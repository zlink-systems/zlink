using System.Collections.Concurrent;
using System.Diagnostics.Metrics;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Net.WebSockets;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Xunit;

public sealed partial class StreamConnectorTests
{
    private static readonly ConnectorMetricCollector ConnectorMetrics = new();

    [Fact]
    public async Task TcpFramesDoNotEmitRemovedByteOrHandshakeMetrics()
    {
        var metrics = ConnectorMetrics.BeginScope();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var inboundPayload = "inbound"u8.ToArray();
        var inboundHeader = headerCodec.Encode(new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            "metric.inbound",
            ZlinkStreamMetadata.Empty)).ToArray();
        var outboundSize = new TaskCompletionSource<long>(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var outbound = await ReadPacketAsync(stream);
            outboundSize.SetResult(6L + outbound.Header.Length + outbound.Payload.Length);
            await WritePacketAsync(stream, inboundHeader, inboundPayload);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var received = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.On("metric.inbound", (_, _) =>
        {
            received.TrySetResult();
            return ValueTask.CompletedTask;
        });

        await connector.Connect.Async();
        await connector.Send(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, "outbound"u8.ToArray()))
            .PacketName("metric.outbound")
            .Async();

        await received.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server;

        _ = await outboundSize.Task;
        Assert.Equal(0, metrics.Count("zlink.stream.handshake.duration"));
        Assert.Equal(0, metrics.Count("zlink.stream.handshake.failures"));
        Assert.Equal(0, metrics.Count("zlink.stream.inbound.bytes"));
        Assert.Equal(0, metrics.Count("zlink.stream.outbound.bytes"));
    }

    [Fact]
    public async Task AutomaticReconnectRecordsOneAttemptButInitialConnectRecordsNone()
    {
        var metrics = ConnectorMetrics.BeginScope();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var reconnected = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using (var first = await listener.AcceptTcpClientAsync()) first.Close();
            using var second = await listener.AcceptTcpClientAsync();
            reconnected.SetResult();
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Reconnect = new ZlinkStreamReconnectOptions
            {
                InitialDelay = TimeSpan.FromMilliseconds(10),
                MaxDelay = TimeSpan.FromMilliseconds(10),
                BackoffFactor = 1,
                MaxAttempts = 3
            }
        });

        await connector.Connect.Async();
        await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(() => connector.IsConnected, TimeSpan.FromSeconds(5));

        Assert.Equal(1, metrics.SumLong("zlink.stream.reconnects"));
        var reconnect = Assert.Single(metrics.Samples("zlink.stream.reconnects"));
        Assert.Equal("tcp", reconnect.Tags["transport"]);
        Assert.Equal("connected", reconnect.Tags["outcome"]);
        Assert.Equal("transport_closed", reconnect.Tags["reason"]);
        metrics.AssertInstrument("zlink.stream.reconnects", "{reconnect}", typeof(Counter<long>));
        await server;
    }

    [Fact]
    public async Task TlsAndWebSocketHandshakesDoNotEmitRemovedMetrics()
    {
        var metrics = ConnectorMetrics.BeginScope();
        using var certificate = CreateSelfSignedCertificate();
        using var successListener = new TcpListener(IPAddress.Loopback, 0);
        successListener.Start();
        var successEndpoint = (IPEndPoint)successListener.LocalEndpoint;
        var successServer = Task.Run(async () =>
        {
            using var tcp = await successListener.AcceptTcpClientAsync();
            await using var ssl = new SslStream(tcp.GetStream(), false);
            await ssl.AuthenticateAsServerAsync(certificate);
            await Task.Delay(TimeSpan.FromMilliseconds(100));
        });

        await using (var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
                     {
                         Endpoint = new Uri($"tls://127.0.0.1:{successEndpoint.Port}"),
                         Heartbeat = DisabledHeartbeat(),
                         Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                         SkipServerCertificateValidation = true
                     }))
            await connector.Connect.Async();
        await successServer;

        using var webSocketListener = new HttpListener();
        var webSocketPort = GetFreeTcpPort();
        webSocketListener.Prefixes.Add($"http://127.0.0.1:{webSocketPort}/metrics/");
        webSocketListener.Start();
        var webSocketServer = Task.Run(async () =>
        {
            var context = await webSocketListener.GetContextAsync();
            var accepted = await context.AcceptWebSocketAsync(null);
            using var webSocket = accepted.WebSocket;
            var close = await webSocket.ReceiveAsync(new byte[1], CancellationToken.None);
            Assert.Equal(WebSocketMessageType.Close, close.MessageType);
            await webSocket.CloseOutputAsync(
                WebSocketCloseStatus.NormalClosure,
                "closed",
                CancellationToken.None);
        });
        var webSocketOptions = new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"ws://127.0.0.1:{webSocketPort}/metrics/"),
            Heartbeat = DisabledHeartbeat(),
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        };
        var webSocketConnection = await ZlinkStreamTransportFactory.ConnectAsync(
            webSocketOptions,
            CancellationToken.None);
        await webSocketConnection.CloseAsync(CancellationToken.None);
        await webSocketServer;

        using var failureListener = new TcpListener(IPAddress.Loopback, 0);
        failureListener.Start();
        var failureEndpoint = (IPEndPoint)failureListener.LocalEndpoint;
        var failureServer = Task.Run(async () =>
        {
            using var tcp = await failureListener.AcceptTcpClientAsync();
            tcp.Close();
        });
        await using (var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
                     {
                         Endpoint = new Uri($"tls://127.0.0.1:{failureEndpoint.Port}"),
                         Heartbeat = DisabledHeartbeat(),
                         Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
                         SkipServerCertificateValidation = true
                     }))
            await Assert.ThrowsAsync<ZlinkStreamException>(async () => await connector.Connect.Async());
        await failureServer;

        Assert.Equal(0, metrics.Count("zlink.stream.handshake.duration"));
        Assert.Equal(0, metrics.Count("zlink.stream.handshake.failures"));
    }

    [Fact]
    public async Task ThrowingMetricsListenerDoesNotChangeRequestResult()
    {
        using var throwOnMeasurement = ConnectorMetrics.ThrowOnMeasurement();
        ZlinkStreamRuntimeMetrics.RecordReconnect(
            "tcp",
            "failed",
            "connect_failed");
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var request = await ReadPacketAsync(stream);
            var requestHeader = headerCodec.Decode(request.Header);
            var responseHeader = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                requestHeader.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty);
            await WritePacketAsync(
                stream,
                headerCodec.Encode(responseHeader).ToArray(),
                "reply"u8.ToArray());
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        await connector.Connect.Async();

        var reply = await connector
            .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, "request"u8.ToArray()))
            .PacketName("metric.request")
            .Timeout(TimeSpan.FromSeconds(5))
            .Async();

        Assert.Equal("reply"u8.ToArray(), reply.Payload.ToArray());
        await server;
    }

    [Fact]
    public void TestMetricReaderRetainsOnlyABoundedSnapshot()
    {
        var metrics = ConnectorMetrics.BeginScope();
        for (var index = 0; index < ConnectorMetricCollector.MaxRetainedSamples * 2; index++)
            ZlinkStreamRuntimeMetrics.RecordReconnect(
                "tcp",
                "failed",
                "connect_failed");

        Assert.Equal(
            ConnectorMetricCollector.MaxRetainedSamples,
            metrics.Count("zlink.stream.reconnects"));
        Assert.InRange(
            ConnectorMetrics.StoredSampleCount,
            1,
            ConnectorMetricCollector.MaxRetainedSamples);
    }

    private sealed class ConnectorMetricCollector
    {
        internal const int MaxRetainedSamples = 4096;
        private static readonly AsyncLocal<bool> ThrowCurrentMeasurement = new();
        private readonly ConcurrentDictionary<string, Instrument> _instruments = new(StringComparer.Ordinal);
        private readonly object _samplesGate = new();
        private readonly Queue<MetricSample> _samples = new();
        private readonly MeterListener _listener = new();
        private long _nextSequence;

        public ConnectorMetricCollector()
        {
            _listener.InstrumentPublished = (instrument, meterListener) =>
            {
                if (instrument.Meter.Name != ZlinkStreamRuntimeMetrics.MeterName) return;
                _instruments[instrument.Name] = instrument;
                meterListener.EnableMeasurementEvents(instrument);
            };
            _listener.SetMeasurementEventCallback<long>(Record);
            _listener.SetMeasurementEventCallback<double>(Record);
            _listener.Start();
        }

        public MetricScope BeginScope() => new(this, Interlocked.Read(ref _nextSequence));

        public int StoredSampleCount
        {
            get
            {
                lock (_samplesGate) return _samples.Count;
            }
        }

        public IDisposable ThrowOnMeasurement()
        {
            ThrowCurrentMeasurement.Value = true;
            return new CallbackDisposable(static () => ThrowCurrentMeasurement.Value = false);
        }

        public void AssertInstrument(string name, string unit, Type instrumentType)
        {
            var instrument = Assert.Contains(name, _instruments);
            Assert.Equal(unit, instrument.Unit);
            Assert.Equal(instrumentType, instrument.GetType());
        }

        private IReadOnlyList<MetricSample> Samples(string name, long afterSequence)
        {
            lock (_samplesGate)
                return _samples
                    .Where(sample => sample.Sequence > afterSequence && sample.Name == name)
                    .ToArray();
        }

        private void Record<T>(
            Instrument instrument,
            T measurement,
            ReadOnlySpan<KeyValuePair<string, object?>> tags,
            object? state)
            where T : struct
        {
            _ = state;

            var copiedTags = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var tag in tags) copiedTags[tag.Key] = tag.Value?.ToString() ?? string.Empty;
            lock (_samplesGate)
            {
                _samples.Enqueue(new MetricSample(
                    Interlocked.Increment(ref _nextSequence),
                    instrument.Name,
                    measurement,
                    copiedTags));
                while (_samples.Count > MaxRetainedSamples) _samples.Dequeue();
            }
            if (ThrowCurrentMeasurement.Value)
                throw new InvalidOperationException("metrics listener failed");
        }

        public sealed class MetricScope(ConnectorMetricCollector collector, long afterSequence)
        {
            public int Count(string name) => Samples(name).Count;

            public int Count(string name, string tag, string value) =>
                Samples(name).Count(sample => sample.Tags.TryGetValue(tag, out var actual) && actual == value);

            public long SumLong(string name, string? tag = null, string? value = null) =>
                Samples(name)
                    .Where(sample => tag is null
                                     || sample.Tags.TryGetValue(tag, out var actual) && actual == value)
                    .Sum(sample => Convert.ToInt64(sample.Value));

            public IReadOnlyList<MetricSample> Samples(string name) => collector.Samples(name, afterSequence);

            public void AssertInstrument(string name, string unit, Type instrumentType) =>
                collector.AssertInstrument(name, unit, instrumentType);
        }

        private sealed class CallbackDisposable(Action dispose) : IDisposable
        {
            public void Dispose() => dispose();
        }
    }

    private sealed record MetricSample(
        long Sequence,
        string Name,
        object Value,
        IReadOnlyDictionary<string, string> Tags);
}
