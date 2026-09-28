using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime;
using Xunit;

public sealed partial class StreamConnectorTests
{
    [Theory]
    [InlineData(typeof(ArgumentException), ZlinkStreamErrorCode.ValidationFailed)]
    [InlineData(typeof(IOException), ZlinkStreamErrorCode.Disconnected)]
    [InlineData(typeof(InvalidOperationException), ZlinkStreamErrorCode.UserCallbackFailed)]
    public async Task RequestCallbackClassifiesFailureByCause(
        Type failureType,
        ZlinkStreamErrorCode expected
    )
    {
        using var shutdown = new CancellationTokenSource();
        var runner = new ZlinkStreamTaskRunner(shutdown.Token);
        var callbacks = new ZlinkStreamConnectorCallbacks(
            runner,
            ZlinkStreamDispatchMode.Immediate
        );
        var received = new TaskCompletionSource<ZlinkStreamError>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var failure = (Exception)Activator.CreateInstance(failureType, "request failed")!;

        callbacks.QueueRequestCallback(
            () => throw failure,
            _ => throw new InvalidOperationException("Unexpected success"),
            error => error,
            received.SetResult
        );

        Assert.Equal(expected, (await received.Task.WaitAsync(TimeSpan.FromSeconds(5))).Code);
        await runner.StopAndDrainAsync();
    }

    [Fact]
    public async Task ReplyHookCompletionDoesNotDelayRequestResult()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var codec = new ZlinkStreamHeaderCodec();
        var releaseServer = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseHook = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var request = codec.Decode((await ReadPacketAsync(stream)).Header);
            var response = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                request.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty
            );
            await WritePacketAsync(stream, codec.Encode(response).ToArray(), [2]);
            await releaseServer.Task;
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
        connector.OnReplyReceived((_, _) => new ValueTask(releaseHook.Task));
        await connector.Connect.Async();

        try
        {
            var reply = await connector
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                .PacketName("query")
                .Async()
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal((byte)2, reply.Payload.Span[0]);
        }
        finally
        {
            releaseHook.TrySetResult();
            releaseServer.TrySetResult();
            await server;
        }
    }

    [Fact]
    public async Task ManualReceiveCallbackCanAwaitRequestWithSendingHook()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var codec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var push = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                "trigger",
                ZlinkStreamMetadata.Empty
            );
            await WritePacketAsync(stream, codec.Encode(push).ToArray(), [1]);
            var request = codec.Decode((await ReadPacketAsync(stream)).Header);
            Assert.Equal("inline", request.Metadata.Get("hook"));
            var response = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                request.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty
            );
            await WritePacketAsync(stream, codec.Encode(response).ToArray(), [2]);
        });
        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                DispatchMode = ZlinkStreamDispatchMode.Manual,
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            }
        );
        connector.OnRequestSending(context =>
        {
            context.SetMetadata("hook", "inline");
        });
        var completed = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        connector.On(
            "trigger",
            async (_, _) =>
            {
                var reply = await connector
                    .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                    .PacketName("query")
                    .Async();
                Assert.Equal((byte)2, reply.Payload.Span[0]);
                completed.TrySetResult();
            }
        );
        await connector.Connect.Async();
        await WaitUntilAsync(() => connector.PendingDispatchCount > 0, TimeSpan.FromSeconds(5));
        await connector.Dispatch.Async().AsTask().WaitAsync(TimeSpan.FromSeconds(5));
        await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await server;
    }

    [Fact]
    public async Task ManualSendingHookRunsOnRequestCallerAndReplyHookWaitsForDispatch()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var codec = new ZlinkStreamHeaderCodec();
        var releaseServer = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var request = codec.Decode((await ReadPacketAsync(stream)).Header);
            Assert.Equal("before-dispatch", request.Metadata.Get("hook"));
            var response = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                request.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty
            );
            await WritePacketAsync(stream, codec.Encode(response).ToArray(), [2]);
            await releaseServer.Task;
        });
        await using var connector = ZlinkStreamConnectorFactory.Create(
            new ZlinkStreamConnectorOptions
            {
                Endpoint = new Uri($"tcp://127.0.0.1:{endpoint.Port}"),
                Heartbeat = DisabledHeartbeat(),
                DispatchMode = ZlinkStreamDispatchMode.Manual,
                Reconnect = new ZlinkStreamReconnectOptions { Enabled = false },
            }
        );
        var order = new List<string>();
        var sendingThread = 0;
        var replyThread = 0;
        connector.OnRequestSending(context =>
        {
            sendingThread = Environment.CurrentManagedThreadId;
            order.Add("sending");
            context.SetMetadata("hook", "before-dispatch");
        });
        connector.OnReplyReceived(
            (_, _) =>
            {
                replyThread = Environment.CurrentManagedThreadId;
                order.Add("reply");
                return ValueTask.CompletedTask;
            }
        );
        await connector.Connect.Async();
        var requestCallerThread = Environment.CurrentManagedThreadId;
        var requestTask = connector
            .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
            .PacketName("query")
            .Async()
            .AsTask();
        Assert.Equal(new[] { "sending" }, order);
        Assert.Equal(requestCallerThread, sendingThread);
        await WaitUntilAsync(() => connector.PendingDispatchCount > 0, TimeSpan.FromSeconds(5));
        Assert.Equal(new[] { "sending" }, order);
        var secondDispatchThread = await Task.Run(() =>
        {
            var thread = Environment.CurrentManagedThreadId;
            connector.Dispatch.Async().AsTask().GetAwaiter().GetResult();
            return thread;
        });
        Assert.Equal(new[] { "sending", "reply" }, order);
        Assert.Equal(secondDispatchThread, replyThread);
        Assert.Equal((byte)2, (await requestTask).Payload.Span[0]);
        releaseServer.SetResult();
        await server;
    }

    [Fact]
    public async Task RequestHooksRunInOrderAddWireMetadataAndIsolateFailures()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var codec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var request = codec.Decode((await ReadPacketAsync(stream)).Header);
            Assert.Equal("one", request.Metadata.Get("first"));
            Assert.Equal("three", request.Metadata.Get("last"));
            Assert.False(request.Flags.HasFlag(ZlinkStreamHeaderFlags.HasFlowId));
            var response = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Response,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                request.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty
            );
            await WritePacketAsync(stream, codec.Encode(response).ToArray(), [7]);
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
        var order = new List<string>();
        var errors = new ConcurrentQueue<ZlinkStreamError>();
        connector.OnErrorReceived(
            (error, _) =>
            {
                errors.Enqueue(error);
                return ValueTask.CompletedTask;
            }
        );
        connector.OnRequestSending(context =>
        {
            Assert.Equal("query", context.RequestPacketName);
            Assert.Null(context.ActorId);
            order.Add("send-1");
            context.SetMetadata("first", "one");
        });
        connector.OnRequestSending(_ =>
        {
            order.Add("send-2");
            throw new InvalidOperationException("hook failed");
        });
        connector.OnRequestSending(context =>
        {
            order.Add("send-3");
            context.SetMetadata("last", "three");
        });
        connector.OnReplyReceived(
            (context, _) =>
            {
                order.Add("reply-1");
                Assert.True(context.Succeeded);
                Assert.Equal("query", context.RequestPacketName);
                Assert.Equal((byte)7, context.Reply!.Payload.Payload.Span[0]);
                Assert.Null(context.Error);
                Assert.True(context.Elapsed >= TimeSpan.Zero);
                throw new InvalidOperationException("reply hook failed");
            }
        );
        connector.OnReplyReceived(
            (_, _) =>
            {
                order.Add("reply-2");
                return ValueTask.CompletedTask;
            }
        );

        await connector.Connect.Async();
        var reply = await connector
            .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
            .PacketName("query")
            .Async();
        Assert.Equal((byte)7, reply.Payload.Span[0]);
        Assert.Equal(new[] { "send-1", "send-2", "send-3", "reply-1", "reply-2" }, order);
        await WaitUntilAsync(
            () => errors.Count(error => error.Code == ZlinkStreamErrorCode.UserCallbackFailed) == 2,
            TimeSpan.FromSeconds(5)
        );
        Assert.Equal(
            2,
            errors.Count(error => error.Code == ZlinkStreamErrorCode.UserCallbackFailed)
        );
        await server;
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    public async Task ReplyHookObservesRemoteFailureTimeoutAndClose(int outcome)
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var endpoint = (IPEndPoint)listener.LocalEndpoint;
        var codec = new ZlinkStreamHeaderCodec();
        var server = Task.Run(async () =>
        {
            using var tcp = await listener.AcceptTcpClientAsync();
            await using var stream = tcp.GetStream();
            var request = codec.Decode((await ReadPacketAsync(stream)).Header);
            if (outcome == 1)
            {
                await Task.Delay(TimeSpan.FromMilliseconds(250));
                return;
            }
            if (outcome == 2)
                return;
            var error = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Error,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                request.RequestSeq,
                string.Empty,
                ZlinkStreamMetadata.Empty
            );
            await WritePacketAsync(
                stream,
                codec.Encode(error).ToArray(),
                "{\"code\":\"denied\",\"message\":\"no\"}"u8.ToArray()
            );
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
        var observed = new List<ZlinkStreamReplyReceivedContext>();
        connector.OnReplyReceived(
            (context, _) =>
            {
                observed.Add(context);
                return ValueTask.CompletedTask;
            }
        );
        await connector.Connect.Async();
        var exception = await Assert.ThrowsAsync<ZlinkStreamException>(async () =>
            await connector
                .Request(new ZlinkStreamEncodedPayload(ZlinkStreamCodec.Raw, new byte[] { 1 }))
                .PacketName("query")
                .Timeout(TimeSpan.FromMilliseconds(outcome == 1 ? 50 : 500))
                .Async()
        );
        Assert.Single(observed);
        Assert.False(observed[0].Succeeded);
        Assert.Null(observed[0].Reply);
        Assert.Equal(exception.Error.Code, observed[0].Error!.Code);
        if (outcome == 2)
            Assert.Equal(ZlinkStreamErrorCode.Disconnected, observed[0].Error!.Code);
        await server;
    }
}
