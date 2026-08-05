using System.Buffers.Binary;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Streams;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class StandaloneActorRelocationRuntimeTests
{
    [Fact]
    public void Reconciliation_accepts_only_the_exact_normalized_target_authority()
    {
        var targetRid = RoutingId.From("normalized-target");
        var target = new ZLinkActorAuthorityPayload(
            ZLinkActorAuthorityState.Ready,
            "player",
            "actor-1",
            "entry-target",
            9,
            ZLinkSpotKind.Entry,
            "target-owner",
            7,
            "mesh",
            targetRid,
            9);
        var allocation = new ZLinkPlacementAllocation(
            ZLinkPlacementAllocationState.Active,
            ZLinkPlacementObjectKind.Actor,
            target.StableType,
            new ZLinkMeshNodeDescriptorKey(target.MeshName, targetRid),
            target.NodeGeneration,
            new ZLinkCapacityVector(1, 0, null));
        var normalized = new ZLinkAuthoritySnapshot(
            "version-1",
            ZLinkActorAuthorityPayloadCodec.Encode(target),
            12,
            31,
            target.OwnerId,
            checked((long)target.OwnerLeaseGeneration),
            allocation,
            null,
            DateTimeOffset.UtcNow);

        Assert.True(ZLinkStandaloneActorRelocationRuntime.IsExactSteadyTarget(
            normalized,
            objectGeneration: 12,
            authorityOwnerGeneration: 31,
            target));
        Assert.False(ZLinkStandaloneActorRelocationRuntime.IsExactSteadyTarget(
            normalized with { AuthorityOwnerGeneration = 32 },
            objectGeneration: 12,
            authorityOwnerGeneration: 31,
            target));
        Assert.False(ZLinkStandaloneActorRelocationRuntime.IsExactSteadyTarget(
            normalized with { OwnerId = "stale-owner" },
            objectGeneration: 12,
            authorityOwnerGeneration: 31,
            target));
        Assert.False(ZLinkStandaloneActorRelocationRuntime.IsExactSteadyTarget(
            normalized with
            {
                Payload = ZLinkActorAuthorityPayloadCodec.Encode(
                    target with { NodeGeneration = 10 })
            },
            objectGeneration: 12,
            authorityOwnerGeneration: 31,
            target));
    }

    [Fact]
    public async Task Production_command35_normalizes_steady_before_applied_marker_and_is_idempotent()
    {
        HostedRecoveryActorFactory.Reset();
        var suffix = Guid.NewGuid().ToString("N");
        var endpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        var store = new ZLinkInMemoryLocationStore();
        var relocationStore = new ProgressRelocationStore();
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.AddLocationStore(store);
            options.AddRelocationStore(relocationStore);
            options.AddRouteMesh("mesh")
                .Listen(endpoint)
                .SetRoutingIdPrefix($"completion-target-{suffix}")
                .SetActorLimit(100)
                .Objects()
                .Server()
                .AddActorFactory<HostedRecoveryActor, HostedRecoveryActorFactory>(
                    "player", factory => factory.RecreateOnRelocation());
        });
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();
        var autoConnect = provider.GetRequiredService<ZLinkLocationAutoConnectHost>();
        var replacementRid = runtime.PrepareLocationNodeRoutingId();
        await locations.StartAsync(replacementRid, CancellationToken.None);
        await runtime.StartAsync(CancellationToken.None);
        await autoConnect.StartAsync(
            await runtime.GetStartedStateForRoutingAsync(CancellationToken.None),
            CancellationToken.None);
        try
        {
            replacementRid = runtime.GetSpotNodeRuntime("mesh").Node.RoutingId;
            var replacement = Assert.Single(
                (await store.ListMeshNodesAsync("mesh", new ZLinkPageRequest(100)))
                .Items,
                descriptor => descriptor.Rid == replacementRid);
            var source = await PublishActorNodeAsync(
                store,
                $"completion-source-owner-{suffix}",
                RoutingId.From($"completion-source-{suffix}"),
                lifecycleGeneration: 7);
            var actorId = $"completion-actor-{suffix}";
            var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
            var intent = System.Text.Encoding.UTF8.GetBytes($"create:{actorId}");
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await store.ReserveAsync(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.Actor,
                        key,
                        "player",
                        $"inline:{actorId}",
                        System.Security.Cryptography.SHA256.HashData(intent),
                        intent.Length,
                        new ZLinkMeshNodeDescriptorKey("mesh", source.Descriptor.Rid),
                        source.Descriptor.LifecycleGeneration,
                        source.Owner,
                        new byte[] { 1 },
                        new ZLinkCapacityVector(1, 0, null))));
            var sourceActor = new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Ready,
                "player",
                actorId,
                source.Descriptor.EntrySpotId!,
                source.Descriptor.LifecycleGeneration,
                ZLinkSpotKind.Entry,
                source.Owner.OwnerId,
                checked((ulong)source.Owner.LeaseGeneration),
                "mesh",
                source.Descriptor.Rid,
                source.Descriptor.LifecycleGeneration);
            var sourceSnapshot = Assert.IsType<ZLinkObjectCommitResult.Committed>(
                await store.CommitAsync(
                    reserved.Reservation,
                    ZLinkActorAuthorityPayloadCodec.Encode(sourceActor)))
                .Snapshot;
            var relocationId = Guid.NewGuid();
            var boundRoute = default(ZLinkRemoteActorBoundSessionRoute);
            var envelope = ZLinkCanonicalActorRelocationWriter.CreateInitial(
                ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                    sourceSnapshot,
                    sourceActor,
                    replacement,
                    relocationId,
                    ReadOnlyMemory<byte>.Empty,
                    [],
                    boundRoute),
                applicationVersion: 1);
            var stored = await ZLinkRelocationTreeStore.PutAsync(
                relocationStore,
                envelope,
                TimeSpan.FromHours(24),
                CancellationToken.None);
            var rawPrepare = ZLinkStandaloneActorRelocationRuntime.CreatePrepare(
                sourceSnapshot,
                sourceActor,
                replacement,
                envelope,
                stored.Root,
                boundSession: null,
                applicationVersion: 1);
            var prepare = rawPrepare with
            {
                Object = rawPrepare.Object with
                {
                    StableType = sourceActor.StableType
                }
            };
            var capacity = Assert.IsType<
                ZLinkRelocationCapacityReserveResult.Reserved>(
                await store.ReserveRelocationCapacityAsync(
                    new ZLinkRelocationCapacityReservationRequest(
                        Guid.ParseExact(
                            ZLinkCanonicalRelocationReservationOwner
                                .CapacityFence(prepare.RelocationId, 1).Value,
                            "N"),
                        key,
                        sourceSnapshot.StoreVersion,
                        ZLinkPlacementObjectKind.Actor,
                        "player",
                        sourceSnapshot.Allocation.Descriptor,
                        sourceSnapshot.Allocation.DescriptorLifecycleGeneration,
                        source.Owner,
                        new ZLinkMeshNodeDescriptorKey("mesh", replacement.Rid),
                        replacement.LifecycleGeneration,
                        new ZLinkLocationOwnerToken(
                            replacement.OwnerId,
                            replacement.LeaseGeneration),
                        new ZLinkCapacityVector(1, 0, null))));
            var targetAuthorityOwnerGeneration =
                capacity.TargetAuthorityOwnerGeneration;
            var standalone = runtime.StandaloneActorRelocationRuntime;
            await standalone.StageTargetAsync(
                prepare,
                source.Descriptor.Rid,
                targetAuthorityOwnerGeneration,
                CancellationToken.None);

            var targetActor = sourceActor with
            {
                CurrentSpotId = replacement.EntrySpotId!,
                CurrentSpotGeneration = replacement.LifecycleGeneration,
                OwnerId = replacement.OwnerId,
                OwnerLeaseGeneration = checked((ulong)replacement.LeaseGeneration),
                NodeRid = replacement.Rid,
                NodeGeneration = replacement.LifecycleGeneration
            };
            var id = relocationId.ToByteArray(bigEndian: true);
            var canonicalPayload = ZLinkCanonicalRelocationAuthorityStateCodec
                .ReplaceRelocationState(
                    ZLinkActorAuthorityPayloadCodec.Encode(targetActor),
                    new ZLinkCanonicalRelocationAuthorityState(
                        BinaryPrimitives.ReadUInt64BigEndian(id),
                        BinaryPrimitives.ReadUInt64BigEndian(id.AsSpan(8)),
                        1,
                        source.Descriptor.Rid.ToHex(),
                        source.Descriptor.LifecycleGeneration,
                        source.Owner.OwnerId,
                        checked((ulong)source.Owner.LeaseGeneration),
                        replacement.Rid.ToHex(),
                        replacement.LifecycleGeneration,
                        replacement.OwnerId,
                        checked((ulong)replacement.LeaseGeneration),
                        targetAuthorityOwnerGeneration,
                        replacement.OwnerId,
                        checked((ulong)replacement.LeaseGeneration),
                        replacement.Rid.ToHex(),
                        replacement.LifecycleGeneration,
                        (byte)ZLinkStandaloneActorCanonicalPhase.Committed,
                        stored.Root.Reference,
                        stored.Root.ChecksumCrc32c,
                        1,
                        0),
                    envelope);
            _ = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                await store.CompareExchangeAuthorityAsync(
                    key,
                    sourceSnapshot.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        canonicalPayload,
                        ZLinkAuthorityGenerationTransition.NewOwner,
                        new ZLinkLocationOwnerToken(
                            replacement.OwnerId,
                            replacement.LeaseGeneration),
                        capacity.Fence)));
            standalone.MarkAuthorityPublished(prepare);
            await standalone.ActivatePublishedTargetAsync(
                prepare,
                CancellationToken.None);

            var targetOwner = new ZLinkLocationOwnerToken(
                replacement.OwnerId,
                replacement.LeaseGeneration);
            var coordinator =
                new ZLinkStandaloneActorRelocationProgressCoordinator(
                    store,
                    relocationStore,
                    new ZLinkStandaloneActorRelocationTargetFence(
                        relocationId,
                        1,
                        replacement.Rid,
                        replacement.LifecycleGeneration,
                        targetOwner));
            _ = await coordinator.AdvanceCanonicalPhaseAsync(
                envelope,
                ZLinkStandaloneActorCanonicalPhase.Activated,
                ZLinkStandaloneActorCanonicalPhase.Cleaning,
                targetOwner,
                CancellationToken.None);
            _ = await coordinator.PublishAdmissionReadyAuthorityAsync(
                envelope,
                targetOwner,
                CancellationToken.None);

            var complete = new ZLinkServiceWireCodec.RelocationCompleteRecord(
                prepare.RelocationId,
                prepare.TargetAttemptGeneration,
                prepare.Coordinator,
                1,
                new ZLinkServiceWireCodec.RequestSourceFence(
                    source.Owner.OwnerId,
                    checked((ulong)source.Owner.LeaseGeneration),
                    source.Descriptor.Rid,
                    source.Descriptor.LifecycleGeneration),
                1);
            var prefix =
                $"zlink-completion-{prepare.RelocationId.High:x16}"
                + $"-{prepare.RelocationId.Low:x16}"
                + $"-{prepare.TargetAttemptGeneration:x16}";
            await ((IZLinkRelocationRepository)relocationStore)
                .PutRelocationAtAsync(
                $"{prefix}-terminal",
                ZLinkCanonicalRelocationReservationOwner.EncodeTerminalReceipt(
                    prepare,
                    replacement.LifecycleGeneration,
                    targetAuthorityOwnerGeneration),
                TimeSpan.FromHours(24));
            await using var owner = new ZLinkCanonicalRelocationReservationOwner(
                store,
                runtime.RelocationPermits,
                "mesh",
                replacement.Rid,
                replacement.LifecycleGeneration,
                TimeSpan.FromSeconds(5),
                relocationStore: relocationStore,
                standaloneActorRuntime: standalone);

            Assert.False(relocationStore.Payloads.ContainsKey(
                $"{prefix}-applied"));
            var beforeCompletion =
                Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(key));
            Assert.True(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                beforeCompletion.Snapshot.Payload.Span,
                out var pendingCompletion));
            Assert.Equal(
                (byte)ZLinkStandaloneActorCanonicalPhase.Cleaning,
                pendingCompletion.Phase);
            Assert.Equal(1, pendingCompletion.SourceCleanupState);

            await owner.CompleteAsync(
                complete,
                source.Descriptor.Rid,
                CancellationToken.None);

            Assert.True(relocationStore.Payloads.ContainsKey(
                $"{prefix}-applied"));
            Assert.True(runtime.TryGetCreatedActorState(
                actorId,
                out var completedActorState));
            Assert.False(completedActorState.Handoff.BlocksLocalDispatch);
            Assert.False(completedActorState.TryGetBoundSession(out _));
            Assert.DoesNotContain(
                stored.Root.Reference,
                relocationStore.Payloads.Keys);
            var steadyRead = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(key));
            Assert.False(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                steadyRead.Snapshot.Payload.Span,
                out _));
            Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecode(
                steadyRead.Snapshot.Payload.Span,
                out var steady));
            Assert.Equal(targetActor, steady);

            await owner.CompleteAsync(
                complete,
                source.Descriptor.Rid,
                CancellationToken.None);
            Assert.True(relocationStore.Payloads.ContainsKey(
                $"{prefix}-applied"));
            Assert.True(runtime.TryGetCreatedActorState(
                actorId,
                out var idempotentActorState));
            Assert.False(idempotentActorState.Handoff.BlocksLocalDispatch);
        }
        finally
        {
            await autoConnect.StopAsync(CancellationToken.None);
            await runtime.StopAsync(CancellationToken.None);
            await locations.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(
                CancellationToken.None);
            await locations.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public void Canonical_target_import_is_idempotent_and_replays_fifo()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        var frames = new[]
        {
            AcceptedFrame(2),
            AcceptedFrame(1)
        };
        handoff.BeginCanonicalMaintenanceImport("handoff", frames);
        handoff.BeginCanonicalMaintenanceImport("handoff", frames);
        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        var replay = handoff.PrepareCanonicalMaintenanceReplay("handoff");

        Assert.Equal(new ulong[] { 1, 2 },
            replay.Select(static frame => frame.RequestId));
        handoff.AcknowledgeReplayedFrame();
        handoff.AcknowledgeReplayedFrame();
        handoff.Complete("handoff");
    }

    [Fact]
    public void Canonical_replay_acknowledgement_drains_the_first_sequence()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCanonicalMaintenanceImport("handoff", [AcceptedFrame(1)]);
        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        _ = handoff.PrepareCanonicalMaintenanceReplay("handoff");

        handoff.AcknowledgeCanonicalReplayThrough(1);

        Assert.Empty(handoff.SnapshotFinalReplay());
        handoff.Complete("handoff");
    }

    [Fact]
    public void Canonical_replay_opens_live_admission_at_the_empty_boundary()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCanonicalMaintenanceImport(
            "handoff",
            [AcceptedFrame(1)]);
        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        _ = handoff.PrepareCanonicalMaintenanceReplay("handoff");

        handoff.AcknowledgeReplayedFrame();

        Assert.True(
            handoff.TryOpenCanonicalMaintenanceAdmission("handoff", 1));
        Assert.True(
            handoff.TryCompleteCanonicalMaintenanceReplay("handoff"));
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "source-owner",
            3,
            RoutingId.From("source"),
            7);
        using var restored = ZLinkActorHandoffFrames.Restore(
            new ZLinkBackendActorRef(
                RoutingId.From("target"),
                "actor-1",
                42),
            [AcceptedFrame(2, requestSource)]);
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.NotSealed,
            handoff.TryCapture(restored[0]));
        Assert.True(
            handoff.IsCanonicalMaintenanceReplayComplete("handoff"));

        // The completion command performs idempotent cleanup after live
        // admission is already open.
        handoff.Complete("handoff");
    }

    [Fact]
    public async Task Canonical_replay_reserves_trailing_before_live_admission()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCanonicalMaintenanceImport(
            "handoff",
            [AcceptedFrame(1)]);
        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        _ = handoff.PrepareCanonicalMaintenanceReplay("handoff");
        handoff.AcknowledgeCanonicalReplayThrough(1);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "source-owner",
            3,
            RoutingId.From("source"),
            7);
        using var captured = ZLinkActorHandoffFrames.Restore(
            new ZLinkBackendActorRef(
                RoutingId.From("target"),
                "actor-1",
                42),
            [AcceptedFrame(2, requestSource)]);
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(captured[0]));
        using var concurrent = ZLinkActorHandoffFrames.Restore(
            new ZLinkBackendActorRef(
                RoutingId.From("target"),
                "actor-1",
                42),
            [AcceptedFrame(3, requestSource)]);

        Task<ZLinkActorHandoffCaptureResult>? concurrentCapture = null;
        var reserved = new List<long>();
        handoff.ReserveCanonicalMaintenanceTrailingAndOpenAdmission(
            "handoff",
            0,
            frame =>
            {
                reserved.Add(frame.ArrivalIndex);
                concurrentCapture = Task.Run(
                    () => handoff.TryCapture(concurrent[0]));
                Assert.False(concurrentCapture.Wait(
                    TimeSpan.FromMilliseconds(50)));
            });

        Assert.Single(reserved);
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.NotSealed,
            await concurrentCapture!);
    }

    [Fact]
    public void Canonical_replay_keeps_capturing_until_final_admission_open()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCanonicalMaintenanceImport(
            "handoff",
            [AcceptedFrame(1)]);
        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        _ = handoff.PrepareCanonicalMaintenanceReplay("handoff");
        handoff.AcknowledgeCanonicalReplayThrough(1);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "source-owner",
            3,
            RoutingId.From("source"),
            7);
        var actor = new ZLinkBackendActorRef(
            RoutingId.From("target"),
            "actor-1",
            42);
        using var beforeNormalization = ZLinkActorHandoffFrames.Restore(
            actor,
            [AcceptedFrame(2, requestSource)]);
        using var duringNormalization = ZLinkActorHandoffFrames.Restore(
            actor,
            [AcceptedFrame(3, requestSource)]);
        using var afterAdmission = ZLinkActorHandoffFrames.Restore(
            actor,
            [AcceptedFrame(4, requestSource)]);

        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(beforeNormalization[0]));
        var prepared = new List<long>();
        handoff.ReserveCanonicalMaintenanceTrailing(
            "handoff",
            1,
            frame => prepared.Add(frame.ArrivalIndex));
        Assert.Single(prepared);
        handoff.AcknowledgeReplayedFrame(prepared[0]);

        // Authority normalization may await storage while the Spot execution
        // seal remains closed. Actor ingress must still join the replay tail.
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(duringNormalization[0]));

        var final = new List<long>();
        handoff.ReserveCanonicalMaintenanceTrailingAndOpenAdmission(
            "handoff",
            1,
            frame => final.Add(frame.ArrivalIndex));
        Assert.Single(final);
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.NotSealed,
            handoff.TryCapture(afterAdmission[0]));
    }

    [Fact]
    public void Canonical_cutover_reserves_message_follow_before_new_owner_ingress()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCanonicalMaintenanceImport(
            "handoff",
            [AcceptedFrame(1)]);
        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        _ = handoff.PrepareCanonicalMaintenanceReplay("handoff");
        handoff.AcknowledgeCanonicalReplayThrough(1);
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            "source-owner",
            3,
            RoutingId.From("source"),
            7);
        var actor = new ZLinkBackendActorRef(
            RoutingId.From("target"),
            "actor-1",
            42);
        using var direct = ZLinkActorHandoffFrames.Restore(
            actor,
            [AcceptedFrame(2, source)]);
        using var followed = ZLinkActorHandoffFrames.Restore(
            actor,
            [AcceptedFrame(3, source, messageFollowHopCount: 1)]);

        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(direct[0]));
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(followed[0]));

        var reserved = new List<ulong>();
        handoff.ReserveCanonicalMaintenanceTrailingAndOpenAdmission(
            "handoff",
            1,
            frame => reserved.Add(frame.RequestId));

        Assert.Equal([3UL, 2UL], reserved);
    }

    [Fact]
    public void Canonical_trailing_replay_acknowledges_the_completed_frame()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCanonicalMaintenanceImport(
            "handoff",
            [AcceptedFrame(1), AcceptedFrame(2)]);
        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        _ = handoff.PrepareCanonicalMaintenanceReplay("handoff");
        handoff.ReserveCanonicalMaintenanceTrailingAndOpenAdmission(
            "handoff",
            0,
            static _ => { });

        handoff.AcknowledgeReplayedFrame(2);

        Assert.Equal(
            [1L],
            handoff.SnapshotFinalReplay()
                .Select(static frame => frame.ArrivalIndex));
        handoff.AcknowledgeCanonicalReplayThrough(1);
        Assert.True(
            handoff.TryCompleteCanonicalMaintenanceReplay("handoff"));
    }

    [Fact]
    public async Task Canonical_replay_retry_reuses_a_partially_reserved_frame()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCanonicalMaintenanceImport(
            "handoff",
            [AcceptedFrame(1), AcceptedFrame(2)]);
        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        _ = handoff.PrepareCanonicalMaintenanceReplay("handoff");
        var firstDrain = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var attempts = new Dictionary<long, int>();

        Task ReserveFirstAttempt(ZLinkActorHandoffFrame frame)
        {
            attempts[frame.ArrivalIndex] =
                attempts.GetValueOrDefault(frame.ArrivalIndex) + 1;
            if (frame.ArrivalIndex == 2)
                throw new InvalidOperationException("queue full");
            return firstDrain.Task;
        }

        Assert.Throws<InvalidOperationException>(() =>
            handoff.ReserveCanonicalMaintenanceTrailing(
                "handoff",
                0,
                ReserveFirstAttempt));
        // Handler ACK can precede post-handler reconciliation. The retry must
        // still wait for the original dispatch task after the frame leaves
        // the replay inventory.
        handoff.AcknowledgeReplayedFrame(1);

        Task ReserveRetry(ZLinkActorHandoffFrame frame)
        {
            attempts[frame.ArrivalIndex] =
                attempts.GetValueOrDefault(frame.ArrivalIndex) + 1;
            return Task.CompletedTask;
        }

        var reservations =
            handoff.ReserveCanonicalMaintenanceTrailingAndOpenAdmission(
                "handoff",
                0,
                ReserveRetry);

        Assert.Equal(2, reservations.Count);
        Assert.Same(firstDrain.Task, reservations[0]);
        Assert.Equal(1, attempts[1]);
        Assert.Equal(2, attempts[2]);
        firstDrain.TrySetResult();
        await Task.WhenAll(reservations);
    }

    [Fact]
    public void Canonical_target_import_applies_the_bounded_backlog_gate()
    {
        var admission = new ZLinkBoundedIngressAdmission(
            recordCapacity: 1,
            byteCapacity: long.MaxValue);
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System,
            targetIngressAdmission: admission);

        var error = Assert.Throws<ZLinkFrameworkException>(() =>
            handoff.BeginCanonicalMaintenanceImport(
                "overflow",
                [AcceptedFrame(1), AcceptedFrame(2)]));

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.Equal((0, 0L), admission.Snapshot());
        handoff.BeginCanonicalMaintenanceImport("retry", [AcceptedFrame(1)]);
        Assert.Equal((1, 1L), admission.Snapshot());
    }

    [Fact]
    public void Canonical_target_import_accepts_the_negotiated_2048_record_boundary()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        var initial = Enumerable.Range(1, 1024)
            .Select(static value => AcceptedFrame(value))
            .ToArray();
        var delta = Enumerable.Range(1025, 1024)
            .Select(static value => AcceptedFrame(value))
            .ToArray();

        handoff.BeginCanonicalMaintenanceImport(
            "handoff",
            initial,
            negotiatedMessages: 2048,
            negotiatedBytes: 2048);
        handoff.AppendCanonicalMaintenanceImport("handoff", delta);
        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        _ = handoff.PrepareCanonicalMaintenanceReplay("handoff");

        Assert.Equal(2048, handoff.SnapshotFinalReplay().Count);
    }

    [Fact]
    public void Canonical_target_rejects_object_generation_change()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCanonicalMaintenanceImport("handoff", []);

        var error = Assert.Throws<ZLinkFrameworkException>(() =>
            handoff.MarkAuthorityCommitted("handoff", 42, 43));

        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
    }

    [Fact]
    public void Startup_recovery_routes_canonical_actor_root_to_standalone_owner()
    {
        var source = SourceAuthority();
        var actor = SourceActorAuthority();
        var target = TargetDescriptor();
        var root = ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
            source,
            actor,
            target,
            Guid.NewGuid(),
            ReadOnlyMemory<byte>.Empty,
            [],
            default);
        var candidate = new ZLinkRelocationRecoveryCandidate(
            new ZLinkRelocationManifestReference(
                "relocation-root",
                1,
                root.AggregateId,
                root.AggregateGeneration,
                root.InventoryDigest),
            root,
            [
                new ZLinkAuthorityEntry(
                    root.Participants[0].AuthorityKey,
                    source)
            ]);

        Assert.True(ZLinkStandaloneActorRelocationRuntime.OwnsRecovery(
            candidate));
        Assert.False(ZLinkStandaloneActorRelocationRuntime.OwnsRecovery(
            candidate with
            {
                Envelope = root with
                {
                    Participants =
                    [root.Participants[0] with { RecoveryPayload = "{}"u8.ToArray() }]
                }
            }));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Immutable_root_preserves_generation_and_orders_accepted_queue(
        bool snapshot)
    {
        var source = SourceAuthority();
        var authority = SourceActorAuthority();
        var target = TargetDescriptor();
        var targetRid = target.Rid;
        var frames = new[]
        {
            AcceptedFrame(2),
            AcceptedFrame(1)
        };
        var acceptedRecords = frames.Select(static frame =>
            new ZLinkActorAcceptedRecord(
                frame,
                new ZLinkServiceWireCodec.RequestSourceFence(
                    "source-owner", 3, RoutingId.From("source"), 7)))
            .ToArray();

        var root = ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
            source,
            authority,
            target,
            Guid.NewGuid(),
            snapshot ? new byte[] { 9, 8, 7 } : [],
            acceptedRecords,
            default);

        var participant = Assert.Single(root.Participants);
        Assert.Equal(42UL, participant.ObjectGeneration);
        Assert.Equal(11UL, participant.AuthorityOwnerGeneration);
        Assert.Equal(1UL, participant.CanonicalParticipantId);
        Assert.Equal(new ulong[] { 1, 2 },
            participant.AcceptedJobs.Select(static job => job.AcceptedSequence));
        Assert.Equal(snapshot ? 3 : 0, participant.ApplicationState.Length);
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            participant.RecoveryPayload.Span);
        Assert.Equal("v9", recovery.ExpectedStoreVersion);
        Assert.True(ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
            recovery.AuthorityPayload.Span,
            out var relocating));
        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
            relocating.ApplicationPayload.Span,
            out var targetAuthority));
        Assert.Equal(42UL, recovery.ObjectGeneration);
        Assert.Equal(11UL, recovery.AuthorityOwnerGeneration);
        Assert.Equal(targetRid, targetAuthority.NodeRid);
        Assert.Equal("target-entry", targetAuthority.CurrentSpotId);
        Assert.Equal("target-owner", targetAuthority.OwnerId);
        Assert.Equal(4UL, targetAuthority.OwnerLeaseGeneration);
        var sourceFence = ZLinkActorRelocationSourceFenceCodec.Decode(
            recovery.MembershipMutation.Span);
        Assert.Equal("source-owner", sourceFence.OwnerId);
        Assert.Equal(3UL, sourceFence.OwnerLeaseGeneration);
        Assert.Equal(RoutingId.From("source"), sourceFence.NodeRid);
        Assert.Equal(7UL, sourceFence.NodeGeneration);
    }

    [Fact]
    public void Immutable_root_preserves_exact_per_actor_user_spot_destination()
    {
        var source = SourceAuthority();
        var authority = SourceActorAuthority();
        var destination = new ZLinkStandaloneActorRelocationDestination(
            "lobby-42",
            73,
            ZLinkSpotKind.User,
            RoutingId.From("target"),
            9,
            "actors",
            new ZLinkLocationOwnerToken("target-owner", 4));

        var root = ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
            source,
            authority,
            destination,
            Guid.NewGuid(),
            ReadOnlyMemory<byte>.Empty,
            [],
            default);

        var participant = Assert.Single(root.Participants);
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            participant.RecoveryPayload.Span);
        Assert.True(ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
            recovery.AuthorityPayload.Span,
            out var relocating));
        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
            relocating.ApplicationPayload.Span,
            out var targetAuthority));
        Assert.Equal("lobby-42", targetAuthority.CurrentSpotId);
        Assert.Equal(73UL, targetAuthority.CurrentSpotGeneration);
        Assert.Equal(ZLinkSpotKind.User, targetAuthority.CurrentSpotKind);
        Assert.Equal(RoutingId.From("target"), targetAuthority.NodeRid);
        Assert.Equal(9UL, targetAuthority.NodeGeneration);
        Assert.Equal("actors", targetAuthority.MeshName);
        Assert.Equal("target-owner", targetAuthority.OwnerId);
        Assert.Equal(4UL, targetAuthority.OwnerLeaseGeneration);
        Assert.Equal(42UL, participant.ObjectGeneration);
    }

    [Fact]
    public void Standalone_command40_bounds_initial_root_with_exact_negotiated_allowance()
    {
        var source = SourceAuthority();
        var authority = SourceActorAuthority();
        var target = TargetDescriptor();
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner", 13, RoutingId.From("caller"), 17);
        var envelope = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                source,
                authority,
                target,
                Guid.NewGuid(),
                ReadOnlyMemory<byte>.Empty,
                AcceptedRecords(requestSource),
                default),
            applicationVersion: 7);
        var now = DateTimeOffset.UtcNow;
        var stored = new ZLinkRelocationStored(
            "root",
            ZLinkCrc32C.Compute(
                ZLinkRelocationEnvelopeCodec.Encode(envelope)),
            now.AddHours(1),
            now);
        var prepare = ZLinkStandaloneActorRelocationRuntime.CreatePrepare(
            source,
            authority,
            target,
            envelope,
            stored,
            boundSession: null,
            applicationVersion: 7);

        Assert.Empty(prepare.Object.StableType);
        ZLinkCanonicalRelocationReservationOwner.ValidateStandaloneRoot(
            prepare,
            envelope,
            ZLinkPlacementObjectKind.Actor);
        ZLinkCanonicalRelocationReservationOwner
            .ValidateStandaloneRootStableType(
                envelope,
                authority.StableType);
        var participant = Assert.Single(envelope.Participants);
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            participant.RecoveryPayload.Span);
        var corruptStableType = envelope with
        {
            Participants =
            [
                participant with
                {
                    RecoveryPayload =
                        ZLinkCanonicalParticipantRecoveryCodec.Encode(
                            recovery with
                            {
                                StableType = "different-actor-type"
                            })
                }
            ]
        };
        Assert.Throws<InvalidDataException>(() =>
            ZLinkCanonicalRelocationReservationOwner
                .ValidateStandaloneRootStableType(
                    corruptStableType,
                    authority.StableType));
        ZLinkCanonicalRelocationReservationOwner.ValidateStandaloneRoot(
            prepare with
            {
                RequiredMessages = prepare.RequiredMessages + 1,
                RequiredBytes = prepare.RequiredBytes + 1024,
                Participants =
                [prepare.Participants[0] with
                {
                    AllowanceMessages = prepare.Participants[0]
                        .AllowanceMessages + 1,
                    AllowanceBytes = prepare.Participants[0]
                        .AllowanceBytes + 512
                }]
            },
            envelope,
            ZLinkPlacementObjectKind.Actor);
        Assert.Throws<InvalidDataException>(() =>
            ZLinkCanonicalRelocationReservationOwner.ValidateStandaloneRoot(
                prepare with
                {
                    RequiredMessages = prepare.RequiredMessages + 1
                },
                envelope,
                ZLinkPlacementObjectKind.Actor));
        Assert.Throws<InvalidDataException>(() =>
            ZLinkCanonicalRelocationReservationOwner.ValidateStandaloneRoot(
                prepare with { RequiredBytes = prepare.RequiredBytes - 1 },
                envelope,
                ZLinkPlacementObjectKind.Actor));
        Assert.Throws<InvalidDataException>(() =>
            ZLinkCanonicalRelocationReservationOwner.ValidateStandaloneRoot(
                prepare with
                {
                    Participants =
                    [prepare.Participants[0] with
                    {
                        AllowanceMessages = prepare.Participants[0]
                            .AllowanceMessages + 1
                    }]
                },
                envelope,
                ZLinkPlacementObjectKind.Actor));
        Assert.Throws<InvalidDataException>(() =>
            ZLinkCanonicalRelocationReservationOwner.ValidateStandaloneRoot(
                prepare with
                {
                    Participants =
                    [prepare.Participants[0] with
                    {
                        AllowanceBytes = prepare.RequiredBytes + 1
                    }]
                },
                envelope,
                ZLinkPlacementObjectKind.Actor));
    }

    [Fact]
    public void Startup_recovery_requires_exact_source_lease_expiry()
    {
        var now = DateTimeOffset.UtcNow;
        var source = new ZLinkActorRelocationSourceFence(
            "source-owner", 3, RoutingId.From("source"), 7);

        Assert.True(ZLinkStandaloneActorRelocationRuntime
            .IsExactSourceLeaseExpired(
                new ZLinkOwnerLeaseReadResult.Missing(), source));
        Assert.True(ZLinkStandaloneActorRelocationRuntime
            .IsExactSourceLeaseExpired(
                new ZLinkOwnerLeaseReadResult.Found(
                    new ZLinkLocationOwnerToken("source-owner", 4),
                    now.AddMinutes(1), now), source));
        Assert.True(ZLinkStandaloneActorRelocationRuntime
            .IsExactSourceLeaseExpired(
                new ZLinkOwnerLeaseReadResult.Found(
                    new ZLinkLocationOwnerToken("source-owner", 3),
                    now, now), source));
        Assert.False(ZLinkStandaloneActorRelocationRuntime
            .IsExactSourceLeaseExpired(
                new ZLinkOwnerLeaseReadResult.Found(
                    new ZLinkLocationOwnerToken("source-owner", 3),
                    now.AddMinutes(1), now), source));
    }

    [Fact]
    public void Replacement_target_preserves_root_and_fences_stale_attempt()
    {
        var relocationId = Guid.NewGuid();
        var id = new byte[16];
        relocationId.TryWriteBytes(id, bigEndian: true, out _);
        var root = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                SourceAuthority(),
                SourceActorAuthority(),
                TargetDescriptor(),
                relocationId,
                ReadOnlyMemory<byte>.Empty,
                [],
                default),
            applicationVersion: 7);
        var firstTarget = ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
            ZLinkCanonicalParticipantRecoveryCodec.Decode(
                    root.Participants[0].RecoveryPayload.Span)
                .AuthorityPayload.Span,
            out var decoded)
            ? decoded
            : throw new InvalidDataException();
        var initialState = new ZLinkCanonicalRelocationAuthorityState(
            BinaryPrimitives.ReadUInt64BigEndian(id),
            BinaryPrimitives.ReadUInt64BigEndian(id[8..]),
            1,
            "source",
            7,
            "source-owner",
            3,
            firstTarget.NodeRid.ToHex(),
            firstTarget.NodeGeneration,
            firstTarget.OwnerId,
            firstTarget.OwnerLeaseGeneration,
            1,
            firstTarget.OwnerId,
            firstTarget.OwnerLeaseGeneration,
            firstTarget.NodeRid.ToHex(),
            firstTarget.NodeGeneration,
            4,
            "root-stable",
            17,
            7,
            0);
        var firstPayload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                ZLinkActorAuthorityPayloadCodec.Encode(firstTarget),
                initialState,
                root);
        var firstSnapshot = PublishedSnapshot(firstPayload, firstTarget, 12);

        var replacementTarget = firstTarget with
        {
            CurrentSpotId = "replacement-entry",
            CurrentSpotGeneration = 9,
            OwnerId = "replacement-owner",
            OwnerLeaseGeneration = 5,
            NodeRid = RoutingId.From("replacement"),
            NodeGeneration = 9
        };
        var replacementPayload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                ZLinkActorAuthorityPayloadCodec.Encode(replacementTarget),
                initialState with
                {
                    TargetAttemptGeneration = 2,
                    TargetNodeRid = replacementTarget.NodeRid.ToHex(),
                    TargetNodeGeneration = replacementTarget.NodeGeneration,
                    TargetOwnerId = replacementTarget.OwnerId,
                    TargetOwnerLeaseGeneration =
                        replacementTarget.OwnerLeaseGeneration,
                    ReservationGeneration = 2,
                    CoordinatorOwnerId = replacementTarget.OwnerId,
                    CoordinatorLeaseGeneration =
                        replacementTarget.OwnerLeaseGeneration,
                    CoordinatorNodeRid = replacementTarget.NodeRid.ToHex(),
                    CoordinatorNodeGeneration = replacementTarget.NodeGeneration
                },
                root);
        var replacementSnapshot = PublishedSnapshot(
            replacementPayload,
            replacementTarget,
            13);

        Assert.True(ZLinkStandaloneActorRelocationTakeoverCoordinator
            .IsCurrentAttempt(firstSnapshot, relocationId, 1, firstTarget));
        Assert.False(ZLinkStandaloneActorRelocationTakeoverCoordinator
            .IsCurrentAttempt(
                replacementSnapshot,
                relocationId,
                1,
                firstTarget));
        Assert.True(ZLinkStandaloneActorRelocationTakeoverCoordinator
            .IsCurrentAttempt(
                replacementSnapshot,
                relocationId,
                2,
                replacementTarget));
        Assert.True(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
            replacementPayload,
            out var replacement));
        Assert.Equal(initialState.RelocationHigh, replacement.RelocationHigh);
        Assert.Equal(initialState.RelocationLow, replacement.RelocationLow);
        Assert.Equal("root-stable", replacement.RelocationReference);
        Assert.Equal<uint>(17, replacement.RelocationChecksumCrc32c);
    }

    [Fact]
    public void Ingress_source_fence_survives_source_descriptor_restart()
    {
        var oldSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "source-owner-old", 3, RoutingId.From("source"), 7);
        var restartedSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "source-owner-new", 9, RoutingId.From("source"), 8);
        var frame = AcceptedFrame(1, oldSource);

        var accepted = Assert.Single(
            ZLinkStandaloneActorRelocationRuntime.CreateAcceptedRecords(
                [frame]));

        Assert.Equal(oldSource, accepted.RequestSource);
        Assert.NotEqual(restartedSource, accepted.RequestSource);
    }

    [Fact]
    public async Task Relocation_source_validation_rejects_a_forged_owner()
    {
        var now = DateTimeOffset.UtcNow;
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            "forged-owner", 3, RoutingId.From("caller"), 7);
        var accepted = AcceptedRecords(source);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            ZLinkActorRequestSourceFenceValidator.ValidateAsync(
                    "mesh",
                    LocalNodeStatus(),
                    [AdmittedPeer(source.NodeRid, source.NodeGeneration)],
                    accepted,
                    TimeSpan.FromSeconds(5),
                    (_, _) => ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                        new ZLinkOwnerLeaseReadResult.Found(
                            new ZLinkLocationOwnerToken("forged-owner", 3),
                            now.AddMinutes(1),
                            now)),
                    _ => ValueTask.FromResult<IReadOnlyList<
                        ZLinkMeshNodeDescriptor>>(
                        [RequestSourceDescriptor(source) with
                        {
                            OwnerId = "actual-owner"
                        }]),
                    CancellationToken.None)
                .AsTask());

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.True(error.RetryAdvice != ZLinkRetryAdvice.DoNotRetry);
    }

    [Fact]
    public async Task Relocation_source_validation_rejects_an_expired_lease()
    {
        var now = DateTimeOffset.UtcNow;
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner", 3, RoutingId.From("caller"), 7);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            ZLinkActorRequestSourceFenceValidator.ValidateAsync(
                    "mesh",
                    LocalNodeStatus(),
                    [AdmittedPeer(source.NodeRid, source.NodeGeneration)],
                    AcceptedRecords(source),
                    TimeSpan.FromSeconds(5),
                    (_, _) => ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                        new ZLinkOwnerLeaseReadResult.Found(
                            new ZLinkLocationOwnerToken("caller-owner", 3),
                            now,
                            now)),
                    _ => ValueTask.FromResult<IReadOnlyList<
                        ZLinkMeshNodeDescriptor>>(
                        [RequestSourceDescriptor(source)]),
                    CancellationToken.None)
                .AsTask());

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
    }

    [Fact]
    public async Task Relocation_source_validation_fences_a_same_rid_restart()
    {
        var now = DateTimeOffset.UtcNow;
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner", 3, RoutingId.From("caller"), 7);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            ZLinkActorRequestSourceFenceValidator.ValidateAsync(
                    "mesh",
                    LocalNodeStatus(),
                    [AdmittedPeer(source.NodeRid, 8)],
                    AcceptedRecords(source),
                    TimeSpan.FromSeconds(5),
                    (_, _) => ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                        new ZLinkOwnerLeaseReadResult.Found(
                            new ZLinkLocationOwnerToken("caller-owner", 3),
                            now.AddMinutes(1),
                            now)),
                    _ => ValueTask.FromResult<IReadOnlyList<
                        ZLinkMeshNodeDescriptor>>(
                        [RequestSourceDescriptor(source) with
                        {
                            LifecycleGeneration = 8
                        }]),
                    CancellationToken.None)
                .AsTask());

        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
    }

    [Fact]
    public async Task Relocation_source_validation_deduplicates_valid_records()
    {
        var now = DateTimeOffset.UtcNow;
        var first = new ZLinkServiceWireCodec.RequestSourceFence(
            "owner-a", 3, RoutingId.From("caller-a"), 7);
        var second = new ZLinkServiceWireCodec.RequestSourceFence(
            "owner-b", 4, RoutingId.From("caller-b"), 8);
        var leaseReads = new Dictionary<string, int>(StringComparer.Ordinal);
        var descriptorReads = 0;

        await ZLinkActorRequestSourceFenceValidator.ValidateAsync(
            "mesh",
            LocalNodeStatus(),
            [
                AdmittedPeer(first.NodeRid, first.NodeGeneration),
                AdmittedPeer(second.NodeRid, second.NodeGeneration)
            ],
            [
                .. AcceptedRecords(first),
                .. AcceptedRecords(first),
                .. AcceptedRecords(second)
            ],
            TimeSpan.FromSeconds(5),
            (ownerId, _) =>
            {
                lock (leaseReads)
                {
                    leaseReads[ownerId] = leaseReads.GetValueOrDefault(ownerId) + 1;
                }
                var source = ownerId == first.OwnerId ? first : second;
                return ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                    new ZLinkOwnerLeaseReadResult.Found(
                        new ZLinkLocationOwnerToken(
                            ownerId,
                            checked((long)source.LeaseGeneration)),
                        now.AddMinutes(1),
                        now));
            },
            _ =>
            {
                Interlocked.Increment(ref descriptorReads);
                return ValueTask.FromResult<IReadOnlyList<
                    ZLinkMeshNodeDescriptor>>(
                    [
                        RequestSourceDescriptor(first),
                        RequestSourceDescriptor(second)
                    ]);
            },
            CancellationToken.None);

        Assert.Equal(2, leaseReads.Count);
        Assert.All(leaseReads.Values, count => Assert.Equal(1, count));
        Assert.Equal(1, descriptorReads);
    }

    [Fact]
    public void Direct_handoff_admission_rejects_a_missing_source_fence()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCapture();
        var frame = AcceptedFrame(1);
        using var restored = ZLinkActorHandoffFrames.Restore(
            new ZLinkBackendActorRef(
                RoutingId.From("source"), "actor-1", 42),
            [frame]);

        Assert.Throws<ZLinkActorHandoffRejectedException>(() =>
            handoff.TryCapture(restored[0]));
    }

    [Fact]
    public void Canonical_root_preserves_durable_request_route_and_request_count()
    {
        var source = SourceAuthority();
        var sourceActor = new ZLinkBackendActorRef(
            RoutingId.From("source"), "actor-1", 42);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner", 13, RoutingId.From("caller"), 17);
        var accepted = new ZLinkActorAcceptedRecord(
            AcceptedRequestFrame(requestSource, replyRouteId: 91),
            requestSource);
        var inventory = ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
            source,
            SourceActorAuthority(),
            TargetDescriptor(),
            Guid.NewGuid(),
            ReadOnlyMemory<byte>.Empty,
            [accepted],
            default);

        var canonical = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            inventory,
            applicationVersion: 7);
        var participant = Assert.Single(canonical.Participants);
        var request = Assert.Single(participant.AcceptedJobs).CanonicalRequest;
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            participant.RecoveryPayload.Span);
        Assert.True(ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
            recovery.AuthorityPayload.Span,
            out var phase));

        Assert.False(canonical.CanonicalLogicalStream.IsEmpty);
        Assert.Equal<ulong>(91, request!.ReplyRouteId);
        Assert.Equal<uint>(1, phase.AcceptedRequestCount);
        Assert.Equal<uint>(0, phase.TerminalCompletionCount);
        Assert.Equal(sourceActor,
            ZLinkCanonicalActorAcceptedJournal.Decode(
                participant.AcceptedJobs[0].Payload.Span,
                1).TargetActor);
    }

    [Fact]
    public void Canonical_replay_validates_source_target_then_rebinds_current_target()
    {
        var sourceActor = new ZLinkBackendActorRef(
            RoutingId.From("source"), "actor-1", 42);
        var targetActor = new ZLinkBackendActorRef(
            RoutingId.From("target"), "actor-1", 42);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner", 13, RoutingId.From("caller"), 17);
        var accepted = ZLinkCanonicalActorAcceptedJournal.Decode(
            ZLinkCanonicalActorAcceptedJournal.Encode(
                new ZLinkActorAcceptedRecord(
                    AcceptedRequestFrame(requestSource, 91),
                    requestSource),
                sourceActor),
            1).Accepted;

        using var rebound = ZLinkActorHandoffFrames.RestoreCanonical(
            targetActor,
            sourceActor,
            [accepted]);

        Assert.Equal(targetActor, rebound[0].Actor);
        Assert.Equal(sourceActor, accepted.FrozenTargetActor);
        Assert.Throws<ZLinkRelocationDataLostException>(() =>
            ZLinkActorHandoffFrames.RestoreCanonical(
                targetActor,
                sourceActor with { NodeRid = RoutingId.From("wrong-source") },
                [accepted]));
    }

    [Fact]
    public void Pending_reply_payload_preserves_route_after_journal_pruning()
    {
        var replyFrame = new byte[] { 3, 5, 8 };
        var completion = new ZLinkCanonicalTerminalCompletion(
            1,
            2,
            "source-owner",
            3,
            RoutingId.From("source").ToHex(),
            4,
            1,
            1,
            0,
            0,
            0,
            ReadOnlyMemory<byte>.Empty)
        {
            Payload = new ZLinkCanonicalApplicationPayload(
                "reply",
                "application/x-zlink-actor-relocation-reply-v1",
                ZLinkFrameworkRuntime.EncodeDurableActorReply(91, replyFrame))
        };

        var decoded = ZLinkFrameworkRuntime.DecodeDurableActorReply(completion);

        Assert.Equal<ulong>(91, decoded.ReplyRouteId);
        Assert.Equal(replyFrame, decoded.ReplyFrame);
    }

    [Fact]
    public async Task Replay_progress_publishes_counts_and_retains_predecessor_proof()
    {
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner", 13, RoutingId.From("caller"), 17);
        var identity = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                SourceAuthority(),
                SourceActorAuthority(),
                TargetDescriptor(),
                Guid.NewGuid(),
                ReadOnlyMemory<byte>.Empty,
                [new ZLinkActorAcceptedRecord(
                    AcceptedRequestFrame(requestSource, 91),
                    requestSource)],
                default),
            applicationVersion: 7);
        var relocationStore = new ProgressRelocationStore();
        var initial = await ZLinkRelocationTreeStore.PutAsync(
            relocationStore,
            identity,
            TimeSpan.FromHours(24),
            CancellationToken.None);
        var participant = Assert.Single(identity.Participants);
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            participant.RecoveryPayload.Span);
        Assert.True(ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
            recovery.AuthorityPayload.Span,
            out var phase));
        phase = phase with { Phase = ZLinkActorRelocationAuthorityPhase.Cleaning };
        var publication = new ZLinkRelocationAuthorityPayload(
            initial.Root.Reference,
            initial.Root.ChecksumCrc32c,
            identity.AggregateId,
            identity.AggregateGeneration,
            identity.InventoryDigest,
            "target-owner",
            4,
            ZLinkActorRelocationAuthorityPayloadCodec.Encode(phase))
        {
            IsCanonical = true,
            ApplicationVersion = 7
        };
        var authorityStore = new ProgressAuthorityStore(
            participant.AuthorityKey,
            new ZLinkAuthoritySnapshot(
                "1",
                ZLinkRelocationAuthorityPayloadCodec.Encode(publication),
                participant.ObjectGeneration,
                checked(participant.AuthorityOwnerGeneration + 1),
                "target-owner",
                4,
                new ZLinkPlacementAllocation(
                    ZLinkPlacementAllocationState.Active,
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    new ZLinkMeshNodeDescriptorKey(
                        "mesh",
                        RoutingId.From("target")),
                    8,
                    new ZLinkCapacityVector(1, 0, null)),
                null,
                DateTimeOffset.UtcNow));
        var coordinator = new ZLinkStandaloneActorRelocationProgressCoordinator(
            authorityStore,
            relocationStore);
        var completion = ZLinkRelocationEnvelopeCodec
            .CreateCanonicalTerminalCompletion(
            81,
            82,
            requestSource.OwnerId,
            requestSource.LeaseGeneration,
            requestSource.NodeRid.ToHex(),
            requestSource.NodeGeneration,
            participant.CanonicalParticipantId,
            1,
            0,
            0,
            0,
            new ZLinkCanonicalApplicationPayload(
                "reply",
                "application/x-zlink-actor-relocation-reply-v1",
                ZLinkFrameworkRuntime.EncodeDurableActorReply(91, [5])));

        var advanced = await coordinator.AdvanceReplayAsync(
            identity,
            1,
            completion,
            new ZLinkLocationOwnerToken("target-owner", 4),
            CancellationToken.None);

        Assert.Equal<ulong>(1, Assert.Single(advanced.Root.Participants).ReplayCursor);
        Assert.Equal<uint>(1, advanced.Phase.TerminalCompletionCount);
        Assert.Equal<uint>(1, advanced.Phase.PendingRelayCount);
        Assert.Contains(initial.Root.Reference, relocationStore.Payloads.Keys);
        var acknowledged = await coordinator.CompleteReplyAsync(
            identity,
            completion,
            1,
            new ZLinkLocationOwnerToken("target-owner", 4),
            CancellationToken.None);
        Assert.Equal<uint>(0, acknowledged.Phase.PendingRelayCount);
        var version = acknowledged.Authority.StoreVersion;
        var duplicate = await coordinator.CompleteReplyAsync(
            identity,
            completion,
            2,
            new ZLinkLocationOwnerToken("target-owner", 4),
            CancellationToken.None);
        Assert.Equal(version, duplicate.Authority.StoreVersion);
    }

    [Fact]
    public async Task Replay_progress_accepts_only_the_exact_next_sequence()
    {
        var relocationStore = new ProgressRelocationStore();
        var seed = await CreateReplayProgressSeedAsync(3, relocationStore);
        var authorityStore = new ProgressAuthorityStore(
            seed.Participant.AuthorityKey,
            seed.Snapshot);
        var coordinator = new ZLinkStandaloneActorRelocationProgressCoordinator(
            authorityStore,
            relocationStore);

        var first = await coordinator.AdvanceReplayAsync(
            seed.Identity, 1, null, seed.Owner, CancellationToken.None);
        var duplicate = await coordinator.AdvanceReplayAsync(
            seed.Identity, 1, null, seed.Owner, CancellationToken.None);

        Assert.Equal(first.Authority.StoreVersion, duplicate.Authority.StoreVersion);
        var gap = await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            () => coordinator.AdvanceReplayAsync(
                seed.Identity, 3, null, seed.Owner, CancellationToken.None).AsTask());
        Assert.Contains("does not follow cursor '1'", gap.Message);

        var second = await coordinator.AdvanceReplayAsync(
            seed.Identity, 2, null, seed.Owner, CancellationToken.None);
        Assert.Equal<ulong>(2, Assert.Single(second.Root.Participants).ReplayCursor);
    }

    [Fact]
    public async Task Concurrent_same_content_replay_keeps_the_published_root()
    {
        var relocationStore = new ProgressRelocationStore(contentAddressed: true);
        var seed = await CreateReplayProgressSeedAsync(1, relocationStore);
        var authorityStore = new ProgressAuthorityStore(
            seed.Participant.AuthorityKey,
            seed.Snapshot,
            synchronizedCompareExchanges: 2);
        var firstCoordinator = new ZLinkStandaloneActorRelocationProgressCoordinator(
            authorityStore,
            relocationStore);
        var secondCoordinator = new ZLinkStandaloneActorRelocationProgressCoordinator(
            authorityStore,
            relocationStore);

        var advances = await Task.WhenAll(
            firstCoordinator.AdvanceReplayAsync(
                seed.Identity, 1, null, seed.Owner, CancellationToken.None).AsTask(),
            secondCoordinator.AdvanceReplayAsync(
                seed.Identity, 1, null, seed.Owner, CancellationToken.None).AsTask());

        Assert.Equal(advances[0].Authority.StoreVersion,
            advances[1].Authority.StoreVersion);
        var current = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await authorityStore.ReadAuthorityAsync(
                seed.Participant.AuthorityKey,
                CancellationToken.None));
        Assert.True(ZLinkRelocationAuthorityPayloadCodec.TryDecode(
            current.Snapshot.Payload.Span,
            out var publication));
        Assert.Contains(publication.Reference, relocationStore.Payloads.Keys);
        Assert.All(advances, advanced =>
            Assert.Equal<ulong>(1,
                Assert.Single(advanced.Root.Participants).ReplayCursor));
    }

    [Fact]
    public async Task Replay_progress_reconciles_a_lost_stored_response()
    {
        var relocationStore = new ProgressRelocationStore(contentAddressed: true);
        var seed = await CreateReplayProgressSeedAsync(1, relocationStore);
        var authorityStore = new ProgressAuthorityStore(
            seed.Participant.AuthorityKey,
            seed.Snapshot,
            loseFirstStoredResponse: true);
        var coordinator = new ZLinkStandaloneActorRelocationProgressCoordinator(
            authorityStore,
            relocationStore);

        var advanced = await coordinator.AdvanceReplayAsync(
            seed.Identity, 1, null, seed.Owner, CancellationToken.None);

        Assert.Equal<ulong>(1, Assert.Single(advanced.Root.Participants).ReplayCursor);
        Assert.True(ZLinkRelocationAuthorityPayloadCodec.TryDecode(
            advanced.Authority.Payload.Span,
            out var publication));
        Assert.Contains(publication.Reference, relocationStore.Payloads.Keys);
    }

    [Fact]
    public async Task Canonical_replay_and_phase_progress_never_regresses_to_legacy_publication()
    {
        var relocationId = Guid.NewGuid();
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner", 13, RoutingId.From("caller"), 17);
        var identity = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                SourceAuthority(),
                SourceActorAuthority(),
                TargetDescriptor(),
                relocationId,
                ReadOnlyMemory<byte>.Empty,
                [new ZLinkActorAcceptedRecord(
                    AcceptedRequestFrame(requestSource, 91),
                    requestSource)],
                default),
            applicationVersion: 7);
        var relocationStore = new ProgressRelocationStore();
        var stored = await ZLinkRelocationTreeStore.PutAsync(
            relocationStore,
            identity,
            TimeSpan.FromHours(24),
            CancellationToken.None);
        var participant = Assert.Single(identity.Participants);
        var target = TargetDescriptor();
        var targetAuthority = ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
            ZLinkCanonicalParticipantRecoveryCodec.Decode(
                    participant.RecoveryPayload.Span)
                .AuthorityPayload.Span,
            out var decoded)
            ? decoded
            : throw new InvalidDataException();
        var id = new byte[16];
        relocationId.TryWriteBytes(id, bigEndian: true, out _);
        var canonicalPayload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                ZLinkActorAuthorityPayloadCodec.Encode(targetAuthority),
                new ZLinkCanonicalRelocationAuthorityState(
                    BinaryPrimitives.ReadUInt64BigEndian(id),
                    BinaryPrimitives.ReadUInt64BigEndian(id[8..]),
                    1,
                    "source",
                    7,
                    "source-owner",
                    3,
                    target.Rid.ToHex(),
                    target.LifecycleGeneration,
                    target.OwnerId,
                    checked((ulong)target.LeaseGeneration),
                    1,
                    target.OwnerId,
                    checked((ulong)target.LeaseGeneration),
                    target.Rid.ToHex(),
                    target.LifecycleGeneration,
                    4,
                    stored.Root.Reference,
                    stored.Root.ChecksumCrc32c,
                    7,
                    0),
                identity);
        var authorityStore = new ProgressAuthorityStore(
            participant.AuthorityKey,
            PublishedSnapshot(canonicalPayload, targetAuthority, 29));
        var committed = await authorityStore.ReadAuthorityAsync(
            participant.AuthorityKey);
        Assert.True(
            ZLinkStandaloneActorRelocationRuntime
                .IsExactCommittedTargetAuthority(
                    committed,
                    SourceAuthority(),
                    stored.Root,
                    relocationId,
                    target,
                    1,
                    requireActivated: false));
        Assert.False(
            ZLinkStandaloneActorRelocationRuntime
                .IsExactCommittedTargetAuthority(
                    committed,
                    SourceAuthority(),
                    stored.Root,
                    relocationId,
                    target,
                    1,
                    requireActivated: true));
        var owner = new ZLinkLocationOwnerToken(target.OwnerId, target.LeaseGeneration);
        var coordinator = new ZLinkStandaloneActorRelocationProgressCoordinator(
            authorityStore,
            relocationStore,
            new ZLinkStandaloneActorRelocationTargetFence(
                relocationId,
                1,
                target.Rid,
                target.LifecycleGeneration,
                owner));
        var completion = ZLinkRelocationEnvelopeCodec
            .CreateCanonicalTerminalCompletion(
            81,
            82,
            requestSource.OwnerId,
            requestSource.LeaseGeneration,
            requestSource.NodeRid.ToHex(),
            requestSource.NodeGeneration,
            participant.CanonicalParticipantId,
            1,
            0,
            0,
            0,
            new ZLinkCanonicalApplicationPayload(
                "reply",
                "application/x-zlink-actor-relocation-reply-v1",
                ZLinkFrameworkRuntime.EncodeDurableActorReply(91, [5])));

        var replayed = await coordinator.AdvanceReplayAsync(
            identity, 1, completion, owner, CancellationToken.None);
        AssertCanonical(replayed.Authority.Payload);
        var acknowledged = await coordinator.CompleteReplyAsync(
            identity, completion, 1, owner, CancellationToken.None);
        AssertCanonical(acknowledged.Authority.Payload);
        var activating = await coordinator.AdvanceCanonicalPhaseAsync(
            identity,
            ZLinkStandaloneActorCanonicalPhase.Committed,
            ZLinkStandaloneActorCanonicalPhase.Activating,
            owner,
            CancellationToken.None);
        Assert.Equal((byte)ZLinkStandaloneActorCanonicalPhase.Activating,
            activating.Canonical!.Phase);
        coordinator = new ZLinkStandaloneActorRelocationProgressCoordinator(
            authorityStore,
            relocationStore,
            new ZLinkStandaloneActorRelocationTargetFence(
                relocationId,
                1,
                target.Rid,
                target.LifecycleGeneration,
                owner));
        var activated = await coordinator.AdvanceCanonicalPhaseAsync(
            identity,
            ZLinkStandaloneActorCanonicalPhase.Activating,
            ZLinkStandaloneActorCanonicalPhase.Activated,
            owner,
            CancellationToken.None);
        Assert.Equal((byte)ZLinkStandaloneActorCanonicalPhase.Activated,
            activated.Canonical!.Phase);
        var activatedRoot = new ZLinkRelocationStored(
            activated.Canonical.State.RelocationReference,
            activated.Canonical.State.RelocationChecksumCrc32c,
            stored.Root.ExpiresAt,
            stored.Root.StoreNow);
        Assert.True(
            ZLinkStandaloneActorRelocationRuntime
                .IsExactCommittedTargetAuthority(
                    await authorityStore.ReadAuthorityAsync(
                        participant.AuthorityKey),
                    SourceAuthority(),
                    activatedRoot,
                    relocationId,
                    target,
                    1,
                    requireActivated: true));
        var staleFences = new[]
        {
            new ZLinkStandaloneActorRelocationTargetFence(
                relocationId, 2, target.Rid, target.LifecycleGeneration, owner),
            new ZLinkStandaloneActorRelocationTargetFence(
                relocationId, 1, RoutingId.From("old-target"),
                target.LifecycleGeneration, owner),
            new ZLinkStandaloneActorRelocationTargetFence(
                relocationId, 1, target.Rid,
                target.LifecycleGeneration + 1, owner),
            new ZLinkStandaloneActorRelocationTargetFence(
                relocationId, 1, target.Rid, target.LifecycleGeneration,
                new ZLinkLocationOwnerToken("old-owner", owner.LeaseGeneration))
        };
        foreach (var staleFence in staleFences)
        {
            var version = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await authorityStore.ReadAuthorityAsync(participant.AuthorityKey))
                .Snapshot.StoreVersion;
            var stale = new ZLinkStandaloneActorRelocationProgressCoordinator(
                authorityStore,
                relocationStore,
                staleFence);
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                stale.AdvancePhaseAsync(
                        identity,
                        ZLinkActorRelocationAuthorityPhase.Activated,
                        ZLinkActorRelocationAuthorityPhase.Cleaning,
                        owner,
                        CancellationToken.None)
                    .AsTask());
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
            Assert.Equal(version, Assert.IsType<ZLinkAuthorityReadResult.Found>(
                    await authorityStore.ReadAuthorityAsync(participant.AuthorityKey))
                .Snapshot.StoreVersion);
        }
        var cleaning = await coordinator.AdvanceCanonicalPhaseAsync(
            identity,
            ZLinkStandaloneActorCanonicalPhase.Activated,
            ZLinkStandaloneActorCanonicalPhase.Cleaning,
            owner,
            CancellationToken.None);
        AssertCanonical(cleaning.Authority.Payload);
        Assert.Equal((byte)ZLinkStandaloneActorCanonicalPhase.Cleaning,
            cleaning.Canonical!.Phase);
        coordinator = new ZLinkStandaloneActorRelocationProgressCoordinator(
            authorityStore,
            relocationStore,
            new ZLinkStandaloneActorRelocationTargetFence(
                relocationId,
                1,
                target.Rid,
                target.LifecycleGeneration,
                owner));
        var sourceCleaned = await coordinator.PublishAdmissionReadyAuthorityAsync(
            identity,
            owner,
            CancellationToken.None);
        AssertCanonical(sourceCleaned.Authority.Payload);
        Assert.Equal(1, sourceCleaned.Canonical!.SourceCleanupState);
        var completed = await coordinator.AdvanceCanonicalPhaseAsync(
            identity,
            ZLinkStandaloneActorCanonicalPhase.Cleaning,
            ZLinkStandaloneActorCanonicalPhase.Completed,
            owner,
            CancellationToken.None);
        AssertCanonical(completed.Authority.Payload);
        Assert.Equal((byte)ZLinkStandaloneActorCanonicalPhase.Completed,
            completed.Canonical!.Phase);
        coordinator = new ZLinkStandaloneActorRelocationProgressCoordinator(
            authorityStore,
            relocationStore,
            new ZLinkStandaloneActorRelocationTargetFence(
                relocationId,
                1,
                target.Rid,
                target.LifecycleGeneration,
                owner));
        await coordinator.NormalizeSteadyAsync(
            identity,
            owner,
            CancellationToken.None);

        var final = await authorityStore.ReadAuthorityAsync(
            participant.AuthorityKey,
            CancellationToken.None);
        var found = Assert.IsType<ZLinkAuthorityReadResult.Found>(final);
        Assert.False(ZLinkRelocationAuthorityPayloadCodec.TryDecode(
            found.Snapshot.Payload.Span,
            out _));
        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecode(
            found.Snapshot.Payload.Span,
            out var steady));
        Assert.Equal(targetAuthority.ActorId, steady.ActorId);

        static void AssertCanonical(ReadOnlyMemory<byte> payload)
        {
            Assert.True(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                payload.Span,
                out _));
            Assert.True(ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                payload.Span,
                out var publication));
            Assert.True(publication.IsCanonical);
        }
    }

    [Theory]
    [InlineData(4, false)]
    [InlineData(6, false)]
    [InlineData(6, true)]
    [InlineData(2, false)]
    [InlineData(3, false)]
    public async Task Hosted_recovery_respects_remote_liveness_and_source_owned_phases(
        byte phase,
        bool targetRemainsLive)
    {
        HostedRecoveryActorFactory.Reset();
        var suffix = Guid.NewGuid().ToString("N");
        var endpoint = $"tcp://127.0.0.1:{FindFreeTcpPort()}";
        var store = new ZLinkInMemoryLocationStore();
        var relocationStore = new ProgressRelocationStore();
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.ConfigureInboundDispatch().ApplicationHwmBytes = 0;
            options.AddLocationStore(store);
            options.AddRelocationStore(relocationStore);
            options.AddRouteMesh("mesh")
                .Listen(endpoint)
                .SetRoutingIdPrefix($"replacement-{suffix}")
                .SetActorLimit(100)
                .Objects()
                .Server()
                .AddActorFactory<HostedRecoveryActor, HostedRecoveryActorFactory>(
                    "player", factory => factory.RecreateOnRelocation());
        });
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();
        var autoConnect = provider.GetRequiredService<ZLinkLocationAutoConnectHost>();
        var replacementRid = runtime.PrepareLocationNodeRoutingId();
        await locations.StartAsync(replacementRid, CancellationToken.None);
        await runtime.StartAsync(CancellationToken.None);
        await autoConnect.StartAsync(
            await runtime.GetStartedStateForRoutingAsync(CancellationToken.None),
            CancellationToken.None);
        try
        {
            replacementRid = runtime.GetSpotNodeRuntime("mesh").Node.RoutingId;
            var replacement = Assert.Single(
                (await store.ListMeshNodesAsync("mesh", new ZLinkPageRequest(100)))
                .Items,
                descriptor => descriptor.Rid == replacementRid);
            var source = await PublishActorNodeAsync(
                store,
                $"source-owner-{suffix}",
                RoutingId.From($"source-{suffix}"),
                lifecycleGeneration: 7);
            var failed = await PublishActorNodeAsync(
                store,
                $"failed-owner-{suffix}",
                RoutingId.From($"failed-{suffix}"),
                lifecycleGeneration: 8);
            var actorId = $"actor-{suffix}";
            var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
            var intent = System.Text.Encoding.UTF8.GetBytes($"create:{actorId}");
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await store.ReserveAsync(
                    new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.Actor,
                        key,
                        "player",
                        $"inline:{actorId}",
                        System.Security.Cryptography.SHA256.HashData(intent),
                        intent.Length,
                        new ZLinkMeshNodeDescriptorKey("mesh", source.Descriptor.Rid),
                        source.Descriptor.LifecycleGeneration,
                        source.Owner,
                        new byte[] { 1 },
                        new ZLinkCapacityVector(1, 0, null))));
            var sourceActor = new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Ready,
                "player",
                actorId,
                source.Descriptor.EntrySpotId!,
                source.Descriptor.LifecycleGeneration,
                ZLinkSpotKind.Entry,
                source.Owner.OwnerId,
                checked((ulong)source.Owner.LeaseGeneration),
                "mesh",
                source.Descriptor.Rid,
                source.Descriptor.LifecycleGeneration);
            var sourceSnapshot = Assert.IsType<ZLinkObjectCommitResult.Committed>(
                await store.CommitAsync(
                    reserved.Reservation,
                    ZLinkActorAuthorityPayloadCodec.Encode(sourceActor)))
                .Snapshot;
            var relocationId = Guid.NewGuid();
            var identity = ZLinkCanonicalActorRelocationWriter.CreateInitial(
                ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                    sourceSnapshot,
                    sourceActor,
                    failed.Descriptor,
                    relocationId,
                    ReadOnlyMemory<byte>.Empty,
                    [],
                    default),
                applicationVersion: 0);
            var stored = await ZLinkRelocationTreeStore.PutAsync(
                relocationStore,
                identity,
                TimeSpan.FromHours(24),
                CancellationToken.None);
            var id = new byte[16];
            relocationId.TryWriteBytes(id, bigEndian: true, out _);
            var relocationWireId = new ZLinkServiceWireCodec.RelocationWireId(
                BinaryPrimitives.ReadUInt64BigEndian(id),
                BinaryPrimitives.ReadUInt64BigEndian(id.AsSpan(8)));
            ZLinkRelocationCapacityFence? capacityFence = null;
            if (phase != 2)
                capacityFence = Assert.IsType<
                    ZLinkRelocationCapacityReserveResult.Reserved>(
                    await store.ReserveRelocationCapacityAsync(
                    new ZLinkRelocationCapacityReservationRequest(
                        Guid.ParseExact(
                            ZLinkCanonicalRelocationReservationOwner
                                .CapacityFence(relocationWireId, 1).Value,
                            "N"),
                        key,
                        sourceSnapshot.StoreVersion,
                        ZLinkPlacementObjectKind.Actor,
                        "player",
                        sourceSnapshot.Allocation.Descriptor,
                        sourceSnapshot.Allocation.DescriptorLifecycleGeneration,
                        source.Owner,
                        new ZLinkMeshNodeDescriptorKey("mesh", failed.Descriptor.Rid),
                        failed.Descriptor.LifecycleGeneration,
                        failed.Owner,
                        new ZLinkCapacityVector(1, 0, null))))
                    .Fence;
            var failedAuthority = sourceActor with
            {
                CurrentSpotId = failed.Descriptor.EntrySpotId!,
                CurrentSpotGeneration = failed.Descriptor.LifecycleGeneration,
                OwnerId = failed.Owner.OwnerId,
                OwnerLeaseGeneration = checked((ulong)failed.Owner.LeaseGeneration),
                NodeRid = failed.Descriptor.Rid,
                NodeGeneration = failed.Descriptor.LifecycleGeneration
            };
            var publishedPayload = ZLinkCanonicalRelocationAuthorityStateCodec
                .ReplaceRelocationState(
                    ZLinkActorAuthorityPayloadCodec.Encode(
                        phase is 2 or 3 ? sourceActor : failedAuthority),
                    new ZLinkCanonicalRelocationAuthorityState(
                        BinaryPrimitives.ReadUInt64BigEndian(id),
                        BinaryPrimitives.ReadUInt64BigEndian(id.AsSpan(8)),
                        phase == 2 ? 0UL : 1UL,
                        source.Descriptor.Rid.ToHex(),
                        source.Descriptor.LifecycleGeneration,
                        source.Owner.OwnerId,
                        checked((ulong)source.Owner.LeaseGeneration),
                        phase == 2 ? string.Empty : failed.Descriptor.Rid.ToHex(),
                        phase == 2 ? 0UL : failed.Descriptor.LifecycleGeneration,
                        phase == 2 ? string.Empty : failed.Owner.OwnerId,
                        phase == 2 ? 0UL : checked((ulong)failed.Owner.LeaseGeneration),
                        phase == 2 ? 0UL : 1UL,
                        phase is 2 or 3
                            ? source.Owner.OwnerId
                            : failed.Owner.OwnerId,
                        checked((ulong)(phase is 2 or 3
                            ? source.Owner.LeaseGeneration
                            : failed.Owner.LeaseGeneration)),
                        phase is 2 or 3
                            ? source.Descriptor.Rid.ToHex()
                            : failed.Descriptor.Rid.ToHex(),
                        phase is 2 or 3
                            ? source.Descriptor.LifecycleGeneration
                            : failed.Descriptor.LifecycleGeneration,
                        phase,
                        stored.Root.Reference,
                        stored.Root.ChecksumCrc32c,
                        0,
                        phase >= (byte)ZLinkStandaloneActorCanonicalPhase.Cleaning
                            ? (byte)1
                            : (byte)0),
                    identity);
            _ = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
                await store.CompareExchangeAuthorityAsync(
                    key,
                    sourceSnapshot.StoreVersion,
                    new ZLinkAuthorityMutation.Put(
                        publishedPayload,
                        phase is 2 or 3
                            ? ZLinkAuthorityGenerationTransition.Preserve
                            : ZLinkAuthorityGenerationTransition.NewOwner,
                        phase is 2 or 3 ? null : failed.Owner,
                        capacityFence)));
            if (phase is 2 or 3)
                _ = await store.ReleaseOwnerLeaseAsync(source.Owner);
            if (phase == 3 || !targetRemainsLive)
                _ = await store.ReleaseOwnerLeaseAsync(failed.Owner);

            if (phase >= 4)
                _ = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
                    await store.ReadOwnerLeaseAsync(source.Owner.OwnerId));

            if (phase >=
                (byte)ZLinkStandaloneActorCanonicalPhase.Committed)
            {
                var waiting = await Assert.ThrowsAsync<ZLinkFrameworkException>(
                    () => runtime.RecoverPublishedRelocationsAsync(
                            CancellationToken.None)
                        .AsTask());
                Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, waiting.Kind);

                var beforeSourceExpiry =
                    Assert.IsType<ZLinkAuthorityReadResult.Found>(
                        await store.ReadAuthorityAsync(key));
                Assert.Equal(failed.Owner.OwnerId,
                    beforeSourceExpiry.Snapshot.OwnerId);
                Assert.Equal(0, HostedRecoveryActorFactory.CreatedCount);

                _ = await store.ReleaseOwnerLeaseAsync(source.Owner);
                await runtime.RecoverPublishedRelocationsAsync(
                    CancellationToken.None);
            }
            else
            {
                await runtime.RecoverPublishedRelocationsAsync(
                    CancellationToken.None);
            }

            var recovered = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(key));
            if (targetRemainsLive)
            {
                Assert.Equal(failed.Owner.OwnerId, recovered.Snapshot.OwnerId);
                Assert.True(ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    recovered.Snapshot.Payload.Span, out _));
                Assert.Equal(0, HostedRecoveryActorFactory.CreatedCount);
                Assert.Equal(0,
                    runtime.RelocationPermits.Snapshot().InboundUnits);
                return;
            }
            if (phase >=
                (byte)ZLinkStandaloneActorCanonicalPhase.Committed)
            {
                Assert.Equal(1, HostedRecoveryActorFactory.CreatedCount);
            }
            Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecode(
                recovered.Snapshot.Payload.Span,
                out var steady));
            Assert.Equal(replacementRid, steady.NodeRid);
            Assert.Equal(replacement.LifecycleGeneration, steady.NodeGeneration);
            Assert.Equal(1, HostedRecoveryActorFactory.CreatedCount);
            Assert.Equal(0,
                runtime.RelocationPermits.Snapshot().InboundUnits);
            Assert.False(ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                recovered.Snapshot.Payload.Span,
                out _));
        }
        finally
        {
            await autoConnect.StopAsync(CancellationToken.None);
            await runtime.StopAsync(CancellationToken.None);
            await locations.RemoveOwnedRowsBeforeRoutingIdReleaseAsync(
                CancellationToken.None);
            await locations.StopAsync(CancellationToken.None);
        }
    }

    private static ZLinkAuthoritySnapshot SourceAuthority()
    {
        var sourceRid = RoutingId.From("source");
        return new ZLinkAuthoritySnapshot(
            "v9",
            ReadOnlyMemory<byte>.Empty,
            42,
            11,
            "source-owner",
            3,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.Active,
                ZLinkPlacementObjectKind.Actor,
                "player",
                new ZLinkMeshNodeDescriptorKey("mesh", sourceRid),
                7,
                new ZLinkCapacityVector(1, 0, null)),
            null,
            DateTimeOffset.UtcNow);
    }

    private static ZLinkActorAuthorityPayload SourceActorAuthority() => new(
        ZLinkActorAuthorityState.Ready,
        "player",
        "actor-1",
        "source-entry",
        7,
        ZLinkSpotKind.Entry,
        "source-owner",
        3,
        "mesh",
        RoutingId.From("source"),
        7);

    private static ZLinkAuthoritySnapshot PublishedSnapshot(
        ReadOnlyMemory<byte> payload,
        ZLinkActorAuthorityPayload target,
        ulong authorityOwnerGeneration) => new(
        authorityOwnerGeneration.ToString(),
        payload,
        42,
        authorityOwnerGeneration,
        target.OwnerId,
        checked((long)target.OwnerLeaseGeneration),
        new ZLinkPlacementAllocation(
            ZLinkPlacementAllocationState.Active,
            ZLinkPlacementObjectKind.Actor,
            target.StableType,
            new ZLinkMeshNodeDescriptorKey(target.MeshName, target.NodeRid),
            target.NodeGeneration,
            new ZLinkCapacityVector(1, 0, null)),
        null,
        DateTimeOffset.UtcNow);

    private static ZLinkMeshNodeDescriptor TargetDescriptor() => new(
        "mesh",
        RoutingId.From("target"),
        8,
        1,
        "tcp://127.0.0.1:1",
        new Dictionary<string, int>(),
        "plain",
        "target-owner",
        4,
        DateTimeOffset.UtcNow)
    {
        EntrySpotId = "target-entry"
    };

    private static async ValueTask<(ZLinkMeshNodeDescriptor Descriptor,
        ZLinkLocationOwnerToken Owner)> PublishActorNodeAsync(
        ZLinkInMemoryLocationStore store,
        string ownerId,
        RoutingId rid,
        ulong lifecycleGeneration)
    {
        var owner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(ownerId, TimeSpan.FromMinutes(5)))
            .Token;
        var descriptor = new ZLinkMeshNodeDescriptor(
            "mesh",
            rid,
            lifecycleGeneration,
            1,
            $"tcp://127.0.0.1:{FindFreeTcpPort()}",
            new Dictionary<string, int>(StringComparer.Ordinal),
        ZLinkTransportSecurityIdentity.Plaintext,
            owner.OwnerId,
            owner.LeaseGeneration,
            DateTimeOffset.UtcNow)
        {
            EntrySpotId = $"{rid}-entry-{Guid.NewGuid():D}",
            State = ZLinkFrameworkRuntimeState.Serving,
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Recreate,
                    false,
                    0)
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 100),
                new ZLinkPopulationCapacity(0, 0, 100),
                [])
        };
        var write = await store.UpdateMeshNodeAsync(
            descriptor,
            ZLinkLocationWriteIntent.NewClaim);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, write.Status);
        return (descriptor, owner);
    }

    private static int FindFreeTcpPort()
    {
        using var listener = new System.Net.Sockets.TcpListener(
            System.Net.IPAddress.Loopback,
            0);
        listener.Start();
        return ((System.Net.IPEndPoint)listener.LocalEndpoint).Port;
    }

    private static ZLinkActorHandoffFrame AcceptedFrame(
        long arrivalIndex,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource = null,
        byte messageFollowHopCount = 0)
    {
        var header = ZLinkStreamProtocolDefaults.EncodeHeader(
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                default,
                "packet",
                ZlinkStreamMetadata.Empty));
        return new ZLinkActorHandoffFrame(
            [],
            0,
            RoutingId.From("source").ToBytes().ToArray(),
            [],
            checked((ulong)arrivalIndex),
            0,
            header.ToArray(),
            [(byte)(arrivalIndex % byte.MaxValue)],
            arrivalIndex,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(1, checked((ulong)arrivalIndex)),
                messageFollowHopCount,
                7,
                11,
                3),
            7,
            requestSource,
            CanonicalEncodedLength: 1);
    }

    private static ZLinkActorHandoffFrame AcceptedRequestFrame(
        ZLinkServiceWireCodec.RequestSourceFence source,
        ulong replyRouteId,
        int ordinal = 1)
    {
        var header = ZLinkStreamProtocolDefaults.EncodeHeader(
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                new ZlinkStreamRequestSeq(checked((ulong)ordinal)),
                "request",
                ZlinkStreamMetadata.Empty));
        return new ZLinkActorHandoffFrame(
            [],
            0,
            source.NodeRid.ToBytes().ToArray(),
            [],
            checked((ulong)(70 + ordinal)),
            1,
            header.ToArray(),
            [checked((byte)(8 + ordinal))],
            ordinal,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(81, checked((ulong)(81 + ordinal))),
                0,
                7,
                11,
                3,
                ReplyRequestId: replyRouteId),
            source.NodeGeneration,
            source,
            replyRouteId);
    }

    private static ZLinkActorAcceptedRecord[] AcceptedRecords(
        ZLinkServiceWireCodec.RequestSourceFence source) =>
        [new ZLinkActorAcceptedRecord(
            AcceptedRequestFrame(source, replyRouteId: 91),
            source)];

    private static ZLinkMeshNodeDescriptor RequestSourceDescriptor(
        ZLinkServiceWireCodec.RequestSourceFence source) => new(
        "mesh",
        source.NodeRid,
        source.NodeGeneration,
        1,
        "tcp://127.0.0.1:1",
        new Dictionary<string, int>(),
        "plain",
        source.OwnerId,
        checked((long)source.LeaseGeneration),
        DateTimeOffset.UtcNow);

    private static MeshNodeStatus LocalNodeStatus() => new(
        MeshNodeState.Ready,
        RoutingId.From("relocation-source"),
        "mesh",
        "tcp://127.0.0.1:2",
        11,
        1,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0);

    private static MeshNodePeer AdmittedPeer(
        RoutingId rid,
        ulong lifecycleGeneration) => new(
        1,
        MeshPeerSource.Discovery,
        MeshPeerState.Admitted,
        rid,
        lifecycleGeneration,
        1,
        "tcp://127.0.0.1:3",
        0,
        0,
        0);

    private sealed record ReplayProgressSeed(
        ZLinkRelocationEnvelope Identity,
        ZLinkRelocationParticipantEnvelope Participant,
        ZLinkAuthoritySnapshot Snapshot,
        ZLinkLocationOwnerToken Owner);

    private static async ValueTask<ReplayProgressSeed>
        CreateReplayProgressSeedAsync(
        int acceptedCount,
        ProgressRelocationStore relocationStore)
    {
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner", 13, RoutingId.From("caller"), 17);
        var accepted = Enumerable.Range(1, acceptedCount)
            .Select(index => new ZLinkActorAcceptedRecord(
                AcceptedRequestFrame(
                    requestSource,
                    checked(90UL + (ulong)index),
                    index),
                requestSource))
            .ToArray();
        var identity = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
                SourceAuthority(),
                SourceActorAuthority(),
                TargetDescriptor(),
                Guid.NewGuid(),
                ReadOnlyMemory<byte>.Empty,
                accepted,
                default),
            applicationVersion: 7);
        var stored = await ZLinkRelocationTreeStore.PutAsync(
            relocationStore,
            identity,
            TimeSpan.FromHours(24),
            CancellationToken.None);
        var participant = Assert.Single(identity.Participants);
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            participant.RecoveryPayload.Span);
        Assert.True(ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
            recovery.AuthorityPayload.Span,
            out var phase));
        phase = phase with { Phase = ZLinkActorRelocationAuthorityPhase.Cleaning };
        var publication = new ZLinkRelocationAuthorityPayload(
            stored.Root.Reference,
            stored.Root.ChecksumCrc32c,
            identity.AggregateId,
            identity.AggregateGeneration,
            identity.InventoryDigest,
            "target-owner",
            4,
            ZLinkActorRelocationAuthorityPayloadCodec.Encode(phase))
        {
            IsCanonical = true,
            ApplicationVersion = 7
        };
        var snapshot = new ZLinkAuthoritySnapshot(
            "1",
            ZLinkRelocationAuthorityPayloadCodec.Encode(publication),
            participant.ObjectGeneration,
            checked(participant.AuthorityOwnerGeneration + 1),
            "target-owner",
            4,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.Active,
                ZLinkPlacementObjectKind.Actor,
                "player",
                new ZLinkMeshNodeDescriptorKey(
                    "mesh",
                    RoutingId.From("target")),
                8,
                new ZLinkCapacityVector(1, 0, null)),
            null,
            DateTimeOffset.UtcNow);
        return new ReplayProgressSeed(
            identity,
            participant,
            snapshot,
            new ZLinkLocationOwnerToken("target-owner", 4));
    }

    private sealed class ProgressAuthorityStore(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot snapshot,
        int synchronizedCompareExchanges = 0,
        bool loseFirstStoredResponse = false)
        : global::Zlink.Framework.UnitTests.ZLinkLocationStoreTestDouble
    {
        private ZLinkAuthoritySnapshot _snapshot = snapshot;
        private int _version = int.Parse(snapshot.StoreVersion);
        private readonly object _gate = new();
        private readonly TaskCompletionSource _compareBarrier = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private int _compareArrivals;
        private int _lostResponse;

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey requested,
            CancellationToken cancellationToken = default)
        {
            lock (_gate)
                return ValueTask.FromResult<ZLinkAuthorityReadResult>(
                requested == key
                    ? new ZLinkAuthorityReadResult.Found(_snapshot)
                    : new ZLinkAuthorityReadResult.Missing(DateTimeOffset.UtcNow));
        }

        public override async ValueTask<ZLinkAuthorityCompareExchangeResult>
            CompareExchangeAuthorityAsync(
                ZLinkAuthorityKey requested,
                string expectedStoreVersion,
                ZLinkAuthorityMutation mutation,
                CancellationToken cancellationToken = default)
        {
            if (synchronizedCompareExchanges > 0
                && Interlocked.Increment(ref _compareArrivals)
                   <= synchronizedCompareExchanges)
            {
                if (Volatile.Read(ref _compareArrivals)
                    >= synchronizedCompareExchanges)
                    _compareBarrier.TrySetResult();
                await _compareBarrier.Task.WaitAsync(cancellationToken);
            }

            ZLinkAuthorityCompareExchangeResult result;
            lock (_gate)
            {
                if (requested != key
                    || expectedStoreVersion != _snapshot.StoreVersion
                    || mutation is not ZLinkAuthorityMutation.Put put)
                    result = new ZLinkAuthorityCompareExchangeResult.Conflict(
                        new ZLinkAuthorityReadResult.Found(_snapshot));
                else
                {
                    _snapshot = _snapshot with
                    {
                        StoreVersion = (++_version).ToString(),
                        Payload = put.Payload.ToArray()
                    };
                    result = new ZLinkAuthorityCompareExchangeResult.Stored(_snapshot);
                }
            }
            if (result is ZLinkAuthorityCompareExchangeResult.Stored
                && loseFirstStoredResponse
                && Interlocked.Exchange(ref _lostResponse, 1) == 0)
                throw new IOException("The stored CAS response was lost.");
            return result;
        }

    }

    private sealed class ProgressRelocationStore(
        bool contentAddressed = false) : IZLinkRelocationRepository
    {
        private int _next;
        internal System.Collections.Concurrent.ConcurrentDictionary<string, byte[]>
            Payloads { get; } = [];

        public ValueTask<ZLinkRelocationStored> PutRelocationAsync(
            ReadOnlyMemory<byte> payload, TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            var reference = contentAddressed
                ? $"root-{Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(payload.Span))}"
                : $"root-{Interlocked.Increment(ref _next)}";
            Payloads[reference] = payload.ToArray();
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult(new ZLinkRelocationStored(
                reference,
                ZLinkCrc32C.Compute(payload.Span),
                now + retention,
                now));
        }

        public ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
            string reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            ArgumentException.ThrowIfNullOrWhiteSpace(reference);
            Payloads[reference] = payload.ToArray();
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult(new ZLinkRelocationStored(
                reference,
                ZLinkCrc32C.Compute(payload.Span),
                now + retention,
                now));
        }

        public ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<ZLinkRelocationReadResult>(
                Payloads.TryGetValue(reference, out var payload)
                    ? new ZLinkRelocationReadResult.Found(payload)
                    : new ZLinkRelocationReadResult.Missing());

        public ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
            string reference, TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult<ZLinkRelocationRenewResult>(
                Payloads.ContainsKey(reference)
                    ? new ZLinkRelocationRenewResult.Renewed(
                        now + retention, now)
                    : new ZLinkRelocationRenewResult.Missing());
        }

        public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(
                Payloads.TryRemove(reference, out _)
                    ? ZLinkRelocationDeleteResult.Deleted
                    : ZLinkRelocationDeleteResult.Missing);
    }

    private sealed class HostedRecoveryActor(
        string actorId,
        IZLinkActorContext context) : IZLinkActor
    {
        public string ActorId { get; } = actorId;
        public IZLinkActorContext Context { get; } = context;
    }

    private sealed class HostedRecoveryActorFactory
        : IZLinkActorFactory<HostedRecoveryActor>
    {
        private static int _created;

        internal static int CreatedCount => Volatile.Read(ref _created);

        internal static void Reset() => Volatile.Write(ref _created, 0);

        public ValueTask<HostedRecoveryActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default)
        {
            Interlocked.Increment(ref _created);
            return ValueTask.FromResult(
                new HostedRecoveryActor(context.ActorId, context));
        }
    }
}
