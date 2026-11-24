using System.Collections.Concurrent;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed partial class RelocationRuntimeTests
{
    [Fact]
    public async Task TargetCutoverRequiresTheExactStagedRelayJournal()
    {
        var stage = CreateTargetStageForHeldJournal();
        var spot = stage.SpotParticipant;
        var relocationId = new ZLinkServiceWireCodec.RelocationWireId(1, 2);
        var coordinator = new ZLinkServiceWireCodec.RelocationCoordinatorFence(
            stage.SourceOwner.OwnerId,
            checked((ulong)stage.SourceOwner.LeaseGeneration),
            stage.SourceNodeRid,
            stage.SourceNodeLifecycleGeneration,
            "v-source"
        );
        var subject = ZLinkSpotRetireTargetRuntime.CreateCanonicalRelocationObject(
            spot.ObjectKind,
            stage.StableType,
            "room",
            spot.ObjectGeneration,
            spot.AuthorityOwnerGeneration
        );
        var data = new ZLinkServiceWireCodec.RelocationDataRecord(
            relocationId,
            stage.TargetAttemptGeneration,
            coordinator,
            1,
            subject,
            new(new byte[] { 4, 3 })
        );
        await stage.AppendCanonicalInboundDataAsync(data);
        var cutover = new ZLinkServiceWireCodec.RelocationCutoverRecord(
            relocationId,
            stage.TargetAttemptGeneration,
            coordinator,
            1,
            subject,
            1,
            ZLinkRelocationBoundaryBatch.ComputeChecksum([data.FrozenRecord.Encoded])
        );

        stage.ValidateBoundary(cutover);
        Assert.Throws<InvalidDataException>(() =>
            stage.ValidateBoundary(cutover with { BoundaryRecordCount = 2 })
        );
        var changed = data with { FrozenRecord = new(new byte[] { 4, 2 }) };
        Assert.Throws<InvalidDataException>(() =>
            stage.ValidateBoundary(
                cutover with
                {
                    BoundaryChecksumCrc32c = ZLinkRelocationBoundaryBatch.ComputeChecksum([
                        changed.FrozenRecord.Encoded,
                    ]),
                }
            )
        );
        Assert.Equal(
            new byte[] { 4, 3 },
            Assert.Single(stage.SnapshotHeldRecords()).Payload.ToArray()
        );
    }

    [Theory]
    [InlineData(StagedAuthorityPublication.Complete)]
    [InlineData(StagedAuthorityPublication.MissingMember)]
    [InlineData(StagedAuthorityPublication.WrongAttempt)]
    [InlineData(StagedAuthorityPublication.UnchangedOwnerGeneration)]
    public async Task ExactReconciliationRequiresEveryStagedAuthority(
        StagedAuthorityPublication publication
    )
    {
        var authorityReads = new ConcurrentQueue<ZLinkAuthorityKey>();
        var authority = new RecordingAuthorityStore { AuthorityReads = authorityReads };
        var provider = new ZLinkInMemoryProviderLocationStore();
        var leaseStore = new ZLinkProviderLocationRepository(provider);
        var registration = new ZLinkFrameworkRegistration();
        registration.Locations.StoreInstance = provider;
        registration.SpotNodes["mesh"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "mesh",
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectRoleSelected = true,
            Router = new ZLinkSpotRouterCapabilityRegistration
            {
                BindEndpoint = "tcp://127.0.0.1:0",
                AcquisitionMode = ZLinkPeerAcquisitionMode.Manual,
            },
        };
        var options = registration.Locations.Options;
        await using var location = new ZLinkLocationRuntime(options, leaseStore);
        Assert.True(await location.RenewOwnerLeaseOnceAsync());
        var resolver = new ZLinkStoreLocationResolvers(
            leaseStore,
            new ZLinkOwnerLeaseTracker(leaseStore, options)
        );
        await using var lifecycle = new ZLinkLocationLifecycle(location, resolver);
        using var services = new ServiceCollection()
            .AddSingleton(registration)
            .AddSingleton(lifecycle)
            .BuildServiceProvider();
        var runtime = CreateBareRuntime(services);
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            var (stage, candidate) = CreateCanonicalPublishedReconciliationFixture();
            var spot = stage.SpotParticipant;
            var actor = spot with
            {
                AuthorityKey = ZLinkActorAuthorityPayloadCodec.AuthorityKey("staged-member"),
                ObjectKind = ZLinkPlacementObjectKind.Actor,
            };
            var node = runtime.GetSpotNodeRuntime("mesh");
            stage = stage with
            {
                Node = node,
                Envelope = stage.Envelope with { Participants = [spot, actor] },
                TargetNodeLifecycleGeneration = node.Node.MeshStatus().LifecycleGeneration,
                TargetOwnerLeaseGeneration = checked((ulong)lifecycle.OwnerToken.LeaseGeneration),
                TargetAttemptGeneration = 2,
            };
            var descriptor = new ZLinkMeshNodeDescriptorKey("mesh", node.Node.RoutingId);
            var owner = lifecycle.OwnerToken;
            var template = Assert.Single(candidate.Authorities).Snapshot;
            var committed = template with
            {
                OwnerId = owner.OwnerId,
                OwnerLeaseGeneration = owner.LeaseGeneration,
                AuthorityOwnerGeneration = stage.TargetAuthorityOwnerGeneration + 1,
                Allocation = template.Allocation with
                {
                    Descriptor = descriptor,
                    DescriptorLifecycleGeneration = stage.TargetNodeLifecycleGeneration,
                },
                Relocation = template.Relocation! with
                {
                    Target = new(
                        descriptor.Rid.ToHex(),
                        stage.TargetNodeLifecycleGeneration,
                        owner.OwnerId,
                        stage.TargetOwnerLeaseGeneration,
                        stage.TargetAttemptGeneration
                    ),
                },
            };
            authority.Seed(spot.AuthorityKey, committed);
            authority.Seed(
                ZLinkActorAuthorityPayloadCodec.AuthorityKey("outside-stage"),
                committed
            );
            if (publication != StagedAuthorityPublication.MissingMember)
            {
                var member = committed with
                {
                    Allocation = committed.Allocation with { ObjectKind = actor.ObjectKind },
                };
                if (publication == StagedAuthorityPublication.WrongAttempt)
                    member = member with
                    {
                        Relocation = member.Relocation! with
                        {
                            Target = member.Relocation.Target! with { AttemptGeneration = 1 },
                        },
                    };
                if (publication == StagedAuthorityPublication.UnchangedOwnerGeneration)
                    member = member with
                    {
                        AuthorityOwnerGeneration = actor.AuthorityOwnerGeneration,
                    };
                authority.Seed(actor.AuthorityKey, member);
            }

            var target = new ZLinkSpotRetireTargetRuntime(
                services,
                runtime,
                new ZLinkFrameworkRegistration { Locations = { _repository = authority } }
            );
            var fence = new ZLinkAggregateFence(
                stage.Envelope.AggregateId,
                stage.TargetAttemptGeneration
            );
            Assert.True(target.TryTrackStage(fence, stage));

            Assert.True(
                ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(committed, out var previous)
            );
            Assert.Same(
                stage.Terminal.Task,
                target.FindTargetTerminal(previous.State, actor.AuthorityKey, out _)
            );
            Assert.Null(
                target.FindTargetTerminal(
                    previous.State with
                    {
                        TargetAttemptGeneration = stage.TargetAttemptGeneration + 1,
                    },
                    actor.AuthorityKey,
                    out _
                )
            );

            // Stop after authority confirmation and before authority normalization.
            stage.CapturedRequestDelivery = Task.FromCanceled(
                new CancellationToken(canceled: true)
            );
            if (publication == StagedAuthorityPublication.Complete)
            {
                // The live cutover owns its verified root while source completion is absent.
                stage.RememberFinalRoot("published-root", 37);
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                    target
                        .PublishInboundAsync(fence, stage.SourceNodeRid, CancellationToken.None)
                        .AsTask()
                );
                Assert.Equal(1, Volatile.Read(ref stage.AuthorityPublished));
                Assert.Equal(("published-root", 37U), stage.GetFinalRoot());
                Assert.Equal(
                    committed.AuthorityOwnerGeneration,
                    stage.TargetAuthorityOwnerGeneration
                );
                Assert.Equal(
                    committed.AuthorityOwnerGeneration,
                    stage.TargetAuthorityOwnerGenerationFor(actor)
                );
            }
            else
            {
                Assert.False(
                    await target.PublishInboundAsync(
                        fence,
                        stage.SourceNodeRid,
                        CancellationToken.None
                    )
                );
                Assert.False(stage.HasAuthorityPublication);
                Assert.Equal(0, Volatile.Read(ref stage.AuthorityPublished));
            }
            Assert.Equal(new[] { spot.AuthorityKey, actor.AuthorityKey }, authorityReads.ToArray());
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    public enum StagedAuthorityPublication
    {
        Complete,
        MissingMember,
        WrongAttempt,
        UnchangedOwnerGeneration,
    }
}
