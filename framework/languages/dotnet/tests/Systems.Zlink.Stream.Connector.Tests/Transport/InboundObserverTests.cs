using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Fact]
    public async Task InboundObserverSeesResponseBeforeRequestCompletesWithoutWaitingForCallback()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var request = await ReadPacketAsync(stream);
            var requestHeader = headerCodec.Decode(request.Header);
            var responseHeader = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                requestHeader.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty);
            await WritePacketAsync(
                stream,
                headerCodec.Encode(responseHeader).ToArray(),
                JsonSerializer.SerializeToUtf8Bytes(new Pong("pong")));
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        var observerCanFinish = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var observed =
            new TaskCompletionSource<ZlinkStreamInboundObservation>(TaskCreationOptions.RunContinuationsAsynchronously);
        using var registration = connector.ObserveInbound(async (observation, _) =>
        {
            observed.TrySetResult(observation);
            await observerCanFinish.Task.WaitAsync(TimeSpan.FromSeconds(5));
        });

        await connector.Connect.Async();
        var reply = await connector.Request(new Ping("hello"))
            .PacketName("ping")
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<Pong>();

        Assert.Equal("pong", reply.Text);
        var snapshot = await observed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ZlinkStreamMessageKind.Response, snapshot.Kind);
        Assert.Equal(string.Empty, snapshot.Name);
        Assert.NotNull(snapshot.RequestSeq);
        Assert.Equal(ZlinkStreamCodec.Json, snapshot.Codec);
        Assert.True(snapshot.PayloadLength > 0);
        observerCanFinish.SetResult();
        await server;
    }

    [Fact]
    public async Task InboundObserverSeesSendAndControlFrames()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "push",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                "payload"u8.ToArray());
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Control,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    ZlinkStreamConnector.HeartbeatPongName,
                    ZlinkStreamMetadata.Empty)).ToArray(),
                []);
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        var observations = new List<ZlinkStreamInboundObservation>();
        using var registration = connector.ObserveInbound((observation, _) =>
        {
            lock (observations)
            {
                observations.Add(observation);
            }

            return ValueTask.CompletedTask;
        });
        var handled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.On("push", (_, _) =>
        {
            handled.SetResult();
            return ValueTask.CompletedTask;
        });

        await connector.Connect.Async();
        await handled.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(
            () =>
            {
                lock (observations)
                {
                    return observations.Any(item => item.Kind == ZlinkStreamMessageKind.Control);
                }
            },
            TimeSpan.FromSeconds(5));

        lock (observations)
        {
            Assert.Contains(observations, item => item.Kind == ZlinkStreamMessageKind.Send && item.Name == "push");
            Assert.Contains(observations, item => item.Kind == ZlinkStreamMessageKind.Control);
        }

        await server;
    }

    [Fact]
    public async Task InboundObserverRegistrationIsRejectedAfterConnectAndStopsAfterDispose()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var firstPacketRead = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var sendSecond = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "first",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                "a"u8.ToArray());
            firstPacketRead.SetResult();
            await sendSecond.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "second",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                "b"u8.ToArray());
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        var observed = 0;
        var registration = connector.ObserveInbound((_, _) =>
        {
            Interlocked.Increment(ref observed);
            return ValueTask.CompletedTask;
        });

        await connector.Connect.Async();
        var exception = Assert.Throws<ZlinkStreamException>(() =>
            connector.ObserveInbound((_, _) => ValueTask.CompletedTask));
        Assert.Equal(ZlinkStreamErrorCode.ValidationFailed, exception.Error.Code);

        await firstPacketRead.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(() => Volatile.Read(ref observed) == 1, TimeSpan.FromSeconds(5));
        registration.Dispose();
        sendSecond.SetResult();
        await Task.Delay(TimeSpan.FromMilliseconds(100));

        Assert.Equal(1, Volatile.Read(ref observed));
        await server;
    }

    [Fact]
    public async Task Dispose_Waits_For_Cancellation_Ignoring_Inbound_Observer()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "blocked-observer",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                "payload"u8.ToArray());
            var buffer = new byte[1];
            Assert.Equal(0, await stream.ReadAsync(buffer, 0, buffer.Length));
        });
        var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        var observerStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseObserver = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var observerCompleted = 0;
        using var registration = connector.ObserveInbound(async (_, _) =>
        {
            observerStarted.SetResult();
            await releaseObserver.Task.ConfigureAwait(false);
            Interlocked.Increment(ref observerCompleted);
        });

        await connector.Connect.Async();
        await observerStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var dispose = connector.DisposeAsync().AsTask();
        var repeatedDispose = connector.DisposeAsync().AsTask();
        Assert.Same(dispose, repeatedDispose);
        await Task.Delay(TimeSpan.FromMilliseconds(50));
        Assert.False(dispose.IsCompleted);
        Assert.False(repeatedDispose.IsCompleted);
        Assert.Equal(0, Volatile.Read(ref observerCompleted));

        releaseObserver.SetResult();
        await Task.WhenAll(dispose, repeatedDispose).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(1, Volatile.Read(ref observerCompleted));
        await Task.Delay(TimeSpan.FromMilliseconds(50));
        Assert.Equal(1, Volatile.Read(ref observerCompleted));
        await server.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task InboundObserverFailureReportsObserverFailedAndMessageStillDispatches()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            await WritePacketAsync(
                stream,
                headerCodec.Encode(new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "push",
                    ZlinkStreamMetadata.Empty)).ToArray(),
                "payload"u8.ToArray());
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate
        });
        using var registration =
            connector.ObserveInbound((_, _) => throw new InvalidOperationException("observer failed"));
        var errorReceived =
            new TaskCompletionSource<ZlinkStreamError>(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ErrorReceived += (error, _) =>
        {
            if (error.Code == ZlinkStreamErrorCode.ObserverFailed) errorReceived.TrySetResult(error);

            return ValueTask.CompletedTask;
        };
        var handled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.On("push", (_, _) =>
        {
            handled.SetResult();
            return ValueTask.CompletedTask;
        });

        await connector.Connect.Async();
        await handled.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var error = await errorReceived.Task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZlinkStreamErrorCode.ObserverFailed, error.Code);
        await server;
    }

    [Fact]
    public async Task InboundObserverOverflowReportsObserverDroppedAndRequestStillCompletes()
    {
        var headerCodec = new ZlinkStreamHeaderCodec();
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            for (var index = 0; index < 3; index++)
            {
                var request = await ReadPacketAsync(stream);
                var requestHeader = headerCodec.Decode(request.Header);
                var responseHeader = new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Response,
                    ZlinkStreamCodec.Json,
                    ZlinkStreamHeaderFlags.HasRequestSeq,
                    requestHeader.RequestSeq,
                    string.Empty,
                    ZlinkStreamMetadata.Empty);
                await WritePacketAsync(
                    stream,
                    headerCodec.Encode(responseHeader).ToArray(),
                    JsonSerializer.SerializeToUtf8Bytes(new Pong(index.ToString())));
            }
        });

        await using var connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
            Heartbeat = DisabledHeartbeat(),
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxInboundObserverNotifications = 1
        });
        var releaseObserver = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var observedNames = new List<string>();
        using var registration = connector.ObserveInbound(async (observation, _) =>
        {
            lock (observedNames)
            {
                observedNames.Add(observation.Name);
            }

            await releaseObserver.Task.WaitAsync(TimeSpan.FromSeconds(5));
        });
        var dropped = new TaskCompletionSource<ZlinkStreamError>(TaskCreationOptions.RunContinuationsAsynchronously);
        connector.ErrorReceived += (error, _) =>
        {
            if (error.Code == ZlinkStreamErrorCode.ObserverDropped) dropped.TrySetResult(error);

            return ValueTask.CompletedTask;
        };

        await connector.Connect.Async();
        for (var index = 0; index < 3; index++)
        {
            var reply = await connector.Request(new Ping(index.ToString()))
                .PacketName($"pong.{index}")
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<Pong>();
            Assert.Equal(index.ToString(), reply.Text);
        }

        var error = await dropped.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ZlinkStreamErrorCode.ObserverDropped, error.Code);
        releaseObserver.SetResult();
        await WaitUntilAsync(
            () =>
            {
                lock (observedNames)
                {
                    return observedNames.Count >= 2;
                }
            },
            TimeSpan.FromSeconds(5));
        lock (observedNames)
        {
            Assert.Equal(2, observedNames.Count);
            Assert.All(observedNames, name => Assert.Equal(string.Empty, name));
        }

        await server;
    }
}
