using System.Buffers.Binary;
using System.Security.Cryptography;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkDeferredActorJoinHandlerScope : IDisposable
{
    private static readonly AsyncLocal<ZLinkDeferredActorJoinHandlerScope?> CurrentScope = new();
    private readonly List<ZLinkDeferredActorJoin> _joins = [];
    private readonly bool _allowed;
    private readonly ZLinkDeferredActorJoinHandlerScope? _previous;
    private readonly ZLinkStateLane _lane = new();
    private bool _completed;
    private bool _sealed;

    private ZLinkDeferredActorJoinHandlerScope(bool allowed)
    {
        _allowed = allowed;
        _previous = CurrentScope.Value;
        CurrentScope.Value = this;
    }

    public static ZLinkDeferredActorJoinHandlerScope Open(bool allowed = true)
    {
        return new ZLinkDeferredActorJoinHandlerScope(allowed);
    }

    public static void Register(ZLinkDeferredActorJoin join)
    {
        var scope =
            CurrentScope.Value
            ?? throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Actor Join Defer is only valid in a Framework-managed Spot or Actor handler."
            );

        AwaitStateLane(
            scope._lane.RunAsync(() =>
            {
                if (!scope._allowed)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        "Instance Spot handlers cannot register Actor Join operations."
                    );
                if (scope._sealed)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        "Actor Join Defer cannot register after the handler has completed."
                    );
                join.ReserveBarrier();
                scope._joins.Add(join);
            })
        );
    }

    public void Complete()
    {
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (_sealed)
                    return;
                _completed = true;
                _sealed = true;
            })
        );
    }

    public void Dispose()
    {
        var completion = AwaitStateLane(
            _lane.RunAsync(() =>
            {
                _sealed = true;
                return new Completion(_completed, [.. _joins]);
            })
        );

        CurrentScope.Value = _previous;
        if (completion.Completed)
        {
            foreach (var join in completion.Joins)
                join.Activate();
            return;
        }

        foreach (var join in completion.Joins)
            join.Discard();
    }

    private readonly record struct Completion(bool Completed, List<ZLinkDeferredActorJoin> Joins);
}

internal sealed class ZLinkDeferredActorJoin(
    ZLinkFrameworkRuntime runtime,
    ZLinkActorRuntimeState actorState,
    IZLinkActor actor,
    ulong objectGeneration,
    string? targetSpotId,
    ZLinkMessage request,
    TimeSpan timeout
)
{
    private readonly long _registeredTimestamp = runtime.Registration.TimeProvider.GetTimestamp();
    private readonly DateTimeOffset _absoluteDeadline =
        runtime.Registration.TimeProvider.GetUtcNow() + timeout;
    private readonly ZLinkActorJoinOperationId _operationId = CreateOperationId();

    // The deferred Join runs after the submitting callback, so the causal flow of
    // that callback is captured here and re-entered in RunAsync.
    private readonly ZLinkFlowValue? _flow = ZLinkFlowContext.Current;

    private ZLinkActorSerialExecutor.BarrierReservation? _barrier;
    private Task? _targetCompletion;

    public void ReserveBarrier()
    {
        if (runtime.ShutdownToken.IsCancellationRequested)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "Actor Join cannot be deferred while the Framework runtime is shutting down."
            );
        actorState.EnsureDeferredJoinIdentity(actor, objectGeneration);
        _barrier = actorState.ReserveDeferredJoinBarrier(out _targetCompletion);
    }

    public void Activate()
    {
        if (
            ZLinkBoundSessionDispatchScope.TryDefer(
                actorState.ActorId,
                ScheduleAfterBoundSessionTerminalAsync
            )
        )
            return;
        Schedule();
    }

    private ValueTask ScheduleAfterBoundSessionTerminalAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        Schedule();
        return ValueTask.CompletedTask;
    }

    private void Schedule()
    {
        // RunAsync claims the reserved Actor lifecycle barrier. Posting it as
        // application work would place its activation behind its own barrier.
        using var suppressed = ExecutionContext.SuppressFlow();
        if (!runtime.TryRunDetached("actor-deferred-join", RunAsync))
            Discard();
    }

    public void Discard()
    {
        var barrier = Interlocked.Exchange(ref _barrier, null);
        barrier?.Discard();
        Interlocked.Exchange(ref _targetCompletion, null);
        var replay = actorState.Handoff.EndDeferredJoinCapture();
        actorState.ReleaseDeferredJoinBarrier();
        ReplayDeferredJoinFrames(replay);
    }

    private async ValueTask RunAsync(CancellationToken cancellationToken)
    {
        var barrier = Interlocked.Exchange(ref _barrier, null);
        try
        {
            ZLinkFrameworkErrorKind? failure = null;
            if (_targetCompletion is { } targetCompletion)
            {
                try
                {
                    var remaining = RemainingTimeout();
                    if (remaining <= TimeSpan.Zero)
                        failure = ZLinkFrameworkErrorKind.DeadlineExceeded;
                    else
                        await targetCompletion
                            .WaitAsync(
                                remaining,
                                runtime.Registration.TimeProvider,
                                cancellationToken
                            )
                            .ConfigureAwait(false);
                }
                catch (Exception exception)
                {
                    failure = MapFailure(exception);
                    Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"deferred_join_target_wait_failed kind={failure} {exception}"
                    );
                }
                barrier ??= actorState.ReserveDeferredJoinBarrierAfterTarget();
            }

            if (barrier is not { } reservedBarrier)
                throw new InvalidOperationException(
                    "Deferred Actor Join barrier was not reserved."
                );
            // The deferred Join runs as its reserved barrier's turn, and every
            // outcome, including an elapsed deadline, completes inside that
            // turn. The reservation is therefore always claimed and released.
            using var turn = await reservedBarrier.ClaimAsync().ConfigureAwait(false);
            await actorState.Handoff.WithdrawDeferredSuffixAsync().ConfigureAwait(false);
            // Re-enter the flow captured at registration, but only while the
            // live level still allows capture (spec 27 §4): an On→Off flip
            // before the deferred join runs must not re-install flow context.
            using var flow = ZLinkFlowContext.Enter(
                _flow?.FlowId,
                _flow?.Origin,
                runtime.Flow.CaptureEnabled,
                ZLinkFlowOrigin.Application,
                createIfAbsent: false
            );
            using var dispatch = actorState.EnterDeferredJoinExecution();
            ZLinkActorJoinCompletion? completion = null;
            ZLinkActorJoinResult.Relayed? relayed = null;
            if (failure is null && RemainingTimeout() <= TimeSpan.Zero)
                failure = ZLinkFrameworkErrorKind.DeadlineExceeded;
            if (failure is { } failed)
                completion = new ZLinkActorJoinCompletion.Failed(_operationId, failed);
            else
                try
                {
                    actorState.EnsureDeferredJoinIdentity(actor, objectGeneration);
                    var sourceNodeRid = actorState.NativeActorRef?.NodeRid;
                    var result = targetSpotId is { } spotId
                        ? await runtime
                            .JoinActorAsync(
                                spotId,
                                actor,
                                request,
                                _operationId,
                                cancellationToken,
                                _absoluteDeadline
                            )
                            .ConfigureAwait(false)
                        : await runtime
                            .JoinActorEntrySpotAsync(
                                actor,
                                request,
                                _operationId,
                                cancellationToken,
                                _absoluteDeadline
                            )
                            .ConfigureAwait(false);

                    switch (result)
                    {
                        case ZLinkActorJoinResult.Relayed transferred:
                            relayed = transferred;
                            break;
                        case ZLinkActorJoinResult.Accepted accepted
                            when sourceNodeRid is null
                                || accepted.Actor.NodeRid == sourceNodeRid.Value:
                            completion = new ZLinkActorJoinCompletion.Accepted(
                                _operationId,
                                accepted.Actor,
                                accepted.Reply.IsEmpty ? null : accepted.Reply
                            );
                            break;
                        case ZLinkActorJoinResult.Accepted:
                            // The remote target owns its public completion.
                            break;
                        case ZLinkActorJoinResult.Rejected rejected:
                            completion = new ZLinkActorJoinCompletion.Rejected(
                                _operationId,
                                rejected.Reply.IsEmpty ? null : rejected.Reply
                            );
                            break;
                    }
                }
                catch (Exception exception)
                {
                    var kind = MapFailure(exception);
                    //  The completion carries only a kind, so without this the
                    //  originating exception is lost and every throw site that maps
                    //  to the same kind looks identical from the outside. Trace it on
                    //  the message flow as well, so the cause carries the same flow
                    //  identity as the Join that produced it.
                    Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"deferred_join_failed kind={kind} {exception}"
                    );
                    if (runtime.Flow.CaptureEnabled)
                        runtime.Flow.TraceDispatchError(
                            new ZLinkDispatchFailure(
                                ZLinkDispatchErrorSurface.SpotActor,
                                ZLinkDispatchMessageKind.ActorRequest,
                                ZLinkDispatchErrorReason.HandlerException,
                                ZLinkDispatchErrorAction.ReplyError,
                                "JoinSpot",
                                ActorId: actor.Context.ActorId,
                                Exception: exception,
                                FlowId: _flow?.FlowId,
                                FlowOrigin: _flow?.Origin
                            )
                        );
                    completion = new ZLinkActorJoinCompletion.Failed(_operationId, kind);
                }

            // Source cleanup failures belong to the infrastructure task owner,
            // outside the public completion conversion above.
            if (relayed is not null)
                await relayed.SourceCleanup().ConfigureAwait(false);
            if (completion is not null)
                await NotifySourceAsync(completion, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            barrier?.Discard();
            var replay = actorState.Handoff.EndDeferredJoinCapture();
            actorState.ReleaseDeferredJoinBarrier();
            ReplayDeferredJoinFrames(replay);
        }
    }

    private void ReplayDeferredJoinFrames(IReadOnlyList<ZLinkActorHandoffFrame> frames)
    {
        if (frames.Count == 0)
            return;

        var actorRef =
            actorState.NativeActorRef
            ?? throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor '{actorState.ActorId}' has no local Actor reference for Deferred Join replay."
            );
        var batch = ZLinkActorHandoffFrames.Restore(actorRef, frames);
        if (
            !runtime.TryRunDetached(
                "actor-deferred-join-source-replay",
                cancellationToken =>
                    new ZLinkActorInboundPipeline(
                        runtime,
                        new ZLinkEntrySpotActorInboundEndpoint(runtime)
                    ).DispatchAsync(batch, cancellationToken)
            )
        )
            batch.Dispose();
    }

    private TimeSpan RemainingTimeout() =>
        timeout - runtime.Registration.TimeProvider.GetElapsedTime(_registeredTimestamp);

    private ValueTask NotifySourceAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken
    )
    {
        return actor.OnJoinCompletedAsync(completion, cancellationToken);
    }

    private ZLinkFrameworkErrorKind MapFailure(Exception exception)
    {
        if (exception is ZLinkFrameworkException framework)
            return framework.Kind;
        if (
            exception is OperationCanceledException
            && runtime.ShutdownToken.IsCancellationRequested
        )
            return ZLinkFrameworkErrorKind.ShuttingDown;
        if (exception is TimeoutException || exception is OperationCanceledException)
            return ZLinkFrameworkErrorKind.DeadlineExceeded;
        return ZLinkFrameworkErrorKind.InternalFailure;
    }

    private static ZLinkActorJoinOperationId CreateOperationId()
    {
        Span<byte> bytes = stackalloc byte[16];
        while (true)
        {
            RandomNumberGenerator.Fill(bytes);
            var high = BinaryPrimitives.ReadUInt64BigEndian(bytes);
            var low = BinaryPrimitives.ReadUInt64BigEndian(bytes[8..]);
            if (high != 0 || low != 0)
                return new ZLinkActorJoinOperationId(high, low);
        }
    }
}
