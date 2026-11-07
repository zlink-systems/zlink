using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshReceiveHandoffTests
{
    private static readonly TimeSpan CompletionBound = TimeSpan.FromSeconds(10);

    // Spec 08 §4.4: existing application workers execute independently of the receiver.
    [Fact]
    public async Task ReadyOwner_RunsOnApplicationWorkerWhileReceiveAndManagementProgress()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                typeof(MeshReceiveHandoffTests),
                nameof(ReadyOwner_RunsOnApplicationWorkerWhileReceiveAndManagementProgress)
            )
        )
            return;
        await RunHandoffAsync(false);
    }

    [Fact]
    public async Task ReceiveRoleHandoff_WhenPoolIsFull_KeepsNativeReceiveAndDeadlineProgressing()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                typeof(MeshReceiveHandoffTests),
                nameof(ReceiveRoleHandoff_WhenPoolIsFull_KeepsNativeReceiveAndDeadlineProgressing)
            )
        )
            return;
        await RunHandoffAsync(true);
    }

    [Fact]
    public async Task ReadyOwner_StartsBeforeInjectedReceiveTurnReturns()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                typeof(MeshReceiveHandoffTests),
                nameof(ReadyOwner_StartsBeforeInjectedReceiveTurnReturns)
            )
        )
            return;
        using var started = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var finished = Signal();
        var errors = new System.Collections.Concurrent.ConcurrentQueue<Exception>();
        var receiverThread = 0;
        var handlerThread = 0;
        await using var receiver = ZLinkProcessExecutionPool.CreateReceiveSource(() =>
            new RecordingPoller(() => { })
        );
        await using var application = ZLinkProcessExecutionPool.CreateApplicationSource(
            _ =>
            {
                handlerThread = Environment.CurrentManagedThreadId;
                started.Set();
                Assert.True(release.Wait(CompletionBound));
                return ValueTask.CompletedTask;
            },
            errors.Enqueue
        );
        var wait = receiver.WaitAsync([], Timeout.InfiniteTimeSpan).GetAwaiter();
        // Inject a finite receive turn: it commits a ready owner and retains
        // the receive resource until that owner's existing worker has started.
        wait.UnsafeOnCompleted(() =>
        {
            try
            {
                wait.GetResult();
                receiverThread = Environment.CurrentManagedThreadId;
                application.Signal();
                Assert.True(started.Wait(CompletionBound));
                Assert.NotEqual(receiverThread, handlerThread);
                finished.TrySetResult();
            }
            catch (Exception error)
            {
                finished.TrySetException(error);
            }
        });
        receiver.Wake();
        try
        {
            await finished.Task.WaitAsync(CompletionBound + CompletionBound);
        }
        finally
        {
            release.Set();
        }
        await application.DisposeAsync();
        Assert.Empty(errors);
    }

    [ThreadStatic]
    private static int _mailboxTurn;

    [Fact]
    public async Task ChannelMailbox_ExecutesHandlerInClaimTurn_AndRetainsReplyDuringDrain()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                typeof(MeshReceiveHandoffTests),
                nameof(ChannelMailbox_ExecutesHandlerInClaimTurn_AndRetainsReplyDuringDrain)
            )
        )
            return;
        await RunChannelMailboxAsync(false);
    }

    [Fact]
    public async Task ChannelMailbox_WithNoIdleWorker_KeepsReceiverAndManagementIndependent()
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                typeof(MeshReceiveHandoffTests),
                nameof(ChannelMailbox_WithNoIdleWorker_KeepsReceiverAndManagementIndependent)
            )
        )
            return;
        await RunChannelMailboxAsync(true);
    }

    private static async Task RunChannelMailboxAsync(bool exhaustApplicationResources)
    {
        var capture = new ChannelTurnCapture();
        var builder = Host.CreateApplicationBuilder();
        builder.Logging.ClearProviders().AddConsole().SetMinimumLevel(LogLevel.Trace);
        builder.Services.AddSingleton(capture).AddTransient<ChannelTurnHandler>();
        builder.Services.AddZLinkFramework(_ => { });
        using var host = builder.Build();
        var runtime = host.Services.GetRequiredService<ZLinkFrameworkRuntime>();
        var registration = host.Services.GetRequiredService<ZLinkFrameworkRegistration>();
        registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var spotNode = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "channel-turn",
            RoutingId = RoutingId.From("channel-turn-target"),
        };
        foreach (var channel in new[] { "work", "other" })
        {
            var membership = new ZLinkMeshChannelMembership { ChannelName = channel };
            membership.RequestHandlers.Add(
                new ZLinkChannelHandlerRegistration(
                    typeof(ChannelTurnHandler),
                    typeof(ChannelTurnMessage),
                    typeof(ChannelTurnMessage),
                    nameof(ChannelTurnMessage)
                )
            );
            spotNode.ChannelMemberships.Add(membership);
        }
        var failures = new System.Collections.Concurrent.ConcurrentQueue<Exception>();
        var reporter = new Runtime.AuditRuntimeFailureReporter();
        var runner = new ZLinkRuntimeTaskRunner(reporter, CancellationToken.None);
        await using var native = Systems.Zlink.Zlink.CreateContext();
        var context = new RecordingContext(native, channelRequests: true);
        var admitted = Signal();
        ZLinkManagedMeshNode? sourceNode = null;
        ZLinkManagedMeshNode? targetNode = null;
        IPoller Poller() =>
            new RecordingPoller(() =>
            {
                if (
                    sourceNode?.Status().AdmittedPeerCount == 1
                    && targetNode?.Status().AdmittedPeerCount == 1
                )
                    admitted.TrySetResult();
            });
        await using var source = sourceNode = new ZLinkManagedMeshNode(
            native,
            "channel-turn",
            receivePollerFactory: Poller
        );
        await using var target = targetNode = new ZLinkManagedMeshNode(
            context,
            "channel-turn",
            receivePollerFactory: Poller
        );
        var suffix = Guid.NewGuid().ToString("N");
        var sourceRid = RoutingId.From("channel-turn-source");
        var targetRid = spotNode.RoutingId;
        var targetEndpoint = $"inproc://channel-turn-target-{suffix}";
        source.SetRoutingId(sourceRid);
        source.SetBind($"inproc://channel-turn-source-{suffix}");
        target.SetRoutingId(targetRid);
        target.SetBind(targetEndpoint);
        target.AddChannel("work");
        target.AddChannel("other");
        source.ConnectPeer(targetEndpoint, targetRid);
        target.Start();
        source.Start();
        // The pump retains dispatch results; the dispatcher does not own another worker.
        var dispatcher = Assert.IsType<ZLinkMeshNodeRouteDispatcher>(
            ZLinkMeshNodeRouteDispatcher.Create(
                host.Services,
                registration,
                spotNode,
                runtime,
                runner
            )
        );
        await using var pump = new ZLinkMeshDispatchPump(target, new ZLinkMeshCompletionTable());
        var turns = 0;
        pump.SetNodeRouteHandler(
            (records, token) =>
            {
                _mailboxTurn = Interlocked.Increment(ref turns);
                foreach (var record in records)
                {
                    var header = record.ApplicationPayloadView is { } view
                        ? ZLinkEnvelopeCodec.DecodeHeader(view)
                        : ZLinkEnvelopeCodec.DecodeHeader(record.Parts);
                    capture.Claims[header.CorrelationId!] = (
                        Environment.CurrentManagedThreadId,
                        _mailboxTurn
                    );
                }
                try
                {
                    return dispatcher.DispatchBatchAsync(records, token);
                }
                finally
                {
                    _mailboxTurn = 0;
                }
            }
        );
        pump.EnsureStarted();
        await host.StartAsync();
        var blockers = new List<ZLinkProcessExecutionPool.ApplicationSource>();
        using var releaseWorkers = new ManualResetEventSlim();
        using var workersEntered = new CountdownEvent(
            ZLinkProcessExecutionPool.ApplicationWorkerCount
        );
        Task? stopping = null;
        try
        {
            await admitted.Task.WaitAsync(CompletionBound);
            if (exhaustApplicationResources)
            {
                for (var i = 0; i < ZLinkProcessExecutionPool.ApplicationWorkerCount; i++)
                {
                    var blocker = ZLinkProcessExecutionPool.CreateApplicationSource(
                        _ =>
                        {
                            workersEntered.Signal();
                            Assert.True(releaseWorkers.Wait(CompletionBound));
                            return ValueTask.CompletedTask;
                        },
                        failures.Enqueue
                    );
                    blockers.Add(blocker);
                    blocker.Signal();
                }
                Assert.True(workersEntered.Wait(CompletionBound));
            }
            var first = Submit("work", "first");
            var router = Assert.IsType<RecordingRouter>(context.Router);
            var receiveThread = await router.FirstReceived.Task.WaitAsync(CompletionBound);
            if (exhaustApplicationResources)
            {
                Assert.False(capture.FirstEntered.Task.IsCompleted);
                await CheckDeadline();
                Assert.Equal(0, Volatile.Read(ref turns));
                releaseWorkers.Set();
            }
            await capture.FirstEntered.Task.WaitAsync(CompletionBound);
            var second = Submit("work", "second");
            await router.SecondReceived.Task.WaitAsync(CompletionBound);
            var other = Submit("other", "other");
            Assert.Equal("other", await other.WaitAsync(CompletionBound));
            await CheckDeadline();
            Assert.False(capture.ReleaseSync.IsSet);
            Assert.False(first.IsCompleted);
            Assert.False(second.IsCompleted);
            Assert.Equal(receiveThread, router.SecondReceiveThread);
            Assert.NotEqual(receiveThread, capture.Starts["first"].Thread);
            capture.ReleaseSync.Set();
            Assert.Equal("first", await first.WaitAsync(CompletionBound));
            Assert.Equal("second", await second.WaitAsync(CompletionBound));
            Assert.Equal(new[] { "first", "second" }, capture.WorkOrder.ToArray());
            var held = Submit("other", "held");
            await capture.AsyncEntered.Task.WaitAsync(CompletionBound);
            runtime.SealApplicationAdmissionsForDrain(CancellationToken.None);
            Assert.Equal(
                "shutting_down",
                await Submit("work", "sealed").WaitAsync(CompletionBound)
            );
            Assert.False(capture.Starts.ContainsKey("sealed"));
            var drained = runtime.WaitForAcceptedOperationsForDrainAsync();
            stopping = pump.DisposeAsync().AsTask();
            Assert.False(stopping.IsCompleted);
            Assert.False(drained.IsCompleted);
            capture.ReleaseAsync.TrySetResult();
            Assert.Equal("held", await held.WaitAsync(CompletionBound));
            await Task.WhenAll(stopping, drained).WaitAsync(CompletionBound);
            foreach (var (key, started) in capture.Starts)
            {
                Assert.Equal(capture.Claims[key], started);
                Assert.NotEqual(0, started.Turn);
            }
            Assert.Equal(1, router.MaximumConcurrentReceives);
            Assert.Empty(failures);
            Assert.Empty(reporter.Failures);
        }
        finally
        {
            releaseWorkers.Set();
            capture.ReleaseSync.Set();
            capture.ReleaseAsync.TrySetResult();
            foreach (var blocker in blockers)
                await blocker.DisposeAsync();
            if (stopping is not null)
                await stopping;
            await pump.DisposeAsync();
            await runner.StopAsync();
            await host.StopAsync();
            capture.ReleaseSync.Dispose();
        }

        Task<string> Submit(string channel, string value)
        {
            var completion = new TaskCompletionSource<string>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var header = new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Request,
                channel,
                nameof(ChannelTurnMessage),
                ZLinkEnvelopeCodec.DefaultContentType,
                value,
                null,
                null,
                null,
                null
            );
            var parts = ZLinkEnvelopeCodec.EncodeParts(
                header,
                new ChannelTurnMessage(value),
                typeof(ChannelTurnMessage),
                null
            );
            try
            {
                Assert.Equal(
                    SubmitResult.Ok,
                    source
                        .EntrySpot()
                        .RequestToChannel(
                            channel,
                            parts,
                            (result, reply) =>
                            {
                                try
                                {
                                    Assert.Equal(RequestResult.Ok, result);
                                    var response = ZLinkEnvelopeCodec.DecodeHeader(reply);
                                    completion.TrySetResult(
                                        response.Kind == ZLinkMessageKind.Error
                                            ? response.ErrorCode!
                                            : Assert
                                                .IsType<ChannelTurnMessage>(
                                                    ZLinkEnvelopeCodec.DecodeBody(
                                                        reply,
                                                        typeof(ChannelTurnMessage)
                                                    )
                                                )
                                                .Value
                                    );
                                }
                                catch (Exception error)
                                {
                                    completion.TrySetException(error);
                                }
                            },
                            CompletionBound
                        )
                );
            }
            finally
            {
                foreach (var part in parts)
                    part.Dispose();
            }
            return completion.Task;
        }

        async Task CheckDeadline()
        {
            var deadline = source.RunDeadlineRequestAsync<int>(
                TimeSpan.FromMilliseconds(10),
                default,
                completion =>
                    throw new ZlinkRequestException(
                        (ZlinkRequestException.ErrorCode)(int)completion.Result
                    ),
                static (_, _) => Task.CompletedTask
            );
            var error = await Assert.ThrowsAsync<ZlinkRequestException>(() =>
                deadline.WaitAsync(CompletionBound)
            );
            Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, error.Result);
        }
    }

    private sealed record ChannelTurnMessage(string Value);

    private sealed class ChannelTurnCapture
    {
        internal readonly System.Collections.Concurrent.ConcurrentDictionary<
            string,
            (int Thread, int Turn)
        > Claims = new();
        internal readonly System.Collections.Concurrent.ConcurrentDictionary<
            string,
            (int Thread, int Turn)
        > Starts = new();
        internal readonly System.Collections.Concurrent.ConcurrentQueue<string> WorkOrder = new();
        internal readonly TaskCompletionSource FirstEntered = Signal();
        internal readonly TaskCompletionSource AsyncEntered = Signal();
        internal readonly ManualResetEventSlim ReleaseSync = new();
        internal readonly TaskCompletionSource ReleaseAsync = Signal();
    }

    private sealed class ChannelTurnHandler(ChannelTurnCapture capture)
        : IZLinkRequestHandler<ChannelTurnMessage, ChannelTurnMessage>
    {
        public async ValueTask<ChannelTurnMessage> HandleAsync(
            ChannelTurnMessage request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        )
        {
            capture.Starts[request.Value] = (Environment.CurrentManagedThreadId, _mailboxTurn);
            if (request.Value is "first" or "second")
                capture.WorkOrder.Enqueue(request.Value);
            if (request.Value == "first")
            {
                capture.FirstEntered.TrySetResult();
                Assert.True(capture.ReleaseSync.Wait(CompletionBound));
            }
            if (request.Value == "held")
            {
                capture.AsyncEntered.TrySetResult();
                await capture.ReleaseAsync.Task;
            }
            return request;
        }
    }

    private static async Task RunHandoffAsync(bool exhaustApplicationResources)
    {
        await using var nativeContext = Systems.Zlink.Zlink.CreateContext();
        var targetContext = new RecordingContext(nativeContext);
        var sourceAdmitted = Signal();
        var targetAdmitted = Signal();
        RecordingPoller? processPoller = null;
        ZLinkManagedMeshNode? sourceNode = null;
        ZLinkManagedMeshNode? targetNode = null;
        await using var source = sourceNode = new ZLinkManagedMeshNode(
            nativeContext,
            "receive-handoff",
            receivePollerFactory: () =>
                processPoller = new RecordingPoller(() =>
                {
                    if (sourceNode?.Status().AdmittedPeerCount == 1)
                        sourceAdmitted.TrySetResult();
                    if (targetNode?.Status().AdmittedPeerCount == 1)
                        targetAdmitted.TrySetResult();
                })
        );
        await using var target = targetNode = new ZLinkManagedMeshNode(
            targetContext,
            "receive-handoff",
            receivePollerFactory: () =>
                processPoller = new RecordingPoller(() =>
                {
                    if (targetNode?.Status().AdmittedPeerCount == 1)
                        targetAdmitted.TrySetResult();
                    if (sourceNode?.Status().AdmittedPeerCount == 1)
                        sourceAdmitted.TrySetResult();
                })
        );
        var sourceRid = RoutingId.From("handoff-source");
        var targetRid = RoutingId.From("handoff-target");
        var suffix = Guid.NewGuid().ToString("N");
        var targetEndpoint = $"inproc://handoff-target-{suffix}";
        source.SetRoutingId(sourceRid);
        target.SetRoutingId(targetRid);
        source.SetBind($"inproc://handoff-source-{suffix}");
        target.SetBind(targetEndpoint);
        source.ConnectPeer(targetEndpoint, targetRid);

        var handlerEntered = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var secondHandlerCompleted = Signal();
        var handlerCount = 0;
        using var releaseHandler = new ManualResetEventSlim();
        await using var pump = new ZLinkMeshDispatchPump(target, new ZLinkMeshCompletionTable());
        pump.SetNodeRouteHandler(
            (records, _) =>
            {
                foreach (var record in records)
                {
                    using (record)
                    {
                        Assert.Equal(sourceRid, record.SourceNodeRid);
                        Assert.Equal(1, record.PartCount);
                        Assert.True(record.ApplicationPayloadView.HasValue);
                        var marker = record.ApplicationPayloadView.Value.GetSpan(0)[0];
                        Assert.Equal(Interlocked.Increment(ref handlerCount), marker);
                        if (marker == 1)
                        {
                            handlerEntered.TrySetResult(Environment.CurrentManagedThreadId);
                            Assert.True(releaseHandler.Wait(CompletionBound));
                        }
                        else
                        {
                            Assert.Equal(2, marker);
                            secondHandlerCompleted.TrySetResult();
                        }
                    }
                }
                return ValueTask.CompletedTask;
            }
        );

        target.Start();
        source.Start();
        pump.EnsureStarted();
        var router = Assert.IsType<RecordingRouter>(targetContext.Router);
        var secondSubmitted = false;
        using var releaseApplicationResources = new ManualResetEventSlim();
        using var applicationResourcesEntered = new CountdownEvent(
            ZLinkProcessExecutionPool.WorkerCount - 1
        );
        var applicationSources = new List<ZLinkProcessExecutionPool.ApplicationSource>();
        var resourceFailures = new System.Collections.Concurrent.ConcurrentQueue<Exception>();
        try
        {
            await Task.WhenAll(sourceAdmitted.Task, targetAdmitted.Task).WaitAsync(CompletionBound);
            if (exhaustApplicationResources)
            {
                for (var index = 0; index < ZLinkProcessExecutionPool.WorkerCount - 1; index++)
                {
                    var application = ZLinkProcessExecutionPool.CreateApplicationSource(
                        _ =>
                        {
                            applicationResourcesEntered.Signal();
                            Assert.True(releaseApplicationResources.Wait(CompletionBound));
                            return ValueTask.CompletedTask;
                        },
                        resourceFailures.Enqueue
                    );
                    applicationSources.Add(application);
                    application.Signal();
                }
                Assert.True(applicationResourcesEntered.Wait(CompletionBound));
            }
            using var first = Message.From(new byte[] { 1 });
            Assert.Equal(SubmitResult.Ok, source.SendToNode(targetRid, [first]));
            var handlerThread = exhaustApplicationResources
                ? 0
                : await handlerEntered.Task.WaitAsync(CompletionBound);
            var receiveThread = await router.FirstReceived.Task.WaitAsync(CompletionBound);

            // The next physical receive must finish while the first handler is
            // still held. No elapsed timeout or posted-work count proves progress.
            using var second = Message.From(new byte[] { 2 });
            Assert.Equal(SubmitResult.Ok, source.SendToNode(targetRid, [second]));
            secondSubmitted = true;
            await router.SecondReceived.Task.WaitAsync(CompletionBound);
            Assert.False(releaseHandler.IsSet);
            Assert.False(secondHandlerCompleted.Task.IsCompleted);
            Assert.Equal(1, router.MaximumConcurrentReceives);
            Assert.Equal(2, router.NodeSendCount);
            Assert.Equal(1, processPoller!.MaximumConcurrentWaits);
            if (exhaustApplicationResources)
            {
                Assert.False(handlerEntered.Task.IsCompleted);
                Assert.False(releaseApplicationResources.IsSet);
                Assert.True(
                    source.TryBeginOperation(
                        MeshOperationKind.NodeRequest,
                        TimeSpan.FromMilliseconds(10),
                        out var operation
                    )
                );
                var expired = Signal();
                using var cancellation = operation.Token.Register(() => expired.TrySetResult());
                await expired.Task.WaitAsync(CompletionBound);
                Assert.False(handlerEntered.Task.IsCompleted);
            }
            else
            {
                Assert.NotEqual(receiveThread, handlerThread);
                Assert.Equal(receiveThread, router.SecondReceiveThread);
            }
            var deadline = source.RunDeadlineRequestAsync<int>(
                TimeSpan.FromMilliseconds(10),
                default,
                completion =>
                {
                    throw new ZlinkRequestException(
                        (ZlinkRequestException.ErrorCode)(int)completion.Result
                    );
                },
                static (_, _) => Task.CompletedTask
            );
            var timedOut = await Assert.ThrowsAsync<ZlinkRequestException>(() =>
                deadline.WaitAsync(CompletionBound)
            );
            Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, timedOut.Result);
            Assert.False(releaseHandler.IsSet);
        }
        finally
        {
            releaseApplicationResources.Set();
            releaseHandler.Set();
            foreach (var application in applicationSources)
                await application.DisposeAsync();
            Assert.Empty(resourceFailures);
            if (secondSubmitted)
            {
                await secondHandlerCompleted.Task.WaitAsync(CompletionBound);
                Assert.Equal(2, Volatile.Read(ref handlerCount));
            }
            await pump.DisposeAsync();
            await target.DisposeAsync();
            await source.DisposeAsync();
            Assert.Equal(0, processPoller!.MutationOverlapsWait);
            Assert.Equal(1, processPoller.MaximumConcurrentWaits);
        }
    }

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private sealed class RecordingRouter(IRouterSocket inner, bool channelRequests = false)
        : PressureSocket<IRouterSocket>(inner),
            IRouterSocket
    {
        private int _receiving;
        private int _maximumConcurrentReceives;
        private int _nodeSends;
        internal IRouterSocket Inner => InnerSocket;
        internal int MaximumConcurrentReceives => Volatile.Read(ref _maximumConcurrentReceives);
        internal int NodeSendCount => Volatile.Read(ref _nodeSends);
        internal int SecondReceiveThread;
        internal TaskCompletionSource<int> FirstReceived { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource SecondReceived { get; } = Signal();
        public new RouterSocketOptions Options => InnerSocket.Options;

        public void SetRoutingId(RoutingId rid) => InnerSocket.SetRoutingId(rid);

        public RoutingId GetRoutingId() => InnerSocket.GetRoutingId();

        public SendOperation Send(RoutingId rid) => InnerSocket.Send(rid);

        public RequestOperation Request(RoutingId rid) => InnerSocket.Request(rid);

        public ReplyOperation Reply(RoutingId rid, ReplyToken token) =>
            InnerSocket.Reply(rid, token);

        public IReadOnlyList<RouterRoute> RoutesSnapshot() => InnerSocket.RoutesSnapshot();

        public new bool Recv(Received result, RecvFlags flags = RecvFlags.None)
        {
            var concurrent = Interlocked.Increment(ref _receiving);
            int observed;
            do
            {
                observed = Volatile.Read(ref _maximumConcurrentReceives);
                if (observed >= concurrent)
                    break;
            } while (
                Interlocked.CompareExchange(ref _maximumConcurrentReceives, concurrent, observed)
                != observed
            );
            try
            {
                if (!InnerSocket.Recv(result, flags))
                    return false;
                var header = result.Parts[0].AsReadOnlySpan();
                if (
                    ZLinkServiceWireCodec.TryDecodePrefix(header, out var command, out _, out _)
                    && command
                        == (
                            channelRequests
                                ? ServiceWireConstants.Command.ChannelRequest
                                : ServiceWireConstants.Command.NodeSend
                        )
                )
                {
                    // The service wire packs payload into this frame. Match its
                    // NodeSend ordinal; the handler checks the decoded marker.
                    var ordinal = Interlocked.Increment(ref _nodeSends);
                    if (ordinal == 1)
                        FirstReceived.TrySetResult(Environment.CurrentManagedThreadId);
                    else if (ordinal == 2)
                    {
                        SecondReceiveThread = Environment.CurrentManagedThreadId;
                        SecondReceived.TrySetResult();
                    }
                }
                return true;
            }
            finally
            {
                Interlocked.Decrement(ref _receiving);
            }
        }
    }

    private sealed class RecordingContext(IContext inner, bool channelRequests = false) : IContext
    {
        internal RecordingRouter? Router { get; private set; }
        public IContextOptions Options => inner.Options;

        public IRouterSocket CreateRouterSocket() =>
            Router = new(inner.CreateRouterSocket(), channelRequests);

        public IPairSocket CreatePairSocket() => inner.CreatePairSocket();

        public IDealerSocket CreateDealerSocket() => inner.CreateDealerSocket();

        public IPubSocket CreatePubSocket() => inner.CreatePubSocket();

        public ISubSocket CreateSubSocket() => inner.CreateSubSocket();

        public IXPubSocket CreateXPubSocket() => inner.CreateXPubSocket();

        public IXSubSocket CreateXSubSocket() => inner.CreateXSubSocket();

        public IStreamSocket CreateStreamSocket() => inner.CreateStreamSocket();

        public void Shutdown() => inner.Shutdown();

        public void RecalculateAutoHwm() => inner.RecalculateAutoHwm();

        public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot() => inner.GetCoreHwmBudgetSnapshot();

        public void ResetCoreHwmBudgetMetrics() => inner.ResetCoreHwmBudgetMetrics();

        public void Dispose() => inner.Dispose();

        public ValueTask DisposeAsync() => inner.DisposeAsync();
    }

    private sealed class RecordingPoller(Action beforeWait) : IPoller
    {
        private readonly IPoller _inner = Systems.Zlink.Zlink.CreatePoller();
        private int _waiting;
        private int _maximumConcurrentWaits;
        internal int MaximumConcurrentWaits => Volatile.Read(ref _maximumConcurrentWaits);
        private int _mutationOverlapsWait;
        internal int MutationOverlapsWait => Volatile.Read(ref _mutationOverlapsWait);

        private void BeforeMutation()
        {
            if (Volatile.Read(ref _waiting) != 0)
                Interlocked.Increment(ref _mutationOverlapsWait);
        }

        private static IZlinkSocket Unwrap(IZlinkSocket socket) =>
            socket is RecordingRouter router ? router.Inner : socket;

        public int Size => _inner.Size;

        public void Add(IZlinkSocket socket, PollEventFlags events, nuint slot)
        {
            BeforeMutation();
            _inner.Add(Unwrap(socket), events, slot);
        }

        public void Add(ISocketMonitor monitor, PollEventFlags events, nuint slot)
        {
            BeforeMutation();
            _inner.Add(monitor, events, slot);
        }

        public void Add(IZlinkTimer timer, nuint slot)
        {
            BeforeMutation();
            _inner.Add(timer, slot);
        }

        public void AddFd(int fd, PollEventFlags events, nuint slot) =>
            _inner.AddFd(fd, events, slot);

        public void Modify(IZlinkSocket socket, PollEventFlags events)
        {
            BeforeMutation();
            _inner.Modify(Unwrap(socket), events);
        }

        public void Modify(ISocketMonitor monitor, PollEventFlags events)
        {
            BeforeMutation();
            _inner.Modify(monitor, events);
        }

        public void ModifyFd(int fd, PollEventFlags events) => _inner.ModifyFd(fd, events);

        public bool Remove(IZlinkSocket socket)
        {
            BeforeMutation();
            return _inner.Remove(Unwrap(socket));
        }

        public bool Remove(ISocketMonitor monitor)
        {
            BeforeMutation();
            return _inner.Remove(monitor);
        }

        public bool Remove(IZlinkTimer timer)
        {
            BeforeMutation();
            return _inner.Remove(timer);
        }

        public bool Remove(int fd) => _inner.Remove(fd);

        public void Clear()
        {
            BeforeMutation();
            _inner.Clear();
        }

        public void Close() => _inner.Close();

        public int Wait(Span<PollEvent> destination, TimeSpan timeout)
        {
            var waiting = Interlocked.Increment(ref _waiting);
            var previous = Volatile.Read(ref _maximumConcurrentWaits);
            while (waiting > previous)
            {
                var observed = Interlocked.CompareExchange(
                    ref _maximumConcurrentWaits,
                    waiting,
                    previous
                );
                if (observed == previous)
                    break;
                previous = observed;
            }
            try
            {
                beforeWait();
                return _inner.Wait(destination, timeout);
            }
            finally
            {
                Interlocked.Decrement(ref _waiting);
            }
        }

        public void Dispose()
        {
            BeforeMutation();
            _inner.Dispose();
        }

        public ValueTask DisposeAsync() => _inner.DisposeAsync();
    }
}
