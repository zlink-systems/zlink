using System.Buffers.Binary;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Configuration.Builders;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Streams;
using Zlink.Framework.LocationProvider;

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
    public void Direct_post_admission_ingress_orders_after_replayed_journal_and_held_ingress()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCanonicalMaintenanceImport(
            "handoff",
            [AcceptedFrame(1)]);
        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        _ = handoff.PrepareCanonicalMaintenanceReplay("handoff");
        // The staged journal replays and acknowledges before the
        // trailing-reserve-then-open primitive runs from the publish path.
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
        using var held = ZLinkActorHandoffFrames.Restore(
            actor,
            [AcceptedFrame(2, source, messageFollowHopCount: 1)]);
        using var directBeforeOpen = ZLinkActorHandoffFrames.Restore(
            actor,
            [AcceptedFrame(3, source)]);

        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(held[0]));
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(directBeforeOpen[0]));

        // Opening admission reserves the followed held ingress before direct
        // ingress captured while sealed, both behind the acknowledged
        // journal. A direct request arriving after admission cannot be
        // captured any more: it joins the queue strictly behind every
        // reserved frame.
        var reserved = new List<ulong>();
        handoff.ReserveCanonicalMaintenanceTrailingAndOpenAdmission(
            "handoff",
            1,
            frame => reserved.Add(frame.RequestId));
        Assert.Equal([2UL, 3UL], reserved);

        using var directAfterOpen = ZLinkActorHandoffFrames.Restore(
            actor,
            [AcceptedFrame(4, source)]);
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.NotSealed,
            handoff.TryCapture(directAfterOpen[0]));
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
    public void Canonical_target_import_accepts_saved_relay_and_temporary_backlog_without_a_local_cap()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);

        // The saved journal plus relay/temporary tail is one durable aggregate;
        // the shared live-admission policy is applied only as each item runs.
        handoff.BeginCanonicalMaintenanceImport(
            "handoff",
            [AcceptedFrame(1), AcceptedFrame(2)]);
        handoff.AppendCanonicalMaintenanceImport(
            "handoff",
            [AcceptedFrame(3), AcceptedFrame(4)]);

        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        _ = handoff.PrepareCanonicalMaintenanceReplay("handoff");
        Assert.Equal(
            new long[] { 1, 2, 3, 4 },
            handoff.SnapshotFinalReplay()
                .Select(static frame => frame.ArrivalIndex));
    }

    [Fact]
    public void Canonical_temporary_target_backlog_does_not_clone_live_capacity_policy()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCanonicalMaintenanceImport(
            "handoff",
            [AcceptedFrame(1)]);
        handoff.MarkAuthorityCommitted("handoff", 42, 42);
        _ = handoff.PrepareCanonicalMaintenanceReplay("handoff");
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            "source-owner",
            3,
            RoutingId.From("source"),
            7);
        var actor = new ZLinkBackendActorRef(
            RoutingId.From("target"),
            "actor-1",
            42);
        using var first = ZLinkActorHandoffFrames.Restore(
            actor,
            [AcceptedFrame(2, source)]);
        using var second = ZLinkActorHandoffFrames.Restore(
            actor,
            [AcceptedFrame(3, source)]);

        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(first[0]));
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(second[0]));

        handoff.AcknowledgeCanonicalReplayThrough(1);
        handoff.AcknowledgeReplayedFrame(2);
        handoff.AcknowledgeReplayedFrame(3);
        Assert.True(
            handoff.TryOpenCanonicalMaintenanceAdmission("handoff", 3));
        Assert.True(
            handoff.TryCompleteCanonicalMaintenanceReplay("handoff"));
    }

    [Fact]
    public async Task Canonical_durable_backlog_queues_fifo_one_shared_permit_per_turn()
    {
        using var queue = new ZLinkApplicationJobQueue(
            new ZLinkApplicationJobQueueCapacity(
                ZLinkApplicationJobQueueProfile.Balanced,
                ConfiguredManualMax: 1,
                EffectiveProcessorCount: 8,
                EffectiveMaxQueuedApplicationJobs: 1));
        using var serialTurn = new SemaphoreSlim(1, 1);
        var invoked = Enumerable.Range(0, 3)
            .Select(static _ => new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously))
            .ToArray();
        var started = Enumerable.Range(0, 3)
            .Select(static _ => new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously))
            .ToArray();
        var finish = Enumerable.Range(0, 3)
            .Select(static _ => new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously))
            .ToArray();
        var order = new System.Collections.Concurrent.ConcurrentQueue<int>();
        var completions = new List<Task>();
        var active = 0;
        var maxActive = 0;

        async Task Dispatch(int item)
        {
            invoked[item - 1].TrySetResult();
            await serialTurn.WaitAsync();
            try
            {
                ZLinkApplicationJobQueueInvocation.ReleaseForHandlerStart();
                var nowActive = Interlocked.Increment(ref active);
                maxActive = Math.Max(maxActive, nowActive);
                order.Enqueue(item);
                started[item - 1].TrySetResult();
                await finish[item - 1].Task;
            }
            finally
            {
                Interlocked.Decrement(ref active);
                serialTurn.Release();
            }
        }

        var scheduling = ZLinkFrameworkRuntime
            .QueueDurableBacklogWithSharedPermitsAsync(
                queue,
                new[] { 1, 2, 3 },
                Dispatch,
                completions,
                CancellationToken.None)
            .AsTask();

        await started[0].Task.WaitAsync(TimeSpan.FromSeconds(1));
        await invoked[1].Task.WaitAsync(TimeSpan.FromSeconds(1));
        for (var attempt = 0;
             attempt < 100 && queue.GetStatus().CapacityWaiters != 1;
             attempt++)
            await Task.Delay(10);
        Assert.False(scheduling.IsCompleted);
        Assert.False(invoked[2].Task.IsCompleted);
        Assert.Equal(1UL, queue.GetStatus().QueuedApplicationJobs);
        Assert.Equal(1UL, queue.GetStatus().CapacityWaiters);
        Assert.Equal(new[] { 1 }, order.ToArray());

        finish[0].TrySetResult();
        await started[1].Task.WaitAsync(TimeSpan.FromSeconds(1));
        await invoked[2].Task.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.Equal(new[] { 1, 2 }, order.ToArray());

        finish[1].TrySetResult();
        await started[2].Task.WaitAsync(TimeSpan.FromSeconds(1));
        finish[2].TrySetResult();
        await scheduling.WaitAsync(TimeSpan.FromSeconds(1));
        await Task.WhenAll(completions).WaitAsync(TimeSpan.FromSeconds(1));

        Assert.Equal(new[] { 1, 2, 3 }, order.ToArray());
        Assert.Equal(1, maxActive);
        Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
    }

    [Fact]
    public async Task Canonical_durable_backlog_failure_returns_the_shared_permit()
    {
        using var queue = new ZLinkApplicationJobQueue(
            new ZLinkApplicationJobQueueCapacity(
                ZLinkApplicationJobQueueProfile.Balanced,
                ConfiguredManualMax: 1,
                EffectiveProcessorCount: 8,
                EffectiveMaxQueuedApplicationJobs: 1));
        var completions = new List<Task>();

        await ZLinkFrameworkRuntime
            .QueueDurableBacklogWithSharedPermitsAsync(
                queue,
                new[] { 1 },
                static _ => Task.FromException(
                    new InvalidOperationException("dispatch failed")),
                completions,
                CancellationToken.None);

        await Assert.ThrowsAsync<InvalidOperationException>(
            () => Task.WhenAll(completions));
        Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
        Assert.Equal(0UL, queue.GetStatus().QueuedApplicationJobs);
    }

    [Fact]
    public void Captured_target_payload_transfers_to_durable_bytes_and_returns_raw_owners_exactly_once()
    {
        using var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration
        {
            ImplicitHandlerAutoRegistrationEnabled = false
        };
        registration.FreezeScannedHandlerCatalog();
        var runtime = new ZLinkFrameworkRuntime(
            services,
            null!,
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var state = runtime.GetOrCreateActorState("actor-1");
        state.Handoff.BeginCapture();
        var actor = new ZLinkBackendActorRef(
            RoutingId.From("target"),
            "actor-1",
            42);
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            "source-owner",
            3,
            RoutingId.From("source"),
            7);
        var route = new ZLinkBackendActorRouteContext(
            new MeshOperationId(1, 1),
            0,
            7,
            11,
            3);
        var header = Message.From(
            ZLinkStreamProtocolDefaults.EncodeHeader(
                new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "captured",
                    ZlinkStreamMetadata.Empty)).Span);
        var body = Message.From("retained");
        var creditOwner = new DisposeProbe();
        var parts = new[]
        {
            new ZLinkBackendActorPart(
                actor,
                source.NodeRid,
                default,
                0,
                0,
                header,
                true,
                RouteContext: route,
                SourceNodeGeneration: source.NodeGeneration,
                RequestSource: source),
            new ZLinkBackendActorPart(
                actor,
                source.NodeRid,
                default,
                0,
                0,
                body,
                false)
        };

        var batch = ZLinkActorHandoffIngress.CaptureMovingFrames(
            runtime,
            parts,
            creditOwner);

        Assert.Equal(0, batch.Count);
        Assert.True(IsDisposed(header));
        Assert.True(IsDisposed(body));
        Assert.Equal(0, creditOwner.DisposeCount);
        Assert.Equal("retained", System.Text.Encoding.UTF8.GetString(
            Assert.Single(state.Handoff.SnapshotFrames()).Body));

        batch.Dispose();
        batch.Dispose();
        Assert.Equal(1, creditOwner.DisposeCount);

        var rejectedHeader = Message.From(
            ZLinkStreamProtocolDefaults.EncodeHeader(
                new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "rejected",
                    ZlinkStreamMetadata.Empty)).Span);
        var rejectedBody = Message.From("rejected-retained");
        var rejectedCreditOwner = new DisposeProbe();
        var rejectedParts = new[]
        {
            new ZLinkBackendActorPart(
                actor,
                source.NodeRid,
                default,
                0,
                0,
                rejectedHeader,
                true,
                RouteContext: route,
                SourceNodeGeneration: source.NodeGeneration),
            new ZLinkBackendActorPart(
                actor,
                source.NodeRid,
                default,
                0,
                0,
                rejectedBody,
                false)
        };

        Assert.Throws<ZLinkActorHandoffRejectedException>(() =>
            ZLinkActorHandoffIngress.CaptureMovingFrames(
                runtime,
                rejectedParts,
                rejectedCreditOwner));
        Assert.True(IsDisposed(rejectedHeader));
        Assert.True(IsDisposed(rejectedBody));
        Assert.Equal(1, rejectedCreditOwner.DisposeCount);
        state.Handoff.Reset();
    }

    [Fact]
    public void Canonical_target_import_accepts_a_large_durable_backlog()
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
            initial);
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
            firstTarget.OwnerId,
            firstTarget.OwnerLeaseGeneration,
            firstTarget.NodeRid.ToHex(),
            firstTarget.NodeGeneration,
            4,
            "root-stable",
            17,
            7);
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
    public void Canonical_root_preserves_saved_request_route()
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
        var obsoleteTerminalVector = ZLinkRelocationEnvelopeCodec
            .Encode(canonical)
            .Concat(new byte[sizeof(uint)])
            .ToArray();
        Assert.Throws<InvalidDataException>(() =>
            ZLinkRelocationEnvelopeCodec.Decode(obsoleteTerminalVector));
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
    public void Live_reply_payload_preserves_reply_route()
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
            0)
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
    public async Task Canonical_target_normalizes_from_activated_without_source_mutation()
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
        var targetAuthority = SourceActorAuthority() with
        {
            State = ZLinkActorAuthorityState.Ready,
            CurrentSpotId = target.EntrySpotId!,
            CurrentSpotGeneration = target.LifecycleGeneration,
            CurrentSpotKind = ZLinkSpotKind.Entry,
            OwnerId = target.OwnerId,
            OwnerLeaseGeneration = checked((ulong)target.LeaseGeneration),
            MeshName = target.MeshName,
            NodeRid = target.Rid,
            NodeGeneration = target.LifecycleGeneration
        };
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
                    target.OwnerId,
                    checked((ulong)target.LeaseGeneration),
                    target.Rid.ToHex(),
                    target.LifecycleGeneration,
                    4,
                    stored.Root.Reference,
                    stored.Root.ChecksumCrc32c,
                    7),
                identity);
        var authorityStore = new ProgressAuthorityStore(
            participant.AuthorityKey,
            PublishedSnapshot(canonicalPayload, targetAuthority, 12));
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
        var activatedAuthority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await authorityStore.ReadAuthorityAsync(participant.AuthorityKey));
        var observedVersion = activatedAuthority.Snapshot.StoreVersion;
        Assert.True(
            ZLinkStandaloneActorRelocationRuntime
                .IsExactCommittedTargetAuthority(
                    activatedAuthority,
                    SourceAuthority(),
                    activatedRoot,
                    relocationId,
                    target,
                    1,
                    requireActivated: true));
        Assert.Equal(observedVersion,
            Assert.IsType<ZLinkAuthorityReadResult.Found>(
                    await authorityStore.ReadAuthorityAsync(
                        participant.AuthorityKey))
                .Snapshot.StoreVersion);
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
                stale.ReadAsync(
                        identity,
                        owner,
                        CancellationToken.None)
                    .AsTask());
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
            Assert.Equal(version, Assert.IsType<ZLinkAuthorityReadResult.Found>(
                    await authorityStore.ReadAuthorityAsync(participant.AuthorityKey))
                .Snapshot.StoreVersion);
        }
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
        IZLinkLocationRepository store,
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

    private static bool IsDisposed(Message message)
    {
        try
        {
            _ = message.Size;
            return false;
        }
        catch (ObjectDisposedException)
        {
            return true;
        }
    }

    private sealed class DisposeProbe : IDisposable
    {
        private int _disposeCount;

        internal int DisposeCount => Volatile.Read(ref _disposeCount);

        public void Dispose() => Interlocked.Increment(ref _disposeCount);
    }


    private sealed class ProgressAuthorityStore(
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot snapshot)
        : global::Zlink.Framework.UnitTests.ZLinkLocationStoreTestDouble
    {
        private ZLinkAuthoritySnapshot _snapshot = snapshot;
        private int _version = int.Parse(snapshot.StoreVersion);
        private readonly object _gate = new();

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

        public override ValueTask<ZLinkAuthorityCompareExchangeResult>
            CompareExchangeAuthorityAsync(
                ZLinkAuthorityKey requested,
                string expectedStoreVersion,
                ZLinkAuthorityMutation mutation,
                CancellationToken cancellationToken = default)
        {
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
            return ValueTask.FromResult(result);
        }

    }

    private sealed class ProgressRelocationStore :
        IZLinkRelocationRepository,
        IZLinkRelocationStore
    {
        private int _next;
        internal System.Collections.Concurrent.ConcurrentDictionary<string, byte[]>
            Payloads { get; } = [];

        public ValueTask<ZLinkBlobPutResult> PutAsync(
            ZLinkBlobReference reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var now = DateTimeOffset.UtcNow;
            var expiresAt = now + retention;
            if (Payloads.TryGetValue(reference.Value, out var current))
            {
                return ValueTask.FromResult<ZLinkBlobPutResult>(
                    current.AsSpan().SequenceEqual(payload.Span)
                        ? new ZLinkBlobPutResult.AlreadyStored(expiresAt, now)
                        : new ZLinkBlobPutResult.Conflict(now));
            }
            return ValueTask.FromResult<ZLinkBlobPutResult>(
                Payloads.TryAdd(reference.Value, payload.ToArray())
                    ? new ZLinkBlobPutResult.Stored(expiresAt, now)
                    : new ZLinkBlobPutResult.Conflict(now));
        }

        public ValueTask<ZLinkBlobReadResult> ReadAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult<ZLinkBlobReadResult>(
                Payloads.TryGetValue(reference.Value, out var payload)
                    ? new ZLinkBlobReadResult.Found(
                        payload,
                        now + TimeSpan.FromHours(24),
                        now)
                    : new ZLinkBlobReadResult.Missing(now));
        }

        public ValueTask<ZLinkBlobRenewResult> RenewAsync(
            ZLinkBlobReference reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult<ZLinkBlobRenewResult>(
                Payloads.ContainsKey(reference.Value)
                    ? new ZLinkBlobRenewResult.Renewed(
                        now + retention,
                        now)
                    : new ZLinkBlobRenewResult.Missing(now));
        }

        public ValueTask DeleteAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Payloads.TryRemove(reference.Value, out _);
            return ValueTask.CompletedTask;
        }

        public ValueTask<ZLinkRelocationStored> PutRelocationAsync(
            ReadOnlyMemory<byte> payload, TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            var reference = $"root-{Interlocked.Increment(ref _next)}";
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

    [Fact]
    public void PreserveStateWith_wires_the_plain_relocation_adapter_invoker()
    {
        var builder = new ZLinkActorFactoryBuilder<TestActor>();
        builder.PreserveStateWith<PlainRelocationAdapter>();

        Assert.IsType<ZLinkActorRelocationAdapterInvoker<TestActor>>(
            builder.Relocation.AdapterInvoker);
    }

    private sealed class PlainRelocationAdapter
        : IZLinkActorRelocationAdapter<TestActor>
    {
        public ValueTask<byte[]> CaptureAsync(
            TestActor actor, CancellationToken cancellationToken) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask RestoreAsync(
            TestActor actor,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }
}
