namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    private readonly ZLinkActorJoinPrewarmRegistry _actorJoinPrewarm = new();

    /// <summary>
    /// Spec 15 §4.2 production ingress entry point: called from
    /// <c>ZLinkActorInboundPipeline.DispatchFrameAsync</c> right before it
    /// would otherwise treat a null-resolved Actor as NotFound. Parks the
    /// arrival if a cross-node Actor Join admission is in flight for this
    /// exact object (Accepted but not yet Restored/PREPAREd locally) and
    /// returns true so the caller stops instead of falling into NotFound.
    /// Returns false when no such admission exists — including once
    /// PREPARE already migrated it, since the real Actor then resolves the
    /// normal way — so the caller's existing NotFound handling is
    /// unchanged for every other case.
    /// </summary>
    internal bool TryParkActorJoinPrewarmIngress(ZLinkSpotActorFrame frame)
    {
        var durable = ZLinkActorHandoffFrames.Capture(frame, arrivalIndex: 0);
        var actorRef = frame.Actor;
        var sourceNodeRid = frame.SourceNodeRid;
        var sourceSessionRid = frame.SourceSessionRid;
        var requestId = frame.RequestId;
        var flags = frame.Flags;
        var replyCapability = frame.RouteContext.ReplyCapability;
        var header = frame.Header;
        var directReply = frame.DirectReply;
        var route = _actorJoinPrewarm.ParkOrDeliver(
            actorRef.ActorId,
            actorRef.Generation,
            durable,
            onFailed: () => RunDetached(
                "actor-join-prewarm-fail-parked",
                ct => ZLinkActorBoundSessionRelay.ReplyStaleActorAsync(
                    this,
                    actorRef,
                    sourceNodeRid,
                    sourceSessionRid,
                    requestId,
                    flags,
                    replyCapability,
                    header,
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NotFound,
                        $"Actor '{actorRef.ActorId}' relocation admission ended before it completed."),
                    ct,
                    directReply)));
        return route == ZLinkActorJoinPrewarmRegistry.IngressRoute.Parked;
    }

    /// <summary>
    /// Registers a cross-node Actor Join prewarm attempt once admission
    /// decides Accepted (spec 15 §4.2), and arms its expiry to the
    /// admission's own validity window — an accepted-but-never-started
    /// move (DisableRelocation, capacity, compat failure) releases the
    /// prewarm attempt once that window elapses, mirroring the admission
    /// reservation's own cleanup.
    /// </summary>
    internal void RegisterActorJoinPrewarm(
        string handoffId,
        string actorId,
        ulong actorGeneration,
        DateTimeOffset deadline)
    {
        _actorJoinPrewarm.Register(
            handoffId,
            actorId,
            actorGeneration,
            onEvicted: evictedHandoffId => ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_join_prewarm_evicted handoff={evictedHandoffId}"));
        RunDetached(
            "actor-join-prewarm-expiry",
            async ct =>
            {
                var remaining = deadline - TimeProvider.System.GetUtcNow();
                if (remaining > TimeSpan.Zero)
                {
                    try
                    {
                        await Task.Delay(remaining, TimeProvider.System, ct)
                            .ConfigureAwait(false);
                    }
                    catch (OperationCanceledException)
                    {
                        return;
                    }
                }
                _actorJoinPrewarm.Release(handoffId);
            });
    }

    /// <summary>
    /// Releases an Actor Join prewarm attempt outside its normal
    /// migration or expiry path — explicit admission abort, or a
    /// rejected/failed admission that registered the attempt before the
    /// outcome was known.
    /// </summary>
    internal void ReleaseActorJoinPrewarm(string handoffId) =>
        _actorJoinPrewarm.Release(handoffId);

    /// <summary>
    /// PREPARE (Restore) hook (spec 15 §4.2): installs the real per-actor
    /// import and migrates every arrival parked since Accepted into it,
    /// atomically, then the attempt no longer exists. A late PREPARE for
    /// an identity a newer exact identity already evicted is discarded —
    /// logged, not thrown into the caller's own PREPARE flow.
    /// </summary>
    internal void CompleteActorJoinPrewarmMigration(
        string handoffId,
        ZLinkActorRuntimeState actorState)
    {
        try
        {
            _actorJoinPrewarm.CompleteMigration(
                handoffId,
                frames => actorState.Handoff.AppendPreparedImport(
                    handoffId,
                    frames));
        }
        catch (InvalidOperationException)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"actor_join_prewarm_migration_discarded handoff={handoffId}");
        }
    }
}
