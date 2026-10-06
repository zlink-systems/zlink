using Zlink.Framework.Runtime.Identifiers;

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
            meshName: ZLinkMeshName.FromBoundary("actors", "meshName"),
            ownerLeaseGeneration: 1
        );
        Assert.True(
            state.TryUseBoundSession(
                originalToken,
                _ =>
                {
                    calls++;
                    return true;
                }
            )
        );

        state.BindSession(
            null,
            replacementRid,
            replacementToken,
            objectGeneration: 1,
            authorityOwnerGeneration: 2,
            meshName: ZLinkMeshName.FromBoundary("actors", "meshName"),
            ownerLeaseGeneration: 1
        );
        Assert.True(
            state.TryUseBoundSession(
                originalToken,
                _ =>
                {
                    calls++;
                    return true;
                }
            )
        );

        Assert.Equal(1, calls);
        Assert.True(state.TryGetBoundSession(out var current));
        Assert.Equal(replacementToken, current.BindingToken);
    }

    [Fact]
    public void Failed_Replacement_Preserves_The_Previous_Binding_And_Allows_Exact_Retry()
    {
        var state = new ZLinkActorRuntimeState("actor-rollback");
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        var failure = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.Unavailable,
            "authority changed",
            ZLinkRetryAdvice.RetryAfterBackoff
        );
        Assert.Same(
            failure,
            Assert.Throws<ZLinkFrameworkException>(() =>
                Replace(state, "binding-b", "session-b", 2, _ => throw failure)
            )
        );
        Assert.True(state.TryGetBoundSession(out var retained));
        Assert.Equal("binding-a", retained.BindingToken);
        Assert.Equal(0, state.SessionBindingTombstoneCount);
        var retry = Replace(state, "binding-b", "session-b", 2);
        Assert.True(retry.Changed);
        Assert.Equal("binding-a", retry.Previous?.BindingToken);
    }

    [Fact]
    public void Completed_Replacement_Rejects_A_Response_Loss_Replay_Of_The_Old_Bind()
    {
        var state = new ZLinkActorRuntimeState("actor-response-loss");
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        Assert.True(Replace(state, "binding-b", "session-b", 2).Changed);
        var stale = Assert.Throws<ZLinkFrameworkException>(() =>
            Replace(state, "binding-a", "session-a", 1)
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, stale.Kind);
        Assert.Equal(ZLinkRetryAdvice.DoNotRetry, stale.RetryAdvice);
        Assert.True(state.TryGetBoundSession(out var current));
        Assert.Equal("binding-b", current.BindingToken);
        var exactReplay = Replace(state, "binding-b", "session-b", 2);
        Assert.False(exactReplay.Changed);
        Assert.Null(exactReplay.Previous);
    }

    [Fact]
    public async Task Concurrent_Exact_Duplicates_Have_One_Installation_And_One_Retirement()
    {
        var state = new ZLinkActorRuntimeState("actor-duplicates");
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        var results = await Task.WhenAll(
            Enumerable
                .Range(0, 8)
                .Select(_ => Task.Run(() => Replace(state, "binding-b", "session-b", 2)))
        );
        Assert.Single(results.Where(result => result.Changed));
        Assert.Single(results.Where(result => result.Previous is not null));
        Assert.Equal(1, state.SessionBindingTombstoneCount);
        Assert.True(state.TryGetBoundSession(out var current));
        Assert.Equal("binding-b", current.BindingToken);
    }

    [Fact]
    public async Task Previous_Session_Unbind_Cannot_Invalidate_The_Replacement_Turn()
    {
        var state = new ZLinkActorRuntimeState("actor-previous-unbind-race");
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        Task cleanup = Task.CompletedTask;
        var replacement = Replace(
            state,
            "binding-b",
            "session-b",
            2,
            _ =>
            {
                Assert.NotNull(ZLinkStateLane.Current);
                using (ExecutionContext.SuppressFlow())
                    cleanup = Task.Run(() => state.UnbindSession("binding-a"));
            }
        );
        await cleanup;
        Assert.True(replacement.Changed);
        Assert.Equal("binding-a", replacement.Previous?.BindingToken);
        Assert.True(state.TryGetBoundSession(out var current));
        Assert.Equal("binding-b", current.BindingToken);
        state.UnbindSession("binding-a");
        Assert.True(state.TryGetBoundSession(out current));
        Assert.Equal("binding-b", current.BindingToken);
        var delayedOld = Assert.Throws<ZLinkFrameworkException>(() =>
            Bind(state, "binding-a", "session-a", authorityGeneration: 1)
        );
        Assert.Equal(ZLinkRetryAdvice.DoNotRetry, delayedOld.RetryAdvice);
        state.UnbindSession("binding-b");
        Assert.False(state.TryGetBoundSession(out _));
    }

    [Fact]
    public async Task Replacement_Token_Unbind_Runs_After_Installation()
    {
        var state = new ZLinkActorRuntimeState("actor-replacement-unbind");
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        Task cleanup = Task.CompletedTask;
        var result = Replace(
            state,
            "binding-b",
            "session-b",
            2,
            _ =>
            {
                using (ExecutionContext.SuppressFlow())
                    cleanup = Task.Run(() => state.UnbindSession("binding-b"));
            }
        );
        await cleanup;
        Assert.True(result.Changed);
        Assert.False(state.TryGetBoundSession(out _));
        Assert.Equal(1, state.SessionBindingTombstoneCount);
    }

    [Fact]
    public void Tombstone_Capacity_Failure_Preserves_Current_And_Exact_Retry_Can_Install()
    {
        var time = new ManualTimeProvider();
        var state = new ZLinkActorRuntimeState(
            "actor-capacity",
            time,
            sessionBindingTombstoneRetention: TimeSpan.FromSeconds(1),
            maxSessionBindingTombstones: 1
        );
        Replace(state, "binding-a", "session-a", 1);
        Replace(state, "binding-b", "session-b", 2);
        var failure = Assert.Throws<ZLinkFrameworkException>(() =>
            Replace(state, "binding-c", "session-c", 3)
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
        Assert.True(state.TryGetBoundSession(out var retained));
        Assert.Equal("binding-b", retained.BindingToken);
        Assert.False(Replace(state, "binding-b", "session-b", 2).Changed);
        time.Advance(TimeSpan.FromSeconds(2));
        Assert.True(Replace(state, "binding-c", "session-c", 3).Changed);
    }

    [Fact]
    public void Actor_Side_Tombstone_Is_Idempotent_And_Rejects_Delayed_Old_Bind()
    {
        var state = new ZLinkActorRuntimeState("actor-exact-tombstone");
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        Assert.True(state.TryGetBoundSession(out var retired));

        state.TombstoneSession(retired);
        state.TombstoneSession(retired);

        Assert.False(state.TryGetBoundSession(out _));
        var delayed = Assert.Throws<ZLinkFrameworkException>(() =>
            Bind(state, "binding-a", "session-a", authorityGeneration: 1)
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, delayed.Kind);
        Assert.Equal(ZLinkRetryAdvice.DoNotRetry, delayed.RetryAdvice);
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
            Peer(reusedRid, lifecycleGeneration: 23, MeshPeerState.Admitted),
        };

        Assert.True(ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(local, peers, peerRid, 17));
        Assert.True(ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(local, peers, localRid, 11));
        Assert.False(
            ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(local, peers, localRid, 10)
        );
        Assert.False(
            ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(
                local,
                peers,
                RoutingId.From("wrong-source"),
                17
            )
        );
        Assert.False(
            ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(local, peers, reusedRid, 22)
        );
        Assert.False(
            ZLinkFrameworkRuntime.MatchesAdmittedNodeLifecycle(local, peers, reusedRid, 0)
        );
    }

    [Fact]
    public void Duplicate_Token_Requires_The_Full_Immutable_Binding_Identity()
    {
        var state = new ZLinkActorRuntimeState("actor-conflict");
        var installed = Replace(state, "binding-a", "session-a", authorityGeneration: 1);

        var conflict = Assert.Throws<ZLinkFrameworkException>(() =>
            state.ReplaceSessionBinding(
                RoutingId.From("session-node"),
                RoutingId.From("session-a"),
                "binding-a",
                bindingGeneration: 1,
                objectGeneration: 7,
                authorityOwnerGeneration: 1,
                meshName: ZLinkMeshName.FromBoundary("actors", "meshName"),
                targetNodeGeneration: 1,
                ownerLeaseGeneration: 99,
                sessionOwnerNodeGeneration: 1,
                acceptedHighWater: 0
            )
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, conflict.Kind);
        Assert.True(installed.Changed);
    }

    [Fact]
    public void Same_Physical_Session_Requires_The_Exact_Owner_Identity()
    {
        var nodeRid = RoutingId.From("session-node");
        var sessionRid = RoutingId.From("session-rid");
        var binding = new ZLinkActorBoundSession(
            nodeRid,
            sessionRid,
            "binding",
            SessionOwnerNodeGeneration: 11,
            SessionOwnerId: "owner-a",
            SessionOwnerLeaseGeneration: 13
        );

        Assert.True(binding.IsSamePhysicalSessionOwner(nodeRid, 11, sessionRid, "owner-a", 13));
        Assert.False(
            binding.IsSamePhysicalSessionOwner(
                RoutingId.From("other-node"),
                11,
                sessionRid,
                "owner-a",
                13
            )
        );
        Assert.False(binding.IsSamePhysicalSessionOwner(nodeRid, 12, sessionRid, "owner-a", 13));
        Assert.False(
            binding.IsSamePhysicalSessionOwner(
                nodeRid,
                11,
                RoutingId.From("other-session"),
                "owner-a",
                13
            )
        );
        Assert.False(binding.IsSamePhysicalSessionOwner(nodeRid, 11, sessionRid, "owner-b", 13));
        Assert.False(binding.IsSamePhysicalSessionOwner(nodeRid, 11, sessionRid, "owner-a", 14));

        var legacyIdentity = binding with { SessionOwnerId = "", SessionOwnerLeaseGeneration = 0 };
        Assert.True(
            legacyIdentity.IsSamePhysicalSessionOwner(nodeRid, 11, sessionRid, nodeRid.ToHex(), 11)
        );
        Assert.False(
            (binding with { SessionNodeRid = null }).IsSamePhysicalSessionOwner(
                nodeRid,
                11,
                sessionRid,
                "owner-a",
                13
            )
        );
    }

    [Fact]
    public void Actor_Owner_Tombstones_Are_Time_And_Count_Bounded()
    {
        var time = new ManualTimeProvider();
        var state = new ZLinkActorRuntimeState(
            "actor-bounds",
            time,
            sessionBindingTombstoneRetention: TimeSpan.FromSeconds(1),
            maxSessionBindingTombstones: 2
        );
        _ = Bind(state, "binding-a", "session-a", authorityGeneration: 1);
        _ = Bind(state, "binding-b", "session-b", authorityGeneration: 2);
        _ = Bind(state, "binding-c", "session-c", authorityGeneration: 3);
        Assert.Equal(2, state.SessionBindingTombstoneCount);

        var capacity = Assert.Throws<ZLinkFrameworkException>(() =>
            Bind(state, "binding-d", "session-d", authorityGeneration: 4)
        );
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
        ulong authorityGeneration
    )
    {
        return state.BindSession(
            RoutingId.From("session-node"),
            RoutingId.From(session),
            token,
            bindingGeneration: 1,
            objectGeneration: 7,
            authorityOwnerGeneration: authorityGeneration,
            meshName: ZLinkMeshName.FromBoundary("actors", "meshName"),
            targetNodeGeneration: 1,
            ownerLeaseGeneration: 1,
            sessionOwnerNodeGeneration: 1,
            acceptedHighWater: 0
        );
    }

    private static ZLinkActorSessionBindingTransition Replace(
        ZLinkActorRuntimeState state,
        string token,
        string session,
        ulong authorityGeneration,
        Action<ZLinkActorRuntimeState>? acceptAuthority = null
    )
    {
        return state.ReplaceSessionBinding(
            RoutingId.From("session-node"),
            RoutingId.From(session),
            token,
            bindingGeneration: 1,
            objectGeneration: 7,
            authorityOwnerGeneration: authorityGeneration,
            meshName: ZLinkMeshName.FromBoundary("actors", "meshName"),
            targetNodeGeneration: 1,
            ownerLeaseGeneration: 1,
            sessionOwnerNodeGeneration: 1,
            acceptedHighWater: 0,
            acceptAuthority: acceptAuthority
        );
    }

    private static MeshNodeStatus NodeStatus(RoutingId rid, ulong lifecycleGeneration) =>
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
            LastChangedMs: 1
        );

    private static MeshNodePeer Peer(
        RoutingId rid,
        ulong lifecycleGeneration,
        MeshPeerState state
    ) =>
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
            LastChangedMs: 1
        );
}
