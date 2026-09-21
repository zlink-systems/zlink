using System.Net;
using System.Net.Sockets;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Protocol.Compression;
using Xunit;

/// <summary>
///     Covers the connector behaviour the stream-connector spec fixes and that the .NET
///     surface used to miss: disposable connection-event registrations, the close-reason
///     read surface, flow identifiers on received messages, arrival-based received counts,
///     typed option validation failures, the non-blocking synchronous diagnostics setter,
///     inherited packet-name attributes and the reconnect jitter window.
/// </summary>
public sealed partial class StreamConnectorTests
{
    [Fact]
    public async Task DisposingAConnectionEventRegistrationStopsTheHandler()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var accepted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            accepted.TrySetResult();
            await Task.Delay(TimeSpan.FromMilliseconds(200));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            }
        );

        var states = new List<ZlinkStreamConnectionState>();
        var registration = connector.OnConnectionStateChanged(
            (change, _) =>
            {
                lock (states)
                    states.Add(change.Current);
                return ValueTask.CompletedTask;
            }
        );

        await connector.Connect.Async();
        await accepted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(
            () =>
            {
                lock (states)
                    return states.Contains(ZlinkStreamConnectionState.Connected);
            },
            TimeSpan.FromSeconds(5)
        );

        int observedBeforeDispose;
        lock (states)
            observedBeforeDispose = states.Count;

        // Registering hands back the value that removes the registration, and removing it
        // twice is not an error (stream-connector spec §7).
        registration.Dispose();
        registration.Dispose();

        await connector.Close.Async();
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        lock (states)
            Assert.Equal(observedBeforeDispose, states.Count);
    }

    [Fact]
    public void EveryConnectionEventRegistrationReturnsADisposable()
    {
        var options = new ZlinkStreamConnectorOptions { Endpoint = new Uri("tcp://127.0.0.1:1") };
        var connector = ZlinkStreamConnectorFactory.Create(options);

        using var error = connector.OnErrorReceived((_, _) => ValueTask.CompletedTask);
        using var disconnected = connector.OnDisconnected((_, _) => ValueTask.CompletedTask);
        using var state = connector.OnConnectionStateChanged((_, _) => ValueTask.CompletedTask);

        Assert.NotNull(error);
        Assert.NotNull(disconnected);
        Assert.NotNull(state);
    }

    [Fact]
    public async Task AFailedFirstConnectLeavesATransportErrorCloseReason()
    {
        var port = GetFreeTcpPort();
        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{port}"),
                Heartbeat = DisabledHeartbeat(),
                ConnectTimeout = TimeSpan.FromMilliseconds(200),
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            }
        );

        // Nothing was ever connected, so nothing read the reason off a disconnect event.
        Assert.Null(connector.CloseReason);

        await Assert.ThrowsAsync<ZlinkStreamException>(async () => await connector.Connect.Async());

        // A first connect that fails still records how the attempt ended, and §9 maps
        // both ConnectTimeout and TlsValidationFailed to TransportError (spec §6.2).
        Assert.Equal(ZlinkStreamCloseReason.TransportError, connector.CloseReason);
    }

    [Fact]
    public async Task ReconnectingKeepsTheLastCloseReason()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var reconnected = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var server = Task.Run(async () =>
        {
            using (var first = await listener.AcceptTcpClientAsync())
                first.Close();
            using var second = await listener.AcceptTcpClientAsync();
            reconnected.TrySetResult();
            await Task.Delay(TimeSpan.FromMilliseconds(200));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new ZlinkStreamReconnectOptions
                {
                    InitialDelay = TimeSpan.FromMilliseconds(10),
                    MaxDelay = TimeSpan.FromMilliseconds(10),
                    BackoffFactor = 1,
                    MaxAttempts = 5,
                },
            }
        );

        await connector.Connect.Async();
        await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(() => connector.IsConnected, TimeSpan.FromSeconds(5));

        // Connecting again does not erase the reason; the read surface keeps the last
        // close (spec §6.2).
        Assert.Equal(ZlinkStreamCloseReason.TransportError, connector.CloseReason);
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SpendingEveryReconnectAttemptDisconnectsAndRunsTheDisconnectHandler()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var accepted = await listener.AcceptTcpClientAsync();
            accepted.Close();

            // The listener goes away with the first connection, so every reconnect
            // attempt that follows fails and the attempt budget runs out.
            listener.Stop();
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                ConnectTimeout = TimeSpan.FromMilliseconds(200),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new ZlinkStreamReconnectOptions
                {
                    InitialDelay = TimeSpan.FromMilliseconds(10),
                    MaxDelay = TimeSpan.FromMilliseconds(10),
                    BackoffFactor = 1,
                    MaxAttempts = 1,
                },
            }
        );

        var exhausted = new TaskCompletionSource<ZlinkStreamDisconnected>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var disconnects = 0;
        using var registration = connector.OnDisconnected(
            (closed, _) =>
            {
                // The first notification is the transport dropping; the one that matters here
                // is the one raised once the attempts are spent.
                if (Interlocked.Increment(ref disconnects) > 1)
                    exhausted.TrySetResult(closed);
                return ValueTask.CompletedTask;
            }
        );

        await connector.Connect.Async();
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        var final = await exhausted.Task.WaitAsync(TimeSpan.FromSeconds(10));

        // The last attempt failed, so the state settles at Disconnected and the registered
        // disconnect handler runs (spec §6).
        Assert.Equal(ZlinkStreamConnectionState.Disconnected, connector.State);
        Assert.Equal(ZlinkStreamCloseReason.TransportError, final.CloseReason);
        Assert.Equal(ZlinkStreamCloseReason.TransportError, connector.CloseReason);
    }

    [Fact]
    public void ReconnectDelayAlwaysFallsBetweenHalfAndAllOfTheBaseDelay()
    {
        var baseDelay = TimeSpan.FromMilliseconds(400);

        // The delay arithmetic is separated from the random draw, so the boundaries and
        // the middle are checked with chosen samples instead of a sampling experiment.
        Assert.Equal(
            TimeSpan.FromMilliseconds(200),
            ZlinkStreamConnectorLifecycle.ScaleReconnectDelay(baseDelay, 0.0)
        );
        Assert.Equal(
            TimeSpan.FromMilliseconds(300),
            ZlinkStreamConnectorLifecycle.ScaleReconnectDelay(baseDelay, 0.5)
        );
        Assert.Equal(
            TimeSpan.FromMilliseconds(400).Ticks,
            ZlinkStreamConnectorLifecycle.ScaleReconnectDelay(baseDelay, 1.0).Ticks
        );

        // Every draw Random.Shared can produce stays inside the window (spec §6).
        for (var index = 0; index < 1000; index++)
        {
            var actual = ZlinkStreamConnectorLifecycle.ScaleReconnectDelay(
                baseDelay,
                Random.Shared.NextDouble()
            );
            Assert.InRange(actual, TimeSpan.FromMilliseconds(200), baseDelay);
        }
    }

    [Fact]
    public void MissingEndpointFailsWithAStreamExceptionCarryingValidationFailed()
    {
        var options = new ZlinkStreamConnectorOptions { Endpoint = null! };

        // A standard exception has no place to carry the code, so the caller could not
        // tell ValidationFailed from ConfigurationError (spec §9.2).
        var failure = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamConnectorFactory.Create(options)
        );
        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, failure.Error.Code);
    }

    [Fact]
    public void OptionValidationCoversDispatchModeCompressionAndCodecPairing()
    {
        var undefinedDispatchMode = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri("tcp://127.0.0.1:1"),
                    DispatchMode = (ZlinkStreamDispatchMode)7,
                }
            )
        );
        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, undefinedDispatchMode.Error.Code);

        var undefinedCompression = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri("tcp://127.0.0.1:1"),
                    Compression = (ZlinkStreamCompression)9,
                }
            )
        );
        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, undefinedCompression.Error.Code);

        // Two options that disagree are a ConfigurationError, not an out-of-range value.
        var codecWithoutCompression = Assert.Throws<ZlinkStreamException>(() =>
            ZlinkStreamConnectorFactory.Create(
                new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri("tcp://127.0.0.1:1"),
                    Compression = ZlinkStreamCompression.None,
                    CompressionCodec = new ZlinkStreamLz4CompressionCodec(),
                }
            )
        );
        Assert.Equal(ZlinkStreamErrorCode.ConfigurationError, codecWithoutCompression.Error.Code);
    }

    [Fact]
    public async Task SynchronousSetDiagnosticsLevelReturnsFromInsideAReceiveCallback()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                headerCodec
                    .Encode(
                        new ZlinkStreamHeader(
                            ZlinkStreamMessageKind.Send,
                            ZlinkStreamCodec.Raw,
                            ZlinkStreamHeaderFlags.None,
                            null,
                            "level.change",
                            ZlinkStreamMetadata.Empty
                        )
                    )
                    .ToArray(),
                "body"u8.ToArray()
            );
            await Task.Delay(TimeSpan.FromMilliseconds(200));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            }
        );

        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var subscription = connector.On(
            "level.change",
            (_, _) =>
            {
                // The synchronous surface writes the value and returns; it never blocks on
                // the asynchronous pair, so this cannot wait on its own completion (§13).
                connector.SetDiagnosticsLevel(ZlinkStreamDiagnosticsLevel.Detailed);
                changed.TrySetResult();
                return ValueTask.CompletedTask;
            }
        );

        await connector.Connect.Async();
        await changed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ZlinkStreamDiagnosticsLevel.Detailed, connector.DiagnosticsLevel);
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public void PacketNameAttributeIsInheritedByDerivedPayloadTypes()
    {
        var resolver = new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://127.0.0.1:1"),
        }.NameResolver;

        Assert.Equal("inherited.packet", resolver.Resolve(typeof(InheritedNamePayload)));
        Assert.Equal("inherited.packet", resolver.Resolve(typeof(DerivedInheritedNamePayload)));
        Assert.Equal("own.packet", resolver.Resolve(typeof(OverriddenNamePayload)));
    }

    [Fact]
    public void FlowOriginOrdinalsAreZeroBasedAndConvertToTheOneBasedWireValues()
    {
        // The enum ordinals are 0..3 while the wire values are 1..4, so an integer cast
        // would move the value (.NET spec §11).
        Assert.Equal(
            new[] { 0, 1, 2, 3 },
            Enum.GetValues<ZlinkStreamFlowOrigin>().Select(origin => (int)origin).ToArray()
        );

        Assert.Equal(
            (byte)1,
            ZlinkStreamHeaderCodec.FlowOriginToWire(ZlinkStreamFlowOrigin.Inbound)
        );
        Assert.Equal(
            (byte)4,
            ZlinkStreamHeaderCodec.FlowOriginToWire(ZlinkStreamFlowOrigin.Lifecycle)
        );
        Assert.Equal(ZlinkStreamFlowOrigin.Inbound, ZlinkStreamHeaderCodec.FlowOriginFromWire(1));
        Assert.Equal(ZlinkStreamFlowOrigin.Lifecycle, ZlinkStreamHeaderCodec.FlowOriginFromWire(4));
        Assert.Null(ZlinkStreamHeaderCodec.FlowOriginFromWire(0));
        Assert.Null(ZlinkStreamHeaderCodec.FlowOriginFromWire(5));
    }

    [Fact]
    public async Task ReceivedMessagesExposeTheFlowPairTheFrameCarried()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var flowId = ZlinkStreamFlowId.Create();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                headerCodec
                    .Encode(
                        new ZlinkStreamHeader(
                            ZlinkStreamMessageKind.Send,
                            ZlinkStreamCodec.Raw,
                            ZlinkStreamHeaderFlags.None,
                            null,
                            "flow.notice",
                            ZlinkStreamMetadata.Empty,
                            FlowId: flowId,
                            FlowOrigin: ZlinkStreamFlowOrigin.Timer
                        )
                    )
                    .ToArray(),
                "body"u8.ToArray()
            );
            await Task.Delay(TimeSpan.FromMilliseconds(200));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            }
        );
        await connector.Connect.Async();

        var message = await connector
            .WaitFor("flow.notice")
            .Timeout(TimeSpan.FromSeconds(5))
            .Async();

        Assert.Equal(flowId, message.FlowId);
        Assert.Equal(ZlinkStreamFlowOrigin.Timer, message.FlowOrigin);
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task TypedProjectionKeepsTheFlowPair()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var flowId = ZlinkStreamFlowId.Create();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                headerCodec
                    .Encode(
                        new ZlinkStreamHeader(
                            ZlinkStreamMessageKind.Send,
                            ZlinkStreamCodec.Json,
                            ZlinkStreamHeaderFlags.None,
                            null,
                            nameof(Pong),
                            ZlinkStreamMetadata.Empty,
                            FlowId: flowId,
                            FlowOrigin: ZlinkStreamFlowOrigin.Inbound
                        )
                    )
                    .ToArray(),
                new Pong("typed").ToJson().Payload.ToArray()
            );
            await Task.Delay(TimeSpan.FromMilliseconds(200));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            }
        );
        await connector.Connect.Async();

        // Decoding the payload must not drop the flow pair (spec §5.5).
        var typed = await connector.WaitFor<Pong>().Timeout(TimeSpan.FromSeconds(5)).Async();

        Assert.Equal("typed", typed.Payload.Text);
        Assert.Equal(flowId, typed.FlowId);
        Assert.Equal(ZlinkStreamFlowOrigin.Inbound, typed.FlowOrigin);
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task ReceivedCountSurvivesConsumptionAndRestartsOnEveryConnection()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var header = headerCodec
            .Encode(
                new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "counted",
                    ZlinkStreamMetadata.Empty
                )
            )
            .ToArray();
        var dropFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var server = Task.Run(async () =>
        {
            using (var first = await listener.AcceptTcpClientAsync())
            {
                var stream = first.GetStream();
                await WritePacketAsync(stream, header, "one"u8.ToArray());
                await WritePacketAsync(stream, header, "two"u8.ToArray());
                await dropFirst.Task.WaitAsync(TimeSpan.FromSeconds(5));
                first.Close();
            }

            using var second = await listener.AcceptTcpClientAsync();
            await Task.Delay(TimeSpan.FromMilliseconds(300));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Reconnect = new ZlinkStreamReconnectOptions
                {
                    InitialDelay = TimeSpan.FromMilliseconds(10),
                    MaxDelay = TimeSpan.FromMilliseconds(10),
                    BackoffFactor = 1,
                    MaxAttempts = 5,
                },
            }
        );

        await connector.Connect.Async();
        await WaitUntilAsync(
            () => connector.ReceivedCount("counted") == 2,
            TimeSpan.FromSeconds(5)
        );

        // Consuming one does not lower the count (spec §10).
        await connector.WaitFor("counted").Timeout(TimeSpan.FromSeconds(5)).Async();
        Assert.Equal(2, connector.ReceivedCount("counted"));

        dropFirst.TrySetResult();
        await WaitUntilAsync(
            () =>
                connector.State == ZlinkStreamConnectionState.Connected
                && connector.ReceivedCount("counted") == 0,
            TimeSpan.FromSeconds(10)
        );

        await server.WaitAsync(TimeSpan.FromSeconds(10));
    }

    [ZlinkStreamPacketName("inherited.packet")]
    private class InheritedNamePayload
    {
        public string Text { get; init; } = string.Empty;
    }

    private sealed class DerivedInheritedNamePayload : InheritedNamePayload;

    [ZlinkStreamPacketName("own.packet")]
    private sealed class OverriddenNamePayload : InheritedNamePayload;
}
