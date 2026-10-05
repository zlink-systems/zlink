using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SubmitAdmissionRuntimeTests
{
    [Fact]
    public void OkSubmission_DoesNotReadAdmissionOrAllocateACompletion()
    {
        // The default public value has Result=Ok and no admission task. This
        // isolates the snapshot decision without accessing binding internals.
        SendSubmission submission = default;
        Assert.Equal(SubmitResult.Ok, submission.Result);
        Assert.Same(Task.CompletedTask, submission.EnsureAcceptedAsync());

        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < 10_000; index++)
            _ = submission.EnsureAcceptedAsync();
        Assert.Equal(0, GC.GetAllocatedBytesForCurrentThread() - before);
    }

    [Fact]
    public async Task BackpressuredRequestSubmission_WaitsForAdmissionBeforeReturningReply()
    {
        var admitted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var expectedReply = Array.Empty<Message>();
        var reply = new TaskCompletionSource<IReadOnlyList<Message>>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var completion = ZLinkRequestSubmissionOutcome.AwaitReplyAsync(
            SubmitResult.Backpressured,
            admitted.Task,
            reply.Task
        );

        reply.SetResult(expectedReply);
        Assert.False(completion.IsCompleted);

        admitted.SetResult();
        Assert.Same(expectedReply, await completion);
    }

    [Fact]
    public async Task BackpressuredRequestSubmission_PropagatesAdmissionFailure()
    {
        var expectedFailure = new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotFound);
        var expectedReply = Array.Empty<Message>();
        // Keep Reply successful so the admission failure must be observed independently.
        var completion = ZLinkRequestSubmissionOutcome.AwaitReplyAsync(
            SubmitResult.Backpressured,
            Task.FromException(expectedFailure),
            Task.FromResult<IReadOnlyList<Message>>(expectedReply)
        );

        var failure = await Assert.ThrowsAsync<ZlinkSubmitException>(() => completion);

        Assert.Same(expectedFailure, failure);
    }

    [Fact]
    public async Task AcceptedRequestSubmission_UsesReplyWithoutWaitingForAdmission()
    {
        var admitted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var expectedReply = Array.Empty<Message>();
        var reply = Task.FromResult<IReadOnlyList<Message>>(expectedReply);

        var completion = ZLinkRequestSubmissionOutcome.AwaitReplyAsync(
            SubmitResult.Ok,
            admitted.Task,
            reply
        );

        Assert.Same(reply, completion);
        Assert.Same(expectedReply, await completion);
        Assert.False(admitted.Task.IsCompleted);
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

        using var cancellation = new CancellationTokenSource();
        var allocatedBefore = GC.GetAllocatedBytesForCurrentThread();
        var completion = ZLinkRequestSubmissionOutcome.SubmitAndAwaitReplyAsync(
            operation,
            TimeSpan.FromSeconds(5),
            cancellation.Token
        );
        var frameworkAllocated =
            GC.GetAllocatedBytesForCurrentThread() - allocatedBefore - operation.AllocatedBytes;

        Assert.Equal(1, operation.SubmissionCount);
        Assert.Equal(SubmitResult.Ok, operation.LastSubmission.Result);
        // Measure only the Framework wrapper: the public binding's allocations
        // are counted separately inside Async on the same thread.
        Assert.Equal(0, frameworkAllocated);
        Assert.Equal(cancellation.Token, operation.SubmissionToken);
        Assert.Same(operation.LastSubmission.Reply, completion);
        using var received = Received.Create();
        Assert.True(pair.Server.Recv(received));
        Assert.Equal("request-once", received.SinglePartOrThrow().GetString());
        received.Reply().Message(replyPart).Submit();
        Assert.True(
            SpinWait.SpinUntil(
                () =>
                {
                    pair.DrainCompletions();
                    return completion.IsCompleted;
                },
                TimeSpan.FromSeconds(5)
            )
        );

        var terminal = await completion;
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
        var completion = ZLinkRequestSubmissionOutcome.SubmitAndAwaitReplyAsync(
            operation,
            TimeSpan.FromMilliseconds(100)
        );
        Assert.Equal(SubmitResult.Backpressured, operation.LastSubmission.Result);
        if (admitBeforeDeadline)
            await RecoverCapacityAsync();
        var failure = await Assert.ThrowsAsync<ZlinkRequestException>(() =>
            completion.WaitAsync(TimeSpan.FromSeconds(2))
        );
        Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, failure.Result);
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
