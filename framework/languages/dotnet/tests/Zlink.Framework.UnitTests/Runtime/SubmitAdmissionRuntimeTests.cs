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
        Assert.Same(blocked.Admitted, blocked.EnsureAcceptedAsync());
        Assert.False(blocked.Admitted.IsCompleted);

        // Receiving releases Core capacity. Only the binding can resubmit the
        // blocked record; the producer resumes with the next sequence number.
        for (var sequence = 0; sequence < recordCount; sequence++)
        {
            using var received = Received.Create();
            Assert.True(pair.Server.Recv(received));
            Assert.Equal(AdmissionPair.Payload(sequence),
                received.SinglePartOrThrow().ToArray());
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
            var error = await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => admission.WaitAsync(TimeSpan.FromSeconds(5)));
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
        using var client = new ZLinkRawRouterServicePort(context,
            RoutingId.From("independent-requests"), endpoint + "-source");
        client.Start();
        client.Connect(endpoint, serverRid);

        const int recordCount = 16;
        var requests = new Task<ZLinkRawReplyEnvelope>[recordCount];
        var received = new Received?[recordCount];
        try
        {
            for (var sequence = 0; sequence < recordCount; sequence++)
                requests[sequence] = client.RequestAsync(serverRid,
                    new ReadOnlyMemory<byte>[] { new byte[] { (byte)sequence } },
                    TimeSpan.FromSeconds(5));

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
                message: null!));
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
                Message.From(new byte[] { 4, 5, 6 }));
            disposed.Add(parts[0]);
            disposed.Add(parts[1]);

            ZLinkRouteSendCall<object>.DisposeBeforeHandoff(parts);
        }

        Assert.Equal(200, disposed.Count);
        Assert.All(disposed, message =>
            Assert.Throws<ObjectDisposedException>(() => _ = message.Size));
    }

    private sealed class AdmissionPair : IDisposable
    {
        private readonly IContext _context = Systems.Zlink.Zlink.CreateContext();
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
        }

        internal static byte[] Payload(int sequence)
        {
            var payload = new byte[65_536];
            payload.AsSpan().Fill((byte)sequence);
            return payload;
        }

        public void Dispose()
        {
            Client.Dispose();
            Server.Dispose();
            _context.Dispose();
        }
    }
}
