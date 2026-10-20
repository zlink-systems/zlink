using System.Security.Cryptography;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData(false, false, false)]
    [InlineData(false, true, false)]
    [InlineData(true, false, false)]
    [InlineData(true, true, false)]
    [InlineData(false, false, true)]
    [InlineData(true, false, true)]
    public async Task Early_session_seal_failure_releases_all_coordinator_terminals(
        bool cancelSourceSeal,
        bool failAbort,
        bool bindingAbsent
    )
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var records = new ZLinkManagedMeshNode(context, "seal-terminal-records");
        using var cancellation = new CancellationTokenSource();
        var node = new EarlySealFailureNode(
            records,
            cancelSourceSeal ? cancellation : null,
            failAbort,
            bindingAbsent
        );
        IZLinkLocationRepository? authorityStore = null;
        var blobs = new SchedulerProbeRelocationStore();
        var target = new SchedulerProbeTarget(
            () => authorityStore!,
            new ZLinkProviderRelocationRepository(blobs),
            SchedulerProbePublishMode.Commit
        );
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            node,
            userSpotType: typeof(SourceLeaveProbeSpot),
            retireTarget: target,
            relocationStore: blobs,
            locationStoreWrapper: store =>
            {
                authorityStore = new ZLinkProviderLocationRepository(store);
                return store;
            }
        );
        try
        {
            var created = await runtime.GetOrCreateAsync<SourceLeaveProbeSpot>(
                "seal-terminal-spot"
            );
            var activation = GetSpotActivation(runtime, created.Spot.SpotId);
            var actor = RegisterProbeActor(runtime, actorRef);
            Assert.True(
                (await activation.JoinActorAsync(actor, ZLinkMessage.Empty, default)).Accepted
            );
            BindRemoteSession(actorRef);
            if (!cancelSourceSeal)
            {
                foreach (var actorId in new[] { "actor-b", "actor-c" })
                {
                    var participant = actorRef with { ActorId = actorId };
                    await CreateTrackedLocalActorOwnershipAsync(
                        authorityStore!,
                        runtime.LocationLifecycle!.OwnerToken,
                        runtime.LocationLifecycle.ActorOwnership,
                        participant,
                        default
                    );
                    var other = RegisterProbeActor(runtime, participant);
                    Assert.True(
                        (
                            await activation.JoinActorAsync(other, ZLinkMessage.Empty, default)
                        ).Accepted
                    );
                    BindRemoteSession(participant);
                }
            }
            runtime.Registration.SpotNodes["entry"].SpotRelocations[
                typeof(SourceLeaveProbeSpot).FullName!
            ] = new ZLinkObjectRelocationRegistration(
                typeof(SourceLeaveProbeSpot),
                new ZLinkObjectPlacementOptions(),
                ZLinkObjectRelocationRegistration.RecreatePolicy,
                null,
                null
            );
            var descriptor = (
                await authorityStore!.ListMeshNodesAsync("entry", default, default)
            ).Items.Single(item => item.Rid == node.RoutingId);
            ZLinkSpotDrainResult result = default;
            var error = await Record.ExceptionAsync(async () =>
                result = await runtime
                    .GetSpotNodeRuntime("entry")
                    .Catalog.TryRelocateForRetireAsync(
                        new(
                            ZLinkFrameworkRelocationMode.PlannedMaintenance,
                            descriptor.ApplicationVersion
                        ),
                        ZLinkSpotRelocationPhase.Aggregates,
                        cancellation.Token
                    )
            );
            if (cancelSourceSeal && !failAbort)
                Assert.IsAssignableFrom<OperationCanceledException>(error);
            else
            {
                Assert.Null(error);
                Assert.False(result.Completed);
                Assert.False(result.SourceTerminalized);
            }
            Assert.Equal(cancelSourceSeal ? 1 : 2, node.PublishedReplies);
            Assert.Equal(0, node.TerminalCount);
            Assert.Equal(1, target.AbortCalls);
            Assert.Equal(
                bindingAbsent ? 0
                    : cancelSourceSeal ? 1
                    : 2,
                node.AbortCalls
            );
        }
        finally
        {
            await runtime.StopAsync(default);
        }

        void BindRemoteSession(ZLinkBackendActorRef reference) =>
            runtime
                .GetOrCreateActorState(reference.ActorId)
                .BindSession(
                    RoutingId.From("remote-session-owner"),
                    RoutingId.From("session-" + reference.ActorId),
                    "binding-" + reference.ActorId,
                    objectGeneration: reference.Generation,
                    authorityOwnerGeneration: 1,
                    meshName: Zlink.Framework.Runtime.Identifiers.ZLinkMeshName.FromBoundary(
                        "entry",
                        "mesh"
                    ),
                    targetNodeGeneration: 1,
                    ownerLeaseGeneration: 1,
                    bindingGeneration: 7,
                    sessionOwnerNodeGeneration: 2,
                    sessionOwnerId: "remote-session-owner",
                    sessionOwnerLeaseGeneration: 1
                );
    }

    private sealed class EarlySealFailureNode(
        ZLinkManagedMeshNode records,
        CancellationTokenSource? cancelSourceSeal,
        bool failAbort,
        bool bindingAbsent
    ) : CapturingSpotNode, IZLinkBackendSessionRelocationBarrier
    {
        internal int PublishedReplies { get; private set; }
        internal int AbortCalls { get; private set; }
        internal int TerminalCount => records._sessionRelocationSealReplyTerminals.Count;

        public void SetSessionRelocationBarrierTarget(ISessionRelocationBarrierTarget target) { }

        public void CompleteSessionRelocation(Guid relocationId) =>
            records.CompleteSessionRelocation(relocationId);

        public ValueTask<ZLinkServiceWireCodec.SessionRelocationSealedRecord> SealSessionRelocationAsync(
            RoutingId sessionOwnerNodeRid,
            ZLinkServiceWireCodec.SessionRelocationSealRecord seal,
            TimeSpan timeout,
            CancellationToken cancellationToken
        )
        {
            if (PublishedReplies == (cancelSourceSeal is null ? 2 : 1))
                return ValueTask.FromException<ZLinkServiceWireCodec.SessionRelocationSealedRecord>(
                    new InvalidOperationException("participant refused seal")
                );
            var reply = new ZLinkServiceWireCodec.SessionRelocationSealedRecord(
                seal.RelocationId,
                seal.Coordinator,
                seal.Actor,
                seal.Session
            )
            {
                SealResult = bindingAbsent
                    ? ServiceWirePilotCodec.SessionSealResult.BindingAbsent
                    : ServiceWirePilotCodec.SessionSealResult.Sealed,
            };
            var terminal = new ZLinkManagedMeshNode.PendingSessionRelocationSeal(
                SHA256.HashData(ZLinkServiceWireCodec.EncodeSessionRelocationSeal(seal)),
                seal
            );
            Assert.True(terminal.TryComplete(reply));
            var key = ZLinkManagedMeshNode.PendingSessionRelocationKey.Create(
                sessionOwnerNodeRid,
                seal
            );
            // Model the remote command 43 terminal at the backend boundary;
            // production CompleteSessionRelocation owns its actual removal.
            Assert.True(records._sessionRelocationSealReplyTerminals.TryAdd(key, terminal));
            PublishedReplies++;
            Assert.Equal(PublishedReplies, TerminalCount);
            cancelSourceSeal?.Cancel();
            return ValueTask.FromResult(reply);
        }

        public ValueTask RouteSessionRelocationAsync(
            RoutingId owner,
            ZLinkServiceWireCodec.SessionRelocationRouteRecord route,
            CancellationToken cancellationToken
        )
        {
            AbortCalls++;
            return failAbort
                ? ValueTask.FromException(new IOException("session route abort failed"))
                : ValueTask.CompletedTask;
        }
    }
}
