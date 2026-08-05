using System.Net;
using System.Net.Sockets;
using System.Buffers.Binary;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Systems.Zlink.Stream.Connector.Runtime.Transport;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public void ReconnectDefaultMaxAttemptsIsThree()
    {
        var options = new ZlinkStreamReconnectOptions();

        Assert.True(options.Enabled);
        Assert.Equal(3, options.MaxAttempts);
    }

    [Fact]
    public void HeartbeatDefaultIsEnabled()
    {
        var options = new ZlinkStreamHeartbeatOptions();

        Assert.True(options.Enabled);
        Assert.Equal(TimeSpan.FromSeconds(1), options.Interval);
        Assert.Equal(TimeSpan.FromSeconds(5), options.Timeout);
    }

    [Fact]
    public async Task HeartbeatSendsReservedControlPing()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var packet = await ReadPacketAsync(stream);
            var header = headerCodec.Decode(packet.Header);
            Assert.Equal(ZlinkStreamMessageKind.Control, header.Kind);
            Assert.Equal("$zlink.heartbeat.ping", header.Name);
            Assert.Empty(packet.Payload);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = new ZlinkStreamHeartbeatOptions
            {
                Interval = TimeSpan.FromMilliseconds(20),
                Timeout = TimeSpan.FromMilliseconds(200)
            }
        });

        await connector.Connect.Async();
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task InboundHeartbeatPingReceivesPongWhenHeartbeatDisabled()
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
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Control,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "$zlink.heartbeat.ping",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                Array.Empty<byte>());

            var packet = await ReadPacketAsync(stream);
            var header = headerCodec.Decode(packet.Header);
            Assert.Equal(ZlinkStreamMessageKind.Control, header.Kind);
            Assert.Equal("$zlink.heartbeat.pong", header.Name);
            Assert.Empty(packet.Payload);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false }
        });

        await connector.Connect.Async();
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SessionClosingPublishesServerDrainReasonAfterDisconnectedState()
    {
        const int repetitions = 32;
        for (var attempt = 0; attempt < repetitions; attempt++)
            await AssertSessionClosingOrderAsync();
    }

    private static async Task AssertSessionClosingOrderAsync()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var payload = new byte[4];
            payload[0] = 1;
            payload[1] = 4;
            BinaryPrimitives.WriteUInt16BigEndian(payload.AsSpan(2, 2), 0);
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Control,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    ZlinkStreamSessionClosingCodec.ControlName,
                    ZlinkStreamMetadata.Empty)).ToArray(),
                payload);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var order = new List<string>();
        var disconnected = new TaskCompletionSource<ZlinkStreamDisconnected>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ConnectionStateChanged += (change, _) =>
        {
            lock (order) order.Add($"state:{change.Current}");
            return ValueTask.CompletedTask;
        };
        connector.Disconnected += (closed, _) =>
        {
            lock (order) order.Add($"disconnected:{closed.CloseReason}");
            disconnected.TrySetResult(closed);
            return ValueTask.CompletedTask;
        };

        await connector.Connect.Async();
        var closed = await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamCloseReason.ServerDrain, closed.CloseReason);
        Assert.Equal(
            new[] { "state:Connecting", "state:Connected", "state:Disconnected", "disconnected:ServerDrain" },
            order);
    }

    [Fact]
    public async Task ImmediateConnectedCallbackCanAwaitCloseWithoutWaitingForItsConnectWork()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = ObserveClientCloseAsync(listener);

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var callbackCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ConnectionStateChanged += async (change, _) =>
        {
            if (change.Current != ZlinkStreamConnectionState.Connected) return;

            await connector.Close.Async();
            callbackCompleted.TrySetResult();
        };

        await Assert.ThrowsAsync<ObjectDisposedException>(async () =>
            await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5)));
        await callbackCompleted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
    }

    [Fact]
    public async Task ImmediateDisconnectedCallbackCanAwaitCloseWithoutWaitingForItsReceiveWork()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var headerCodec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var payload = new byte[] { 1, 4, 0, 0 };
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Control,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    ZlinkStreamSessionClosingCodec.ControlName,
                    ZlinkStreamMetadata.Empty)).ToArray(),
                payload);
            var buffer = new byte[1];
            Assert.Equal(0, await stream.ReadAsync(buffer, 0, buffer.Length));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var callbackCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.Disconnected += async (_, _) =>
        {
            await connector.Close.Async();
            callbackCompleted.TrySetResult();
        };

        var connect = connector.Connect.Async().AsTask();
        await callbackCompleted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await connect.WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
    }

    [Fact]
    public async Task ImmediateReceiveErrorCallbackCanAwaitCloseWithoutWaitingForItsReceiveWork()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(stream, "invalid-header"u8.ToArray(), "payload"u8.ToArray());
            var buffer = new byte[1];
            Assert.Equal(0, await stream.ReadAsync(buffer, 0, buffer.Length));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var callbackCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ErrorReceived += async (error, _) =>
        {
            if (error.Code != ZlinkStreamErrorCode.FrameDecodeFailed) return;

            await connector.Close.Async();
            callbackCompleted.TrySetResult();
        };

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await callbackCompleted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
    }

    [Fact]
    public async Task ImmediateReconnectErrorCallbackCanAwaitCloseWithoutWaitingForItsConnectWork()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            tcp.Close();
            listener.Stop();
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions
            {
                InitialDelay = TimeSpan.FromMilliseconds(10),
                MaxDelay = TimeSpan.FromMilliseconds(10),
                BackoffFactor = 1.0,
                MaxAttempts = 3
            }
        });
        var callbackCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ErrorReceived += async (_, _) =>
        {
            await connector.Close.Async();
            callbackCompleted.TrySetResult();
        };

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));
        await callbackCompleted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
    }

    [Fact]
    public async Task ImmediateTypedPacketCallbackCanAwaitCloseWithoutWaitingForReceiveWorker()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = SendFrameAndObserveClientCloseAsync(
            listener,
            ZlinkStreamMessageKind.Send,
            "close-from-handler",
            Array.Empty<byte>());

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var callbackCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var subscription = connector.On("close-from-handler", async (_, _) =>
        {
            await connector.Close.Async();
            callbackCompleted.TrySetResult();
        });

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await callbackCompleted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
    }

    [Fact]
    public async Task ImmediateUnsolicitedErrorCallbackCanAwaitCloseWithoutWaitingForReceiveWorker()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = SendFrameAndObserveClientCloseAsync(
            listener,
            ZlinkStreamMessageKind.Error,
            "remote-error",
            "{\"code\":\"remote_close\",\"message\":\"close requested\"}"u8.ToArray());

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var callbackCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ErrorReceived += async (error, _) =>
        {
            if (error.Code != ZlinkStreamErrorCode.RemoteError) return;

            await connector.Close.Async();
            callbackCompleted.TrySetResult();
        };

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await callbackCompleted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
    }

    [Fact]
    public async Task ImmediateUserCallbackFailureHandlerCanAwaitCloseWithoutWaitingForReceiveWorker()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = SendFrameAndObserveClientCloseAsync(
            listener,
            ZlinkStreamMessageKind.Send,
            "throw-from-handler",
            Array.Empty<byte>());

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var callbackCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ErrorReceived += async (error, _) =>
        {
            if (error.Code != ZlinkStreamErrorCode.UserCallbackFailed) return;

            await connector.Close.Async();
            callbackCompleted.TrySetResult();
        };
        using var subscription = connector.On("throw-from-handler", (_, _) =>
            throw new InvalidOperationException("expected callback failure"));

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await callbackCompleted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
    }

    [Fact]
    public async Task DetachedChildDisposeAfterCallbackReturnAwaitsWorkerTerminationAndSharedFinalizer()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var clientCloseObserved = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var headerCodec = new ZlinkStreamHeaderCodec();
            foreach (var name in new[] { "spawn-dispose", "block-worker" })
                await WritePacketAsync(
                    stream,
                    headerCodec.Encode(new ZlinkStreamHeader(
                        ZlinkStreamMessageKind.Send,
                        ZlinkStreamCodec.Raw,
                        ZlinkStreamHeaderFlags.None,
                        null,
                        name,
                        ZlinkStreamMetadata.Empty)).ToArray(),
                    Array.Empty<byte>());
            var buffer = new byte[1];
            Assert.Equal(0, await stream.ReadAsync(buffer, 0, buffer.Length));
            clientCloseObserved.TrySetResult();
        });

        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var releaseChild = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseWorker = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var workerBlocked = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var childDisposeCreated = new TaskCompletionSource<Task>(TaskCreationOptions.RunContinuationsAsynchronously);
        using var spawnSubscription = connector.On("spawn-dispose", (_, _) =>
        {
            var childDispose = Task.Run(async () =>
            {
                await releaseChild.Task;
                await connector.DisposeAsync();
            });
            childDisposeCreated.TrySetResult(childDispose);
            return ValueTask.CompletedTask;
        });
        using var blockSubscription = connector.On("block-worker", async (_, _) =>
        {
            workerBlocked.TrySetResult();
            await releaseWorker.Task;
        });

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        var childDispose = await childDisposeCreated.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await workerBlocked.Task.WaitAsync(TimeSpan.FromSeconds(5));
        releaseChild.TrySetResult();
        await clientCloseObserved.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var externalDispose = connector.DisposeAsync().AsTask();

        Assert.False(childDispose.IsCompleted);
        Assert.False(externalDispose.IsCompleted);

        releaseWorker.TrySetResult();
        await childDispose.WaitAsync(TimeSpan.FromSeconds(5));
        await externalDispose.WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task AwaitedChildCloseDuringCallbackRetainsReentrantPermit()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = SendFrameAndObserveClientCloseAsync(
            listener,
            ZlinkStreamMessageKind.Send,
            "await-child-close",
            Array.Empty<byte>());

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var callbackCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var subscription = connector.On("await-child-close", async (_, _) =>
        {
            await Task.Run(async () => await connector.Close.Async());
            callbackCompleted.TrySetResult();
        });

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await callbackCompleted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
    }

    [Fact]
    public async Task CallbackPermitDoesNotExcludeAnotherConnectorWorker()
    {
        using var listenerA = new TcpListener(IPAddress.Loopback, 0);
        using var listenerB = new TcpListener(IPAddress.Loopback, 0);
        listenerA.Start();
        listenerB.Start();
        var endpointA = (IPEndPoint)listenerA.LocalEndpoint;
        var endpointB = (IPEndPoint)listenerB.LocalEndpoint;
        var serverA = SendSingleFrameAsync(listenerA, "close-other");
        var serverB = SendFrameAndObserveClientCloseAsync(
            listenerB,
            ZlinkStreamMessageKind.Send,
            "block-other",
            Array.Empty<byte>());

        await using var connectorA = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpointA.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        await using var connectorB = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpointB.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var releaseB = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var workerBBlocked = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var closeBStarted = new TaskCompletionSource<Task>(TaskCreationOptions.RunContinuationsAsynchronously);
        using var subscriptionB = connectorB.On("block-other", async (_, _) =>
        {
            workerBBlocked.TrySetResult();
            await releaseB.Task;
        });
        using var subscriptionA = connectorA.On("close-other", async (_, _) =>
        {
            var closeB = connectorB.Close.Async().AsTask();
            closeBStarted.TrySetResult(closeB);
            await closeB;
        });

        await connectorB.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await workerBBlocked.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await connectorA.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        var closeB = await closeBStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await serverB.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.False(closeB.IsCompleted);

        releaseB.TrySetResult();
        await closeB.WaitAsync(TimeSpan.FromSeconds(5));
        await serverA.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task CanceledClosePublishesTerminalStateWhileTerminationContinues()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = SendFrameAndObserveClientCloseAsync(
            listener,
            ZlinkStreamMessageKind.Send,
            "block-canceled-close",
            Array.Empty<byte>());

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var releaseWorker = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var workerBlocked = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var disconnected = new TaskCompletionSource<ZlinkStreamDisconnected>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        connector.Disconnected += (closed, _) =>
        {
            disconnected.TrySetResult(closed);
            return ValueTask.CompletedTask;
        };
        using var subscription = connector.On("block-canceled-close", async (_, _) =>
        {
            workerBlocked.TrySetResult();
            await releaseWorker.Task;
        });

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await workerBlocked.Task.WaitAsync(TimeSpan.FromSeconds(5));
        using var canceled = new CancellationTokenSource();
        canceled.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await connector.Close.Async(canceled.Token));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
        Assert.False(disconnected.Task.IsCompleted);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await connector.Close.Async(canceled.Token));

        releaseWorker.TrySetResult();
        await connector.Close.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        var closed = await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ZlinkStreamCloseReason.ClientClose, closed.CloseReason);
    }

    [Fact]
    public async Task DisposeInsideCallbackIsRejectedWithoutStartingFinalization()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = SendFrameAndObserveClientCloseAsync(
            listener,
            ZlinkStreamMessageKind.Send,
            "reject-dispose",
            Array.Empty<byte>());

        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var rejected = new TaskCompletionSource<InvalidOperationException>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        using var subscription = connector.On("reject-dispose", async (_, _) =>
        {
            try
            {
                await connector.DisposeAsync();
            }
            catch (InvalidOperationException ex)
            {
                rejected.TrySetResult(ex);
            }
        });

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        var exception = await rejected.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Contains("Close.Async", exception.Message, StringComparison.Ordinal);
        Assert.Equal(ZlinkStreamConnectionState.Connected, connector.State);

        await connector.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));
        await connector.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task ManualCallbacksRejectDisposeUntilDispatchCallbackReturns()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var framesWritten = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var headerCodec = new ZlinkStreamHeaderCodec();
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "manual-dispose",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                Array.Empty<byte>());
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Error,
                    ZlinkStreamCodec.Json,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    string.Empty,
                    ZlinkStreamMetadata.Empty)).ToArray(),
                "{\"code\":\"manual_error\",\"message\":\"manual error\"}"u8.ToArray());
            framesWritten.TrySetResult();
            var buffer = new byte[1];
            Assert.Equal(0, await stream.ReadAsync(buffer, 0, buffer.Length));
        });
        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Manual,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var stateRejected = new TaskCompletionSource<InvalidOperationException>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var typedRejected = new TaskCompletionSource<InvalidOperationException>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var errorRejected = new TaskCompletionSource<InvalidOperationException>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ConnectionStateChanged += async (change, _) =>
        {
            if (change.Current == ZlinkStreamConnectionState.Connecting)
                await CaptureDisposeRejectionAsync(stateRejected);
        };
        using var subscription = connector.On("manual-dispose", async (_, _) =>
            await CaptureDisposeRejectionAsync(typedRejected));
        connector.ErrorReceived += async (error, _) =>
        {
            if (error.Code == ZlinkStreamErrorCode.RemoteError)
                await CaptureDisposeRejectionAsync(errorRejected);
        };

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await framesWritten.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(
            () => connector.PendingDispatchCount >= 4,
            TimeSpan.FromSeconds(5));
        Assert.Equal(0, connector.ReceivedCount("manual-dispose"));
        await connector.Dispatch.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));

        foreach (var rejection in new[] { stateRejected.Task, typedRejected.Task, errorRejected.Task })
        {
            var exception = await rejection.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Contains("Close.Async", exception.Message, StringComparison.Ordinal);
        }

        Assert.Equal(ZlinkStreamConnectionState.Connected, connector.State);
        await connector.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        async ValueTask CaptureDisposeRejectionAsync(
            TaskCompletionSource<InvalidOperationException> completion)
        {
            try
            {
                await connector.DisposeAsync();
            }
            catch (InvalidOperationException ex)
            {
                completion.TrySetResult(ex);
            }
        }
    }

    [Fact]
    public async Task DetachedRequestCompletionCallbackRejectsDisposeUntilCallbackReturns()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var request = await ReadPacketAsync(stream);
            var requestHeader = new ZlinkStreamHeaderCodec().Decode(request.Header);
            var responseHeader = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                requestHeader.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty);
            await WritePacketAsync(
                stream,
                new ZlinkStreamHeaderCodec().Encode(responseHeader).ToArray(),
                Array.Empty<byte>());
            var buffer = new byte[1];
            Assert.Equal(0, await stream.ReadAsync(buffer, 0, buffer.Length));
        });
        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var rejected = new TaskCompletionSource<InvalidOperationException>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        connector.Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, Array.Empty<byte>()))
            .PacketName("detached-dispose-request")
            .Submit((ZlinkStreamResult<ZlinkStreamEncodedPayload> result) =>
            {
                _ = result;
                try
                {
                    _ = connector.DisposeAsync();
                }
                catch (InvalidOperationException ex)
                {
                    rejected.TrySetResult(ex);
                }
            });

        var exception = await rejected.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Contains("Close.Async", exception.Message, StringComparison.Ordinal);
        Assert.Equal(ZlinkStreamConnectionState.Connected, connector.State);

        await connector.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task SharedCloseWaitsForTerminalCallbacksAndSupportsSelfClose()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = ObserveClientCloseAsync(listener);
        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var releaseTerminal = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var closedEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var disconnectedSelfClose = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new List<string>();
        connector.ConnectionStateChanged += async (change, _) =>
        {
            if (change.Current != ZlinkStreamConnectionState.Closed) return;

            lock (order) order.Add("closed");
            closedEntered.TrySetResult();
            await releaseTerminal.Task;
            await connector.Close.Async();
        };
        connector.Disconnected += async (_, _) =>
        {
            lock (order) order.Add("disconnected");
            await connector.Close.Async();
            disconnectedSelfClose.TrySetResult();
        };

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        var firstClose = connector.Close.Async().AsTask();
        await closedEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var repeatedClose = connector.Close.Async().AsTask();
        var dispose = connector.DisposeAsync().AsTask();
        using var canceled = new CancellationTokenSource();
        canceled.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await connector.Close.Async(canceled.Token));
        Assert.False(firstClose.IsCompleted);
        Assert.False(repeatedClose.IsCompleted);
        Assert.False(dispose.IsCompleted);

        releaseTerminal.TrySetResult();
        await firstClose.WaitAsync(TimeSpan.FromSeconds(5));
        await repeatedClose.WaitAsync(TimeSpan.FromSeconds(5));
        await dispose.WaitAsync(TimeSpan.FromSeconds(5));
        await disconnectedSelfClose.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(new[] { "closed", "disconnected" }, order);
    }

    [Fact]
    public async Task SharedCloseFaultIsObservedByRepeatedCloseAndDispose()
    {
        var closeFailure = new InvalidOperationException("expected transport close failure");
        var connection = new FaultingCloseConnection(closeFailure);
        var connector = new ZlinkStreamConnector(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                DispatchMode = ZlinkStreamDispatchMode.Immediate,
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
            },
            _ => ValueTask.FromResult<IZlinkStreamConnection>(connection));

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        var first = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await connector.Close.Async());
        var repeated = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await connector.Close.Async());
        var disposed = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await connector.DisposeAsync());

        Assert.Same(closeFailure, first);
        Assert.Same(first, repeated);
        Assert.Same(first, disposed);
        Assert.Equal(1, connection.CloseCount);
    }

    [Fact]
    public async Task OneWayAsync_Waits_For_Bounded_Queue_Admission()
    {
        var connection = new BlockingWriteConnection();
        await using var connector = new ZlinkStreamConnector(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
            },
            _ => ValueTask.FromResult<IZlinkStreamConnection>(connection));
        await connector.Connect.Async();

        await Submit(0);
        await connection.WriteStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        for (var index = 1; index <= 4096; index++) await Submit(index);

        var pending = Submit(4097);
        Assert.False(pending.IsCompleted);

        connection.ReleaseWrite.TrySetResult();
        await pending.WaitAsync(TimeSpan.FromSeconds(5));

        Task Submit(int value)
        {
            return connector.Send(new ZlinkStreamEncodedPayload(
                    ZlinkStreamCodec.Raw,
                    BitConverter.GetBytes(value)))
                .PacketName("bounded.send")
                .Async()
                .AsTask();
        }
    }

    [Fact]
    public async Task Dispose_Drains_Accepted_OneWay_Send_Before_Closing_Transport()
    {
        var connection = new OrderedDisposeConnection();
        var connector = new ZlinkStreamConnector(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
            },
            _ => ValueTask.FromResult<IZlinkStreamConnection>(connection));
        await connector.Connect.Async();

        await connector.Send(new ZlinkStreamEncodedPayload(
                ZlinkStreamCodec.Raw,
                new byte[] { 1 }))
            .PacketName("dispose.drain")
            .Async();
        await connection.WriteStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var dispose = connector.DisposeAsync().AsTask();
        await Task.Yield();
        Assert.False(connection.CloseStarted.Task.IsCompleted);
        Assert.False(dispose.IsCompleted);

        connection.ReleaseWrite.TrySetResult();
        await dispose.WaitAsync(TimeSpan.FromSeconds(5));
        await connection.CloseStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task RequestQueueWaitsForEarlierAcceptedOneWaySend()
    {
        using var shutdown = new CancellationTokenSource();
        var taskRunner = new ZlinkStreamTaskRunner(shutdown.Token);
        var callbacks = new ZlinkStreamConnectorCallbacks(
            taskRunner,
            ZlinkStreamDispatchMode.Immediate,
            32);
        var firstEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseFirst = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new List<byte>();
        var queue = new ZlinkStreamOneWaySubmitQueue(
            taskRunner,
            callbacks,
            async (frame, _) =>
            {
                var value = frame.PayloadBytes.Span[0];
                lock (order) order.Add(value);
                if (value != 1) return;
                firstEntered.TrySetResult();
                await releaseFirst.Task;
            });

        await queue.SubmitAsync(
            new ZlinkStreamOutboundFrame(ReadOnlyMemory<byte>.Empty, new byte[] { 1 }),
            CancellationToken.None);
        await firstEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var requestWrite = queue.SendAsync(
                new ZlinkStreamOutboundFrame(ReadOnlyMemory<byte>.Empty, new byte[] { 2 }),
                CancellationToken.None)
            .AsTask();

        Assert.False(requestWrite.IsCompleted);
        releaseFirst.TrySetResult();
        await requestWrite.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(new byte[] { 1, 2 }, order);

        queue.Complete();
        await queue.WaitForCompletionAsync();
        shutdown.Cancel();
    }

    [Fact]
    public async Task CallerCancellationDoesNotInterruptAnInProgressFrameWrite()
    {
        using var shutdown = new CancellationTokenSource();
        using var caller = new CancellationTokenSource();
        var taskRunner = new ZlinkStreamTaskRunner(shutdown.Token);
        var callbacks = new ZlinkStreamConnectorCallbacks(
            taskRunner,
            ZlinkStreamDispatchMode.Immediate,
            32);
        var writeEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseWrite = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var completedWrites = 0;
        var queue = new ZlinkStreamOneWaySubmitQueue(
            taskRunner,
            callbacks,
            async (_, _) =>
            {
                writeEntered.TrySetResult();
                await releaseWrite.Task;
                Interlocked.Increment(ref completedWrites);
            });

        var callerWait = queue.SendAsync(
                new ZlinkStreamOutboundFrame(ReadOnlyMemory<byte>.Empty, new byte[] { 1 }),
                caller.Token)
            .AsTask();
        await writeEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        caller.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () => await callerWait);

        releaseWrite.TrySetResult();
        await queue.SendAsync(
            new ZlinkStreamOutboundFrame(ReadOnlyMemory<byte>.Empty, new byte[] { 2 }),
            CancellationToken.None);
        Assert.Equal(2, Volatile.Read(ref completedWrites));

        queue.Complete();
        await queue.WaitForCompletionAsync();
        shutdown.Cancel();
    }

    [Fact]
    public async Task TransportErrorCloseFaultCompletesTerminalWorkAndStartsReconnect()
    {
        var closeFailure = new InvalidOperationException("expected transport close failure");
        var firstConnection = new FaultingCloseConnection(closeFailure);
        var secondConnection = new RecordingCloseConnection();
        var reconnectStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var connectCount = 0;
        using var shutdown = new CancellationTokenSource();
        var taskRunner = new ZlinkStreamTaskRunner(shutdown.Token);
        var pending = new ZlinkStreamPendingRequests();
        var callbacks = new ZlinkStreamConnectorCallbacks(taskRunner, ZlinkStreamDispatchMode.Immediate, 32);
        var lifecycle = new ZlinkStreamConnectorLifecycle(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions
                {
                    InitialDelay = TimeSpan.FromMilliseconds(1),
                    MaxDelay = TimeSpan.FromMilliseconds(1),
                    BackoffFactor = 1.0,
                    MaxAttempts = 1
                }
            },
            pending,
            taskRunner,
            callbacks,
            _ =>
            {
                if (Interlocked.Increment(ref connectCount) == 1)
                    return ValueTask.FromResult<IZlinkStreamConnection>(firstConnection);

                reconnectStarted.TrySetResult();
                return ValueTask.FromResult<IZlinkStreamConnection>(secondConnection);
            });
        var states = new List<ZlinkStreamConnectionState>();
        var disconnected = new TaskCompletionSource<ZlinkStreamDisconnected>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        callbacks.AddConnectionStateChanged((change, _) =>
        {
            lock (states) states.Add(change.Current);
            return ValueTask.CompletedTask;
        });
        callbacks.AddDisconnected((closed, _) =>
        {
            disconnected.TrySetResult(closed);
            return ValueTask.CompletedTask;
        });
        await lifecycle.ConnectAsync(
            token => Task.Delay(Timeout.InfiniteTimeSpan, token),
            _ => ValueTask.CompletedTask,
            () => { },
            CancellationToken.None);
        var request = pending.Create("pending.request");
        var pendingCompletion = pending.WaitAsync(request, CancellationToken.None).AsTask();

        var observed = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await lifecycle.HandleTransportErrorAsync(
                new ZlinkStreamError(ZlinkStreamErrorCode.Disconnected, "transport failed")));

        Assert.Same(closeFailure, observed);
        Assert.True(pendingCompletion.IsCompleted);
        var pendingFailure = await Assert.ThrowsAsync<ZlinkStreamException>(async () => await pendingCompletion);
        Assert.Equal(ZlinkStreamErrorCode.Disconnected, pendingFailure.Error.Code);
        Assert.Equal(
            ZlinkStreamCloseReason.TransportError,
            (await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5))).CloseReason);
        await reconnectStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(
            () => lifecycle.State == ZlinkStreamConnectionState.Connected,
            TimeSpan.FromSeconds(5));
        Assert.Contains(ZlinkStreamConnectionState.Reconnecting, states);
        Assert.Equal(1, firstConnection.CloseCount);

        await lifecycle.CloseAsync(CancellationToken.None);
        lifecycle.Dispose();
        shutdown.Cancel();
        Assert.Equal(1, secondConnection.CloseCount);
    }

    [Fact]
    public async Task ServerCloseFaultStillReportsCallbacksAndFailsPendingRequest()
    {
        var closeFailure = new InvalidOperationException("expected server-close transport failure");
        var connection = new FaultingCloseConnection(closeFailure);
        using var shutdown = new CancellationTokenSource();
        var taskRunner = new ZlinkStreamTaskRunner(shutdown.Token);
        var pending = new ZlinkStreamPendingRequests();
        var callbacks = new ZlinkStreamConnectorCallbacks(taskRunner, ZlinkStreamDispatchMode.Immediate, 32);
        var lifecycle = new ZlinkStreamConnectorLifecycle(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
            },
            pending,
            taskRunner,
            callbacks,
            _ => ValueTask.FromResult<IZlinkStreamConnection>(connection));
        var disconnected = new TaskCompletionSource<ZlinkStreamDisconnected>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var callbackFailures = new List<ZlinkStreamError>();
        callbacks.AddErrorReceived((error, _) =>
        {
            if (error.Code == ZlinkStreamErrorCode.UserCallbackFailed)
                lock (callbackFailures) callbackFailures.Add(error);
            return ValueTask.CompletedTask;
        });
        callbacks.AddConnectionStateChanged((change, _) =>
        {
            if (change.Current == ZlinkStreamConnectionState.Disconnected)
                throw new InvalidOperationException("expected state callback failure");
            return ValueTask.CompletedTask;
        });
        callbacks.AddDisconnected((closed, _) =>
        {
            disconnected.TrySetResult(closed);
            throw new InvalidOperationException("expected disconnected callback failure");
        });
        await lifecycle.ConnectAsync(
            token => Task.Delay(Timeout.InfiniteTimeSpan, token),
            _ => ValueTask.CompletedTask,
            () => { },
            CancellationToken.None);
        var request = pending.Create("pending.request");
        var pendingCompletion = pending.WaitAsync(request, CancellationToken.None).AsTask();

        var observed = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await lifecycle.HandleServerCloseAsync(
                ZlinkStreamCloseReason.ServerDrain,
                "server drain"));

        Assert.Same(closeFailure, observed);
        Assert.Equal(ZlinkStreamConnectionState.Disconnected, lifecycle.State);
        Assert.True(pendingCompletion.IsCompleted);
        await Assert.ThrowsAsync<ZlinkStreamException>(async () => await pendingCompletion);
        Assert.Equal(
            ZlinkStreamCloseReason.ServerDrain,
            (await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5))).CloseReason);
        Assert.Equal(2, callbackFailures.Count);
        Assert.Equal(1, connection.CloseCount);

        await lifecycle.CloseAsync(CancellationToken.None);
        lifecycle.Dispose();
        shutdown.Cancel();
        Assert.Equal(1, connection.CloseCount);
    }

    [Fact]
    public async Task ReceiveEofCloseFailureDisconnectsOnceAndPreservesReconnectedSession()
    {
        var closeFailure = new InvalidOperationException("expected EOF close failure");
        var firstConnection = new FaultingCloseConnection(closeFailure);
        var secondConnection = new RecordingCloseConnection();
        var connectCount = 0;
        var receiveCount = 0;
        using var shutdown = new CancellationTokenSource();
        var taskRunner = new ZlinkStreamTaskRunner(shutdown.Token);
        var callbacks = new ZlinkStreamConnectorCallbacks(taskRunner, ZlinkStreamDispatchMode.Immediate, 32);
        var lifecycle = new ZlinkStreamConnectorLifecycle(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions
                {
                    InitialDelay = TimeSpan.FromMilliseconds(1),
                    MaxDelay = TimeSpan.FromMilliseconds(1),
                    BackoffFactor = 1.0,
                    MaxAttempts = 1
                }
            },
            new ZlinkStreamPendingRequests(),
            taskRunner,
            callbacks,
            _ => ValueTask.FromResult<IZlinkStreamConnection>(
                Interlocked.Increment(ref connectCount) == 1 ? firstConnection : secondConnection));
        var disconnectCount = 0;
        var disconnected = new TaskCompletionSource<ZlinkStreamDisconnected>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var frameDecodeErrors = 0;
        callbacks.AddDisconnected((closed, _) =>
        {
            Interlocked.Increment(ref disconnectCount);
            disconnected.TrySetResult(closed);
            return ValueTask.CompletedTask;
        });
        callbacks.AddErrorReceived((error, _) =>
        {
            if (error.Code == ZlinkStreamErrorCode.FrameDecodeFailed)
                Interlocked.Increment(ref frameDecodeErrors);
            return ValueTask.CompletedTask;
        });

        await lifecycle.ConnectAsync(
            token => Interlocked.Increment(ref receiveCount) == 1
                ? Task.CompletedTask
                : Task.Delay(Timeout.InfiniteTimeSpan, token),
            _ => ValueTask.CompletedTask,
            () => { },
            CancellationToken.None);
        var closed = await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(
            () => lifecycle.State == ZlinkStreamConnectionState.Connected &&
                  ReferenceEquals(lifecycle.Connection, secondConnection),
            TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamCloseReason.TransportError, closed.CloseReason);
        Assert.Equal(1, disconnectCount);
        Assert.Equal(0, frameDecodeErrors);
        Assert.Equal(1, firstConnection.CloseCount);
        Assert.Same(secondConnection, lifecycle.Connection);

        await lifecycle.CloseAsync(CancellationToken.None);
        lifecycle.Dispose();
        shutdown.Cancel();
        Assert.Equal(1, secondConnection.CloseCount);
    }

    [Fact]
    public async Task ReceiveServerCloseFaultIsNotReclassifiedAsFrameDecodeFailure()
    {
        var closeFailure = new InvalidOperationException("expected server close failure");
        var connection = new FaultingCloseConnection(closeFailure);
        using var shutdown = new CancellationTokenSource();
        var taskRunner = new ZlinkStreamTaskRunner(shutdown.Token);
        var callbacks = new ZlinkStreamConnectorCallbacks(taskRunner, ZlinkStreamDispatchMode.Immediate, 32);
        ZlinkStreamConnectorLifecycle? lifecycle = null;
        lifecycle = new ZlinkStreamConnectorLifecycle(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
            },
            new ZlinkStreamPendingRequests(),
            taskRunner,
            callbacks,
            _ => ValueTask.FromResult<IZlinkStreamConnection>(connection));
        var disconnectCount = 0;
        var disconnected = new TaskCompletionSource<ZlinkStreamDisconnected>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var frameDecodeErrors = 0;
        callbacks.AddDisconnected((closed, _) =>
        {
            Interlocked.Increment(ref disconnectCount);
            disconnected.TrySetResult(closed);
            return ValueTask.CompletedTask;
        });
        callbacks.AddErrorReceived((error, _) =>
        {
            if (error.Code == ZlinkStreamErrorCode.FrameDecodeFailed)
                Interlocked.Increment(ref frameDecodeErrors);
            return ValueTask.CompletedTask;
        });

        await lifecycle.ConnectAsync(
            token => lifecycle!.HandleServerCloseAsync(
                    ZlinkStreamCloseReason.ServerDrain,
                    "server drain",
                    token)
                .AsTask(),
            _ => ValueTask.CompletedTask,
            () => { },
            CancellationToken.None);
        var closed = await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamCloseReason.ServerDrain, closed.CloseReason);
        Assert.Equal(ZlinkStreamConnectionState.Disconnected, lifecycle.State);
        Assert.Equal(1, disconnectCount);
        Assert.Equal(0, frameDecodeErrors);
        Assert.Equal(1, connection.CloseCount);

        await lifecycle.CloseAsync(CancellationToken.None);
        lifecycle.Dispose();
        shutdown.Cancel();
    }

    [Fact]
    public async Task AsyncStateSubscribersRunInRegistrationOrderAndCanSelfClose()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = ObserveClientCloseAsync(listener);
        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var releaseFirst = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var firstEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var secondEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new List<string>();
        connector.ConnectionStateChanged += async (change, _) =>
        {
            if (change.Current != ZlinkStreamConnectionState.Connected) return;

            lock (order) order.Add("first-enter");
            firstEntered.TrySetResult();
            await releaseFirst.Task;
            await connector.Close.Async();
            lock (order) order.Add("first-exit");
        };
        connector.ConnectionStateChanged += (change, _) =>
        {
            if (change.Current == ZlinkStreamConnectionState.Connected)
            {
                lock (order) order.Add("second");
                secondEntered.TrySetResult();
            }

            return ValueTask.CompletedTask;
        };

        var connect = connector.Connect.Async().AsTask();
        await firstEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(secondEntered.Task.IsCompleted);

        releaseFirst.TrySetResult();
        await Assert.ThrowsAsync<ObjectDisposedException>(async () => await connect);
        await secondEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await connector.Close.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(new[] { "first-enter", "first-exit", "second" }, order);
    }

    [Fact]
    public async Task SubscriberFailureReportsOnceAndContinuesRemainingSubscribers()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var serverRelease = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await serverRelease.Task;
        });
        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var order = new List<string>();
        var secondStateSubscriber = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var reported = new TaskCompletionSource<ZlinkStreamError>(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ErrorReceived += async (error, _) =>
        {
            if (error.Code != ZlinkStreamErrorCode.UserCallbackFailed) return;
            lock (order) order.Add("error-first");
            await Task.Yield();
            throw new InvalidOperationException("secondary error handler failure");
        };
        connector.ErrorReceived += (error, _) =>
        {
            if (error.Code == ZlinkStreamErrorCode.UserCallbackFailed)
            {
                lock (order) order.Add("error-second");
                reported.TrySetResult(error);
            }

            return ValueTask.CompletedTask;
        };
        connector.ConnectionStateChanged += (change, _) =>
        {
            if (change.Current == ZlinkStreamConnectionState.Connecting)
            {
                lock (order) order.Add("state-first");
                throw new InvalidOperationException("primary subscriber failure");
            }

            return ValueTask.CompletedTask;
        };
        connector.ConnectionStateChanged += (change, _) =>
        {
            if (change.Current == ZlinkStreamConnectionState.Connecting)
            {
                lock (order) order.Add("state-second");
                secondStateSubscriber.TrySetResult();
            }

            return ValueTask.CompletedTask;
        };

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await secondStateSubscriber.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var error = await reported.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamErrorCode.UserCallbackFailed, error.Code);
        Assert.Equal(
            new[] { "state-first", "error-first", "error-second", "state-second" },
            order);

        serverRelease.TrySetResult();
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task ManualConnectedCallbackCanAwaitCloseFromDispatch()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = ObserveClientCloseAsync(listener);

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Manual,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var callbackCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ConnectionStateChanged += async (change, _) =>
        {
            if (change.Current != ZlinkStreamConnectionState.Connected) return;

            await connector.Close.Async();
            callbackCompleted.TrySetResult();
        };

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await connector.Dispatch.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await callbackCompleted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
    }

    [Fact]
    public async Task ManualDisconnectedCallbackCanAwaitCloseFromDispatch()
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
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Control,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    ZlinkStreamSessionClosingCodec.ControlName,
                    ZlinkStreamMetadata.Empty)).ToArray(),
                new byte[] { 1, 4, 0, 0 });
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Manual,
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            Reconnect = new ZlinkStreamReconnectOptions { Enabled = false }
        });
        var callbackCompleted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var callbackFailure = new TaskCompletionSource<Exception>(TaskCreationOptions.RunContinuationsAsynchronously);
        var reportedErrors = new List<ZlinkStreamError>();
        connector.Disconnected += async (_, _) =>
        {
            try
            {
                await connector.Close.Async();
                callbackCompleted.TrySetResult();
            }
            catch (Exception exception)
            {
                callbackFailure.TrySetResult(exception);
                throw;
            }
        };
        connector.ErrorReceived += (error, _) =>
        {
            lock (reportedErrors) reportedErrors.Add(error);
            return ValueTask.CompletedTask;
        };

        await connector.Connect.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await server.WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(
            () => connector.State == ZlinkStreamConnectionState.Disconnected &&
                  connector.PendingDispatchCount >= 1,
            TimeSpan.FromSeconds(5));
        await connector.Dispatch.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));

        var callbackException = callbackFailure.Task.IsCompleted
            ? await callbackFailure.Task
            : null;
        Assert.True(
            callbackCompleted.Task.IsCompleted,
            callbackException is not null
                ? $"Disconnected callback Close failed: {callbackException}"
                : "Disconnected callback was not dispatched.");
        Assert.False(callbackFailure.Task.IsCompleted);
        lock (reportedErrors) Assert.Empty(reportedErrors);
        Assert.Equal(ZlinkStreamConnectionState.Closed, connector.State);
    }

    private static async Task ObserveClientCloseAsync(TcpListener listener)
    {
        using var tcp = await listener.AcceptTcpClientAsync();
        await using var stream = tcp.GetStream();
        var buffer = new byte[1];
        Assert.Equal(0, await stream.ReadAsync(buffer, 0, buffer.Length));
    }

    private static async Task SendFrameAndObserveClientCloseAsync(
        TcpListener listener,
        ZlinkStreamMessageKind kind,
        string name,
        byte[] payload)
    {
        using var tcp = await listener.AcceptTcpClientAsync();
        await using var stream = tcp.GetStream();
        var headerCodec = new ZlinkStreamHeaderCodec();
        await WritePacketAsync(
            stream,
            headerCodec.Encode(new ZlinkStreamHeader(
                kind,
                kind == ZlinkStreamMessageKind.Error ? ZlinkStreamCodec.Json : ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                kind is ZlinkStreamMessageKind.Response or ZlinkStreamMessageKind.Error
                    ? string.Empty
                    : name,
                ZlinkStreamMetadata.Empty)).ToArray(),
            payload);
        var buffer = new byte[1];
        Assert.Equal(0, await stream.ReadAsync(buffer, 0, buffer.Length));
    }

    private static async Task SendSingleFrameAsync(TcpListener listener, string name)
    {
        using var tcp = await listener.AcceptTcpClientAsync();
        await using var stream = tcp.GetStream();
        var headerCodec = new ZlinkStreamHeaderCodec();
        await WritePacketAsync(
            stream,
            headerCodec.Encode(new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                name,
                ZlinkStreamMetadata.Empty)).ToArray(),
            Array.Empty<byte>());
    }

    private sealed class FaultingCloseConnection(Exception closeFailure) : IZlinkStreamConnection
    {
        public int CloseCount { get; private set; }

        public bool CanWriteSegments => true;

        public async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken)
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            return 0;
        }

        public ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask CloseAsync(CancellationToken cancellationToken)
        {
            CloseCount++;
            return ValueTask.FromException(closeFailure);
        }
    }

    private sealed class RecordingCloseConnection : IZlinkStreamConnection
    {
        public int CloseCount { get; private set; }

        public bool CanWriteSegments => true;

        public async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken)
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            return 0;
        }

        public ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;

        public ValueTask CloseAsync(CancellationToken cancellationToken)
        {
            CloseCount++;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class BlockingWriteConnection : IZlinkStreamConnection
    {
        public TaskCompletionSource WriteStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseWrite { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public bool CanWriteSegments => true;

        public async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken)
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            return 0;
        }

        public async ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken)
        {
            WriteStarted.TrySetResult();
            await ReleaseWrite.Task.WaitAsync(cancellationToken);
        }

        public ValueTask CloseAsync(CancellationToken cancellationToken)
        {
            ReleaseWrite.TrySetResult();
            return ValueTask.CompletedTask;
        }
    }

    private sealed class OrderedDisposeConnection : IZlinkStreamConnection
    {
        public TaskCompletionSource WriteStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource ReleaseWrite { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource CloseStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public bool CanWriteSegments => true;

        public async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken)
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            return 0;
        }

        public async ValueTask WriteAsync(
            ReadOnlyMemory<byte> buffer,
            CancellationToken cancellationToken)
        {
            WriteStarted.TrySetResult();
            await ReleaseWrite.Task.WaitAsync(cancellationToken);
        }

        public ValueTask CloseAsync(CancellationToken cancellationToken)
        {
            CloseStarted.TrySetResult();
            return ValueTask.CompletedTask;
        }
    }

    [Fact]
    public async Task ReconnectRestoresConnectionAfterTransportClose()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var secondConnected = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var reconnectedObserved = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using (var first = await listener.AcceptTcpClientAsync())
            {
                first.Close();
            }

            using var second = await listener.AcceptTcpClientAsync();
            secondConnected.SetResult();
            await reconnectedObserved.Task.WaitAsync(TimeSpan.FromSeconds(5));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Reconnect = new ZlinkStreamReconnectOptions
            {
                InitialDelay = TimeSpan.FromMilliseconds(10),
                MaxDelay = TimeSpan.FromMilliseconds(20),
                BackoffFactor = 1.0,
                MaxAttempts = 3
            }
        });
        await connector.Connect.Async();
        await secondConnected.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(
            () => connector.State == ZlinkStreamConnectionState.Connected,
            TimeSpan.FromSeconds(5));
        reconnectedObserved.SetResult();

        Assert.True(connector.IsConnected);
        Assert.Equal(ZlinkStreamConnectionState.Connected, connector.State);
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task HeartbeatTimeoutFailsPendingRequestsWithTimeoutCause()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await Task.Delay(TimeSpan.FromSeconds(1));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions
            {
                Interval = TimeSpan.FromMilliseconds(20),
                Timeout = TimeSpan.FromMilliseconds(80)
            }
        });

        await connector.Connect.Async();

        var exception = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
            await connector.Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, "b"u8.ToArray()))
                .PacketName("h")
                .Timeout(TimeSpan.FromSeconds(5))
                .Async());

        Assert.Equal(ZlinkStreamErrorCode.Disconnected, exception.Error.Code);
        Assert.Contains("Heartbeat", exception.Error.Message, StringComparison.Ordinal);
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task CloseCompletesWhenReconnectTransportArrivesAfterCloseStarted()
    {
        var firstConnection = new RecordingCloseConnection();
        var secondConnection = new RecordingCloseConnection();
        var reconnectTransportEntered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseReconnectTransport = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var connectCount = 0;
        using var shutdown = new CancellationTokenSource();
        var taskRunner = new ZlinkStreamTaskRunner(shutdown.Token);
        var pending = new ZlinkStreamPendingRequests();
        var callbacks = new ZlinkStreamConnectorCallbacks(
            taskRunner,
            ZlinkStreamDispatchMode.Immediate,
            32);
        var lifecycle = new ZlinkStreamConnectorLifecycle(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri("tcp://127.0.0.1:1"),
                Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
                Reconnect = new ZlinkStreamReconnectOptions
                {
                    InitialDelay = TimeSpan.FromMilliseconds(1),
                    MaxDelay = TimeSpan.FromMilliseconds(1),
                    BackoffFactor = 1.0,
                    MaxAttempts = 1
                }
            },
            pending,
            taskRunner,
            callbacks,
            OpenTransportAsync);

        await lifecycle.ConnectAsync(
            token => Task.Delay(Timeout.InfiniteTimeSpan, token),
            _ => ValueTask.CompletedTask,
            () => { },
            CancellationToken.None);
        await lifecycle.HandleTransportErrorAsync(
            new ZlinkStreamError(
                ZlinkStreamErrorCode.Disconnected,
                "transport failed"));
        await reconnectTransportEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var close = lifecycle.CloseAsync(CancellationToken.None).AsTask();
        releaseReconnectTransport.TrySetResult();
        await close.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamConnectionState.Closed, lifecycle.State);
        Assert.Equal(1, firstConnection.CloseCount);
        Assert.Equal(1, secondConnection.CloseCount);
        lifecycle.Dispose();
        shutdown.Cancel();
        return;

        ValueTask<IZlinkStreamConnection> OpenTransportAsync(CancellationToken _)
        {
            if (Interlocked.Increment(ref connectCount) == 1)
                return ValueTask.FromResult<IZlinkStreamConnection>(firstConnection);
            return AwaitReconnectTransportAsync();
        }

        async ValueTask<IZlinkStreamConnection> AwaitReconnectTransportAsync()
        {
            reconnectTransportEntered.TrySetResult();
            await releaseReconnectTransport.Task.ConfigureAwait(false);
            return secondConnection;
        }
    }

    [Fact]
    public async Task ConnectAsyncWhileReconnectingFailsWhenClosed()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var first = await listener.AcceptTcpClientAsync();
            first.Close();
            await Task.Delay(TimeSpan.FromSeconds(1));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            Reconnect = new ZlinkStreamReconnectOptions
            {
                InitialDelay = TimeSpan.FromSeconds(10),
                MaxDelay = TimeSpan.FromSeconds(10),
                BackoffFactor = 1.0,
                MaxAttempts = 3
            }
        });
        var reconnecting = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ConnectionStateChanged += (change, _) =>
        {
            if (change.Current == ZlinkStreamConnectionState.Reconnecting) reconnecting.TrySetResult();

            return ValueTask.CompletedTask;
        };

        await connector.Connect.Async();
        await reconnecting.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var waitingConnect = connector.Connect.Async().AsTask();

        await connector.Close.Async();

        await Assert.ThrowsAsync<ObjectDisposedException>(async () => await waitingConnect);
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task ReservedPacketNamesAreRejectedForUserHandlers()
    {
        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("tcp://127.0.0.1:1")
        });

        var exception = Assert.Throws<ZlinkStreamException>(() =>
            connector.On("$zlink.user", (_, _) => ValueTask.CompletedTask));

        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, exception.Error.Code);
        await Task.CompletedTask;
    }
}
