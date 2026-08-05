using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkDeferredActorJoinHandlerScope : IDisposable
{
    private const int MaxOperations = 64;
    private const int MaxRequestBytes = 1024 * 1024;
    private const int MaxTotalRequestBytes = 8 * 1024 * 1024;
    private static readonly AsyncLocal<ZLinkDeferredActorJoinHandlerScope?> CurrentScope = new();
    private readonly List<ZLinkDeferredActorJoin> _joins = [];
    private readonly bool _allowed;
    private readonly ZLinkDeferredActorJoinHandlerScope? _previous;
    private readonly object _sync = new();
    private int _requestBytes;
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

    public static void Register(ZLinkDeferredActorJoin join, int requestBytes)
    {
        var scope = CurrentScope.Value
                    ?? throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.InvalidOperation,
                        "Actor Join Defer is only valid in a Framework-managed Spot or Actor handler.");

        if (requestBytes > MaxRequestBytes)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor Join request exceeds the {MaxRequestBytes}-byte limit.");

        lock (scope._sync)
        {
            if (!scope._allowed)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    "Instance Spot handlers cannot register Actor Join operations.");
            if (scope._sealed)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    "Actor Join Defer cannot register after the handler has completed.");
            if (scope._joins.Count >= MaxOperations)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"A handler turn cannot register more than {MaxOperations} Actor Joins.");
            if (scope._requestBytes > MaxTotalRequestBytes - requestBytes)
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    $"Actor Join requests in one handler turn exceed {MaxTotalRequestBytes} bytes.");

            join.ReserveBarrier();
            scope._joins.Add(join);
            scope._requestBytes += requestBytes;
        }
    }

    public void Complete()
    {
        lock (_sync)
        {
            if (_sealed) return;
            _completed = true;
            _sealed = true;
        }
    }

    public void Dispose()
    {
        List<ZLinkDeferredActorJoin> joins;
        lock (_sync)
        {
            _sealed = true;
            joins = [.. _joins];
        }

        CurrentScope.Value = _previous;
        if (_completed)
        {
            foreach (var join in joins) join.Activate();
            return;
        }

        foreach (var join in joins) join.Discard();
    }
}

internal sealed class ZLinkDeferredActorJoin(
    ZLinkFrameworkRuntime runtime,
    ZLinkActorRuntimeState actorState,
    IZLinkActor actor,
    ulong objectGeneration,
    string? targetSpotId,
    ZLinkMessage request,
    TimeSpan timeout)
{
    private readonly long _registeredTimestamp = Stopwatch.GetTimestamp();
    private readonly DateTimeOffset _absoluteDeadline =
        DateTimeOffset.UtcNow + timeout;
    private readonly ZLinkActorJoinOperationId _operationId = CreateOperationId();

    // The deferred Join runs after the submitting callback, so the causal flow of
    // that callback is captured here and re-entered in RunAsync.
    private readonly ZLinkFlowValue? _flow = ZLinkFlowContext.Current;

    // Ledger §2.3: C++ posts the deferred work back onto the serial queue that
    // owns the turn, Java chains it after the handler stage and Node runs it on
    // the next event-loop tick. All three keep registration order and run the
    // join right after the handler terminal, so .NET posts onto the submitting
    // turn instead of detaching an unordered task.
    private readonly ZLinkSerialTurn? _turn = ZLinkSerialTurn.Current;
    private readonly IZLinkCurrentSpotActivation? _spotActivation =
        ZLinkSpotAmbientContext.CurrentOrDefault;
    private ZLinkActorDispatchMailbox.BarrierReservation? _barrier;
    private Task? _targetCompletion;

    public void ReserveBarrier()
    {
        if (runtime.ShutdownToken.IsCancellationRequested)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.ShuttingDown,
                "Actor Join cannot be deferred while the Framework runtime is shutting down.");
        actorState.EnsureDeferredJoinIdentity(actor, objectGeneration);
        _barrier = actorState.ReserveDeferredJoinBarrier(out _targetCompletion);
    }

    public void Activate()
    {
        if (ZLinkBoundSessionDispatchScope.TryDefer(
                actorState.ActorId,
                ScheduleAfterBoundSessionTerminalAsync)) return;
        Schedule();
    }

    private ValueTask ScheduleAfterBoundSessionTerminalAsync(
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        Schedule();
        return ValueTask.CompletedTask;
    }

    private void Schedule()
    {
        if (_turn is { } turn && turn.TryPost(RunOnSubmittingSpotAsync)) return;
        if (!runtime.TryRunDetached("actor-deferred-join", RunAsync))
            Discard();
    }

    private async ValueTask RunOnSubmittingSpotAsync(
        CancellationToken cancellationToken)
    {
        using var spot = _spotActivation is null
            ? null
            : ZLinkSpotAmbientContext.Push(_spotActivation);
        await RunAsync(cancellationToken).ConfigureAwait(false);
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
            var remaining = timeout - Stopwatch.GetElapsedTime(_registeredTimestamp);
            if (remaining <= TimeSpan.Zero)
            {
                await NotifySourceAsync(
                        new ZLinkActorJoinCompletion.Failed(
                            _operationId,
                            ZLinkFrameworkErrorKind.DeadlineExceeded),
                        cancellationToken)
                    .ConfigureAwait(false);
                return;
            }

            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            deadline.CancelAfter(remaining);
            if (_targetCompletion is { } targetCompletion)
            {
                try
                {
                    await targetCompletion.WaitAsync(deadline.Token)
                        .ConfigureAwait(false);
                    barrier = actorState.ReserveDeferredJoinBarrierAfterTarget();
                }
                catch (Exception exception)
                {
                    var kind = MapFailure(exception, deadline);
                    Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                        $"deferred_join_target_wait_failed kind={kind} {exception}");
                    await NotifySourceAsync(
                            new ZLinkActorJoinCompletion.Failed(_operationId, kind),
                            cancellationToken)
                        .ConfigureAwait(false);
                    return;
                }
            }

            if (barrier is not { } reservedBarrier)
                throw new InvalidOperationException(
                    "Deferred Actor Join barrier was not reserved.");
            using var turn = await reservedBarrier.ClaimAsync().ConfigureAwait(false);
            using var flow = ZLinkFlowContext.Enter(
                _flow?.FlowId,
                _flow?.Origin,
                createIfAbsent: false,
                ZLinkFlowOrigin.Application);
            using var dispatch = actorState.EnterDeferredJoinExecution();
            ZLinkActorJoinCompletion? completion = null;
            try
            {
                actorState.EnsureDeferredJoinIdentity(actor, objectGeneration);
                var sourceNodeRid = actorState.NativeActorRef?.NodeRid;
                var result = targetSpotId is { } spotId
                    ? await runtime.JoinActorAsync(
                            spotId,
                            actor,
                            request,
                            _operationId,
                            deadline.Token,
                            _absoluteDeadline)
                        .ConfigureAwait(false)
                    : await runtime.JoinActorEntrySpotAsync(
                            actor,
                            request,
                            _operationId,
                            deadline.Token,
                            _absoluteDeadline)
                        .ConfigureAwait(false);

                switch (result)
                {
                    case ZLinkActorJoinResult.Accepted accepted
                        when sourceNodeRid is null || accepted.Actor.NodeRid == sourceNodeRid.Value:
                        completion = new ZLinkActorJoinCompletion.Accepted(
                            _operationId,
                            accepted.Actor,
                            accepted.Reply.IsEmpty ? null : accepted.Reply);
                        break;
                    case ZLinkActorJoinResult.Accepted:
                        // The target runtime delivers durable cross-node Accepted
                        // after replay and source cleanup through the handoff
                        // completion request.
                        break;
                    case ZLinkActorJoinResult.Rejected rejected:
                        completion = new ZLinkActorJoinCompletion.Rejected(
                            _operationId,
                            rejected.Reply.IsEmpty ? null : rejected.Reply);
                        break;
                }
            }
            catch (Exception exception)
            {
                var kind = MapFailure(exception, deadline);
                //  The completion carries only a kind, so without this the
                //  originating exception is lost and every throw site that maps
                //  to the same kind looks identical from the outside.
                Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"deferred_join_failed kind={kind} {exception}");
                completion = new ZLinkActorJoinCompletion.Failed(_operationId, kind);
            }

            if (completion is not null)
                await NotifySourceAsync(completion, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            var replay = actorState.Handoff.EndDeferredJoinCapture();
            actorState.ReleaseDeferredJoinBarrier();
            ReplayDeferredJoinFrames(replay);
        }
    }

    private void ReplayDeferredJoinFrames(
        IReadOnlyList<ZLinkActorHandoffFrame> frames)
    {
        if (frames.Count == 0) return;

        var actorRef = actorState.NativeActorRef
                       ?? throw new ZLinkFrameworkException(
                           ZLinkFrameworkErrorKind.NotFound,
                           $"Actor '{actorState.ActorId}' has no local Actor reference for Deferred Join replay.");
        var batch = ZLinkActorHandoffFrames.Restore(actorRef, frames);
        if (!runtime.TryRunDetached(
                "actor-deferred-join-source-replay",
                cancellationToken => new ZLinkActorInboundPipeline(
                        runtime,
                        new ZLinkEntrySpotActorInboundEndpoint(runtime))
                    .DispatchAsync(batch, cancellationToken)))
            batch.Dispose();
    }

    private ValueTask NotifySourceAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken)
    {
        return actor.OnJoinCompletedAsync(completion, cancellationToken);
    }

    private ZLinkFrameworkErrorKind MapFailure(
        Exception exception,
        CancellationTokenSource deadline)
    {
        if (exception is ZLinkFrameworkException framework)
            return framework.Kind;
        if (exception is OperationCanceledException
            && runtime.ShutdownToken.IsCancellationRequested)
            return ZLinkFrameworkErrorKind.ShuttingDown;
        if (exception is TimeoutException
            || exception is OperationCanceledException && deadline.IsCancellationRequested)
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
            if (high != 0 || low != 0) return new ZLinkActorJoinOperationId(high, low);
        }
    }
}
