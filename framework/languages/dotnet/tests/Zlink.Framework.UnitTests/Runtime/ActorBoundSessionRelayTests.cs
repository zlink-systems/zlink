namespace Zlink.Framework.UnitTests;

using Zlink.Framework.Runtime.Backend.Contracts;

public sealed class ActorBoundSessionRelayTests
{
    [Fact]
    public void Bound_Session_Operation_Runs_Only_For_The_Expected_Binding()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var originalRid = RoutingId.From("session-original");
        var replacementRid = RoutingId.From("session-replacement");
        var originalToken = ZLinkActorBoundSessionBindingToken.Native(originalRid);
        var replacementToken = ZLinkActorBoundSessionBindingToken.Native(replacementRid);
        var calls = 0;

        state.BindSession(
            null,
            originalRid,
            originalToken,
            objectGeneration: 1,
            authorityOwnerGeneration: 1,
            meshName: "actors",
            ownerLeaseGeneration: 1);
        Assert.True(state.TryUseBoundSession(originalToken, _ =>
        {
            calls++;
            return true;
        }));

        state.BindSession(
            null,
            replacementRid,
            replacementToken,
            objectGeneration: 1,
            authorityOwnerGeneration: 2,
            meshName: "actors",
            ownerLeaseGeneration: 1);
        Assert.True(state.TryUseBoundSession(originalToken, _ =>
        {
            calls++;
            return true;
        }));

        Assert.Equal(1, calls);
        Assert.True(state.TryGetBoundSession(out var current));
        Assert.Equal(replacementToken, current.BindingToken);
    }

    [Fact]
    public async Task Failed_Replacement_Rolls_Back_And_Releases_Duplicate_Joiners()
    {
        var state = new ZLinkActorRuntimeState("actor-rollback");
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        var replacement = Begin(
            state,
            "binding-b",
            "session-b",
            authorityGeneration: 2);
        var duplicate = Begin(
            state,
            "binding-b",
            "session-b",
            authorityGeneration: 2);
        Assert.True(replacement.OwnsExecution);
        Assert.False(duplicate.OwnsExecution);
        Assert.Same(replacement.Completion, duplicate.Completion);
        Assert.True(state.TryGetBoundSession(out var beforeTerminal));
        Assert.Equal("binding-a", beforeTerminal.BindingToken);

        var failure = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            "tombstone failed",
            ZLinkRetryAdvice.RetryAfterBackoff);
        state.AbortSessionReplacement(replacement, failure);

        Assert.Same(failure, await duplicate.Completion);
        Assert.True(state.TryGetBoundSession(out var rolledBack));
        Assert.Equal("binding-a", rolledBack.BindingToken);
        var next = Begin(
            state,
            "binding-c",
            "session-c",
            authorityGeneration: 3);
        Assert.True(next.OwnsExecution);
        var cancellation = new OperationCanceledException(
            new CancellationToken(canceled: true));
        state.AbortSessionReplacement(next, cancellation);
        Assert.Same(cancellation, await next.Completion);
        Assert.True(state.TryGetBoundSession(out var afterCancellation));
        Assert.Equal("binding-a", afterCancellation.BindingToken);
    }

    [Fact]
    public async Task Completed_Replacement_Rejects_A_Response_Loss_Replay_Of_The_Old_Bind()
    {
        var state = new ZLinkActorRuntimeState("actor-response-loss");
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        var replacement = Begin(
            state,
            "binding-b",
            "session-b",
            authorityGeneration: 2);
        var duplicate = Begin(
            state,
            "binding-b",
            "session-b",
            authorityGeneration: 2);
        state.PublishSessionReplacement(replacement);
        state.CompleteSessionReplacement(replacement);
        Assert.Null(await replacement.Completion);
        Assert.Null(await duplicate.Completion);

        var stale = Assert.Throws<ZLinkFrameworkException>(() =>
            Begin(
                state,
                "binding-a",
                "session-a",
                authorityGeneration: 1));
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, stale.Kind);
        Assert.Equal(ZLinkRetryAdvice.DoNotRetry, stale.RetryAdvice);
        Assert.True(state.TryGetBoundSession(out var current));
        Assert.Equal("binding-b", current.BindingToken);

        var exactReplay = Begin(
            state,
            "binding-b",
            "session-b",
            authorityGeneration: 2);
        Assert.False(exactReplay.OwnsExecution);
        Assert.Null(await exactReplay.Completion);
    }

    [Fact]
    public async Task Published_Replacement_Survives_Cleanup_Failure_And_Exact_Retry_Completes()
    {
        var state = new ZLinkActorRuntimeState("actor-forward-completion");
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        var first = Begin(
            state,
            "binding-b",
            "session-b",
            authorityGeneration: 2);

        state.PublishSessionReplacement(first);
        var cleanupFailure = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            "second tombstone failed",
            ZLinkRetryAdvice.RetryAfterBackoff);
        state.AbortSessionReplacement(first, cleanupFailure);

        Assert.Same(cleanupFailure, await first.Completion);
        Assert.True(state.TryGetBoundSession(out var terminal));
        Assert.Equal("binding-b", terminal.BindingToken);

        var retry = Begin(
            state,
            "binding-b",
            "session-b",
            authorityGeneration: 2);
        Assert.True(retry.OwnsExecution);
        state.PublishSessionReplacement(retry);
        state.CompleteSessionReplacement(retry);

        Assert.Null(await retry.Completion);
        Assert.True(state.TryGetBoundSession(out terminal));
        Assert.Equal("binding-b", terminal.BindingToken);
        var delayedOld = Assert.Throws<ZLinkFrameworkException>(() =>
            Begin(
                state,
                "binding-a",
                "session-a",
                authorityGeneration: 1));
        Assert.Equal(ZLinkRetryAdvice.DoNotRetry, delayedOld.RetryAdvice);
    }

    [Fact]
    public async Task Prepared_Replacement_Resumes_After_The_First_Durable_Side_Effect()
    {
        var state = new ZLinkActorRuntimeState("actor-prepared-forward-completion");
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        var first = Begin(
            state,
            "binding-b",
            "session-b",
            authorityGeneration: 2);

        state.MarkPreviousSessionBindingTombstoned(first);
        var publishFailure = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            "authority changed before publish",
            ZLinkRetryAdvice.RetryAfterBackoff);
        state.AbortSessionReplacement(first, publishFailure);

        Assert.Same(publishFailure, await first.Completion);
        Assert.True(state.TryGetBoundSession(out var unchanged));
        Assert.Equal("binding-a", unchanged.BindingToken);

        var retry = Begin(
            state,
            "binding-b",
            "session-b",
            authorityGeneration: 2);
        Assert.True(retry.OwnsExecution);
        Assert.True(retry.PreviousBindingTombstoned);
        state.PublishSessionReplacement(retry);
        state.CompleteSessionReplacement(retry);
        Assert.Null(await retry.Completion);

        var next = Begin(
            state,
            "binding-c",
            "session-c",
            authorityGeneration: 3);
        Assert.True(next.OwnsExecution);
        state.AbortSessionReplacement(
            next,
            new OperationCanceledException(new CancellationToken(true)));
    }

    [Fact]
    public void Destroy_Or_Relocation_During_Replacement_Makes_Publish_Fail_Closed()
    {
        var destroyed = new ZLinkActorRuntimeState("actor-destroy-race");
        _ = Bind(destroyed, "binding-a", "session-a", authorityGeneration: 1);
        var destroyAttempt = Begin(
            destroyed,
            "binding-b",
            "session-b",
            authorityGeneration: 2);
        destroyed.ClearAfterDestroy();
        var destroyFailure = Assert.Throws<ZLinkFrameworkException>(() =>
            destroyed.PublishSessionReplacement(destroyAttempt));
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, destroyFailure.Kind);

        var relocated = new ZLinkActorRuntimeState("actor-relocate-race");
        _ = Bind(relocated, "binding-a", "session-a", authorityGeneration: 1);
        var relocationAttempt = Begin(
            relocated,
            "binding-b",
            "session-b",
            authorityGeneration: 2);
        var source = new ZLinkBackendActorRef(
            RoutingId.From("source-node"),
            relocated.ActorId,
            7);
        var target = new ZLinkBackendActorRef(
            RoutingId.From("target-node"),
            relocated.ActorId,
            7);
        relocated.Handoff.BeginCapture();
        _ = relocated.Handoff.CutoverCaptureToMessageFollow(
            0,
            source,
            target,
            "actors",
            sourceNodeGeneration: 1,
            targetNodeGeneration: 1,
            sourceAuthorityOwnerGeneration: 1,
            targetAuthorityOwnerGeneration: 2,
            sourceOwnerLeaseGeneration: 1,
            targetOwnerLeaseGeneration: 2);
        relocated.Handoff.CommitMessageFollow(TimeSpan.FromSeconds(1));
        relocated.RetireMigratedActorInstance(source);
        var relocationFailure = Assert.Throws<ZLinkFrameworkException>(() =>
            relocated.PublishSessionReplacement(relocationAttempt));
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, relocationFailure.Kind);
        Assert.True(relocated.TryGetBoundSession(out var retained));
        Assert.Equal("binding-a", retained.BindingToken);
    }

    [Fact]
    public void Actor_Side_Tombstone_Is_Idempotent_And_Rejects_Delayed_Old_Bind()
    {
        var state = new ZLinkActorRuntimeState("actor-exact-tombstone");
        _ = Bind(
            state,
            "binding-a",
            "session-a",
            authorityGeneration: 1);
        Assert.True(state.TryGetBoundSession(out var retired));

        state.TombstoneSession(retired);
        state.TombstoneSession(retired);

        Assert.False(state.TryGetBoundSession(out _));
        var delayed = Assert.Throws<ZLinkFrameworkException>(() =>
            Bind(state, "binding-a", "session-a", authorityGeneration: 1));
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, delayed.Kind);
        Assert.Equal(ZLinkRetryAdvice.DoNotRetry, delayed.RetryAdvice);
    }

    [Fact]
    public void Exact_Retry_Rejects_A_Different_Previous_Binding_Fence()
    {
        var state = new ZLinkActorRuntimeState("actor-previous-fence");
        var firstFence = PreviousFence("previous-a");
        var first = Begin(
            state,
            "binding-b",
            "session-b",
            authorityGeneration: 2,
            previousFence: firstFence);

        var conflict = Assert.Throws<ZLinkFrameworkException>(() =>
            Begin(
                state,
                "binding-b",
                "session-b",
                authorityGeneration: 2,
                previousFence: PreviousFence("previous-b")));
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, conflict.Kind);
        state.AbortSessionReplacement(
            first,
            new InvalidOperationException("test cleanup"));
    }

    [Fact]
    public void Session_Binding_Source_Requires_Current_Admitted_Lifecycle()
    {
        var localRid = RoutingId.From("actor-owner");
        var peerRid = RoutingId.From("session-owner");
        var reusedRid = RoutingId.From("reused-owner");
        var local = NodeStatus(localRid, lifecycleGeneration: 11);
        var peers = new[]
        {
            Peer(peerRid, lifecycleGeneration: 17, MeshPeerState.Admitted),
            Peer(reusedRid, lifecycleGeneration: 23, MeshPeerState.Admitted)
        };

        Assert.True(ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(
            local,
            peers,
            peerRid,
            17));
        Assert.True(ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(
            local,
            peers,
            localRid,
            11));
        Assert.False(ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(
            local,
            peers,
            localRid,
            10));
        Assert.False(ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(
            local,
            peers,
            RoutingId.From("wrong-source"),
            17));
        Assert.False(ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(
            local,
            peers,
            reusedRid,
            22));
        Assert.False(ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(
            local,
            peers,
            reusedRid,
            0));
    }

    [Fact]
    public void Duplicate_Token_Requires_The_Full_Immutable_Binding_Identity()
    {
        var state = new ZLinkActorRuntimeState("actor-conflict");
        var pending = Begin(
            state,
            "binding-a",
            "session-a",
            authorityGeneration: 1);

        var conflict = Assert.Throws<ZLinkFrameworkException>(() =>
            state.BeginSessionReplacement(
                RoutingId.From("session-node"),
                RoutingId.From("session-a"),
                "binding-a",
                bindingGeneration: 1,
                objectGeneration: 7,
                authorityOwnerGeneration: 1,
                meshName: "actors",
                targetNodeGeneration: 1,
                ownerLeaseGeneration: 99,
                sessionOwnerNodeGeneration: 1,
                acceptedHighWater: 0));
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, conflict.Kind);
        Assert.True(pending.OwnsExecution);
    }

    [Fact]
    public void Actor_Owner_Tombstones_Are_Time_And_Count_Bounded()
    {
        var time = new ManualTimeProvider();
        var state = new ZLinkActorRuntimeState(
            "actor-bounds",
            time,
            sessionBindingTombstoneRetention: TimeSpan.FromSeconds(1),
            maxSessionBindingTombstones: 2);
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        _ = Bind(state, "binding-b", "session-b", authorityGeneration: 2);
        _ = Bind(state, "binding-c", "session-c", authorityGeneration: 3);
        Assert.Equal(2, state.SessionBindingTombstoneCount);

        var capacity = Assert.Throws<ZLinkFrameworkException>(() =>
            Bind(state, "binding-d", "session-d", authorityGeneration: 4));
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, capacity.Kind);
        Assert.True(state.TryGetBoundSession(out var retained));
        Assert.Equal("binding-c", retained.BindingToken);

        time.Advance(TimeSpan.FromSeconds(2));
        Assert.Equal(0, state.SessionBindingTombstoneCount);
        _ = Bind(state, "binding-d", "session-d", authorityGeneration: 4);
        Assert.Equal(1, state.SessionBindingTombstoneCount);
    }

    private static ZLinkActorBoundSession? Bind(
        ZLinkActorRuntimeState state,
        string token,
        string session,
        ulong authorityGeneration)
    {
        return state.BindSession(
            RoutingId.From("session-node"),
            RoutingId.From(session),
            token,
            bindingGeneration: 1,
            objectGeneration: 7,
            authorityOwnerGeneration: authorityGeneration,
            meshName: "actors",
            targetNodeGeneration: 1,
            ownerLeaseGeneration: 1,
            sessionOwnerNodeGeneration: 1,
            acceptedHighWater: 0);
    }

    private static ZLinkActorSessionReplacementAttempt Begin(
        ZLinkActorRuntimeState state,
        string token,
        string session,
        ulong authorityGeneration,
        ZLinkActorPreviousBindingFence? previousFence = null)
    {
        return state.BeginSessionReplacement(
            RoutingId.From("session-node"),
            RoutingId.From(session),
            token,
            bindingGeneration: 1,
            objectGeneration: 7,
            authorityOwnerGeneration: authorityGeneration,
            meshName: "actors",
            targetNodeGeneration: 1,
            ownerLeaseGeneration: 1,
            sessionOwnerNodeGeneration: 1,
            acceptedHighWater: 0,
            previousFence: previousFence);
    }

    private static ZLinkActorPreviousBindingFence PreviousFence(string token) =>
        new(
            RoutingId.From("previous-node"),
            RoutingId.From("session-node"),
            RoutingId.From("session-a"),
            token,
            BindingGeneration: 1,
            ObjectGeneration: 7,
            MeshName: "actors",
            TargetNodeGeneration: 1,
            AuthorityOwnerGeneration: 1,
            OwnerLeaseGeneration: 1,
            SessionOwnerNodeGeneration: 1,
            AcceptedHighWater: 0);

    private static MeshNodeStatus NodeStatus(
        RoutingId rid,
        ulong lifecycleGeneration) =>
        new(
            MeshNodeState.Ready,
            rid,
            "actors",
            "inproc://actors",
            lifecycleGeneration,
            DescriptorRevision: 1,
            ChannelCount: 1,
            ConfiguredPeerCount: 1,
            AdmittedPeerCount: 1,
            DrainingPeerCount: 0,
            PendingApplicationMessages: 0,
            PendingInfrastructureMessages: 0,
            PendingBytes: 0,
            LastError: 0,
            LastChangedMs: 1);

    private static MeshNodePeer Peer(
        RoutingId rid,
        ulong lifecycleGeneration,
        MeshPeerState state) =>
        new(
            ConnectionIntentId: 1,
            Source: MeshPeerSource.Discovery,
            State: state,
            RoutingId: rid,
            LifecycleGeneration: lifecycleGeneration,
            DescriptorRevision: 1,
            Endpoint: "inproc://peer",
            ChannelCount: 1,
            LastError: 0,
            LastChangedMs: 1);
}
