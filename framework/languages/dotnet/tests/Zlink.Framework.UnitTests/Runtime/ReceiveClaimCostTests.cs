using System.Reflection;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class ReceiveClaimCostTests(Xunit.Abstractions.ITestOutputHelper output)
{
    [Theory]
    [InlineData(true, 1)]
    [InlineData(false, 1)]
    [InlineData(true, 64)]
    [InlineData(false, 64)]
    public async Task NodeClaimAllocatesOnlyItsRoutesAndClaimSizedList(bool request, int batchSize)
    {
        var node = DispatchProxy.Create<IMeshNode, ClaimedNode>();
        var source = (ClaimedNode)(object)node;
        using var frame = Message.From(Array.Empty<byte>());
        var replies = 0;
        Func<IReadOnlyList<Message>, SubmitResult>? reply = request
            ? _ =>
            {
                replies++;
                return SubmitResult.Ok;
            }
            : null;
        source.Record = new MeshReceiveRecord(
            request ? MeshRecordKind.NodeRequest : MeshRecordKind.NodeSend,
            MeshReadyDomains.Application,
            default,
            string.Empty,
            0,
            default,
            default,
            default,
            null,
            null,
            null,
            0,
            0,
            0,
            0,
            null,
            reply: reply,
            applicationPayloadView: new ZLinkMultipartPayloadView(frame, 0, default, default, null)
        );
        source.BatchSize = batchSize;
        await using var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
        TaskCompletionSource<(long Actual, long Baseline)>? completed = null;
        pump.SetNodeRouteHandler(
            (records, _) =>
            {
                var actual = GC.GetAllocatedBytesForCurrentThread() - source.StartBytes;
                var start = GC.GetAllocatedBytesForCurrentThread();
                var baseline = new List<ZLinkBackendRouteReceived>(batchSize);
                for (var index = 0; index < batchSize; index++)
                    baseline.Add(
                        new ZLinkBackendRouteReceived(
                            Array.Empty<Message>(),
                            default(RoutingId),
                            string.Empty,
                            null,
                            reply,
                            applicationPayloadView: source.Record.ApplicationPayloadView
                        )
                    );
                var expected = GC.GetAllocatedBytesForCurrentThread() - start;
                try
                {
                    Assert.Equal(source.ReceiveThread, Environment.CurrentManagedThreadId);
                    Assert.Equal(batchSize, records.Count);
                    foreach (var record in records)
                    {
                        Assert.Equal(request, record.CanReply);
                        if (request)
                            Assert.Equal(SubmitResult.Ok, record.Reply(Array.Empty<Message>()));
                        record.Dispose();
                    }
                    completed!.TrySetResult((actual, expected));
                }
                catch (Exception failure)
                {
                    completed!.TrySetException(failure);
                }
                GC.KeepAlive(baseline);
                return ValueTask.CompletedTask;
            }
        );
        pump.EnsureStarted();
        const int warmup = 128;
        const int iterations = 1000;
        var actualSamples = new List<long>();
        var baselineSamples = new List<long>();
        var warmupStart = GC.GetTotalAllocatedBytes(precise: true);
        var enteredNoGc = false;
        try
        {
            for (var index = 0; index < warmup + iterations; index++)
            {
                if (index == warmup)
                {
                    var warmupBytes = GC.GetTotalAllocatedBytes(precise: true) - warmupStart;
                    var noGcBudget = checked(warmupBytes * ((iterations + warmup - 1) / warmup));
                    // Background GC can count an unused allocation-context tail, so isolate measurements from GC.
                    Assert.True(GC.TryStartNoGCRegion(noGcBudget));
                    enteredNoGc = true;
                }
                completed = new(TaskCreationOptions.RunContinuationsAsynchronously);
                source.Publish();
                var measured = await completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
                if (index >= warmup)
                {
                    actualSamples.Add(measured.Actual);
                    baselineSamples.Add(measured.Baseline);
                }
            }
        }
        finally
        {
            if (enteredNoGc)
                GC.EndNoGCRegion();
        }
        Assert.Equal(request ? batchSize * (warmup + iterations) : 0, replies);
        var allocated = PerCallAllocation.Mode(actualSamples);
        var baselineBytes = PerCallAllocation.Mode(baselineSamples);
        output.WriteLine(
            $"request={request}, batchSize={batchSize}, claim={allocated}B, baseline={baselineBytes}B, excess={allocated - baselineBytes}B/batch"
        );
        Assert.True(
            allocated == baselineBytes,
            $"claim={allocated}B, routes + claim-sized list={baselineBytes}B, excess={allocated - baselineBytes}B/batch"
        );
    }

    private class ClaimedNode : DispatchProxy
    {
        internal MeshReceiveRecord Record;
        internal int BatchSize;
        internal long StartBytes;
        internal int ReceiveThread;
        private int _pending;
        private Func<MeshReadyDomains, MeshReadyDomains>? _ready;

        internal void Publish()
        {
            Interlocked.Exchange(ref _pending, 1);
            _ready!(MeshReadyDomains.Application);
        }

        protected override object? Invoke(MethodInfo? method, object?[]? arguments)
        {
            switch (method!.Name)
            {
                case nameof(IMeshNode.SetReadyHandler):
                    _ready = (Func<MeshReadyDomains, MeshReadyDomains>?)arguments![0];
                    return null;
                case nameof(IMeshNode.DrainReadyAsync):
                    if (Interlocked.Exchange(ref _pending, 0) != 0)
                        ((MeshReadyBatch)arguments![1]!).Add(
                            new(
                                MeshOwnerKind.Node,
                                MeshReadyDomains.Application,
                                string.Empty,
                                default
                            ),
                            new MeshClaim { Receiver = Receive }
                        );
                    return ValueTask.FromResult(false);
                case nameof(IMeshNode.CloseSubmissions):
                    return null;
                default:
                    throw new NotSupportedException(method.Name);
            }
        }

        private ValueTask<bool> Receive(MeshReceiveBatch batch, RecvFlags flags)
        {
            for (var index = 0; index < BatchSize; index++)
                batch.Add(Record, Array.Empty<Message>());
            ReceiveThread = Environment.CurrentManagedThreadId;
            StartBytes = GC.GetAllocatedBytesForCurrentThread();
            return ValueTask.FromResult(true);
        }
    }
}
