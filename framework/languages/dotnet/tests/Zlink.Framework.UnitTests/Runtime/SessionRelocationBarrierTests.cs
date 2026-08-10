using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests;

public sealed class SessionRelocationBarrierTests
{
    [Fact]
    public async Task Abort_Echoes_The_Sealed_HighWater_And_Releases_The_Waiter()
    {
        var fixture = CreateFixture();
        for (var sequence = 1UL; sequence <= 41; sequence++)
        {
            Assert.True(fixture.Table.TryAccept(
                fixture.ActorId,
                fixture.BindingToken,
                out var accepted));
            Assert.Equal(sequence, accepted);
            fixture.Table.CompleteAccepted(
                fixture.ActorId,
                fixture.BindingToken);
        }

        var seal = CreateSeal(
            fixture,
            new ZLinkServiceWireCodec.RelocationWireId(1, 1),
            fixture.SourceNode,
            fixture.SourceNodeGeneration,
            fixture.SourceAuthority,
            fixture.SourceOwnerLease);
        var sealedRecord = await fixture.Table.SealCanonicalRouteAsync(
            seal,
            CancellationToken.None);
        Assert.Equal(41UL, sealedRecord.LastAcceptedSessionSequence);
        Assert.False(fixture.Table.TryAccept(
            fixture.ActorId,
            fixture.BindingToken,
            out var heldHighWater));
        Assert.Equal(41UL, heldHighWater);

        var waiter = fixture.Table.WaitForRouteAvailableAsync(
                fixture.ActorId,
                fixture.BindingToken,
                CancellationToken.None)
            .AsTask();
        Assert.False(waiter.IsCompleted);
        var abort = CreateAbort(seal);
        var authenticatedSource = Authenticate(
            fixture.SourceNode,
            fixture.SourceNodeGeneration,
            fixture.SourceAuthority,
            fixture.SourceOwnerLease);
        var routed = fixture.Table.RouteCanonical(abort, authenticatedSource);

        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
            routed.Result);
        Assert.Equal(41UL, routed.LastAcceptedSessionSequence);
        Assert.True(await waiter);
        Assert.True(ZLinkManagedMeshNode.IsExactSessionRelocationRouteResponse(
            abort,
            sealedRecord.LastAcceptedSessionSequence,
            routed));
        Assert.False(ZLinkManagedMeshNode.IsExactSessionRelocationRouteResponse(
            abort,
            sealedRecord.LastAcceptedSessionSequence,
            routed with { LastAcceptedSessionSequence = 40 }));

        var duplicate = fixture.Table.RouteCanonical(abort, authenticatedSource);
        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.AlreadyApplied,
            duplicate.Result);
        Assert.Equal(41UL, duplicate.LastAcceptedSessionSequence);
        Assert.Equal(
            sealedRecord,
            await fixture.Table.SealCanonicalRouteAsync(
                seal,
                CancellationToken.None));
        Assert.Throws<InvalidDataException>(() =>
            fixture.Table.RouteCanonical(
                abort with
                {
                    Route = ZLinkServiceWireCodec
                        .SessionRelocationRouteUpdateRecord.Abort(
                            fixture.SourceAuthority + 1)
                },
                authenticatedSource));

        var noSeal = CreateAbort(seal with
        {
            RelocationId = new ZLinkServiceWireCodec.RelocationWireId(1, 2)
        });
        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.Stale,
            fixture.Table.RouteCanonical(noSeal, authenticatedSource).Result);
    }

    [Fact]
    public async Task Consecutive_Relocations_Use_Each_New_Target_Lease_As_The_Next_Seal_Fence()
    {
        var fixture = CreateFixture();
        var firstSeal = CreateSeal(
            fixture,
            new ZLinkServiceWireCodec.RelocationWireId(2, 1),
            fixture.SourceNode,
            fixture.SourceNodeGeneration,
            fixture.SourceAuthority,
            fixture.SourceOwnerLease);
        _ = await fixture.Table.SealCanonicalRouteAsync(
            firstSeal,
            CancellationToken.None);
        var targetOne = RoutingId.From("target-one");
        var firstCommit = CreateCommit(
            firstSeal,
            targetOne,
            targetNodeGeneration: 3,
            targetAuthority: 12,
            replayedHighWater: 0);
        var firstAuthentication = Authenticate(
            targetOne,
            nodeGeneration: 3,
            authority: 12,
            ownerLease: 101);
        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
            fixture.Table.RouteCanonical(
                firstCommit,
                firstAuthentication).Result);
        Assert.True(fixture.Table.TryGet(
            fixture.ActorId,
            fixture.BindingToken,
            out ZLinkSessionBindingEntry afterFirst));
        Assert.Equal(targetOne, afterFirst.Route.Ref.NodeRid);
        Assert.Equal(101UL, afterFirst.OwnerLeaseGeneration);

        var secondSeal = CreateSeal(
            fixture,
            new ZLinkServiceWireCodec.RelocationWireId(2, 2),
            targetOne,
            sourceNodeGeneration: 3,
            sourceAuthority: 12,
            sourceOwnerLease: afterFirst.OwnerLeaseGeneration);
        Assert.Equal(101UL, secondSeal.Actor.OwnerLeaseGeneration);
        _ = await fixture.Table.SealCanonicalRouteAsync(
            secondSeal,
            CancellationToken.None);
        var waiter = fixture.Table.WaitForRouteAvailableAsync(
                fixture.ActorId,
                fixture.BindingToken,
                CancellationToken.None)
            .AsTask();
        var targetTwo = RoutingId.From("target-two");
        var secondCommit = CreateCommit(
            secondSeal,
            targetTwo,
            targetNodeGeneration: 4,
            targetAuthority: 13,
            replayedHighWater: 0);
        var secondAuthentication = Authenticate(
            targetTwo,
            nodeGeneration: 4,
            authority: 13,
            ownerLease: 202);
        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
            fixture.Table.RouteCanonical(
                secondCommit,
                secondAuthentication).Result);
        Assert.True(await waiter);
        Assert.True(fixture.Table.TryGet(
            fixture.ActorId,
            fixture.BindingToken,
            out ZLinkSessionBindingEntry afterSecond));
        Assert.Equal(targetTwo, afterSecond.Route.Ref.NodeRid);
        Assert.Equal(202UL, afterSecond.OwnerLeaseGeneration);

        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.AlreadyApplied,
            fixture.Table.RouteCanonical(
                firstCommit,
                firstAuthentication).Result);
        Assert.True(fixture.Table.TryGet(
            fixture.ActorId,
            fixture.BindingToken,
            out ZLinkSessionBindingEntry retained));
        Assert.Equal(targetTwo, retained.Route.Ref.NodeRid);
        Assert.Equal(202UL, retained.OwnerLeaseGeneration);
    }

    [Fact]
    public void Closed_Binding_Does_Not_Echo_Requested_Replay_As_Owner_HighWater()
    {
        var fixture = CreateFixture();
        var seal = CreateSeal(
            fixture,
            new ZLinkServiceWireCodec.RelocationWireId(2, 3),
            fixture.SourceNode,
            fixture.SourceNodeGeneration,
            fixture.SourceAuthority,
            fixture.SourceOwnerLease);
        var commit = CreateCommit(
            seal,
            RoutingId.From("closed-target"),
            targetNodeGeneration: 9,
            targetAuthority: 21,
            replayedHighWater: 999);
        var closedTable = new ZLinkSessionActorBindingTable(
            TimeSpan.FromMinutes(1));

        var response = closedTable.RouteCanonical(
            commit,
            Authenticate(
                commit.Route.TargetNodeRid,
                commit.Route.TargetNodeGeneration,
                commit.Route.TargetAuthorityOwnerGeneration,
                ownerLease: 301));

        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult
                .SessionOrBindingClosed,
            response.Result);
        Assert.Equal(0UL, response.LastAcceptedSessionSequence);
        Assert.Equal(21UL, response.CurrentAuthorityOwnerGeneration);
    }

    [Fact]
    public void Applied_And_AlreadyApplied_Are_The_Same_Idempotent_Route_Terminal()
    {
        var fixture = CreateFixture();
        var seal = CreateSeal(
            fixture,
            new ZLinkServiceWireCodec.RelocationWireId(2, 4),
            fixture.SourceNode,
            fixture.SourceNodeGeneration,
            fixture.SourceAuthority,
            fixture.SourceOwnerLease);
        var commit = CreateCommit(
            seal,
            RoutingId.From("terminal-target"),
            targetNodeGeneration: 9,
            targetAuthority: 21,
            replayedHighWater: 41);
        var applied = new ZLinkServiceWireCodec.SessionRelocationRoutedRecord(
            commit.RelocationId,
            commit.Coordinator,
            commit.Actor,
            commit.Session,
            commit.Route.Action,
            ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
            commit.Route.TargetAuthorityOwnerGeneration,
            commit.Route.ReplayedHighWater);
        var alreadyApplied = applied with
        {
            Result = ZLinkServiceWireCodec.SessionRelocationRouteResult
                .AlreadyApplied
        };

        Assert.True(
            ZLinkManagedMeshNode.IsEquivalentSessionRelocationRouteTerminal(
                applied,
                alreadyApplied));
        Assert.True(
            ZLinkManagedMeshNode.IsEquivalentSessionRelocationRouteTerminal(
                alreadyApplied,
                applied));
        Assert.False(
            ZLinkManagedMeshNode.IsEquivalentSessionRelocationRouteTerminal(
                applied,
                alreadyApplied with
                {
                    LastAcceptedSessionSequence = 40
                }));
        Assert.False(
            ZLinkManagedMeshNode.IsEquivalentSessionRelocationRouteTerminal(
                applied with
                {
                    Result = ZLinkServiceWireCodec
                        .SessionRelocationRouteResult.Stale
                },
                applied with
                {
                    Result = ZLinkServiceWireCodec
                        .SessionRelocationRouteResult.SessionOrBindingClosed
                }));
    }

    [Fact]
    public void Source_Rollback_Paths_Retain_The_Original_Coordinator_Context()
    {
        var state = new ZLinkActorRuntimeState("actor-rollback");
        var sourceNode = RoutingId.From("source-rollback");
        var session = new ZLinkActorBoundSession(
            RoutingId.From("session-owner"),
            RoutingId.From("session-rollback"),
            "binding-rollback",
            BindingGeneration: 6,
            ObjectGeneration: 5,
            AuthorityOwnerGeneration: 11,
            MeshName: ZLinkMeshName.FromBoundary("actors", "meshName"),
            TargetNodeGeneration: 2,
            OwnerLeaseGeneration: 17,
            SessionOwnerNodeGeneration: 7,
            AcceptedHighWater: 41,
            SessionOwnerId: "session-owner",
            SessionOwnerLeaseGeneration: 8);
        var relocationId = Guid.ParseExact(
            "00112233445566778899aabbccddeeff",
            "N");
        var original = ZLinkSessionRelocationContext.Create(
            relocationId,
            "source-owner",
            17,
            sourceNode,
            2,
            "store-v17");

        Assert.Equal(0x0011223344556677UL, original.RelocationId.High);
        Assert.Equal(0x8899aabbccddeeffUL, original.RelocationId.Low);
        foreach (var rollbackPath in new[]
                 {
                     "remote-join-failure",
                     "standalone-rollback",
                     "spot-retire-rollback"
                 })
        {
            state.RememberSourceSessionRelocation(rollbackPath, original);
            Assert.True(state.TryGetSourceSessionRelocation(
                rollbackPath,
                out var retained));
            var abort = ZLinkSessionRelocationWire.CreateAbort(
                state.ActorId,
                session,
                retained);
            Assert.Equal(original.RelocationId, abort.RelocationId);
            Assert.Equal(original.Coordinator, abort.Coordinator);
            Assert.Equal(1, abort.SenderRole);
            Assert.Equal(11UL, abort.Route.CurrentAuthorityOwnerGeneration);
            Assert.Equal(41UL, session.AcceptedHighWater);
            state.ForgetSourceSessionRelocation(rollbackPath);
        }

        state.RememberSourceSessionRelocation("conflict", original);
        Assert.Throws<ZLinkFrameworkException>(() =>
            state.RememberSourceSessionRelocation(
                "conflict",
                original with
                {
                    Coordinator = original.Coordinator with
                    {
                        ExpectedAuthorityStoreVersion = "different"
                    }
                }));
    }

    [Fact]
    public async Task Managed_Transport_Retries_Commands_42_And_44_With_Exact_Target_Authority()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewManagedNode(context, "barrier-source");
        await using var owner = NewManagedNode(context, "barrier-owner");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://barrier-source-{suffix}";
        var ownerEndpoint = $"inproc://barrier-owner-{suffix}";
        source.SetBind(sourceEndpoint);
        owner.SetBind(ownerEndpoint);
        source.ConnectPeer(ownerEndpoint, owner.RoutingId);
        owner.ConnectPeer(sourceEndpoint, source.RoutingId);
        var target = new DelayedBarrierTarget();
        owner.SetSessionRelocationBarrierTarget(target);
        source.Start();
        owner.Start();
        await WaitUntilAsync(() =>
            source.Status().AdmittedPeerCount == 1
            && owner.Status().AdmittedPeerCount == 1);

        var sourceGeneration = source.Status().LifecycleGeneration;
        var ownerGeneration = owner.Status().LifecycleGeneration;
        var actor = new ZLinkServiceWireCodec.SessionActorIdentityRecord(
            "actor-transport",
            5);
        var session = new ZLinkServiceWireCodec.SessionOwnerFenceRecord(
            owner.RoutingId,
            ownerGeneration,
            "session-owner",
            8,
            RoutingId.From("session-transport"),
            6);
        var seal = new ZLinkServiceWireCodec.SessionRelocationSealRecord(
            new ZLinkServiceWireCodec.RelocationWireId(3, 1),
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "source-owner",
                17,
                source.RoutingId,
                sourceGeneration,
                "store-v17"),
            1,
            new ZLinkServiceWireCodec.SessionActorRouteFenceRecord(
                actor,
                source.RoutingId,
                sourceGeneration,
                11,
                17),
            session);

        var sealedRecord = await source.SealSessionRelocationAsync(
            owner.RoutingId,
            seal,
            TimeSpan.FromSeconds(30),
            CancellationToken.None);
        Assert.Equal(41UL, sealedRecord.LastAcceptedSessionSequence);
        Assert.True(Volatile.Read(ref target.SealCalls) >= 2);

        owner.ObserveActorAuthority(
            new ActorRef(
                actor.ActorId,
                actor.ObjectGeneration,
                "mesh",
                source.RoutingId),
            sourceGeneration,
            authorityOwnerGeneration: 12,
            ownerLeaseGeneration: 101);
        var route = new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
            seal.RelocationId,
            seal.Coordinator,
            2,
            actor,
            session,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                11,
                12,
                source.RoutingId,
                sourceGeneration,
                sealedRecord.LastAcceptedSessionSequence));
        var routed = await source.RouteSessionRelocationAsync(
            owner.RoutingId,
            route,
            sealedRecord.LastAcceptedSessionSequence,
            TimeSpan.FromSeconds(30),
            CancellationToken.None);

        Assert.Equal(
            ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
            routed.Result);
        Assert.Equal(41UL, routed.LastAcceptedSessionSequence);
        Assert.True(Volatile.Read(ref target.RouteCalls) >= 2);
        Assert.Equal(source.RoutingId, target.AuthenticatedRoute.NodeRid);
        Assert.Equal(sourceGeneration,
            target.AuthenticatedRoute.NodeGeneration);
        Assert.Equal(12UL,
            target.AuthenticatedRoute.AuthorityOwnerGeneration);
        Assert.Equal(101UL,
            target.AuthenticatedRoute.OwnerLeaseGeneration);
    }

    private static Fixture CreateFixture()
    {
        const string actorId = "actor-relocation";
        const string bindingToken = "binding-relocation";
        const string meshName = "actors";
        var sourceNode = RoutingId.From("source-node");
        var sessionOwnerNode = RoutingId.From("session-owner-node");
        var sessionRid = RoutingId.From("session-rid");
        var table = new ZLinkSessionActorBindingTable(TimeSpan.FromMinutes(1));
        var route = ZLinkSessionBindingRoute.Create(
            new ActorRef(actorId, 5, meshName, sourceNode),
            meshName,
            targetNodeGeneration: 2,
            authorityOwnerGeneration: 11,
            ownerLeaseGeneration: 17);
        var actor = new ZLinkSessionActor(
            null!,
            actorId,
            sessionRid,
            bindingToken);
        table.Bind(
            ZLinkActorId.FromBoundary(actorId, nameof(actorId)),
            null!,
            bindingToken,
            actor,
            bindingGeneration: 6,
            route,
            sessionOwnerNodeGeneration: 7,
            sessionOwnerNode,
            sessionOwnerId: "session-owner",
            sessionOwnerLeaseGeneration: 8);
        return new Fixture(
            table,
            actorId,
            bindingToken,
            meshName,
            sourceNode,
            SourceNodeGeneration: 2,
            SourceAuthority: 11,
            SourceOwnerLease: 17,
            sessionOwnerNode,
            SessionOwnerNodeGeneration: 7,
            SessionOwnerLease: 8,
            sessionRid,
            BindingGeneration: 6,
            ObjectGeneration: 5);
    }

    private static ZLinkServiceWireCodec.SessionRelocationSealRecord CreateSeal(
        Fixture fixture,
        ZLinkServiceWireCodec.RelocationWireId relocationId,
        RoutingId sourceNode,
        ulong sourceNodeGeneration,
        ulong sourceAuthority,
        ulong sourceOwnerLease) =>
        new(
            relocationId,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                "coordinator",
                sourceOwnerLease,
                sourceNode,
                sourceNodeGeneration,
                $"store-{relocationId.High}-{relocationId.Low}"),
            1,
            new ZLinkServiceWireCodec.SessionActorRouteFenceRecord(
                new ZLinkServiceWireCodec.SessionActorIdentityRecord(
                    fixture.ActorId,
                    fixture.ObjectGeneration),
                sourceNode,
                sourceNodeGeneration,
                sourceAuthority,
                sourceOwnerLease),
            new ZLinkServiceWireCodec.SessionOwnerFenceRecord(
                fixture.SessionOwnerNode,
                fixture.SessionOwnerNodeGeneration,
                "session-owner",
                fixture.SessionOwnerLease,
                fixture.SessionRid,
                fixture.BindingGeneration));

    private static ZLinkServiceWireCodec.SessionRelocationRouteRecord
        CreateAbort(
            ZLinkServiceWireCodec.SessionRelocationSealRecord seal) =>
        new(
            seal.RelocationId,
            seal.Coordinator,
            1,
            seal.Actor.Actor,
            seal.Session,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Abort(
                seal.Actor.AuthorityOwnerGeneration));

    private static ZLinkServiceWireCodec.SessionRelocationRouteRecord
        CreateCommit(
            ZLinkServiceWireCodec.SessionRelocationSealRecord seal,
            RoutingId targetNode,
            ulong targetNodeGeneration,
            ulong targetAuthority,
            ulong replayedHighWater) =>
        new(
            seal.RelocationId,
            seal.Coordinator,
            2,
            seal.Actor.Actor,
            seal.Session,
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                seal.Actor.AuthorityOwnerGeneration,
                targetAuthority,
                targetNode,
                targetNodeGeneration,
                replayedHighWater));

    private static ZLinkSessionRelocationAuthenticatedRoute Authenticate(
        RoutingId node,
        ulong nodeGeneration,
        ulong authority,
        ulong ownerLease) =>
        new(node, nodeGeneration, "actors", authority, ownerLease);

    private static ZLinkManagedMeshNode NewManagedNode(
        Systems.Zlink.IContext context,
        string name)
    {
        var node = new ZLinkManagedMeshNode(context, "mesh");
        node.SetRoutingId(RoutingId.From(name));
        node.AddChannel("mesh");
        return node;
    }

    private static async Task WaitUntilAsync(Func<bool> predicate)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(30);
        while (!predicate())
        {
            if (DateTime.UtcNow >= deadline)
                throw new TimeoutException();
            await Task.Delay(10);
        }
    }

    private sealed class DelayedBarrierTarget : ISessionRelocationBarrierTarget
    {
        internal int SealCalls;
        internal int RouteCalls;
        internal ZLinkSessionRelocationAuthenticatedRoute AuthenticatedRoute;

        public async ValueTask<
            ZLinkServiceWireCodec.SessionRelocationSealedRecord> SealAsync(
                ZLinkServiceWireCodec.SessionRelocationSealRecord seal,
                RoutingId authenticatedSourceNodeRid,
                CancellationToken cancellationToken)
        {
            Assert.Equal(seal.Coordinator.NodeRid,
                authenticatedSourceNodeRid);
            Interlocked.Increment(ref SealCalls);
            await Task.Delay(250, cancellationToken);
            return new ZLinkServiceWireCodec.SessionRelocationSealedRecord(
                seal.RelocationId,
                seal.Coordinator,
                seal.Actor,
                seal.Session,
                41);
        }

        public async ValueTask<
            ZLinkServiceWireCodec.SessionRelocationRoutedRecord> RouteAsync(
                ZLinkServiceWireCodec.SessionRelocationRouteRecord route,
                ZLinkSessionRelocationAuthenticatedRoute authenticatedRoute,
                CancellationToken cancellationToken)
        {
            AuthenticatedRoute = authenticatedRoute;
            Interlocked.Increment(ref RouteCalls);
            await Task.Delay(250, cancellationToken);
            return new ZLinkServiceWireCodec.SessionRelocationRoutedRecord(
                route.RelocationId,
                route.Coordinator,
                route.Actor,
                route.Session,
                route.Route.Action,
                ZLinkServiceWireCodec.SessionRelocationRouteResult.Applied,
                route.Route.TargetAuthorityOwnerGeneration,
                route.Route.ReplayedHighWater);
        }
    }

    private sealed record Fixture(
        ZLinkSessionActorBindingTable Table,
        string ActorId,
        string BindingToken,
        string MeshName,
        RoutingId SourceNode,
        ulong SourceNodeGeneration,
        ulong SourceAuthority,
        ulong SourceOwnerLease,
        RoutingId SessionOwnerNode,
        ulong SessionOwnerNodeGeneration,
        ulong SessionOwnerLease,
        RoutingId SessionRid,
        ulong BindingGeneration,
        ulong ObjectGeneration);
}
