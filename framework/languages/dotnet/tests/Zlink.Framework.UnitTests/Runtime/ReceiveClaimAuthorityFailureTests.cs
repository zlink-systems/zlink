using System.Collections.Concurrent;
using System.Reflection;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;
using FailureCode = Systems.Zlink.Framework.Runtime.Protocol.ServiceWireConstants.FrameworkErrorCode;

namespace Zlink.Framework.UnitTests;

public sealed class ReceiveClaimAuthorityFailureTests
{
    [Theory]
    [InlineData(0, false)]
    [InlineData(1, false)]
    [InlineData(3, false)]
    [InlineData(0, true)]
    [InlineData(1, true)]
    [InlineData(3, true)]
    public async Task AuthorityFailureSettlesOnlyItsRecord(int failedIndex, bool suspended)
    {
        var node = DispatchProxy.Create<IMeshNode, ClaimedNode>();
        var source = (ClaimedNode)(object)node;
        var failure = new IOException("authority read failed");
        var acceptance = new Acceptance(failedIndex, suspended, failure);
        var failures = new Failures();
        var runner = new ZLinkRuntimeTaskRunner(failures, CancellationToken.None);
        await using var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable());
        pump.SetRequestSourceAcceptance(acceptance);
        pump.SetNodeRouteHandler((_, _) => ValueTask.CompletedTask, runner);
        var state = pump.RegisterSpot("owner");
        var delivered = new ConcurrentQueue<string>();
        var complete = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        pump.SetDispatchHandler(
            "owner",
            _ =>
            {
                while (state.Routes.TryDequeue(out var received))
                    using (received)
                    {
                        delivered.Enqueue(received.SourceNodeRid!.Value.ToString());
                        if (delivered.Count == 3)
                            complete.TrySetResult();
                    }
                return (ValueTask.CompletedTask, null);
            }
        );
        var terminals = new ConcurrentQueue<(int Index, RequestResult Result, uint Code)>();
        for (var index = 0; index < 4; index++)
        {
            var recordIndex = index;
            source.Records.Add(
                new MeshReceiveRecord(
                    MeshRecordKind.SpotRequest,
                    MeshReadyDomains.Application,
                    RoutingId.From($"source-{index}"),
                    "sender",
                    1,
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
                    terminalReply: (result, code) =>
                    {
                        terminals.Enqueue((recordIndex, result, code));
                        return SubmitResult.Ok;
                    }
                )
            );
        }
        pump.EnsureStarted();
        source.Publish();
        if (suspended)
        {
            await acceptance.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            acceptance.Release.TrySetException(failure);
        }
        await complete.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await pump.DisposeAsync();
        await runner.StopAsync();
        Assert.Equal(1, source.ReceiveCount);
        Assert.Equal(4, acceptance.Reads);
        Assert.Equal(
            Enumerable.Range(0, 4).Where(i => i != failedIndex).Select(i => $"source-{i}"),
            delivered
        );
        var terminal = Assert.Single(terminals);
        Assert.Equal(failedIndex, terminal.Index);
        Assert.Equal(RequestResult.InternalError, terminal.Result);
        Assert.Equal((uint)FailureCode.RequestFailed, terminal.Code);
        Assert.Same(failure, Assert.Single(failures.Errors));
    }

    private sealed class Acceptance(int failedIndex, bool suspended, Exception failure)
        : IZLinkBackendRequestSourceAcceptance
    {
        internal int Reads;
        internal TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource<ZLinkServiceWireCodec.RequestSourceFence?> Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ValueTask<ZLinkServiceWireCodec.RequestSourceFence?> AcceptAsync(
            ZLinkServiceWireCodec.RequestSourceFence? polled,
            RoutingId sourceNodeRid,
            ulong sourceNodeGeneration,
            CancellationToken cancellationToken
        )
        {
            Reads++;
            if (sourceNodeRid.ToString() != $"source-{failedIndex}")
                return ValueTask.FromResult<ZLinkServiceWireCodec.RequestSourceFence?>(null);
            Entered.TrySetResult();
            if (suspended)
                return new(Release.Task);
            throw failure;
        }
    }

    private sealed class Failures : IZLinkRuntimeFailureReporter
    {
        internal ConcurrentQueue<Exception> Errors { get; } = new();

        public void ReportHandlerException(Exception exception) => Errors.Enqueue(exception);

        public void ReportUnhandledCallbackException(Exception exception) =>
            Errors.Enqueue(exception);

        public void ReportRuntimeTaskException(string name, Exception exception) =>
            Errors.Enqueue(exception);
    }

    private class ClaimedNode : DispatchProxy
    {
        internal List<MeshReceiveRecord> Records { get; } = [];
        internal int ReceiveCount;
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
                            new(MeshOwnerKind.Spot, MeshReadyDomains.Application, "owner", default),
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
            ReceiveCount++;
            foreach (var record in Records)
                batch.Add(record, Array.Empty<Message>());
            return ValueTask.FromResult(true);
        }
    }
}
