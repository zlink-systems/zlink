using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SubmitAdmissionRuntimeTests : IAsyncLifetime
{
    private readonly Zlink.Framework.Runtime.Execution.ZLinkStateLane _deadlineLane = new();
    private ZLinkRequestDeadlineIndex? _deadlines;
    private ZLinkRequestDeadlineIndex Deadlines => _deadlines ??= new(_deadlineLane);

    public Task InitializeAsync() => Task.CompletedTask;

    public async Task DisposeAsync()
    {
        if (_deadlines is not null)
            await _deadlines.DisposeAsync();
        await _deadlineLane.DisposeAsync();
    }

    public static async Task<double[]> MeasureBackpressuredSubmission(int iterations)
    {
        using var process = System.Diagnostics.Process.GetCurrentProcess();
        var cpu = process.TotalProcessorTime;
        var gc0 = GC.CollectionCount(0);
        var gc1 = GC.CollectionCount(1);
        var gc2 = GC.CollectionCount(2);
        long ticks = 0,
            bytes = 0;
        for (var index = 0; index < iterations; index++)
        {
            using var pair = new AdmissionPair();
            using var fillCancellation = new CancellationTokenSource();
            Task? fillAdmission = null;
            for (var sequence = 0; sequence < 32; sequence++)
            {
                using var filler = Message.From(AdmissionPair.Payload(sequence));
                var submission = pair.Client.Send().Message(filler).Async(fillCancellation.Token);
                if (submission.Result == SubmitResult.Backpressured)
                {
                    fillAdmission = submission.Admitted;
                    break;
                }
            }
            try
            {
                using var cancellation = new CancellationTokenSource();
                using var payload = Message.From(AdmissionPair.Payload(100));
                var operation = new CountingRequestSubmitOperation(
                    pair.Client.Request().Message(payload).Timeout(TimeSpan.FromSeconds(5))
                );
                var allocated = GC.GetAllocatedBytesForCurrentThread();
                var started = System.Diagnostics.Stopwatch.GetTimestamp();
                var pending = ZLinkRequestSubmissionOutcome.SubmitAndAwaitReplyAsync(
                    operation,
                    TimeSpan.FromSeconds(5),
                    cancellation.Token
                );
                ticks += System.Diagnostics.Stopwatch.GetTimestamp() - started;
                bytes +=
                    GC.GetAllocatedBytesForCurrentThread() - allocated - operation.AllocatedBytes;
                if (operation.LastSubmission.Result != SubmitResult.Backpressured)
                    throw new InvalidOperationException(
                        "The native request was not backpressured."
                    );
                cancellation.Cancel();
                try
                {
                    ZLinkMessageParts.DisposeAll(await pending.ConfigureAwait(false));
                    throw new InvalidOperationException(
                        "A blocked canceled request completed successfully."
                    );
                }
                catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { }
            }
            finally
            {
                fillCancellation.Cancel();
                if (fillAdmission is not null)
                {
                    try
                    {
                        await fillAdmission.ConfigureAwait(false);
                    }
                    catch (OperationCanceledException)
                        when (fillCancellation.IsCancellationRequested) { }
                }
            }
        }
        return
        [
            ticks * 1_000_000_000.0 / System.Diagnostics.Stopwatch.Frequency / iterations,
            (double)bytes / iterations,
            (process.TotalProcessorTime - cpu).TotalMilliseconds,
            GC.CollectionCount(0) - gc0,
            GC.CollectionCount(1) - gc1,
            GC.CollectionCount(2) - gc2,
        ];
    }

    [Theory]
    [InlineData(ZlinkSubmitException.ErrorCode.Backpressured, false)]
    [InlineData(ZlinkSubmitException.ErrorCode.Backpressured, true)]
    [InlineData(ZlinkSubmitException.ErrorCode.NotConnected, false)]
    [InlineData(ZlinkSubmitException.ErrorCode.NotConnected, true)]
    public async Task NativeCompletion_PreservesTypedAdmissionFailure(
        ZlinkSubmitException.ErrorCode code,
        bool awaited
    )
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        var expected = new ZlinkSubmitException(code);
        var socket = new RefusedRequestSocket(expected);
        await using var node = new ZLinkManagedMeshNode(
            context,
            "typed-completion",
            directRequestSocket: socket
        );
        const System.Reflection.BindingFlags flags =
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        var nodeType = typeof(ZLinkManagedMeshNode);
        MeshReceiveRecord? terminal = null;
        node.SetCompletionHandlerCore(
            (record, parts) =>
            {
                Assert.Empty(parts);
                terminal = record;
                return true;
            }
        );
        var create = nodeType.GetMethod(
            "CreateOperation",
            flags,
            [
                typeof(MeshOperationKind),
                typeof(MeshOperationId),
                typeof(ulong).MakeByRefType(),
                typeof(ZLinkManagedMeshNode.PendingOperation).MakeByRefType(),
                typeof(bool),
                typeof(TimeSpan?),
                typeof(TimeSpan?),
                typeof(ZLinkRequestAdmission),
                typeof(ulong?),
            ]
        )!;
        object?[] arguments =
        [
            MeshOperationKind.ActorRequest,
            node.AllocateOperationId(),
            0UL,
            null,
            awaited,
            TimeSpan.FromSeconds(5),
            null,
            null,
            null,
        ];
        var target = RoutingId.From("peer");
        var peers =
            (Dictionary<RoutingId, ZLinkMeshPeer>)
                nodeType.GetField("_peersByRid", flags)!.GetValue(node)!;
        peers.Add(
            target,
            new ZLinkMeshPeer(1)
            {
                RoutingId = target,
                PhysicalRoutingId = target,
                RouteGeneration = 1,
                Admitted = true,
                State = MeshPeerState.Admitted,
            }
        );
        (
            (ZLinkMeshSelectedRoutes)nodeType.GetField("_selectedRoutes", flags)!.GetValue(node)!
        ).Apply([new RouterRoute(target, 1)]);
        (
            (ZLinkMeshChannelSelection)
                nodeType.GetField("_channelSelection", flags)!.GetValue(node)!
        ).PrepareRouterTargets(peers);
        // Capture the original caller task before the dispatcher clears its receiver.
        var callerTask = (Task?)create.Invoke(node, arguments);
        var pending = (ZLinkManagedMeshNode.PendingOperation)arguments[3]!;
        await (Task)
            nodeType
                .GetMethod("CompleteNativeDurableRequestAsync", flags)!
                .Invoke(
                    node,
                    [
                        target,
                        0UL,
                        pending,
                        new ReadOnlyMemory<byte>[] { "request"u8.ToArray() },
                        (Action<
                            ZLinkManagedMeshNode.PendingOperation,
                            RequestResult,
                            IReadOnlyList<Message>
                        >)(
                            (_, _, _) =>
                                throw new Xunit.Sdk.XunitException(
                                    "Rejected submission completed as a reply."
                                )
                        ),
                        default(CancellationToken),
                        null,
                        null,
                    ]
                )!;
        if (awaited)
        {
            var task = callerTask!;
            var failure = await Assert.ThrowsAsync<ZlinkSubmitException>(() => task);
            Assert.Same(expected, failure);
            Assert.Null(terminal);
            var callbacks = 0;
            await (Task)
                nodeType
                    .GetMethod("DeliverRequestCompletionAsync", flags)!
                    .Invoke(
                        node,
                        [
                            task,
                            (ZLinkBackendRequestCallback)(
                                (_, parts) =>
                                {
                                    callbacks++;
                                    Assert.Empty(parts);
                                }
                            ),
                        ]
                    )!;
            Assert.Equal(1, callbacks);
        }
        else
        {
            Assert.True(terminal.HasValue);
            Assert.Same(expected, terminal.Value.SubmitFailure);
        }
        Assert.Equal(1, socket.Submissions);
    }

    [Theory]
    [InlineData(ZlinkSubmitException.ErrorCode.Backpressured, false)]
    [InlineData(ZlinkSubmitException.ErrorCode.Backpressured, true)]
    [InlineData(ZlinkSubmitException.ErrorCode.NotConnected, false)]
    [InlineData(ZlinkSubmitException.ErrorCode.NotConnected, true)]
    public async Task NativeRequest_PreservesTypedAdmissionFailure(
        ZlinkSubmitException.ErrorCode code,
        bool encodedWire
    )
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        var expected = new ZlinkSubmitException(code);
        var socket = new RefusedRequestSocket(expected);
        await using var node = new ZLinkManagedMeshNode(
            context,
            "typed-admission",
            directRequestSocket: socket
        );
        // Access only Framework state. The socket double implements the public
        // binding contracts and never accesses binding implementation details.
        const System.Reflection.BindingFlags flags =
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        var nodeType = typeof(ZLinkManagedMeshNode);
        using var payload = Message.From("request");
        var method = nodeType.GetMethod(
            "RequestDirectWireAsync",
            flags,
            [
                typeof(RoutingId),
                encodedWire
                    ? typeof(IReadOnlyList<ReadOnlyMemory<byte>>)
                    : typeof(IReadOnlyList<Message>),
                typeof(TimeSpan),
                typeof(CancellationToken),
                typeof(ZLinkServiceLiveness),
                typeof(ZLinkRequestAdmission),
                typeof(TimeSpan?),
                typeof(MeshOperationId),
            ]
        )!;
        var request =
            (ValueTask<IReadOnlyList<Message>>)
                method.Invoke(
                    node,
                    [
                        RoutingId.From("peer"),
                        encodedWire
                            ? new ReadOnlyMemory<byte>[] { "request"u8.ToArray() }
                            : new[] { payload },
                        TimeSpan.FromSeconds(5),
                        default(CancellationToken),
                        null,
                        null,
                        null,
                        default(MeshOperationId),
                    ]
                )!;
        var failure = await Assert.ThrowsAsync<ZlinkSubmitException>(() => request.AsTask());
        Assert.Same(expected, failure);
        Assert.Equal(1, socket.Submissions);
        Assert.Throws<ObjectDisposedException>(() => _ = socket.Payload!.Size);
    }

    private sealed class RefusedRequestSocket(ZlinkSubmitException failure)
        : IRouterSocket,
            RequestOperation,
            RequestSubmitOperation
    {
        internal Message? Payload { get; private set; }
        internal int Submissions { get; private set; }

        public RequestOperation Request(RoutingId peerRid) => this;

        public RequestSubmitOperation Message(Message message)
        {
            Payload = message;
            return this;
        }

        public RequestSubmitOperation Timeout(TimeSpan timeout) => this;

        public IReadOnlyList<Message> Submit() => throw new NotSupportedException();

        public RequestSubmission Async(CancellationToken cancellationToken = default)
        {
            Submissions++;
            throw failure;
        }

        public RouterSocketOptions Options => throw new NotSupportedException();
        CommonSocketOptions ISocket.Options => Options;

        public void Bind(string address) => throw new NotSupportedException();

        public void Unbind(string address) => throw new NotSupportedException();

        public void Connect(string address) => throw new NotSupportedException();

        public void Disconnect(string address) => throw new NotSupportedException();

        public void DisconnectRid(RoutingId routingId) => throw new NotSupportedException();

        public IReadOnlyList<ConnectionAttempt> ConnectionAttemptsSnapshot() =>
            throw new NotSupportedException();

        public void SetRoutingId(RoutingId routingId) => throw new NotSupportedException();

        public RoutingId GetRoutingId() => throw new NotSupportedException();

        public SendOperation Send(RoutingId routingId) => throw new NotSupportedException();

        public ReplyOperation Reply(RoutingId rid, ReplyToken replyToken) =>
            throw new NotSupportedException();

        public IReadOnlyList<RouterRoute> RoutesSnapshot() => throw new NotSupportedException();

        public bool Recv(Received result, RecvFlags flags = RecvFlags.None) =>
            throw new NotSupportedException();

        public ISocketMonitor MonitorOpen(SocketEvent events = SocketEvent.All) =>
            throw new NotSupportedException();

        public ISocketMonitor MonitorOpen(SocketEvent events, ulong monitorHwmBytes) =>
            throw new NotSupportedException();

        public void SetTlsServer(string certPath, string keyPath, bool requireClientCert = false) =>
            throw new NotSupportedException();

        public void SetTlsClient(string caCertPath, string hostname, bool trustSystem = false) =>
            throw new NotSupportedException();

        public void SetReceiveFlowState(ReceiveFlowState state) =>
            throw new NotSupportedException();

        public void Close() { }

        public void Dispose() { }

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }

    [Fact]
    public void OkSubmission_DoesNotReadAdmissionOrAllocateACompletion()
    {
        // The default public value has Result=Ok and no admission task. This
        // isolates the snapshot decision without accessing binding internals.
        SendSubmission submission = default;
        Assert.Equal(SubmitResult.Ok, submission.Result);
        Assert.Same(Task.CompletedTask, submission.EnsureAcceptedAsync());

        Assert.Equal(
            0,
            PerCallAllocation.MinWindow(() => _ = submission.EnsureAcceptedAsync(), 10_000)
        );
    }

    [Fact]
    public async Task RequestSubmissionOutcome_SubmitsOnceAndReturnsOneTerminal()
    {
        using var pair = new AdmissionPair();
        using var requestPart = Message.From("request-once");
        using var replyPart = Message.From("reply-once");
        var operation = new CountingRequestSubmitOperation(
            pair.Client.Request().Message(requestPart).Timeout(TimeSpan.FromSeconds(5))
        );

        // Measure only the Framework submission adapter. The public binding's
        // allocations are counted separately inside Async on the same thread.
        // One submission is one call, so earlier submissions on fresh pairs supply the
        // other samples, 32 per window, and a one-time runtime allocation cannot decide the result.
        var samples = new List<long>();
        for (var round = 0; round < 255; round++)
            if (await SampleFrameworkSubmissionAsync() is { } sample)
                samples.Add(sample);

        using var cancellation = new CancellationTokenSource();
        var (requestResult, completion, mainAllocated) = SubmitMeasured(
            operation,
            cancellation.Token
        );
        if (mainAllocated is { } main)
            samples.Add(main);
        Assert.Equal(1, operation.SubmissionCount);
        Assert.Equal(SubmitResult.Ok, operation.LastSubmission.Result);
        Assert.Equal(0, PerCallAllocation.MinWindow(samples));
        Assert.Equal(cancellation.Token, operation.SubmissionToken);
        Assert.Same(operation.LastSubmission.Reply, completion);
        Assert.True(requestResult.Admitted.IsCompletedSuccessfully);
        Assert.False(requestResult.Reply.IsCompleted);
        using var received = Received.Create();
        Assert.True(pair.Server.Recv(received));
        Assert.Equal("request-once", received.SinglePartOrThrow().GetString());
        received.Reply().Message(replyPart).Submit();
        Assert.True(
            SpinWait.SpinUntil(
                () =>
                {
                    pair.DrainCompletions();
                    return requestResult.Reply.IsCompleted;
                },
                TimeSpan.FromSeconds(5)
            )
        );

        var terminal = await requestResult;
        try
        {
            Assert.Equal("reply-once", Assert.Single(terminal).GetString());
        }
        finally
        {
            foreach (var part in terminal)
                part.Dispose();
        }
        Assert.Equal(1, operation.SubmissionCount);

        using var extra = Received.Create();
        Assert.False(pair.Server.Recv(extra, RecvFlags.DontWait));
    }

    // Allocation of the Framework submission adapter for one request, or null when a GC
    // pause overlapped the call.
    private static (
        ZLinkRequest<IReadOnlyList<Message>> Request,
        Task<IReadOnlyList<Message>>? Completion,
        long? Allocated
    ) SubmitMeasured(CountingRequestSubmitOperation operation, CancellationToken token)
    {
        long? allocated = null;
        Task<IReadOnlyList<Message>>? completion = null;
        var request = ZLinkRequestAdmission.Run<IReadOnlyList<Message>>(admission =>
        {
            var pause = GC.GetTotalPauseDuration();
            var allocatedBefore = GC.GetAllocatedBytesForCurrentThread();
            completion = ZLinkRequestSubmissionOutcome.SubmitAndAwaitReplyAsync(
                operation,
                TimeSpan.FromSeconds(5),
                token,
                requestAdmission: admission
            );
            var bytes =
                GC.GetAllocatedBytesForCurrentThread() - allocatedBefore - operation.AllocatedBytes;
            if (GC.GetTotalPauseDuration() == pause)
                allocated = bytes;
            return new ValueTask<IReadOnlyList<Message>>(completion!);
        });
        return (request, completion, allocated);
    }

    private static async Task<long?> SampleFrameworkSubmissionAsync()
    {
        using var pair = new AdmissionPair();
        using var requestPart = Message.From("request-sample");
        var operation = new CountingRequestSubmitOperation(
            pair.Client.Request().Message(requestPart).Timeout(TimeSpan.FromSeconds(5))
        );
        using var cancellation = new CancellationTokenSource();
        var (request, _, allocated) = SubmitMeasured(operation, cancellation.Token);
        cancellation.Cancel();
        try
        {
            ZLinkMessageParts.DisposeAll(await request.Reply.ConfigureAwait(false));
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { }
        return allocated;
    }

    [Fact]
    public async Task ImmediateRequestSubmissionError_FailsPublicAdmissionAndReplyTogether()
    {
        var expected = new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
        var requiresAdmission = true;
        var request = ZLinkRequestAdmission.Run<int>(admission =>
        {
            requiresAdmission = ZLinkBindingSubmissionOutcome.RequiresAdmission(
                SubmitResult.NotConnected,
                admission
            );
            return new ValueTask<int>(Task.FromException<int>(expected));
        });

        Assert.False(requiresAdmission);
        var replyFailure = await Assert.ThrowsAsync<ZlinkSubmitException>(() => request.AsTask());
        var admissionFailure = await Assert.ThrowsAsync<ZlinkSubmitException>(() =>
            request.Admitted.AsTask()
        );
        Assert.Same(expected, replyFailure);
        Assert.Same(replyFailure, admissionFailure);
    }

    [Fact]
    public async Task BackpressuredSend_StopsTheProducerUntilBindingAdmissionAndDeliversOnce()
    {
        using var pair = new AdmissionPair();
        const int recordCount = 32;
        var attempts = 0;
        SendSubmission blocked = default;
        var producer = ProduceAsync();

        Assert.False(producer.IsCompleted);
        Assert.InRange(attempts, 2, recordCount - 1);
        Assert.Equal(SubmitResult.Backpressured, blocked.Result);

        Assert.False(blocked.Admitted.IsCompleted);

        await Task.Delay(TimeSpan.FromMilliseconds(3100));
        Assert.False(producer.IsCompleted);
        // Receiving releases Core capacity. Only the binding can resubmit the
        // blocked record; the producer resumes with the next sequence number.
        for (var sequence = 0; sequence < recordCount; sequence++)
        {
            using var received = Received.Create();
            Assert.True(await pair.WaitForReceiveAsync());
            Assert.True(pair.Server.Recv(received));
            pair.DrainCompletions();
            Assert.Equal(AdmissionPair.Payload(sequence), received.SinglePartOrThrow().ToArray());
        }
        await producer.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(recordCount, attempts);
        using var extra = Received.Create();
        Assert.False(pair.Server.Recv(extra, RecvFlags.DontWait));

        async Task ProduceAsync()
        {
            for (var sequence = 0; sequence < recordCount; sequence++)
            {
                using var payload = Message.From(AdmissionPair.Payload(sequence));
                var submission = pair.Client.Send().Message(payload).Async();
                attempts++;
                if (submission.Result == SubmitResult.Backpressured)
                    blocked = submission;
                await submission.EnsureAcceptedAsync();
            }
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task BackpressuredRequestBudget_ExpiresAndDisposesLateReply(
        bool admitBeforeDeadline
    )
    {
        using var pair = new AdmissionPair();
        SendSubmission blocked = default;
        var fillerCount = 0;
        for (var sequence = 0; sequence < 32; sequence++)
        {
            using var filler = Message.From(AdmissionPair.Payload(sequence));
            blocked = pair.Client.Send().Message(filler).Async();
            fillerCount++;
            if (blocked.Result == SubmitResult.Backpressured)
                break;
        }
        Assert.Equal(SubmitResult.Backpressured, blocked.Result);
        using var request = Message.From(AdmissionPair.Payload(100));
        var operation = new CountingRequestSubmitOperation(
            pair.Client.Request().Message(request).Timeout(TimeSpan.FromSeconds(5))
        );
        var requestResult = StartPublicRequest(operation, TimeSpan.FromMilliseconds(100));
        var completion = requestResult.AsTask();
        Assert.False(requestResult.Admitted.IsCompleted);
        Assert.Equal(SubmitResult.Backpressured, operation.LastSubmission.Result);
        if (admitBeforeDeadline)
            await RecoverCapacityAsync();
        if (admitBeforeDeadline)
            await requestResult.Admitted.AsTask().WaitAsync(TimeSpan.FromSeconds(2));
        var failure = await Assert.ThrowsAsync<ZlinkRequestException>(() =>
            completion.WaitAsync(TimeSpan.FromSeconds(2))
        );
        Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, failure.Result);
        if (!admitBeforeDeadline)
            Assert.Same(
                failure,
                await Assert.ThrowsAsync<ZlinkRequestException>(() =>
                    requestResult.Admitted.AsTask()
                )
            );
        Assert.Equal(1, operation.SubmissionCount);

        // Ending the caller wait does not cancel binding's pending submission.
        // Recover capacity, then deliver a real reply to the closed caller.
        if (!admitBeforeDeadline)
            await RecoverCapacityAsync();
        using var received = Received.Create();
        Assert.True(pair.Server.Recv(received));
        Assert.Equal(AdmissionPair.Payload(100), received.SinglePartOrThrow().ToArray());
        using var reply = Message.From("late-reply");
        received.Reply().Message(reply).Submit();
        Assert.True(
            SpinWait.SpinUntil(
                () =>
                {
                    pair.DrainCompletions();
                    return operation.LastSubmission.Reply.IsCompleted;
                },
                TimeSpan.FromSeconds(2)
            )
        );
        var lateReply = Assert.Single(await operation.LastSubmission.Reply);
        Assert.True(
            SpinWait.SpinUntil(
                () =>
                {
                    try
                    {
                        _ = lateReply.Size;
                        return false;
                    }
                    catch (ObjectDisposedException)
                    {
                        return true;
                    }
                },
                TimeSpan.FromSeconds(2)
            ),
            "The late reply must be disposed by the existing request completion owner."
        );
        Assert.Equal(1, operation.SubmissionCount);

        async Task RecoverCapacityAsync()
        {
            for (var sequence = 0; sequence < fillerCount; sequence++)
            {
                using var filler = Received.Create();
                Assert.True(pair.Server.Recv(filler));
                Assert.Equal(AdmissionPair.Payload(sequence), filler.SinglePartOrThrow().ToArray());
                pair.DrainCompletions();
            }
            Assert.True(
                SpinWait.SpinUntil(
                    () =>
                    {
                        pair.DrainCompletions();
                        return operation.LastSubmission.Admitted.IsCompleted;
                    },
                    TimeSpan.FromSeconds(2)
                )
            );
            await operation.LastSubmission.Admitted;
            if (admitBeforeDeadline)
                Assert.False(completion.IsCompleted);
        }
    }

    [Fact]
    public async Task BackpressuredSend_PreservesBindingCancellation()
    {
        using var pair = new AdmissionPair();
        using var cancellation = new CancellationTokenSource();
        for (var sequence = 0; sequence < 32; sequence++)
        {
            using var payload = Message.From(AdmissionPair.Payload(sequence));
            var submission = pair.Client.Send().Message(payload).Async(cancellation.Token);
            var admission = submission.EnsureAcceptedAsync();
            if (submission.Result == SubmitResult.Ok)
            {
                Assert.True(admission.IsCompletedSuccessfully);
                continue;
            }

            Assert.False(admission.IsCompleted);
            cancellation.Cancel();
            var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                admission.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(cancellation.Token, error.CancellationToken);
            return;
        }
        Assert.Fail("The undrained native queue did not apply backpressure.");
    }

    [Fact]
    public async Task RequestReplies_ProgressIndependentlyWithoutSerializingSubmissions()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var server = context.CreateRouterSocket();
        var serverRid = RoutingId.From("independent-replies");
        var endpoint = $"inproc://independent-replies-{Guid.NewGuid():N}";
        server.SetRoutingId(serverRid);
        server.Options.ReceiveTimeout = TimeSpan.FromSeconds(5);
        server.Bind(endpoint);
        using var client = new ZLinkRawRouterServicePort(
            context,
            RoutingId.From("independent-requests"),
            endpoint + "-source"
        );
        client.Start();
        client.Connect(endpoint, serverRid);

        const int recordCount = 16;
        var requests = new Task<ZLinkRawReplyEnvelope>[recordCount];
        var received = new Received?[recordCount];
        try
        {
            for (var sequence = 0; sequence < recordCount; sequence++)
                requests[sequence] = client.RequestAsync(
                    serverRid,
                    new ReadOnlyMemory<byte>[] { new byte[] { (byte)sequence } },
                    TimeSpan.FromSeconds(5)
                );

            for (var sequence = 0; sequence < recordCount; sequence++)
            {
                var record = Received.Create();
                received[sequence] = record;
                Assert.True(server.Recv(record));
                Assert.Equal(new byte[] { (byte)sequence }, record.SinglePartOrThrow().ToArray());
            }
            Assert.All(requests, request => Assert.False(request.IsCompleted));

            // Reply to the last request first: its submission must already
            // have reached the binding while every earlier reply was pending.
            for (var sequence = recordCount - 1; sequence >= 0; sequence--)
            {
                using var payload = Message.From(new byte[] { (byte)sequence });
                received[sequence]!.Reply().Message(payload).Submit();
                Assert.True(
                    SpinWait.SpinUntil(
                        () =>
                        {
                            client.TryReceive(out var unexpected);
                            unexpected?.Dispose();
                            return requests[sequence].IsCompleted;
                        },
                        TimeSpan.FromSeconds(5)
                    )
                );
                using var reply = await requests[sequence].WaitAsync(TimeSpan.FromSeconds(5));
                Assert.Equal(new byte[] { (byte)sequence }, Assert.Single(reply.Parts).ToArray());
                for (var pending = 0; pending < sequence; pending++)
                    Assert.False(requests[pending].IsCompleted);
            }
        }
        finally
        {
            foreach (var record in received)
                record?.Dispose();
        }
    }

    [Fact]
    public void RouteSendCall_ValidatesMessageBeforeSubmitCancellationCanRun()
    {
        Assert.Throws<InvalidOperationException>(() =>
            new ZLinkRouteSendCall<object>(
                runtime: null!,
                meshName: "mesh",
                targetNodeRid: RoutingId.From("target"),
                message: null!
            )
        );
    }

    [Fact]
    public void ManualPeerTombstone_ReassignmentKeepsOnlyLatestLogicalIdentity()
    {
        var connections = new ZLinkSpotPeerConnectionSet();
        var first = RoutingId.From("first");
        var second = RoutingId.From("second");

        connections.RetainManualPeerRid("tcp://127.0.0.1:7101", first);
        connections.RetainManualPeerRid("tcp://127.0.0.1:7101", second);

        Assert.False(connections.HasRetainedManualPeer(first));
        Assert.True(connections.HasRetainedManualPeer(second));
        Assert.False(new ZLinkSpotPeerConnectionSet().HasRetainedManualPeer(second));
    }

    [Fact]
    public void RouteSendFastFailure_DisposesEveryMessageBeforeHandoff()
    {
        var disposed = new List<Message>();
        for (var attempt = 0; attempt < 100; attempt++)
        {
            var parts = ZLinkMessageParts.Create(
                Message.From(new byte[] { 1, 2, 3 }),
                Message.From(new byte[] { 4, 5, 6 })
            );
            disposed.Add(parts[0]);
            disposed.Add(parts[1]);

            ZLinkRouteSendCall<object>.DisposeBeforeHandoff(parts);
        }

        Assert.Equal(200, disposed.Count);
        Assert.All(
            disposed,
            message => Assert.Throws<ObjectDisposedException>(() => _ = message.Size)
        );
    }

    [Fact]
    public async Task PublicRequest_AdmissionCompletesBeforeReplyAndCallsAreIsolated()
    {
        using var pair = new AdmissionPair();
        using var firstPayload = Message.From("first");
        using var secondPayload = Message.From("second");
        var first = StartPublicRequest(pair.Client.Request().Message(firstPayload));
        var second = StartPublicRequest(pair.Client.Request().Message(secondPayload));
        await first.Admitted.AsTask().WaitAsync(TimeSpan.FromSeconds(2));
        await second.Admitted.AsTask().WaitAsync(TimeSpan.FromSeconds(2));
        Assert.False(first.Reply.IsCompleted);
        Assert.False(second.Reply.IsCompleted);
        using var receivedFirst = Received.Create();
        using var receivedSecond = Received.Create();
        Assert.True(pair.Server.Recv(receivedFirst));
        Assert.True(pair.Server.Recv(receivedSecond));
        using var firstReply = Message.From("first-reply");
        receivedFirst.Reply().Message(firstReply).Submit();
        Assert.True(
            SpinWait.SpinUntil(
                () =>
                {
                    pair.DrainCompletions();
                    return first.Reply.IsCompleted;
                },
                TimeSpan.FromSeconds(2)
            )
        );
        ZLinkMessageParts.DisposeAll(await first);
        Assert.False(second.Reply.IsCompleted);
        using var secondReply = Message.From("second-reply");
        receivedSecond.Reply().Message(secondReply).Submit();
        Assert.True(
            SpinWait.SpinUntil(
                () =>
                {
                    pair.DrainCompletions();
                    return second.Reply.IsCompleted;
                },
                TimeSpan.FromSeconds(2)
            )
        );
        ValueTask<IReadOnlyList<Message>> compatibleReply = second;
        ZLinkMessageParts.DisposeAll(await compatibleReply);
    }

    [Fact]
    public async Task NativeInfrastructureAdmission_DoesNotCompleteEnclosingUserStage()
    {
        using var pair = new AdmissionPair();
        using var payload = Message.From("authority lookup");
        var infrastructureDone = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var admitUser = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var userReply = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var user = ZLinkRequestAdmission.Run<int>(async admission =>
        {
            var reply = await ZLinkRequestSubmissionOutcome.SubmitAndAwaitReplyAsync(
                pair.Client.Request().Message(payload),
                TimeSpan.FromSeconds(5)
            );
            ZLinkMessageParts.DisposeAll(reply);
            infrastructureDone.SetResult();
            await admitUser.Task;
            admission.Complete();
            return await userReply.Task;
        });
        using var received = Received.Create();
        Assert.True(pair.Server.Recv(received));
        Assert.False(user.Admitted.IsCompleted);
        using var response = Message.From("authority reply");
        received.Reply().Message(response).Submit();
        Assert.True(
            SpinWait.SpinUntil(
                () =>
                {
                    pair.DrainCompletions();
                    return infrastructureDone.Task.IsCompleted;
                },
                TimeSpan.FromSeconds(2)
            )
        );
        Assert.False(user.Admitted.IsCompleted);
        Assert.False(user.Reply.IsCompleted);
        admitUser.SetResult();
        await user.Admitted.AsTask().WaitAsync(TimeSpan.FromSeconds(2));
        Assert.False(user.Reply.IsCompleted);
        userReply.SetResult(42);
        Assert.Equal(42, await user);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task PublicRequest_BlockedAdmissionAndReplyFailWithSameCause(bool cancel)
    {
        using var pair = new AdmissionPair();
        var fillers = 0;
        for (var sequence = 0; sequence < 32; sequence++)
        {
            using var filler = Message.From(AdmissionPair.Payload(sequence));
            var submission = pair.Client.Send().Message(filler).Async();
            fillers++;
            if (submission.Result == SubmitResult.Backpressured)
                break;
        }
        using var cancellation = new CancellationTokenSource();
        using var payload = Message.From(AdmissionPair.Payload(100));
        var operation = new CountingRequestSubmitOperation(
            pair.Client.Request().Message(payload).Timeout(TimeSpan.FromSeconds(5))
        );
        var request = StartPublicRequest(
            operation,
            TimeSpan.FromMilliseconds(100),
            cancellation.Token
        );
        Assert.Equal(SubmitResult.Backpressured, operation.LastSubmission.Result);
        Assert.False(request.Admitted.IsCompleted);
        Assert.False(request.Reply.IsCompleted);
        if (cancel)
            cancellation.Cancel();
        var replyFailure = await Assert.ThrowsAnyAsync<Exception>(() =>
            request.AsTask().WaitAsync(TimeSpan.FromSeconds(2))
        );
        var admissionFailure = await Assert.ThrowsAnyAsync<Exception>(() =>
            request.Admitted.AsTask().WaitAsync(TimeSpan.FromSeconds(2))
        );
        Assert.Same(replyFailure, admissionFailure);
        if (cancel)
            Assert.IsAssignableFrom<OperationCanceledException>(replyFailure);
        else
            Assert.Equal(
                ZlinkRequestException.ErrorCode.TimedOut,
                Assert.IsType<ZlinkRequestException>(replyFailure).Result
            );
        Assert.Equal(1, operation.SubmissionCount);
        // Preserve the existing caller-cancellation connection and release the
        // pending native operation using the fixture's existing completion pump.
        for (var index = 0; index < fillers; index++)
        {
            using var filler = Received.Create();
            Assert.True(pair.Server.Recv(filler));
            pair.DrainCompletions();
        }
    }

    // Spec 04-messaging/08 section 3 E4: only Backpressured consumes the
    // binding's admitted task. The public default submission has Result=Ok and
    // no admitted task, so consuming it would fault inside the Framework.
    [Fact]
    public async Task OkSubmission_CompletesAdmissionWithoutConsumingBindingAdmitted()
    {
        var unobserved = new System.Collections.Concurrent.ConcurrentBag<Exception>();
        void OnUnobserved(object? sender, UnobservedTaskExceptionEventArgs args)
        {
            foreach (var error in args.Exception.Flatten().InnerExceptions)
            {
                if (
                    error is NullReferenceException
                    && error.StackTrace?.Contains(nameof(ZLinkRequestSubmissionOutcome)) == true
                )
                    unobserved.Add(error);
            }
        }

        var admitted = false;
        TaskScheduler.UnobservedTaskException += OnUnobserved;
        try
        {
            admitted = await SubmitDefaultOkSubmissionAdmitsBeforeReply();
            for (var round = 0; round < 3; round++)
            {
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }
            await Task.Yield();
        }
        finally
        {
            TaskScheduler.UnobservedTaskException -= OnUnobserved;
        }
        Assert.Empty(unobserved);
        Assert.True(admitted);
    }

    [System.Runtime.CompilerServices.MethodImpl(
        System.Runtime.CompilerServices.MethodImplOptions.NoInlining
    )]
    private static async Task<bool> SubmitDefaultOkSubmissionAdmitsBeforeReply()
    {
        var pending = new TaskCompletionSource<IReadOnlyList<Message>>();
        Task<IReadOnlyList<Message>>? reply = null;
        var request = ZLinkRequestAdmission.Run<IReadOnlyList<Message>>(admission =>
        {
            reply = ZLinkRequestSubmissionOutcome.SubmitAndAwaitReplyAsync(
                new FixedSubmissionOperation(default),
                TimeSpan.FromSeconds(5),
                requestAdmission: admission
            );
            return new ValueTask<IReadOnlyList<Message>>(pending.Task);
        });
        Assert.Null(reply);
        var admitted = request.Admitted.IsCompletedSuccessfully;
        pending.SetResult([]);
        await request;
        return admitted;
    }

    [Fact]
    public async Task TransportThousandHwmRequestsCreateNoDeadlineTimers()
    {
        using var pair = new AdmissionPair();
        using var setup = new CancellationTokenSource();
        SendSubmission blocked = default;
        for (var sequence = 0; sequence < 32; sequence++)
        {
            using var filler = Message.From(AdmissionPair.Payload(sequence));
            blocked = pair.Client.Send().Message(filler).Async(setup.Token);
            if (blocked.Result == SubmitResult.Backpressured)
                break;
        }
        Assert.Equal(SubmitResult.Backpressured, blocked.Result);
        var warmup = await Deadlines.RegisterAsync(TimeSpan.FromSeconds(5));
        warmup.Entry.Fail(new OperationCanceledException());
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => warmup.Completion);
        var timers = Timer.ActiveCount;
        var requests =
            new List<Zlink.Framework.Contracts.Channels.ZLinkRequest<IReadOnlyList<Message>>>();
        for (var index = 0; index < 1000; index++)
        {
            using var payload = Message.From("blocked request");
            requests.Add(
                StartPublicRequest(
                    pair.Client.Request().Message(payload).Timeout(TimeSpan.FromSeconds(5))
                )
            );
        }
        Console.WriteLine(
            $"hwm_requests=1000 timers_before={timers} timers_after={Timer.ActiveCount}"
        );
        Assert.Equal(timers, Timer.ActiveCount);
        Assert.Equal(1000, Deadlines.PendingCount);
        await Deadlines.DisposeAsync();
        foreach (var request in requests)
        {
            await Assert.ThrowsAsync<ZLinkFrameworkException>(() => request.AsTask());
            await Assert.ThrowsAsync<ZLinkFrameworkException>(() => request.Admitted.AsTask());
        }
        Assert.Equal(0, Deadlines.PendingCount);
        setup.Cancel();
    }

    [Fact]
    public async Task TransportResolveAndHwmConsumeOneAbsoluteEntryDeadline()
    {
        using var pair = new AdmissionPair();
        using var setup = new CancellationTokenSource();
        SendSubmission blocked = default;
        for (var i = 0; i < 32; i++)
        {
            using var filler = Message.From(AdmissionPair.Payload(i));
            blocked = pair.Client.Send().Message(filler).Async(setup.Token);
            if (blocked.Result == SubmitResult.Backpressured)
                break;
        }
        Assert.Equal(SubmitResult.Backpressured, blocked.Result);
        var time = new ControllableTimeProvider();
        await using var lane = new Zlink.Framework.Runtime.Execution.ZLinkStateLane();
        await using var index = new ZLinkRequestDeadlineIndex(lane, time);
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var submitted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var route = new ZLinkPeerRouteObservation(1, 0, changed.Task);
        var started = time.GetTimestamp();
        var remainingAtSubmit = TimeSpan.Zero;
        var call = index.RunAsync(
            TimeSpan.FromSeconds(1),
            CancellationToken.None,
            (entry, terminal) =>
                ZLinkDurableRequest
                    .RequestAsync<IReadOnlyList<Message>>(
                        [],
                        started,
                        TimeSpan.FromSeconds(1),
                        (_, remaining, token) =>
                        {
                            remainingAtSubmit = remaining;
                            using var payload = Message.From("resolved-hwm-request");
                            var request = ZLinkRequestSubmissionOutcome.SubmitAndAwaitReplyAsync(
                                pair.Client.Request().Message(payload).Timeout(remaining),
                                remaining,
                                token
                            );
                            submitted.SetResult();
                            return new(request);
                        },
                        CancellationToken.None,
                        () => route,
                        time,
                        terminal: terminal
                    )
                    .AsTask()
        );
        Assert.Equal(1, time.ActiveTimerCount);
        time.AdvanceMonotonic(TimeSpan.FromMilliseconds(600));
        route = new(1, 1, new TaskCompletionSource().Task);
        changed.SetResult();
        await submitted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(TimeSpan.FromMilliseconds(400), remainingAtSubmit);
        time.AdvanceMonotonic(TimeSpan.FromMilliseconds(400) - TimeSpan.FromTicks(1));
        Assert.False(call.IsCompleted);
        time.AdvanceMonotonic(TimeSpan.FromTicks(1));
        var error = await Assert.ThrowsAsync<ZlinkRequestException>(() =>
            call.WaitAsync(TimeSpan.FromSeconds(5))
        );
        Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, error.Result);
        Assert.Equal(0, index.PendingCount);
        Assert.Equal(1, time.ActiveTimerCount);
        setup.Cancel();
    }

    [Fact]
    public void NativeRequestPathHasNoCtsCreationSites()
    {
        var flags =
            System.Reflection.BindingFlags.Static
            | System.Reflection.BindingFlags.Instance
            | System.Reflection.BindingFlags.Public
            | System.Reflection.BindingFlags.NonPublic;
        var opcodes = typeof(System.Reflection.Emit.OpCodes)
            .GetFields()
            .Where(field => field.FieldType == typeof(System.Reflection.Emit.OpCode))
            .Select(field => (System.Reflection.Emit.OpCode)field.GetValue(null)!)
            .ToDictionary(code => unchecked((ushort)code.Value));
        var types = new[]
        {
            typeof(ZLinkRequestSubmissionOutcome),
            typeof(ZLinkRequestDeadlineIndex),
        }.SelectMany(type => new[] { type }.Concat(type.GetNestedTypes(flags)));
        var creationSites = 0;
        foreach (var method in types.SelectMany(type => type.GetMethods(flags)))
        {
            var il = method.GetMethodBody()?.GetILAsByteArray();
            if (il is null)
                continue;
            for (var offset = 0; offset < il.Length; )
            {
                ushort value = il[offset++];
                if (value == 0xfe)
                    value = (ushort)(0xfe00 | il[offset++]);
                var opcode = opcodes[value];
                if (
                    opcode == System.Reflection.Emit.OpCodes.Call
                    || opcode == System.Reflection.Emit.OpCodes.Newobj
                )
                {
                    var called = method.Module.ResolveMethod(BitConverter.ToInt32(il, offset));
                    if (
                        called?.DeclaringType == typeof(CancellationTokenSource)
                        && (
                            called.IsConstructor
                            || called.Name
                                == nameof(CancellationTokenSource.CreateLinkedTokenSource)
                        )
                    )
                        creationSites++;
                }
                offset += opcode.OperandType switch
                {
                    System.Reflection.Emit.OperandType.InlineNone => 0,
                    System.Reflection.Emit.OperandType.ShortInlineBrTarget
                    or System.Reflection.Emit.OperandType.ShortInlineI
                    or System.Reflection.Emit.OperandType.ShortInlineVar => 1,
                    System.Reflection.Emit.OperandType.InlineVar => 2,
                    System.Reflection.Emit.OperandType.InlineI8
                    or System.Reflection.Emit.OperandType.InlineR => 8,
                    System.Reflection.Emit.OperandType.InlineSwitch => 4
                        + 4 * BitConverter.ToInt32(il, offset),
                    _ => 4,
                };
            }
        }
        Console.WriteLine($"native_request_cts_creation_sites={creationSites}");
        Assert.Equal(0, creationSites);
    }

    private sealed class FixedSubmissionOperation(RequestSubmission submission)
        : RequestSubmitOperation
    {
        public RequestSubmitOperation Message(Message message) => this;

        public RequestSubmitOperation Timeout(TimeSpan timeout) => this;

        public IReadOnlyList<Message> Submit() => throw new NotSupportedException();

        public RequestSubmission Async(CancellationToken cancellationToken = default) => submission;
    }

    private Zlink.Framework.Contracts.Channels.ZLinkRequest<
        IReadOnlyList<Message>
    > StartPublicRequest(
        RequestSubmitOperation operation,
        TimeSpan? timeout = null,
        CancellationToken cancellationToken = default
    ) =>
        ZLinkRequestAdmission.Run<IReadOnlyList<Message>>(admission => new ValueTask<
            IReadOnlyList<Message>
        >(
            ZLinkRequestSubmissionOutcome.SubmitAndAwaitReplyAsync(
                operation,
                timeout ?? TimeSpan.FromSeconds(5),
                cancellationToken,
                requestAdmission: admission,
                deadlines: Deadlines
            )
        ));

    private sealed class AdmissionPair : IDisposable
    {
        private readonly IContext _context = Systems.Zlink.Zlink.CreateContext();
        private readonly IPoller _completionPoller;
        private readonly PollEvent[] _completionEvents = new PollEvent[1];
        internal IRouterSocket Server { get; }
        internal IDealerSocket Client { get; }

        internal AdmissionPair()
        {
            _context.Options.AutoHwmEnabled = false;
            Server = _context.CreateRouterSocket();
            Client = _context.CreateDealerSocket();
            // The production HWM is unchanged; this fixture makes saturation
            // deterministic with a bounded number of full-size records.
            Server.Options.ReceiveHighWaterMark = 65_536UL + 64UL;
            Client.Options.SendHighWaterMark = 65_536UL + 64UL;
            Server.Options.ReceiveTimeout = TimeSpan.FromSeconds(5);
            Server.Options.Linger = TimeSpan.Zero;
            Client.Options.Linger = TimeSpan.Zero;
            var endpoint = $"inproc://submission-admission-{Guid.NewGuid():N}";
            Server.Bind(endpoint);
            Client.Connect(endpoint);
            using var ready = Message.From("ready");
            Client.Send().Message(ready).Submit();
            using var received = Received.Create();
            Assert.True(Server.Recv(received));
            Assert.Equal("ready", received.SinglePartOrThrow().GetString());
            _completionPoller = Systems.Zlink.Zlink.CreatePoller();
            _completionPoller.Add(Client, PollEventFlags.PollCompletion, 1);
        }

        internal void DrainCompletions() =>
            _completionPoller.Wait(_completionEvents, TimeSpan.Zero);

        internal async Task<bool> WaitForReceiveAsync()
        {
            _completionPoller.Add(Server, PollEventFlags.PollIn, 2);
            try
            {
                var events = new PollEvent[2];
                var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
                // Keep the sole completion owner progressing while yielding to
                // the producer continuation before receiving the next record.
                // A completion event can wake the poller first; keep waiting for PollIn.
                while (DateTime.UtcNow < deadline)
                {
                    var remaining = deadline - DateTime.UtcNow;
                    var count = await Task.Run(() => _completionPoller.Wait(events, remaining));
                    if (
                        events
                            .Take(count)
                            .Any(e => e.Slot == 2 && (e.Revents & PollEventFlags.PollIn) != 0)
                    )
                        return true;
                }
                return false;
            }
            finally
            {
                _completionPoller.Remove(Server);
            }
        }

        internal static byte[] Payload(int sequence)
        {
            var payload = new byte[65_536];
            payload.AsSpan().Fill((byte)sequence);
            return payload;
        }

        public void Dispose()
        {
            _completionPoller.Dispose();
            Client.Dispose();
            Server.Dispose();
            _context.Dispose();
        }
    }

    private sealed class CountingRequestSubmitOperation : RequestSubmitOperation
    {
        private readonly RequestSubmitOperation _inner;

        internal CountingRequestSubmitOperation(RequestSubmitOperation inner) => _inner = inner;

        internal int SubmissionCount { get; private set; }

        internal RequestSubmission LastSubmission { get; private set; }

        internal long AllocatedBytes { get; private set; }

        internal CancellationToken SubmissionToken { get; private set; }

        public RequestSubmitOperation Message(Message message)
        {
            _inner.Message(message);
            return this;
        }

        public RequestSubmitOperation Timeout(TimeSpan timeout)
        {
            _inner.Timeout(timeout);
            return this;
        }

        public IReadOnlyList<Message> Submit() => _inner.Submit();

        public RequestSubmission Async(CancellationToken cancellationToken = default)
        {
            SubmissionCount++;
            SubmissionToken = cancellationToken;
            var allocatedBefore = GC.GetAllocatedBytesForCurrentThread();
            LastSubmission = _inner.Async(cancellationToken);
            AllocatedBytes = GC.GetAllocatedBytesForCurrentThread() - allocatedBefore;
            return LastSubmission;
        }
    }
}
