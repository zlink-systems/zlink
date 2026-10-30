using System.Collections;
using System.Collections.Concurrent;
using System.Reflection;
using System.Runtime.ExceptionServices;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class RelocationBehaviorConformanceTests
{
    [Theory]
    [InlineData(false, true)]
    [InlineData(true, true)]
    [InlineData(false, false)]
    [InlineData(true, false)]
    public async Task Relocation_boundary_transport_loss_defers_to_settlement_but_other_failures_escape(
        bool failData,
        bool transient
    )
    {
        Exception failure = transient
            ? new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected)
            : new InvalidOperationException("Non-transport boundary failure.");
        var transport = new BoundaryFailureTransport(failData, failure);
        var coordinator = new ZLinkServiceWireCodec.RelocationCoordinatorFence(
            "owner",
            1,
            RoutingId.From("source"),
            1,
            "version"
        );
        var actor = new ZLinkServiceWireCodec.RelocationObjectRecord(1, "actor", "actor", 1, 1);
        var data = new ZLinkServiceWireCodec.RelocationDataRecord(
            new(1, 2),
            1,
            coordinator,
            1,
            actor,
            new(new byte[] { 1 })
        );
        var cutover = new ZLinkServiceWireCodec.RelocationCutoverRecord(
            new(1, 2),
            1,
            coordinator,
            1,
            actor,
            1,
            0
        );
        var cutoverTerminals = 0;
        async Task Submit() =>
            await ZLinkStandaloneActorRelocationRuntime.SubmitBoundaryAsync(
                transport,
                RoutingId.From("target"),
                [data],
                cutover,
                CancellationToken.None,
                () => cutoverTerminals++
            );
        if (transient)
            await Submit();
        else
            Assert.Same(failure, await Assert.ThrowsAsync<InvalidOperationException>(Submit));
        Assert.Equal(failData ? 1 : 2, transport.Submissions);
        Assert.Equal(failData ? 0 : 1, cutoverTerminals);
    }

    private sealed class BoundaryFailureTransport(bool failData, Exception failure)
        : IZLinkBackendCanonicalRelocation
    {
        internal int Submissions;

        public void SetCanonicalRelocationTarget(ICanonicalRelocationTarget target) =>
            throw new NotSupportedException();

        public ValueTask<ZLinkServiceWireCodec.RelocationReadyRecord> PrepareCanonicalRelocationAsync(
            RoutingId target,
            ZLinkServiceWireCodec.RelocationPrepareRecord prepare,
            ZLinkRelocationTransferPayload payload,
            TimeSpan timeout,
            CancellationToken cancellationToken
        ) => throw new NotSupportedException();

        public ValueTask SendCanonicalRelocationDataAsync(
            RoutingId target,
            ZLinkServiceWireCodec.RelocationDataRecord data,
            CancellationToken cancellationToken
        )
        {
            Submissions++;
            return failData ? ValueTask.FromException(failure) : ValueTask.CompletedTask;
        }

        public ValueTask SendCanonicalRelocationCutoverAsync(
            RoutingId target,
            ZLinkServiceWireCodec.RelocationCutoverRecord cutover,
            CancellationToken cancellationToken
        )
        {
            Submissions++;
            return ValueTask.FromException(failure);
        }
    }

    [Fact]
    public Task ActorJoin_Entry_preserves_source_after_replaced_prepare_peer() =>
        VerifyPrepareFailureAsync(userSpot: false, replayable: true);

    [Fact]
    public Task ActorJoin_UserSpot_preserves_source_after_replaced_prepare_peer() =>
        VerifyPrepareFailureAsync(userSpot: true, replayable: true);

    [Fact]
    public Task ActorJoin_Entry_restores_FIFO_before_failure_completion() =>
        VerifyPrepareFailureAsync(userSpot: false, replayable: false);

    [Fact]
    public Task ActorJoin_UserSpot_restores_FIFO_before_failure_completion() =>
        VerifyPrepareFailureAsync(userSpot: true, replayable: false);

    [Fact]
    public Task Canonical_prepare_native_timeout_preserves_typed_deadline_failure() =>
        VerifyPrepareFailureAsync(
            userSpot: false,
            replayable: true,
            PrepareRollbackFault.NativeTimeout
        );

    [Theory]
    [InlineData(PrepareRollbackFault.StoreFailure)]
    [InlineData(PrepareRollbackFault.ForeignAuthority)]
    [InlineData(PrepareRollbackFault.LeaseExpiredBeforeFifo)]
    public Task ActorJoin_keeps_source_closed_when_prepare_rollback_is_unconfirmed(
        PrepareRollbackFault fault
    ) =>
        VerifyPrepareFailureAsync(
            userSpot: fault == PrepareRollbackFault.LeaseExpiredBeforeFifo,
            replayable: true,
            fault
        );

    public enum PrepareRollbackFault
    {
        None,
        StoreFailure,
        ForeignAuthority,
        LeaseExpiredBeforeFifo,
        NativeTimeout,
    }

    private static async Task VerifyPrepareFailureAsync(
        bool userSpot,
        bool replayable,
        PrepareRollbackFault fault = PrepareRollbackFault.None
    )
    {
        var path = Environment.GetEnvironmentVariable(
            RelocationBehaviorHost.FlowPathEnvironmentVariable
        );
        if (path is not null)
            path +=
                $".{(userSpot ? "user" : "entry")}-{(replayable ? "replay" : "restore")}-{fault}";
        using var listener = path is null ? null : new TestHostMessageFlowListener(path);
        var trace = new RelocationBehaviorTrace
        {
            ExpectedJoinAccepted = false,
            ExpectedDeliveryNode = "source",
            ConfigureTarget = context =>
                context.Handlers.AddActorPacket<BeginUserSpotBehaviorJoinHandler, BehaviorActor>(
                    nameof(BeginBehaviorJoin)
                ),
        };
        trace.ReleaseTargetLifecycle.SetResult();
        trace.ReleaseSourceLeave.SetResult();
        var transport = new CanonicalRelocationTransportProbe(
            trace,
            holdTargetReady: fault == PrepareRollbackFault.NativeTimeout
        );
        transport.ReleaseCutoverSend.SetResult();
        var store = new RecordingLocationStore(new ZLinkInMemoryProviderLocationStore(), trace);
        var sourceTime =
            fault == PrepareRollbackFault.LeaseExpiredBeforeFifo ? new ManualTimeProvider() : null;
        sourceTime?.AdvanceWallClockOnly(DateTimeOffset.UtcNow - sourceTime.GetUtcNow());
        var relocationStore = new SynchronizedRelocationStore();
        var actorId = $"restore-actor-{Guid.NewGuid():N}";
        var targetSpotId = $"restore-target-{Guid.NewGuid():N}";
        trace.ActorId = actorId;
        await using var source = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            store,
            relocationStore,
            registerTargetSpot: userSpot,
            canonicalTransportProbe: transport,
            timeProvider: sourceTime,
            locationTimeProvider: sourceTime,
            configureServices: services =>
                services.AddTransient<BeginUserSpotBehaviorJoinHandler>(),
            spotType: "restore-source-spot"
        );
        var created = Assert.IsType<ZLinkActorCreateResult.Created>(
            await source
                .Services.GetRequiredService<IZLinkActorManager>()
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(7))
                .Async()
        );
        trace.SourceNodeRid = created.Actor.NodeRid;
        trace.SourceObjectGeneration = created.Actor.ObjectGeneration;
        await store.ObserveActorAuthorityAsync(actorId);
        Assert.True(source.Runtime.TryGetActorState(actorId, out var sourceState));
        if (userSpot)
        {
            var localSpot = await source
                .Services.GetRequiredService<IZLinkSpotManager>()
                .GetOrCreate($"restore-source-{Guid.NewGuid():N}", "restore-source-spot")
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(ZLinkMessage.Empty)
                .Async();
            Assert.IsType<ZLinkActorJoinResult.Accepted>(
                await source.Runtime.JoinActorAsync(
                    localSpot.Spot.SpotId,
                    sourceState.Actor!,
                    ZLinkMessage.Empty,
                    CancellationToken.None
                )
            );
        }
        await using var target = await RelocationBehaviorHost.StartAsync(
            "target",
            trace,
            store,
            relocationStore,
            registerTargetSpot: true,
            canonicalTransportProbe: transport
        );
        trace.TargetNodeRid = target.LocalNodeRid;
        var spot = await source
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(targetSpotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Request(ZLinkMessage.Empty)
            .Async();
        Assert.Equal(target.LocalNodeRid, spot.Spot.NodeRid);
        var native = (
            (ProbedBackendSpotNode)
                source.Runtime.GetMeshNodeRuntime(RelocationBehaviorHost.MeshName).Node
        ).NativeNode;
        source.Runtime.Registration.DispatchOptions.Diagnostics.SetLevel(
            ZLinkDiagnosticsLevel.Normal
        );
        var prepares = new ConcurrentQueue<ZLinkServiceWireCodec.RelocationPrepareRecord>();
        var socketField = typeof(ZLinkManagedMeshNode).GetField("_socket", PrivateInstance)!;
        var socket = (IRouterSocket)socketField.GetValue(native)!;
        var nativePrepare = new TaskCompletionSource<RequestSubmission>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var injected = 0;
        socketField.SetValue(
            native,
            PrepareRequestRecorder.Create(
                socket,
                prepares,
                fault == PrepareRollbackFault.NativeTimeout ? TimeSpan.FromMilliseconds(250) : null,
                submission =>
                {
                    nativePrepare.TrySetResult(submission);
                    if (!replayable || Interlocked.Exchange(ref injected, 1) != 0)
                        return;
                    if (fault == PrepareRollbackFault.NativeTimeout)
                    {
                        var nativeId = prepares.Single().RelocationId;
                        store.HeldSourceAbort = Guid.ParseExact(
                            $"{nativeId.High:x16}{nativeId.Low:x16}",
                            "N"
                        );
                        trace.Record("nativePrepareSubmitted");
                        return;
                    }
                    // Submit the peer replacement before the caller can send its first chunk.
                    var lane = (ZLinkStateLane)
                        typeof(ZLinkManagedMeshNode)
                            .GetField("_lane", PrivateInstance)!
                            .GetValue(native)!;
                    var peers = (IDictionary)
                        typeof(ZLinkManagedMeshNode)
                            .GetField("_peersByRid", PrivateInstance)!
                            .GetValue(native)!;
                    AwaitStateLane(
                        lane.RunAsync(() =>
                        {
                            var peer = peers[target.LocalNodeRid]!;
                            peers[target.LocalNodeRid] = typeof(object)
                                .GetMethod("MemberwiseClone", PrivateInstance)!
                                .Invoke(peer, null);
                        })
                    );
                    trace.Record("preparePeerReplaced");
                    var id = prepares.Single().RelocationId;
                    store.HeldSourceAbort = Guid.ParseExact($"{id.High:x16}{id.Low:x16}", "N");
                }
            )
        );
        var completionRelease = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var sourceFifoAdmission = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        trace.OnPublicJoinCompleted = async _ =>
        {
            Assert.Empty(trace.DeliveredMarkers);
            if (fault == PrepareRollbackFault.None)
            {
                Assert.False(sourceState.Handoff.IsSourceMigrationInProgress);
                var mailbox = (ZLinkActorSerialExecutor)
                    typeof(ZLinkActorRuntimeState)
                        .GetField("_dispatchMailbox", PrivateInstance)!
                        .GetValue(sourceState)!;
                var queue = (ZLinkSerialExecutionQueue)
                    typeof(ZLinkActorSerialExecutor)
                        .GetField("_queue", PrivateInstance)!
                        .GetValue(mailbox)!;
                sourceFifoAdmission.SetResult(queue.ApplicationPendingCount);
            }
            trace.Record("completionCallbackStarted");
            await completionRelease.Task;
            trace.Record("completionCallbackFinished");
        };
        var client = source.Services.GetRequiredService<IZLinkActorClient>();
        try
        {
            var join = client
                .RequestToActor(actorId, new BeginBehaviorJoin(targetSpotId))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<BehaviorAck>()
                .AsTask();
            await trace.WaitAsync("relocationRequested");
            // The handler records its intent before Defer seals the ingress.
            // Establish the capture precondition before submitting saved work.
            await WaitUntilAsync(() => !sourceState.Handoff.DurablePrefixCanRun);
            var saved = client
                .RequestToActor(actorId, new BehaviorWork("saved"))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<BehaviorAck>()
                .AsTask();
            await WaitForCapturedFrameAsync(sourceState.Handoff, "_frames");
            trace.ReleaseJoinHandler.SetResult();
            await join.WaitAsync(TimeSpan.FromSeconds(5));
            await transport.PrepareCallStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var prefix = client
                .RequestToActor(actorId, new BehaviorWork("prefix"))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<BehaviorAck>()
                .AsTask();
            await WaitForCapturedFrameAsync(sourceState.Handoff, "_sourceHoldFrames");
            if (replayable)
                transport.ReleasePrepareCall.SetResult();
            else
                transport.ReleasePrepareCall.SetException(
                    new InvalidOperationException("Injected prepare failure.")
                );
            if (fault == PrepareRollbackFault.NativeTimeout)
            {
                await transport.TargetPreparedBeforeReadySend.Task.WaitAsync(
                    TimeSpan.FromSeconds(5)
                );
                var submission = await nativePrepare.Task.WaitAsync(TimeSpan.FromSeconds(5));
                await submission.Admitted.WaitAsync(TimeSpan.FromSeconds(5));
                var terminal = await Assert.ThrowsAsync<ZlinkRequestException>(async () =>
                    await submission.Reply.WaitAsync(TimeSpan.FromSeconds(5))
                );
                Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, terminal.Result);
                var mapped = Assert.IsType<ZLinkFrameworkException>(
                    await transport.PrepareFailure.Task.WaitAsync(TimeSpan.FromSeconds(5))
                );
                if (path is not null)
                    File.WriteAllText(
                        path + ".native-terminal",
                        $"Admitted={submission.Admitted.IsCompletedSuccessfully}\nNative={terminal.Result}\nPublic={mapped.Kind}\n{mapped}"
                    );
                Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, mapped.Kind);
                Assert.Same(terminal, mapped.InnerException);
                store.ReleaseSourceAbortWrite.TrySetResult();
                await trace.WaitAsync("completionCallbackStarted");
                completionRelease.TrySetResult();
                await source
                    .Runtime.WaitForAcceptedActorHandoffsAsync(CancellationToken.None)
                    .WaitAsync(TimeSpan.FromSeconds(5));
                Assert.Single(prepares);
                return;
            }
            if (replayable)
            {
                var completed = await Task.WhenAny(
                    store.SourceAbortWriteStarted.Task,
                    trace.WaitAsync("completionCallbackStarted")
                );
                Assert.Same(store.SourceAbortWriteStarted.Task, completed);
                Assert.True(sourceState.Handoff.IsSourceMigrationInProgress);
                Assert.Empty(trace.DeliveredMarkers);
                Assert.Empty(trace.JoinCompletions);
                Assert.False(saved.IsCompleted);
                Assert.False(prefix.IsCompleted);
                Assert.Equal(0, trace.TargetAuthorityMutationCount);
                if (fault == PrepareRollbackFault.StoreFailure)
                    store.SourceAbortWriteFailure = new IOException(
                        "Injected source Preserve write failure."
                    );
                if (fault == PrepareRollbackFault.ForeignAuthority)
                {
                    var captured = Assert.IsType<ZLinkAuthoritySnapshot>(store.CapturedSourceAbort);
                    Assert.True(
                        ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                            captured.Payload.Span,
                            out var projection
                        )
                    );
                    Assert.True(
                        ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
                            projection.SteadyAuthorityPayload.Span,
                            out var steady
                        )
                    );
                    Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                        await new ZLinkProviderLocationRepository(
                            store
                        ).CompareExchangeAuthorityAsync(
                            ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId),
                            captured.StoreVersion,
                            new ZLinkAuthorityMutation.Put(
                                ZLinkActorAuthorityPayloadCodec.Encode(
                                    steady with
                                    {
                                        NodeRid = RoutingId.From("foreign-source"),
                                    }
                                ),
                                ZLinkAuthorityGenerationTransition.Preserve,
                                null,
                                null
                            )
                        )
                    );
                }
                if (fault == PrepareRollbackFault.LeaseExpiredBeforeFifo)
                {
                    var owner = source.Runtime.LocationLifecycle!.OwnerToken;
                    Assert.True(source.Runtime.LocationLifecycle.IsOwnerLeaseValid(owner));
                    sourceTime!.AdvanceMonotonicOnly(
                        source.Runtime.Registration.Locations.Options.OwnerLeaseTtl
                    );
                    Assert.False(source.Runtime.LocationLifecycle.IsOwnerLeaseValid(owner));
                }
                store.ReleaseSourceAbortWrite.SetResult();
            }
            await trace.WaitAsync("completionCallbackStarted");
            if (fault != PrepareRollbackFault.None)
            {
                Assert.Empty(trace.DeliveredMarkers);
                Assert.Single(prepares);
                Assert.Equal(1, store.SourceAbortWriteCount);
                Assert.IsType<ZLinkActorJoinCompletion.Failed>(
                    Assert.Single(trace.JoinCompletions).Completion
                );
                Assert.False(transport.ReadyReplyReceived.Task.IsCompleted);
                Assert.False(transport.CutoverSendStarted.Task.IsCompleted);
                Assert.Equal(0, trace.TargetAuthorityMutationCount);
                completionRelease.SetResult();
                await source
                    .Runtime.WaitForAcceptedActorHandoffsAsync(CancellationToken.None)
                    .WaitAsync(TimeSpan.FromSeconds(5));
                Assert.Empty(trace.DeliveredMarkers);
                if (fault != PrepareRollbackFault.LeaseExpiredBeforeFifo)
                {
                    Assert.True(sourceState.Handoff.IsSourceMigrationInProgress);
                    Assert.Null(store.ConfirmedSourceAbort);
                    Assert.False(saved.IsCompleted);
                    Assert.False(prefix.IsCompleted);
                }
                else
                {
                    Assert.True(sourceState.Handoff.RetainsSourceTombstone);
                    Assert.Equal(
                        ZLinkFrameworkErrorKind.Unavailable,
                        (
                            await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                                await saved.WaitAsync(TimeSpan.FromSeconds(5))
                            )
                        ).Kind
                    );
                    Assert.Equal(
                        ZLinkFrameworkErrorKind.Unavailable,
                        (
                            await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                                await prefix.WaitAsync(TimeSpan.FromSeconds(5))
                            )
                        ).Kind
                    );
                }
                return;
            }
            Assert.Equal(2, await sourceFifoAdmission.Task.WaitAsync(TimeSpan.FromSeconds(5)));
            if (replayable)
            {
                var confirmed = Assert.IsType<ZLinkAuthoritySnapshot>(store.ConfirmedSourceAbort);
                var captured = Assert.IsType<ZLinkAuthoritySnapshot>(store.CapturedSourceAbort);
                Assert.NotEqual(captured.StoreVersion, confirmed.StoreVersion);
                Assert.Equal(captured.ObjectGeneration, confirmed.ObjectGeneration);
                Assert.Equal(captured.AuthorityOwnerGeneration, confirmed.AuthorityOwnerGeneration);
                Assert.Equal(captured.OwnerId, confirmed.OwnerId);
                Assert.Equal(captured.OwnerLeaseGeneration, confirmed.OwnerLeaseGeneration);
                Assert.True(
                    ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                        captured.Payload.Span,
                        out var capturedProjection
                    )
                );
                Assert.Equal(
                    capturedProjection.SteadyAuthorityPayload.ToArray(),
                    confirmed.Payload.ToArray()
                );
                Assert.False(
                    ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                        confirmed.Payload.Span,
                        out _
                    )
                );
                Assert.True(
                    trace.Events.ToList().IndexOf("sourcePreserveConfirmed")
                        < trace.Events.ToList().IndexOf("completionCallbackStarted")
                );
                Assert.False(transport.ReadyReplyReceived.Task.IsCompleted);
                Assert.False(transport.CutoverSendStarted.Task.IsCompleted);
                Assert.Equal(0, trace.TargetAuthorityMutationCount);
            }
            Assert.False(saved.IsCompleted);
            Assert.False(prefix.IsCompleted);
            completionRelease.SetResult();
            Assert.Equal("saved", (await saved.WaitAsync(TimeSpan.FromSeconds(5))).Marker);
            Assert.Equal("prefix", (await prefix.WaitAsync(TimeSpan.FromSeconds(5))).Marker);
            Assert.Equal(new[] { "saved", "prefix" }, trace.DeliveredMarkers);
            var events = trace.Events.ToList();
            Assert.True(
                events.IndexOf("completionCallbackFinished") < events.IndexOf("savedWorkAdmitted")
            );
            Assert.Single(trace.JoinCompletions);
            if (replayable)
            {
                Assert.Equal(1, injected);
                Assert.Single(prepares);
                Assert.Single(prepares.Select(record => record.RelocationId).Distinct());
                Assert.Single(prepares.Select(record => record.DeadlineUnixMs).Distinct());
                Assert.Single(prepares.Select(record => record.TargetAttemptGeneration).Distinct());
                Assert.Equal(
                    ZLinkFrameworkErrorKind.Unavailable,
                    Assert
                        .IsType<ZLinkActorJoinCompletion.Failed>(
                            trace.JoinCompletions.Single().Completion
                        )
                        .Kind
                );
            }
            else
            {
                Assert.Empty(prepares);
                Assert.Equal(
                    ZLinkFrameworkErrorKind.InternalFailure,
                    Assert
                        .IsType<ZLinkActorJoinCompletion.Failed>(
                            trace.JoinCompletions.Single().Completion
                        )
                        .Kind
                );
            }
        }
        catch (Exception error)
        {
            if (path is not null)
                File.WriteAllText(path + ".failure", error.ToString());
            throw;
        }
        finally
        {
            socketField.SetValue(native, socket);
            completionRelease.TrySetResult();
            trace.ReleaseJoinHandler.TrySetResult();
            transport.ReleasePrepareCall.TrySetResult();
            transport.ReleaseTargetReadySend.TrySetResult();
            store.ReleaseSourceAbortWrite.TrySetResult();
        }
    }

    private static Task WaitForCapturedFrameAsync(ZLinkActorHandoffState handoff, string field)
    {
        var lane = (ZLinkStateLane)
            typeof(ZLinkActorHandoffState).GetField("_lane", PrivateInstance)!.GetValue(handoff)!;
        var frames = (ICollection)
            typeof(ZLinkActorHandoffState).GetField(field, PrivateInstance)!.GetValue(handoff)!;
        return WaitUntilAsync(() =>
            ValueTask.FromResult(AwaitStateLane(lane.RunAsync(() => frames.Count != 0)))
        );
    }

    private const BindingFlags PrivateInstance = BindingFlags.Instance | BindingFlags.NonPublic;
}

internal sealed class BeginUserSpotBehaviorJoinHandler(RelocationBehaviorTrace trace)
    : IZLinkSpotActorRequestHandler<
        BehaviorTargetSpot,
        BehaviorActor,
        BeginBehaviorJoin,
        BehaviorAck
    >
{
    public async ValueTask<BehaviorAck> HandleAsync(
        BehaviorTargetSpot spot,
        BehaviorActor actor,
        IZLinkMessageContext context,
        BeginBehaviorJoin request,
        CancellationToken cancellationToken
    )
    {
        trace.Record("relocationRequested");
        actor
            .Context.JoinSpot(request.TargetSpotId, request)
            .Timeout(TimeSpan.FromSeconds(15))
            .Defer();
        await trace.ReleaseJoinHandler.Task.WaitAsync(cancellationToken);
        return new BehaviorAck("join");
    }
}

internal class PrepareRequestRecorder : DispatchProxy
{
    private IRouterSocket _socket = null!;
    private ConcurrentQueue<ZLinkServiceWireCodec.RelocationPrepareRecord> _prepares = null!;
    private TimeSpan? _nativePrepareTimeout;
    private Action<RequestSubmission>? _observePrepareSubmission;

    internal static IRouterSocket Create(
        IRouterSocket socket,
        ConcurrentQueue<ZLinkServiceWireCodec.RelocationPrepareRecord> prepares,
        TimeSpan? nativePrepareTimeout = null,
        Action<RequestSubmission>? observePrepareSubmission = null
    )
    {
        var proxy = Create<IRouterSocket, PrepareRequestRecorder>();
        var recorder = (PrepareRequestRecorder)(object)proxy;
        recorder._socket = socket;
        recorder._prepares = prepares;
        recorder._nativePrepareTimeout = nativePrepareTimeout;
        recorder._observePrepareSubmission = observePrepareSubmission;
        return proxy;
    }

    protected override object? Invoke(MethodInfo? method, object?[]? arguments)
    {
        ArgumentNullException.ThrowIfNull(method);
        if (method.Name == nameof(IRouterSocket.Request))
            return new RecordedRequest(
                _socket.Request((RoutingId)arguments![0]!),
                _prepares,
                _nativePrepareTimeout,
                _observePrepareSubmission
            );
        try
        {
            return method.Invoke(_socket, arguments);
        }
        catch (TargetInvocationException error) when (error.InnerException is { } cause)
        {
            ExceptionDispatchInfo.Capture(cause).Throw();
            throw;
        }
    }

    private sealed class RecordedRequest(
        RequestOperation request,
        ConcurrentQueue<ZLinkServiceWireCodec.RelocationPrepareRecord> prepares,
        TimeSpan? nativePrepareTimeout,
        Action<RequestSubmission>? observePrepareSubmission
    ) : RequestOperation, RequestSubmitOperation
    {
        private RequestSubmitOperation _submission = null!;
        private bool _prepare;

        public RequestSubmitOperation Message(Message message)
        {
            if (
                ZLinkServiceWireCodec.TryDecodeRelocationPrepare(
                    message.AsReadOnlyMemory().Span,
                    out var prepare,
                    out _
                )
            )
            {
                _prepare = true;
                prepares.Enqueue(prepare!);
            }
            _submission = _submission is null
                ? request.Message(message)
                : _submission.Message(message);
            return this;
        }

        public RequestSubmitOperation Timeout(TimeSpan timeout)
        {
            _submission = _submission.Timeout(_prepare ? nativePrepareTimeout ?? timeout : timeout);
            return this;
        }

        public IReadOnlyList<Message> Submit() => _submission.Submit();

        public RequestSubmission Async(CancellationToken cancellationToken = default)
        {
            var submission = _submission.Async(cancellationToken);
            if (_prepare)
                observePrepareSubmission?.Invoke(submission);
            return submission;
        }
    }
}
