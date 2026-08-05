using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ActorHandoffTests
{
    [Fact]
    public void DeferredJoinCapture_PromotesQueuedFrameIntoCrossNodeJournal()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginDeferredJoinCapture();
        using var body = Message.From("queued-after-defer");
        using var frame = Frame(body, ActorRef("node-a", 1), "session-1");

        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(frame));

        handoff.BeginCapture();
        var journal = handoff.SnapshotFrames();
        Assert.Single(journal);
        Assert.Equal("queued-after-defer", DecodeBody(journal[0]));
        _ = handoff.AbortCapture();
    }

    [Fact]
    public void DeferredJoinCapture_ReleasesQueuedFrameForLocalReplay()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginDeferredJoinCapture();
        using var body = Message.From("queued-after-defer");
        using var frame = Frame(body, ActorRef("node-a", 1), "session-1");
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(frame));

        var replay = handoff.EndDeferredJoinCapture();

        Assert.Single(replay);
        Assert.Equal("queued-after-defer", DecodeBody(replay[0]));
        Assert.Empty(handoff.EndDeferredJoinCapture());
    }

    [Fact]
    public void BoundSessionOneWayDuringDeferredJoin_IsCapturedWithExactBindingFence()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.BeginDeferredJoinCapture();
        using var body = Message.From("one-way");
        var sourceNode = RoutingId.From("session-node");
        var sourceSession = RoutingId.From("session-1");
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            "session-owner", 11, sourceNode, 13);
        var binding = new ZLinkActorBoundSessionHandoffFence(
            "actor-1", 1, sourceSession,
            "binding-token", 17, 19);
        using var frame = new ZLinkSpotActorFrame(
            ActorRef("node-a", 1),
            ActorRef("node-a", 1),
            sourceNode,
            sourceSession,
            requestId: 0,
            flags: 0,
            routeContext: new ZLinkBackendActorRouteContext(
                new MeshOperationId(13, 19), 0, 23, 29, 31,
                IsBoundSessionRoute: true),
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                null,
                "Packet",
                ZlinkStreamMetadata.Empty),
            Message.From(body.AsReadOnlySpan()),
            sourceNodeGeneration: source.NodeGeneration,
            requestSource: source,
            applicationMetadata:
                ZLinkActorBoundSessionHandoffMetadata.Encode(binding));

        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            state.Handoff.TryCapture(frame));
        state.Handoff.BeginCapture();
        var captured = Assert.Single(state.Handoff.SnapshotFrames());
        Assert.Equal(binding, captured.BoundSessionSource);
        Assert.Equal(source, captured.RequestSource);
        Assert.Equal("one-way", DecodeBody(captured));
        _ = state.Handoff.AbortCapture();
    }

    [Fact]
    public void BoundSessionRequestDuringTargetReplay_IsCapturedWithExactBindingFence()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        Assert.True(state.Handoff.Import(CommitRequest("handoff-target-request", []), out _));
        _ = state.Handoff.PrepareImportedReplay([]);

        var sourceNode = RoutingId.From("session-node");
        var sourceSession = RoutingId.From("session-1");
        var source = new ZLinkServiceWireCodec.RequestSourceFence(
            "session-owner", 11, sourceNode, 13);
        var binding = new ZLinkActorBoundSessionHandoffFence(
            "actor-1", 1, sourceSession,
            "binding-token", 17, 19);
        using var frame = new ZLinkSpotActorFrame(
            ActorRef("node-b", 1),
            ActorRef("node-b", 1),
            sourceNode,
            sourceSession,
            requestId: 23,
            flags: 1,
            routeContext: new ZLinkBackendActorRouteContext(
                new MeshOperationId(13, 19), 0, 23, 29, 31,
                ReplyRequestId: 23,
                ReplyFlags: 1,
                IsBoundSessionRoute: true),
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                new ZlinkStreamRequestSeq(23),
                "Packet",
                ZlinkStreamMetadata.Empty),
            Message.From("request"u8.ToArray()),
            sourceNodeGeneration: source.NodeGeneration,
            requestSource: source,
            applicationMetadata:
                ZLinkActorBoundSessionHandoffMetadata.Encode(binding));

        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            state.Handoff.TryCapture(frame));

        var replay = state.Handoff.SnapshotFinalReplay();
        var captured = Assert.Single(replay);
        Assert.Equal((ulong)23, captured.RequestId);
        Assert.Equal(1U, captured.Flags);
        Assert.Equal(binding, captured.BoundSessionSource);
        Assert.Equal(source, captured.RequestSource);
        Assert.Equal((ulong)23, captured.RelocationReplyRouteId);
        Assert.Equal("request", DecodeBody(captured));
    }

    [Fact]
    public void TargetReplayCompletion_ClosesCaptureAtTheEmptyBoundary()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        Assert.True(handoff.Import(CommitRequest("handoff-1", []), out _));
        _ = handoff.PrepareImportedReplay([]);

        using var body = Message.From("queued-during-replay");
        using var frame = Frame(body, ActorRef("node-b", 1), "session-1");
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(frame));

        var replay = handoff.SnapshotFinalReplay();
        Assert.Single(replay);
        Assert.False(handoff.TryCompleteTransferredActorReplay("handoff-1"));

        handoff.AcknowledgeReplayedFrame();
        Assert.True(handoff.TryCompleteTransferredActorReplay("handoff-1"));

        using var postCompletionBody = Message.From("after-replay");
        using var postCompletion = Frame(
            postCompletionBody,
            ActorRef("node-b", 1),
            "session-2");
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.NotSealed,
            handoff.TryCapture(postCompletion));
        Assert.Empty(handoff.SnapshotFinalReplay());
    }

    [Fact]
    public void CaptureFenceFailureDoesNotReserveTheSourceOwnedReplyRoute()
    {
        var state = new ZLinkActorRuntimeState("actor-invalid-source-fence");
        state.Handoff.BeginCapture();
        var actor = ActorRef("node-a", 1);
        var sourceNodeRid = RoutingId.From("source-node");
        using var invalid = new ZLinkSpotActorFrame(
            actor,
            actor,
            sourceNodeRid,
            default,
            requestId: 29,
            flags: 0,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(31, 29),
                MessageFollowHopCount: 0,
                TargetNodeGeneration: 37,
                AuthorityOwnerGeneration: 41,
                OwnerLeaseGeneration: 43,
                ReplyRequestId: 29,
                ReplyFlags: 1),
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                new ZlinkStreamRequestSeq(47),
                "Packet",
                ZlinkStreamMetadata.Empty),
            Message.From(new byte[] { 53 }),
            sourceNodeGeneration: 59,
            new ZLinkServiceWireCodec.RequestSourceFence(
                "source-owner",
                61,
                sourceNodeRid,
                NodeGeneration: 67));
        ZLinkActorInboundPipeline.EnsureRelocationReplyRoute(invalid);
        Assert.Equal(29UL, invalid.RelocationReplyRouteId);
        Assert.Throws<ZLinkActorHandoffRejectedException>(
            () => state.Handoff.TryCapture(invalid));
        Assert.Empty(state.Handoff.SnapshotFrames());

        // The route is owned by the caller pending operation, not by a target
        // registry. A failed capture therefore leaves no receiver reservation
        // that could reject a later valid frame with the same correlation.
        using var valid = new ZLinkSpotActorFrame(
            actor,
            actor,
            sourceNodeRid,
            default,
            requestId: 29,
            flags: 1,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(59, 29),
                MessageFollowHopCount: 0,
                TargetNodeGeneration: 37,
                AuthorityOwnerGeneration: 41,
                OwnerLeaseGeneration: 43,
                ReplyRequestId: 29,
                ReplyFlags: 1),
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Request,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.HasRequestSeq,
                new ZlinkStreamRequestSeq(47),
                "Packet",
                ZlinkStreamMetadata.Empty),
            Message.From(new byte[] { 71 }),
            sourceNodeGeneration: 59,
            new ZLinkServiceWireCodec.RequestSourceFence(
                "source-owner",
                61,
                sourceNodeRid,
                NodeGeneration: 59));
        ZLinkActorInboundPipeline.EnsureRelocationReplyRoute(valid);
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            state.Handoff.TryCapture(valid));
        Assert.Single(state.Handoff.SnapshotFrames());
    }

    [Fact]
    public void SourceIngressHold_Uses1024RecordsAnd16MiBContractDefaults()
    {
        Assert.Equal(
            1_024,
            ZLinkBoundedIngressAdmission.SourceIngressHoldRecordCapacity);
        Assert.Equal(
            16L * 1024 * 1024,
            ZLinkBoundedIngressAdmission.SourceIngressHoldByteCapacity);

        var admission = new ZLinkBoundedIngressAdmission();
        for (var record = 0; record < 1_024; record++)
            Assert.True(admission.TryAcquire(1));
        Assert.False(admission.TryAcquire(0));
        Assert.Equal((1_024, 1_024L), admission.Snapshot());
    }

    [Fact]
    public void SourceIngressHold_RecordBoundaryReturnsFullWithoutTakingOwnership()
    {
        var admission = new ZLinkBoundedIngressAdmission(
            recordCapacity: 2,
            byteCapacity: long.MaxValue);
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System,
            sourceIngressAdmission: admission);
        handoff.BeginCapture();
        using var body = Message.From("record");
        using var first = Frame(body, ActorRef("node-a", 1), "session-1");
        using var second = Frame(body, ActorRef("node-a", 1), "session-2");
        using var overflow = Frame(body, ActorRef("node-a", 1), "session-3");

        Assert.Equal(ZLinkActorHandoffCaptureResult.Captured, handoff.TryCapture(first));
        Assert.Equal(ZLinkActorHandoffCaptureResult.Captured, handoff.TryCapture(second));
        Assert.Equal(ZLinkActorHandoffCaptureResult.Full, handoff.TryCapture(overflow));
        var held = admission.Snapshot();
        Assert.Equal(2, held.Records);
        Assert.True(held.Bytes > 0);
        Assert.Equal(2, handoff.SnapshotFrames().Count);
    }

    [Fact]
    public void FinalJournalSeal_holds_later_ingress_in_the_commit_manifest()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        handoff.BeginCapture();
        using var body = Message.From("record");
        using var included = Frame(body, ActorRef("node-a", 1), "session-1");
        using var afterSeal = Frame(body, ActorRef("node-a", 1), "session-2");
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(included));

        handoff.SealCapture();
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(afterSeal));
        var boundary = handoff.FreezeCaptureCommitBoundary();

        Assert.Equal(2UL, boundary.AcceptedHighWater);
        Assert.Equal(2, boundary.Frames.Count);
        Assert.Equal(new long[] { 0, 1 },
            boundary.Frames.Select(static frame => frame.ArrivalIndex));
        _ = handoff.AbortCapture();
    }

    [Fact]
    public void FinalJournalHold_enforces_its_own_record_and_byte_bound()
    {
        var hold = new ZLinkBoundedIngressAdmission(
            recordCapacity: 1,
            byteCapacity: long.MaxValue);
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System,
            sourceHoldAdmission: hold);
        handoff.BeginCapture();
        handoff.SealCapture();
        using var body = Message.From("held");
        using var admitted = Frame(body, ActorRef("node-a", 1), "session-1");
        using var overflow = Frame(body, ActorRef("node-a", 1), "session-2");

        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(admitted));
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Full,
            handoff.TryCapture(overflow));
        Assert.Equal(1, hold.Snapshot().Records);
        Assert.Single(handoff.FreezeCaptureCommitBoundary().Frames);
        _ = handoff.AbortCapture();
        Assert.Equal((0, 0L), hold.Snapshot());
    }

    [Fact]
    public void FinalJournalAbort_restores_preseal_and_held_ingress_in_order()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.BeginCapture();
        Capture(state, "before-seal", "session-1");
        state.Handoff.SealCapture();
        Capture(state, "before-boundary", "session-1");
        var boundary = state.Handoff.FreezeCaptureCommitBoundary();
        Capture(state, "after-boundary", "session-1");

        Assert.Equal(2UL, boundary.AcceptedHighWater);
        Assert.Equal(
            ["before-seal", "before-boundary", "after-boundary"],
            state.Handoff.AbortCapture().Select(DecodeBody));
    }

    [Fact]
    public void FinalJournalAbort_keeps_dispatch_sealed_until_queue_restore_completes()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.BeginCapture();
        Capture(state, "before", "session-1");
        state.Handoff.SealCapture();
        Capture(state, "held", "session-1");

        var restored = state.Handoff.BeginAbortCaptureRestore();

        Assert.True(state.Handoff.BlocksLocalDispatch);
        Assert.Equal(new[] { "before", "held" }, restored
            .Select(DecodeBody));

        state.Handoff.AcknowledgeAbortRestoreEnqueued(
            restored[0].ArrivalIndex);
        var remaining = state.Handoff.BeginAbortCaptureRestore();
        Assert.Single(remaining);
        Assert.Equal("held", DecodeBody(remaining[0]));
        Assert.Throws<InvalidOperationException>(() =>
            state.Handoff.CompleteAbortCaptureRestore());
        Assert.True(state.Handoff.BlocksLocalDispatch);
        state.Handoff.AcknowledgeAbortRestoreEnqueued(
            remaining[0].ArrivalIndex);
        state.Handoff.CompleteAbortCaptureRestore();

        Assert.False(state.Handoff.BlocksLocalDispatch);
        Assert.False(state.Handoff.IsSourceMigrationInProgress);
    }

    [Fact]
    public void FinalJournalCutover_relays_only_the_post_boundary_suffix()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.BeginCapture();
        Capture(state, "before-seal", "session-1");
        state.Handoff.SealCapture();
        Capture(state, "before-boundary", "session-1");
        var boundary = state.Handoff.FreezeCaptureCommitBoundary();
        Capture(state, "after-boundary", "session-1");

        Assert.Throws<ArgumentOutOfRangeException>(() => Cutover(
            state,
            boundary.Frames.Count - 1,
            ActorRef("node-a", 1),
            ActorRef("node-b", 2)));

        var trailing = Cutover(
            state,
            boundary.Frames.Count,
            ActorRef("node-a", 1),
            ActorRef("node-b", 2));

        Assert.Equal(["after-boundary"], trailing.Select(DecodeBody));
        _ = state.Handoff.AbortCapture();
    }

    [Fact]
    public async Task FinalJournalRuntimeCrash_releases_the_bounded_hold()
    {
        var hold = new ZLinkBoundedIngressAdmission(
            recordCapacity: 1,
            byteCapacity: long.MaxValue);
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System,
            sourceHoldAdmission: hold);
        handoff.BeginCapture();
        handoff.SealCapture();
        using var body = Message.From("held");
        using var frame = Frame(body, ActorRef("node-a", 1), "session-1");
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(frame));
        _ = handoff.FreezeCaptureCommitBoundary();
        var completion = handoff.WaitForSourceCompletionAsync(
            CancellationToken.None);

        handoff.AbortRuntimeGeneration(new IOException("source crashed"));

        await Assert.ThrowsAsync<IOException>(() => completion);
        Assert.Equal((0, 0L), hold.Snapshot());
    }

    [Fact]
    public void TargetArrivalBacklog_uses_the_same_bounded_admission()
    {
        var admission = new ZLinkBoundedIngressAdmission(
            recordCapacity: 1,
            byteCapacity: long.MaxValue);
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System,
            targetIngressAdmission: admission);
        Assert.True(handoff.Import(CommitRequest("handoff-1", []), out _));
        using var body = Message.From("target-arrival");
        using var admitted = Frame(body, ActorRef("node-b", 1), "session-1");
        using var overflow = Frame(body, ActorRef("node-b", 1), "session-2");

        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(admitted));
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Full,
            handoff.TryCapture(overflow));
        Assert.Equal(1, admission.Snapshot().Records);
        Assert.Single(handoff.PrepareImportedReplay([]));
        handoff.AcknowledgeReplayedFrame();
        Assert.Equal((0, 0L), admission.Snapshot());
        handoff.Complete("handoff-1");
    }

    [Fact]
    public async Task DeferredJoinDuringTargetReplay_WaitsAndStartsSourceCaptureAfterCompletion()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        Assert.True(handoff.Import(CommitRequest("handoff-1", []), out _));
        _ = handoff.PrepareImportedReplay([]);

        var targetCompletion = handoff.BeginDeferredJoinCapture();

        Assert.NotNull(targetCompletion);
        Assert.False(targetCompletion.IsCompleted);
        handoff.Complete("handoff-1");
        await targetCompletion.WaitAsync(TimeSpan.FromSeconds(1));

        using var body = Message.From("after-target-completion");
        using var frame = Frame(body, ActorRef("node-a", 1), "session-1");
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(frame));

        handoff.BeginCapture();
        Assert.Equal(
            ["after-target-completion"],
            handoff.SnapshotFrames().Select(DecodeBody));

        // A retry for the completed target remains idempotent while the next
        // source handoff is collecting its own accepted journal.
        handoff.Complete("handoff-1");
        _ = handoff.AbortCapture();
    }

    [Fact]
    public void TargetArrivalBacklog_RemainsSealedThroughJoinedNotification()
    {
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System);
        Assert.True(handoff.Import(CommitRequest("handoff-1", []), out _));
        handoff.MarkAuthorityCommitted("handoff-1", 1, 1);
        Assert.True(handoff.TryBeginJoinedNotification("handoff-1"));
        using var body = Message.From("target-arrival");
        using var notifying = Frame(
            body,
            ActorRef("node-b", 1),
            "session-notifying");
        using var prepared = Frame(
            body,
            ActorRef("node-b", 1),
            "session-prepared");

        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(notifying));
        handoff.AcceptPreparation(
            "handoff-1",
            ZLinkRemoteActorJoinPackets.CreateJoinReply(
                true,
                ActorRef("node-b", 1)));
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            handoff.TryCapture(prepared));

        Assert.Equal(2, handoff.PrepareImportedReplay([]).Count);
    }

    [Fact]
    public void SourceIngressHold_EncodedByteBoundaryIncludesJournalHeader()
    {
        using var body = Message.From("encoded-byte-boundary");
        using var first = Frame(body, ActorRef("node-a", 1), "session-1");
        using var second = Frame(body, ActorRef("node-a", 1), "session-2");
        var frozen = ZLinkActorHandoffFrames.Capture(first, 0);
        var oneRecordBytes = ZLinkActorHandoffFrames.CanonicalEncodedLength(
            frozen,
            first.Actor);
        Assert.True(oneRecordBytes > frozen.Body.LongLength);
        Assert.True(oneRecordBytes > frozen.Header.LongLength + frozen.Body.LongLength);
        var admission = new ZLinkBoundedIngressAdmission(
            recordCapacity: 2,
            byteCapacity: oneRecordBytes);
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System,
            sourceIngressAdmission: admission);
        handoff.BeginCapture();

        Assert.Equal(ZLinkActorHandoffCaptureResult.Captured, handoff.TryCapture(first));
        Assert.Equal(ZLinkActorHandoffCaptureResult.Full, handoff.TryCapture(second));
        Assert.Equal((1, oneRecordBytes), admission.Snapshot());
    }

    [Fact]
    public void SourceIngressHold_AbortAndCutoverReleaseAdmissionOwnership()
    {
        var admission = new ZLinkBoundedIngressAdmission(2, 64 * 1024);
        var handoff = new ZLinkActorHandoffState(
            "actor-1",
            TimeProvider.System,
            sourceIngressAdmission: admission);
        using var body = Message.From("release");
        using var frame = Frame(body, ActorRef("node-a", 1), "session-1");
        handoff.BeginCapture();
        Assert.Equal(ZLinkActorHandoffCaptureResult.Captured, handoff.TryCapture(frame));

        Assert.Single(handoff.AbortCapture());
        Assert.Equal((0, 0L), admission.Snapshot());

        handoff.BeginCapture();
        Assert.Equal(ZLinkActorHandoffCaptureResult.Captured, handoff.TryCapture(frame));
        _ = handoff.CutoverCaptureToMessageFollow(
            committedFrameCount: 0,
            ActorRef("node-a", 1),
            ActorRef("node-b", 1),
            "mesh-a",
            sourceNodeGeneration: 1,
            targetNodeGeneration: 2,
            sourceAuthorityOwnerGeneration: 1,
            targetAuthorityOwnerGeneration: 2,
            sourceOwnerLeaseGeneration: 1,
            targetOwnerLeaseGeneration: 2);

        Assert.Equal((0, 0L), admission.Snapshot());
        _ = handoff.AbortCapture();
        Assert.Equal((0, 0L), admission.Snapshot());
    }

    [Fact]
    public void MessageFollowFence_AcceptsNonContiguousIncreasingGenerations()
    {
        var handoff = new ZLinkActorHandoffState("actor-1", TimeProvider.System);
        handoff.BeginCapture();

        var trailing = handoff.CutoverCaptureToMessageFollow(
            committedFrameCount: 0,
            ActorRef("node-a", 1),
            ActorRef("node-b", 1),
            "mesh-a",
            sourceNodeGeneration: 1,
            targetNodeGeneration: 2,
            sourceAuthorityOwnerGeneration: 1UL << 62,
            targetAuthorityOwnerGeneration: (1UL << 62) + 17,
            sourceOwnerLeaseGeneration: 3,
            targetOwnerLeaseGeneration: 4);

        Assert.Empty(trailing);
    }

    [Theory]
    [InlineData(12UL, 11UL)]
    [InlineData(12UL, 12UL)]
    [InlineData(12UL, 9223372036854775808UL)]
    [InlineData(9223372036854775808UL, 1UL)]
    public void MessageFollowFence_RejectsNonIncreasingOrOutOfRangeGenerations(
        ulong sourceGeneration,
        ulong targetGeneration)
    {
        var handoff = new ZLinkActorHandoffState("actor-1", TimeProvider.System);
        handoff.BeginCapture();

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            handoff.CutoverCaptureToMessageFollow(
                committedFrameCount: 0,
                ActorRef("node-a", 1),
                ActorRef("node-b", 1),
                "mesh-a",
                sourceNodeGeneration: 1,
                targetNodeGeneration: 2,
                sourceAuthorityOwnerGeneration: sourceGeneration,
                targetAuthorityOwnerGeneration: targetGeneration,
                sourceOwnerLeaseGeneration: 3,
                targetOwnerLeaseGeneration: 4));
    }

    [Fact]
    public void InFlightHandoffOrder_PreservesArrivalOrder()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.BeginCapture();

        Capture(state, "P1", "session-1");
        Capture(state, "P2", "session-1");
        Capture(state, "P3", "session-1");

        var frames = state.Handoff.SnapshotFrames();
        Assert.Equal(["P1", "P2", "P3"], frames.Select(DecodeBody));
    }

    [Fact]
    public void DirectOvertakesPrevented_ImportedBacklogStaysAheadOfTargetArrivals()
    {
        var source = new ZLinkActorRuntimeState("actor-1");
        source.Handoff.BeginCapture();
        Capture(source, "B1", "session-1");
        Capture(source, "B2", "session-1");

        var target = new ZLinkActorRuntimeState("actor-1");
        target.Handoff.Import(CommitRequest("handoff-1", source.Handoff.SnapshotFrames()), out _);
        Capture(target, "D1", "session-2");

        var frames = target.Handoff.PrepareImportedReplay([]);
        Assert.Equal(["B1", "B2", "D1"], frames.Select(DecodeBody));
    }

    [Fact]
    public void BoundSessionCrossMoveOrder_PreservesSessionRouteAndSequence()
    {
        var source = new ZLinkActorRuntimeState("actor-1");
        source.Handoff.BeginCapture();
        Capture(source, "S1", "bound-session");
        Capture(source, "S2", "bound-session");

        var target = new ZLinkActorRuntimeState("actor-1");
        target.Handoff.Import(CommitRequest("handoff-1", source.Handoff.SnapshotFrames()), out _);
        Capture(target, "S3", "bound-session");
        Capture(target, "S4", "bound-session");

        var frames = target.Handoff.PrepareImportedReplay([]);
        Assert.Equal(["S1", "S2", "S3", "S4"], frames.Select(DecodeBody));
        Assert.All(frames, frame => Assert.Equal("bound-session", RoutingId.From(frame.SourceSessionRid).ToString()));
    }

    [Fact]
    public void MessageFollowThenReject_UsesBoundedDuration()
    {
        var time = new ManualTimeProvider();
        var state = new ZLinkActorRuntimeState("actor-1", time);
        var source = ActorRef("node-a", 1);
        var target = ActorRef("node-b", 2);
        state.BindNativeActorRef(target);
        state.Handoff.BeginCapture();
        _ = Cutover(state, 0, source, target);
        state.Handoff.CommitMessageFollow(TimeSpan.FromMilliseconds(50));

        Assert.Equal(
            ZLinkActorFrameRoute.MessageFollow,
            state.Handoff.ResolveFrameRoute(state.NativeActorRef, source, out var followed));
        Assert.Equal(target, followed);

        time.Advance(TimeSpan.FromMilliseconds(150));
        Assert.Equal(
            ZLinkActorFrameRoute.MessageFollowExpired,
            state.Handoff.ResolveFrameRoute(state.NativeActorRef, source, out _));
    }

    [Fact]
    public void MessageFollowRouteRemoval_ReplacesTheNodeActorEntry()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var firstSource = ActorRef("node-a", 1);
        var firstTarget = ActorRef("node-b", 2);
        var nextSource = ActorRef("node-a", 3);
        var nextTarget = ActorRef("node-c", 4);
        state.BindNativeActorRef(nextTarget);

        state.Handoff.BeginCapture();
        _ = Cutover(state, 0, firstSource, firstTarget);
        state.Handoff.CommitMessageFollow(TimeSpan.FromSeconds(1));
        state.Handoff.CompleteSourceMigration();
        state.Handoff.BeginCapture();
        _ = Cutover(state, 0, nextSource, nextTarget);
        state.Handoff.CommitMessageFollow(TimeSpan.FromSeconds(1));

        Assert.Equal(
            ZLinkActorFrameRoute.Stale,
            state.Handoff.ResolveFrameRoute(state.NativeActorRef, firstSource, out _));
        Assert.Equal(
            ZLinkActorFrameRoute.MessageFollow,
            state.Handoff.ResolveFrameRoute(state.NativeActorRef, nextSource, out var followed));
        Assert.Equal(nextTarget, followed);
    }

    [Fact]
    public void TransferredActivation_ClearsRetiredSourceFollow_ButKeepsImportedTarget()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var previousSource = ActorRef("node-a", 1);
        var previousTarget = ActorRef("node-b", 2);
        state.Handoff.BeginCapture();
        _ = Cutover(state, 0, previousSource, previousTarget);
        state.Handoff.CommitMessageFollow(TimeSpan.FromMinutes(1));
        state.Handoff.CompleteSourceMigration();

        var imported = HandoffFrame(3, arrivalIndex: 0);
        Assert.True(
            state.Handoff.Import(
                CommitRequest("handoff-return", [imported]),
                out _));

        state.PrepareForTransferredActivation();

        Assert.Equal(
            ZLinkActorFrameRoute.Current,
            state.Handoff.ResolveFrameRoute(previousSource, previousSource, out _));
        Assert.Equal(
            [imported.RequestId],
            state.Handoff.PrepareImportedReplay([])
                .Select(static frame => frame.RequestId));
    }

    [Fact]
    public void MigratedAndDestroyedTransitions_PreserveThenReleaseBoundSessionIdentity()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var source = ActorRef("node-a", 1);
        var target = ActorRef("node-b", 2);
        var sessionRid = RoutingId.From("session-1");
        state.BindNativeActorRef(target);
        state.BindSession(
            RoutingId.From("session-node"),
            sessionRid,
            "binding-1",
            objectGeneration: target.Generation,
            authorityOwnerGeneration: 1,
            meshName: "mesh-a",
            targetNodeGeneration: 1,
            ownerLeaseGeneration: 1,
            sessionOwnerNodeGeneration: 1);
        state.Handoff.BeginCapture();
        _ = Cutover(state, 0, source, target);
        state.Handoff.CommitMessageFollow(TimeSpan.FromSeconds(1));

        state.RetireMigratedActorInstance(source);

        Assert.Equal(target, state.NativeActorRef);
        Assert.Equal(source, state.RetiredLocalActorRef);
        Assert.True(state.TryGetBoundSession(out var retained));
        Assert.Equal(sessionRid, retained.SessionRid);
        Assert.Equal("binding-1", retained.BindingToken);

        var released = state.ClearAfterDestroy();

        Assert.Equal(retained, released);
        Assert.Null(state.NativeActorRef);
        Assert.Null(state.RetiredLocalActorRef);
        Assert.False(state.TryGetBoundSession(out _));
    }

    [Fact]
    public void MigratedTransition_ClearsSourceBinding_BeforeTheNextLocalCreation()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var source = ActorRef("node-a", 1);

        state.BindNativeActorRef(source);
        state.Handoff.BeginCapture();
        _ = Cutover(state, 0, source, ActorRef("node-b", 2));
        state.Handoff.CommitMessageFollow(TimeSpan.FromSeconds(1));
        state.RetireMigratedActorInstance(source);

        Assert.Null(state.NativeActorRef);
        Assert.Equal(source, state.RetiredLocalActorRef);
    }

    [Fact]
    public async Task RecreatingAfterMigration_DropsThePreservedRemoteBinding()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var source = ActorRef("node-a", 1);
        var target = ActorRef("node-b", 2);
        state.BindNativeActorRef(target);
        state.Handoff.BeginCapture();
        _ = Cutover(state, 0, source, target);
        state.Handoff.CommitMessageFollow(TimeSpan.FromSeconds(1));
        state.RetireMigratedActorInstance(source);

        var creation = new TaskCompletionSource<IZLinkActor>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var operation = await state.GetOrStartActorCreationAsync(
            "warrior",
            failIfExists: true,
            () => creation.Task,
            CancellationToken.None);

        Assert.Null(state.NativeActorRef);
        Assert.Equal(source, state.RetiredLocalActorRef);

        creation.SetException(new InvalidOperationException("test creation failure"));
        await Assert.ThrowsAsync<InvalidOperationException>(() => operation.Task);
    }

    [Fact]
    public void NewActivationAfterMigration_ReopensTheClosedActorActivation()
    {
        using var services = new ServiceCollection().BuildServiceProvider();
        var state = new ZLinkActorRuntimeState("actor-1", services: services);
        var source = ActorRef("node-a", 1);

        state.BindNativeActorRef(source);
        state.Handoff.BeginCapture();
        _ = Cutover(state, 0, source, ActorRef("node-b", 2));
        state.Handoff.CommitMessageFollow(TimeSpan.FromSeconds(1));
        state.RetireMigratedActorInstance(source);

        Assert.Throws<InvalidOperationException>(() => state.HandlerInstances);

        state.PrepareForTransferredActivation();

        Assert.False(state.ContextInvalidated);
        Assert.NotNull(state.HandlerInstances);
    }

    [Fact]
    public void Relocation_Session_Route_Is_Hidden_Until_Authority_And_Ack_Complete()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.Import(CommitRequest("handoff-1", []), out _);
        Assert.Empty(state.Handoff.PrepareImportedReplay([]));
        var targetNode = RoutingId.From("target-node");
        var sessionNode = RoutingId.From("session-node");
        var sessionRid = RoutingId.From("session-rid");
        state.BindSession(
            sessionNode,
            sessionRid,
            "binding-1",
            bindingGeneration: 4,
            objectGeneration: 7,
            authorityOwnerGeneration: 11,
            meshName: "play",
            targetNodeGeneration: 4,
            ownerLeaseGeneration: 8,
            sessionOwnerNodeGeneration: 3,
            acceptedHighWater: 9);
        state.StageRelocationSessionRoute(
            "handoff-1",
            new ZLinkRemoteActorBoundSessionRoute(
                sessionNode,
                sessionRid,
                "binding-1",
                BindingGeneration: 4,
                ObjectGeneration: 7,
                AuthorityOwnerGeneration: 11,
                MeshName: "play",
                TargetNodeGeneration: 4,
                OwnerLeaseGeneration: 8,
                SessionOwnerNodeGeneration: 3,
                AcceptedHighWater: 9));

        Assert.True(state.TryGetBoundSession(out var beforeAuthority));
        Assert.Equal((ulong)11, beforeAuthority.AuthorityOwnerGeneration);
        Assert.False(state.TryGetCommittedRelocationSessionRoute("handoff-1", out _));

        state.MarkRelocationSessionAuthorityCommitted(
            "handoff-1",
            new ZLinkBackendActorRef(targetNode, "actor-1", 7),
            targetAuthorityOwnerGeneration: 12,
            targetMeshName: "play",
            targetNodeGeneration: 5,
            targetOwnerLeaseGeneration: 9);
        state.RecordRelocatedSessionAccepted(sessionRid);

        Assert.True(state.TryGetBoundSession(out var beforeAck));
        Assert.Equal((ulong)11, beforeAck.AuthorityOwnerGeneration);
        Assert.True(state.TryGetCommittedRelocationSessionRoute(
            "handoff-1",
            out var firstCommit));
        Assert.True(state.TryGetCommittedRelocationSessionRoute(
            "handoff-1",
            out var retriedCommit));
        Assert.Equal(sessionNode, firstCommit.Route.NodeRid);
        Assert.Equal(firstCommit.Route.NodeRid, retriedCommit.Route.NodeRid);

        state.CompleteRelocationSessionRoute("handoff-1");

        Assert.True(state.Handoff.BlocksLocalDispatch);
        Assert.True(state.TryGetBoundSession(out var completed));
        Assert.Equal(sessionNode, completed.SessionNodeRid);
        Assert.Equal((ulong)7, completed.ObjectGeneration);
        Assert.Equal((ulong)12, completed.AuthorityOwnerGeneration);
        Assert.Equal("play", completed.MeshName);
        Assert.Equal((ulong)5, completed.TargetNodeGeneration);
        Assert.Equal((ulong)9, completed.OwnerLeaseGeneration);
        Assert.Equal((ulong)10, completed.AcceptedHighWater);
        state.Handoff.Complete("handoff-1");
        Assert.False(state.Handoff.BlocksLocalDispatch);
    }

    [Fact]
    public void Relocation_Session_Route_Uses_Source_Fence_High_Water_Exactly_Once()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var targetNode = RoutingId.From("target-node");
        var sessionNode = RoutingId.From("session-node");
        var sessionRid = RoutingId.From("session-rid");
        state.StageRelocationSessionRoute(
            "handoff-1",
            new ZLinkRemoteActorBoundSessionRoute(
                sessionNode,
                sessionRid,
                "binding-1",
                BindingGeneration: 4,
                ObjectGeneration: 7,
                AuthorityOwnerGeneration: 11,
                MeshName: "play",
                TargetNodeGeneration: 4,
                OwnerLeaseGeneration: 8,
                SessionOwnerNodeGeneration: 3,
                AcceptedHighWater: 9));
        state.MarkRelocationSessionAuthorityCommitted(
            "handoff-1",
            new ZLinkBackendActorRef(targetNode, "actor-1", 7),
            targetAuthorityOwnerGeneration: 12,
            targetMeshName: "play",
            targetNodeGeneration: 5,
            targetOwnerLeaseGeneration: 9);

        state.RecordRelocatedSessionAccepted(sessionRid, acceptedHighWater: 13);
        state.RecordRelocatedSessionAccepted(sessionRid, acceptedHighWater: 11);

        Assert.True(state.TryGetCommittedRelocationSessionRoute(
            "handoff-1",
            out var committed));
        Assert.Equal((ulong)13, committed.Route.AcceptedHighWater);

        state.CompleteRelocationSessionRoute("handoff-1");
        Assert.True(state.TryGetBoundSession(out var completed));
        Assert.Equal((ulong)13, completed.AcceptedHighWater);
    }

    [Fact]
    public void Relocation_Session_Route_Allows_Target_Outbound_After_Authority_Commit()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var targetNode = RoutingId.From("target-node");
        var sessionNode = RoutingId.From("session-node");
        var sessionRid = RoutingId.From("session-rid");
        state.StageRelocationSessionRoute(
            "handoff-1",
            new ZLinkRemoteActorBoundSessionRoute(
                sessionNode,
                sessionRid,
                "binding-1",
                BindingGeneration: 4,
                ObjectGeneration: 7,
                AuthorityOwnerGeneration: 11,
                MeshName: "play",
                TargetNodeGeneration: 4,
                OwnerLeaseGeneration: 8,
                SessionOwnerNodeGeneration: 3,
                AcceptedHighWater: 9));

        Assert.False(state.TryGetBoundSessionForOutbound(out _));
        state.MarkRelocationSessionAuthorityCommitted(
            "handoff-1",
            new ZLinkBackendActorRef(targetNode, "actor-1", 7),
            targetAuthorityOwnerGeneration: 12,
            targetMeshName: "play",
            targetNodeGeneration: 5,
            targetOwnerLeaseGeneration: 9);

        Assert.False(state.TryGetBoundSession(out _));
        Assert.True(state.TryGetBoundSessionForOutbound(out var outbound));
        Assert.Equal(sessionNode, outbound.SessionNodeRid);
        Assert.Equal(sessionRid, outbound.SessionRid);
        Assert.Equal("binding-1", outbound.BindingToken);
        Assert.Equal((ulong)12, outbound.AuthorityOwnerGeneration);

        Assert.True(state.TryGetBoundSessionForInbound(out var inbound));
        Assert.Equal(outbound, inbound);
        Assert.Equal((ulong)5, inbound.TargetNodeGeneration);
        Assert.Equal((ulong)9, inbound.OwnerLeaseGeneration);

        state.CompleteRelocationSessionRoute("handoff-1");
        Assert.True(state.TryGetBoundSession(out var completed));
        Assert.Equal(outbound, completed);
    }

    [Fact]
    public void Relocation_Session_Route_Prefers_Target_Outbound_Over_Stale_Source_Binding()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var targetNode = RoutingId.From("target-node");
        var sessionNode = RoutingId.From("session-node");
        var sessionRid = RoutingId.From("session-rid");
        state.BindSession(
            sessionNode,
            sessionRid,
            "binding-1",
            bindingGeneration: 4,
            objectGeneration: 7,
            authorityOwnerGeneration: 11,
            meshName: "play",
            targetNodeGeneration: 4,
            ownerLeaseGeneration: 8,
            sessionOwnerNodeGeneration: 3,
            acceptedHighWater: 9);
        state.StageRelocationSessionRoute(
            "handoff-1",
            new ZLinkRemoteActorBoundSessionRoute(
                sessionNode,
                sessionRid,
                "binding-1",
                BindingGeneration: 4,
                ObjectGeneration: 7,
                AuthorityOwnerGeneration: 11,
                MeshName: "play",
                TargetNodeGeneration: 4,
                OwnerLeaseGeneration: 8,
                SessionOwnerNodeGeneration: 3,
                AcceptedHighWater: 9));

        state.MarkRelocationSessionAuthorityCommitted(
            "handoff-1",
            new ZLinkBackendActorRef(targetNode, "actor-1", 7),
            targetAuthorityOwnerGeneration: 12,
            targetMeshName: "play",
            targetNodeGeneration: 5,
            targetOwnerLeaseGeneration: 9);

        Assert.True(state.TryGetBoundSessionForOutbound(out var outbound));
        Assert.Equal((ulong)12, outbound.AuthorityOwnerGeneration);
        Assert.Equal((ulong)5, outbound.TargetNodeGeneration);
        Assert.Equal((ulong)9, outbound.OwnerLeaseGeneration);
    }

    [Fact]
    public void Pending_Relocation_Disconnect_Uses_The_Target_Projection_And_Cancels_It()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var targetNode = RoutingId.From("target-node");
        var sessionNode = RoutingId.From("session-node");
        var sessionRid = RoutingId.From("session-rid");
        state.StageRelocationSessionRoute(
            "handoff-1",
            new ZLinkRemoteActorBoundSessionRoute(
                sessionNode,
                sessionRid,
                "binding-1",
                BindingGeneration: 4,
                ObjectGeneration: 7,
                AuthorityOwnerGeneration: 11,
                MeshName: "play",
                TargetNodeGeneration: 4,
                OwnerLeaseGeneration: 8,
                SessionOwnerNodeGeneration: 3,
                AcceptedHighWater: 9));
        state.MarkRelocationSessionAuthorityCommitted(
            "handoff-1",
            new ZLinkBackendActorRef(targetNode, "actor-1", 7),
            targetAuthorityOwnerGeneration: 12,
            targetMeshName: "play",
            targetNodeGeneration: 5,
            targetOwnerLeaseGeneration: 9);

        using var disconnect = Message.From(
            ZLinkActorBoundSessionRelay.EncodeSessionDisconnected(
                "binding-1",
                bindingGeneration: 4,
                sessionOwnerNodeGeneration: 3));

        Assert.True(ZLinkActorBoundSessionRelay.TryValidateDisconnectedBinding(
            state,
            sessionNode,
            sessionRid,
            disconnect,
            out var bindingToken));
        Assert.Equal("binding-1", bindingToken);

        state.UnbindSession(bindingToken);

        Assert.False(state.TryGetCommittedRelocationSessionRoute(
            "handoff-1",
            out _));
        Assert.False(state.TryGetBoundSessionForInbound(out _));
    }

    [Fact]
    public void HandoffCompletion_RequiresMatchingTransfer()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.Import(CommitRequest("handoff-1", []), out _);

        Assert.Throws<InvalidOperationException>(() => state.Handoff.Complete("handoff-other"));
        Assert.Throws<InvalidOperationException>(() => state.Handoff.Complete("handoff-1"));
        Assert.Empty(state.Handoff.PrepareImportedReplay([]));
        Assert.Empty(state.Handoff.SnapshotFinalReplay());
        state.Handoff.Complete("handoff-1");
    }

    [Fact]
    public async Task LocalDispatch_IsRejectedWhileHandoffOwnsTheActor()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var invoked = false;
        state.Handoff.BeginCapture();

        var exception = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await state.ExecuteDispatchAsync(
                new ZlinkStreamHeader(
                    ZlinkStreamMessageKind.Send,
                    ZlinkStreamCodec.Raw,
                    ZlinkStreamHeaderFlags.None,
                    null,
                    "blocked",
                    ZlinkStreamMetadata.Empty),
                _ =>
                {
                    invoked = true;
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None));

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, exception.Kind);
        Assert.False(invoked);
        _ = state.Handoff.AbortCapture();
    }

    [Fact]
    public async Task DispatchOwnership_DoesNotEscapeIntoAChildTask()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var childStarted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseChild = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task? escapedChild = null;

        await state.ExecuteDispatchAsync(
            Header("first"),
            _ =>
            {
                escapedChild = Task.Run(async () =>
                {
                    childStarted.TrySetResult();
                    await releaseChild.Task;
                    await state.ExecuteHandoffTransitionAsync(() => true, CancellationToken.None);
                });
                return ValueTask.CompletedTask;
            },
            CancellationToken.None);
        await childStarted.Task;

        var secondEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseSecond = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var secondDispatch = state.ExecuteDispatchAsync(
                Header("second"),
                async _ =>
                {
                    secondEntered.TrySetResult();
                    await releaseSecond.Task;
                },
                CancellationToken.None)
            .AsTask();
        await secondEntered.Task;

        releaseChild.TrySetResult();
        await Task.Delay(25);
        Assert.False(escapedChild!.IsCompleted);

        releaseSecond.TrySetResult();
        await secondDispatch;
        await escapedChild;
    }

    [Fact]
    public void SourceCommitSnapshot_RetainsFramesForRollback_AndReturnsOnlyTrailingFramesOnSuccess()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.BeginCapture();
        Capture(state, "B1", "session-1");
        Capture(state, "B2", "session-1");

        var committed = state.Handoff.SnapshotFrames();
        Capture(state, "T1", "session-1");

        Assert.Equal(["B1", "B2"], committed.Select(DecodeBody));
        var trailing = Cutover(
            state,
            committed.Count,
            ActorRef("node-a", 1),
            ActorRef("node-b", 2));
        Assert.Equal(["T1"], trailing.Select(DecodeBody));
        _ = state.Handoff.AbortCapture();
    }

    [Fact]
    public void AbortedCapture_ReturnsEveryFrameForLocalReplay()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.BeginCapture();
        Capture(state, "B1", "session-1");
        Capture(state, "B2", "session-1");

        Assert.Equal(["B1", "B2"], state.Handoff.AbortCapture().Select(DecodeBody));
    }

    [Fact]
    public void PausedCapture_RetainsEveryFrameUntilRemoteCompletionCommits()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var sourceActor = ActorRef("node-a", 1);
        state.BindNativeActorRef(sourceActor);
        state.Handoff.BeginCapture();
        Capture(state, "B1", "session-1");
        var committed = state.Handoff.SnapshotFrames();
        Capture(state, "T1", "session-1");

        Assert.Equal(
            ["T1"],
            Cutover(
                    state,
                    committed.Count,
                    sourceActor,
                    ActorRef("node-b", 2))
                .Select(DecodeBody));
        Assert.Equal(["B1", "T1"], state.Handoff.AbortCapture().Select(DecodeBody));
        Assert.Equal(
            ZLinkActorFrameRoute.Current,
            state.Handoff.ResolveFrameRoute(sourceActor, sourceActor, out _));
    }

    [Fact]
    public void ImportedCapture_PlacesSourceTrailingFramesBeforeTargetArrivals()
    {
        var source = new ZLinkActorRuntimeState("actor-1");
        source.Handoff.BeginCapture();
        Capture(source, "B1", "session-1");
        var initial = source.Handoff.SnapshotFrames();
        Capture(source, "T1", "session-1");
        var trailing = Cutover(
            source,
            initial.Count,
            ActorRef("node-a", 1),
            ActorRef("node-b", 2));

        var target = new ZLinkActorRuntimeState("actor-1");
        var commit = CommitRequest("handoff-1", initial);
        Assert.True(target.Handoff.Import(commit, out _));
        Capture(target, "D1", "session-2");

        var replay = target.Handoff.PrepareImportedReplay(trailing);
        Assert.Equal(["B1", "T1", "D1"], replay.Select(DecodeBody));
        foreach (var _ in replay) target.Handoff.AcknowledgeReplayedFrame();
        Assert.Empty(target.Handoff.PrepareImportedReplay(trailing));
        Assert.False(target.Handoff.Import(commit, out _));
    }

    [Fact]
    public void ReplayAcknowledgement_RetainsTheFailedSuffixForRetry()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.Import(CommitRequest("handoff-1", []), out _);
        Capture(state, "P1", "session-1");
        Capture(state, "P2", "session-1");
        Capture(state, "P3", "session-1");

        Assert.Equal(
            ["P1", "P2", "P3"],
            state.Handoff.PrepareImportedReplay([]).Select(DecodeBody));
        state.Handoff.AcknowledgeReplayedFrame();

        Assert.Equal(
            ["P2", "P3"],
            state.Handoff.PrepareImportedReplay([]).Select(DecodeBody));
    }

    [Fact]
    public void ReplayAcknowledgement_UsesTheRestoredFrameArrivalIdentity()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.Import(CommitRequest("handoff-1", []), out _);
        Capture(state, "P1", "session-1");
        Capture(state, "P2", "session-1");
        Capture(state, "P3", "session-1");

        var replay = state.Handoff.PrepareImportedReplay([]);
        Assert.Equal([0L, 1L, 2L], replay.Select(frame => frame.ArrivalIndex));

        // Replay dispatch can overlap with another preserved snapshot. The
        // acknowledgement must remove the frame that was actually dispatched,
        // not whatever frame currently happens to be at the head.
        state.Handoff.AcknowledgeReplayedFrame(replay[1].ArrivalIndex);

        Assert.Equal(
            ["P1", "P3"],
            state.Handoff.PrepareImportedReplay([]).Select(DecodeBody));
    }

    [Fact]
    public void SourceCutover_AtomicallyStopsCaptureAndStartsMessageFollow()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var source = ActorRef("node-a", 1);
        var target = ActorRef("node-b", 2);
        state.BindNativeActorRef(source);
        state.Handoff.BeginCapture();
        Capture(state, "P1", "session-1");

        var trailing = Cutover(state, 0, source, target);

        Assert.Equal(["P1"], trailing.Select(DecodeBody));
        using var body = Message.From(System.Text.Encoding.UTF8.GetBytes("after-cutover"));
        using var frame = Frame(body, source, "session-1");
        Assert.Equal(
            ZLinkActorHandoffCaptureResult.NotSealed,
            state.Handoff.TryCapture(frame));
        Assert.Equal(
            ZLinkActorFrameRoute.MessageFollow,
            state.Handoff.ResolveFrameRoute(state.NativeActorRef, source, out var followed));
        Assert.Equal(target, followed);
    }

    [Fact]
    public async Task MessageFollowOperationalMarkers_DoNotExposeObjectIdentityOrGeneration()
    {
        var markers = new System.Collections.Concurrent.ConcurrentQueue<string>();
        var state = new ZLinkActorRuntimeState(
            "actor-private-42",
            handoffDiagnostic: markers.Enqueue);
        var source = ActorRef("node-a", 41);
        var target = ActorRef("node-b", 42);
        state.BindNativeActorRef(source);
        state.Handoff.BeginCapture();

        _ = Cutover(state, 0, source, target);
        state.Handoff.CommitMessageFollow(TimeSpan.FromMilliseconds(10));

        var registered = Assert.Single(markers);
        Assert.Contains("message_follow_registered", registered);
        Assert.Contains("source_rid=node-a", registered);
        Assert.Contains("target_rid=node-b", registered);
        Assert.DoesNotContain("actor=", registered);
        Assert.DoesNotContain("generation=", registered);
        Assert.DoesNotContain("actor-private-42", registered);

        for (var attempt = 0; attempt < 20 && markers.Count == 1; attempt++)
            await Task.Delay(5);

        Assert.Contains("message_follow_route_removed entries=0", markers);
        Assert.All(markers, marker =>
        {
            Assert.DoesNotContain("actor=", marker);
            Assert.DoesNotContain("generation=", marker);
            Assert.DoesNotContain("actor-private-42", marker);
        });
    }

    [Fact]
    public async Task SourceCaptureAndCutover_RacePlacesFrameInExactlyOnePath()
    {
        for (var iteration = 0; iteration < 100; iteration++)
        {
            var admission = new ZLinkBoundedIngressAdmission(1, 64 * 1024);
            var state = new ZLinkActorRuntimeState(
                "actor-1",
                sourceIngressAdmission: admission);
            var source = ActorRef("node-a", 1);
            var target = ActorRef("node-b", 2);
            state.BindNativeActorRef(source);
            state.Handoff.BeginCapture();
            using var body = Message.From(System.Text.Encoding.UTF8.GetBytes("race"));
            using var frame = Frame(body, source, "session-race");
            using var barrier = new Barrier(2);

            var capture = Task.Run(() =>
            {
                barrier.SignalAndWait();
                return state.Handoff.TryCapture(frame);
            });
            var cutover = Task.Run(() =>
            {
                barrier.SignalAndWait();
                return Cutover(state, 0, source, target);
            });

            var captured = await capture;
            var trailing = await cutover;
            Assert.Equal(
                ZLinkActorFrameRoute.MessageFollow,
                state.Handoff.ResolveFrameRoute(source, source, out _));
            Assert.Equal(
                captured == ZLinkActorHandoffCaptureResult.Captured ? 1 : 0,
                trailing.Count);
            Assert.Equal((0, 0L), admission.Snapshot());
            _ = state.Handoff.AbortCapture();
            Assert.Equal((0, 0L), admission.Snapshot());
        }
    }

    [Fact]
    public async Task TargetFinalCaptureAndCompletion_RaceCannotSkipAFrame()
    {
        for (var iteration = 0; iteration < 100; iteration++)
        {
            var state = new ZLinkActorRuntimeState("actor-1");
            var handoffId = $"handoff-{iteration}";
            state.Handoff.Import(CommitRequest(handoffId, []), out _);
            Assert.Empty(state.Handoff.PrepareImportedReplay([]));
            var source = ActorRef("node-a", 1);
            using var body = Message.From(System.Text.Encoding.UTF8.GetBytes("race"));
            using var frame = Frame(body, source, "session-race");
            using var barrier = new Barrier(2);

            var capture = Task.Run(() =>
            {
                barrier.SignalAndWait();
                return state.Handoff.TryCapture(frame);
            });
            var completion = Task.Run(() =>
            {
                barrier.SignalAndWait();
                try
                {
                    state.Handoff.Complete(handoffId);
                    return true;
                }
                catch (InvalidOperationException)
                {
                    return false;
                }
            });

            var captured = await capture;
            var completed = await completion;
            var didCapture =
                captured == ZLinkActorHandoffCaptureResult.Captured;
            Assert.True(didCapture ^ completed);
            if (didCapture)
            {
                state.Handoff.AcknowledgeReplayedFrame();
                state.Handoff.Complete(handoffId);
            }
        }
    }

    [Fact]
    public void RequestHandoff_PreservesReplyRoutingFields()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.BeginCapture();
        Capture(
            state,
            "request",
            "request-session",
            ZlinkStreamMessageKind.Request,
            requestId: 42,
            flags: 7);

        var frames = state.Handoff.SnapshotFrames();
        _ = state.Handoff.AbortCapture();
        var restored = ZLinkActorHandoffFrames.Restore(ActorRef("node-b", 2), frames);
        try
        {
            Assert.Equal(1, restored.Count);
            var restoredFrame = restored[0];
            Assert.Equal((ulong)42, restoredFrame.RequestId);
            Assert.Equal((uint)7, restoredFrame.Flags);
            Assert.Equal("node-a", restoredFrame.ReplyActor.NodeRid.ToString());
            Assert.Equal((ulong)1, restoredFrame.ReplyActor.Generation);
            Assert.Equal("request-session", restoredFrame.SourceSessionRid.ToString());
            Assert.Equal(ZlinkStreamMessageKind.Request, restoredFrame.Header.Kind);
        }
        finally
        {
            restored.Dispose();
        }
    }

    [Fact]
    public async Task DuplicateImport_WaitsForTheOriginalPreparationResult()
    {
        var state = new ZLinkActorRuntimeState("actor-1");

        var commit = CommitRequest("handoff-1", []);
        Assert.True(state.Handoff.Import(commit, out var originalPreparation));
        Assert.False(state.Handoff.Import(commit, out var duplicatePreparation));
        Assert.Same(originalPreparation, duplicatePreparation);
        Assert.False(duplicatePreparation.IsCompleted);

        var reply = ZLinkRemoteActorJoinPackets.CreateJoinReply(true, ActorRef("node-b", 2));
        state.Handoff.MarkAuthorityCommitted("handoff-1", 2, 2);
        Assert.True(state.Handoff.TryBeginJoinedNotification("handoff-1"));
        state.Handoff.AcceptPreparation("handoff-1", reply);

        Assert.Same(reply, await duplicatePreparation);
    }

    [Fact]
    public async Task TargetAuthorityCommit_PrecedesJoinedNotification_AndRetryKeepsTheCommit()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var commit = CommitRequest("handoff-order", []);
        Assert.True(state.Handoff.Import(commit, out var preparation));

        Assert.False(state.Handoff.TryBeginJoinedNotification("handoff-order"));
        state.Handoff.MarkAuthorityCommitted("handoff-order", 7, 7);
        Assert.True(state.Handoff.IsAuthorityCommitted("handoff-order"));
        Assert.True(state.Handoff.TryBeginJoinedNotification("handoff-order"));

        state.Handoff.RetryJoinedNotification("handoff-order");

        Assert.True(state.Handoff.IsAuthorityCommitted("handoff-order"));
        Assert.True(state.Handoff.TryBeginJoinedNotification("handoff-order"));
        var reply = ZLinkRemoteActorJoinPackets.CreateJoinReply(
            true,
            ActorRef("node-b", 7));
        state.Handoff.AcceptPreparation("handoff-order", reply);

        Assert.Same(reply, await preparation);
    }

    [Fact]
    public async Task TargetAuthorityCommit_ReturnsPreparationBeforeJoinedNotification()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        var commit = CommitRequest("handoff-commit-boundary", []);
        Assert.True(state.Handoff.Import(commit, out var preparation));

        state.Handoff.MarkAuthorityCommitted("handoff-commit-boundary", 7, 7);
        var reply = ZLinkRemoteActorJoinPackets.CreateJoinReply(
            true,
            ActorRef("node-b", 7));
        state.Handoff.AcceptCommittedPreparation(
            "handoff-commit-boundary",
            reply);

        Assert.Same(reply, await preparation);
        Assert.True(state.Handoff.IsAuthorityCommitted("handoff-commit-boundary"));
        Assert.True(state.Handoff.TryBeginJoinedNotification("handoff-commit-boundary"));
        state.Handoff.CompleteJoinedNotification("handoff-commit-boundary");
    }

    [Fact]
    public void TargetAuthorityCommit_RejectsObjectGenerationChange()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        Assert.True(state.Handoff.Import(CommitRequest("handoff-generation", []), out _));

        var exception = Assert.Throws<ZLinkFrameworkException>(() =>
            state.Handoff.MarkAuthorityCommitted("handoff-generation", 7, 8));

        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, exception.Kind);
        Assert.False(state.Handoff.IsAuthorityCommitted("handoff-generation"));
    }

    [Fact]
    public void TargetReplay_PreservesAcceptedQueueArrivalOrder_AfterAuthorityCommit()
    {
        var initial = new[]
        {
            HandoffFrame(2, arrivalIndex: 0),
            HandoffFrame(1, arrivalIndex: 1)
        };
        var state = new ZLinkActorRuntimeState("actor-1");
        Assert.True(
            state.Handoff.Import(
                CommitRequest("handoff-queue", initial),
                out _));
        state.Handoff.MarkAuthorityCommitted("handoff-queue", 1, 1);
        Assert.True(state.Handoff.TryBeginJoinedNotification("handoff-queue"));
        state.Handoff.AcceptPreparation(
            "handoff-queue",
            ZLinkRemoteActorJoinPackets.CreateJoinReply(true, ActorRef("node-b", 1)));

        var replay = state.Handoff.PrepareImportedReplay(
            [HandoffFrame(3, arrivalIndex: 2)]);

        Assert.Equal(new ulong[] { 2, 1, 3 }, replay.Select(static frame => frame.RequestId));
        Assert.Equal(new long[] { 0, 1, 2 }, replay.Select(static frame => frame.ArrivalIndex));
    }

    [Fact]
    public void BeginCapture_RejectsConcurrentTransactions_AndRetiresCompletedIdentity()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.Handoff.BeginCapture();
        Assert.Throws<InvalidOperationException>(state.Handoff.BeginCapture);
        _ = state.Handoff.AbortCapture();

        Assert.True(state.Handoff.Import(CommitRequest("handoff-arrive", []), out _));
        Assert.Empty(state.Handoff.PrepareImportedReplay([]));
        Assert.Empty(state.Handoff.SnapshotFinalReplay());
        state.Handoff.Complete("handoff-arrive");

        state.Handoff.BeginCapture();
        _ = state.Handoff.AbortCapture();
        Assert.True(state.Handoff.Import(CommitRequest("handoff-return", []), out _));
    }

    [Fact]
    public void PendingAdmission_IsCorrelatedByHandoffId_AndEnforcesItsDeadline()
    {
        var time = new ManualTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(time);
        var request = new ZLinkRemoteActorAdmissionRequest(
            "actor-1",
            "warrior",
            "source-spot",
            [],
            "application/json",
            [],
            "handoff-1",
            (time.GetUtcNow() + TimeSpan.FromSeconds(5)).ToUnixTimeMilliseconds());
        var reply = new ZLinkRemoteActorAdmissionReply(true, "application/json", [], request.DeadlineUnixTimeMilliseconds);
        const string targetSpotId = "spot-1";

        admissions.Register(request, targetSpotId, reply);
        Assert.True(admissions.TryGetReply(request, targetSpotId, out var stored));
        Assert.Same(reply, stored);
        var wrongActorCommit = JoinRequest("other-actor");
        Assert.Throws<InvalidOperationException>(() =>
            admissions.BeginCommit(wrongActorCommit, targetSpotId));
        Assert.Throws<InvalidOperationException>(() =>
            admissions.BeginCommit(JoinRequest("actor-1") with { SourceNodeRid = [9] }, targetSpotId));
        Assert.Throws<InvalidOperationException>(() =>
            admissions.BeginCommit(JoinRequest("actor-1"), "spot-other"));

        time.Advance(TimeSpan.FromSeconds(6));
        var timeout = Assert.Throws<ZLinkFrameworkException>(() =>
            admissions.BeginCommit(JoinRequest("actor-1"), targetSpotId));
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, timeout.Kind);
        Assert.False(admissions.TryGetReply(request, targetSpotId, out _));

        ZLinkRemoteActorJoinRequest JoinRequest(string actorId) => new(
            ActorId: actorId,
            ActorType: "warrior",
            HandoffId: "handoff-1",
            BoundSessionNodeRid: null,
            BoundSessionRid: null,
            RelocationContentType: "application/json",
            RelocationReference: "root-1",
            RelocationChecksumCrc32c: 7,
            RelocationAggregateId:
                Guid.Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
            RelocationAggregateGeneration: 1,
            RelocationInventoryDigest: new byte[32],
            RequestContentType: "application/json",
            Request: [],
            HandoffFrames: [],
            SourceSpotId: request.SourceSpotId,
            SourceNodeRid: request.SourceNodeRid,
            ActorGeneration: 1,
            ActorAuthorityOwnerGeneration: 1);
    }

    [Fact]
    public async Task ConcurrentAdmission_RunsTheApplicationDecisionOnce()
    {
        var time = new ManualTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(time);
        var request = AdmissionRequest(time, "handoff-1");
        const string targetSpot = "spot-1";
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var calls = 0;

        async ValueTask<ZLinkRemoteActorAdmissionReply> Decide(CancellationToken cancellationToken)
        {
            Interlocked.Increment(ref calls);
            entered.TrySetResult();
            await release.Task.WaitAsync(cancellationToken);
            return new ZLinkRemoteActorAdmissionReply(true, "application/json", [], request.DeadlineUnixTimeMilliseconds);
        }

        var first = admissions.AdmitAsync(request, targetSpot, Decide, CancellationToken.None).AsTask();
        await entered.Task;
        var second = admissions.AdmitAsync(request, targetSpot, Decide, CancellationToken.None).AsTask();
        release.TrySetResult();

        var replies = await Task.WhenAll(first, second);
        Assert.Equal(1, calls);
        Assert.Same(replies[0], replies[1]);
    }

    [Fact]
    public async Task ExpiredAdmission_DoesNotInvokeCallbackOrCreateReservation()
    {
        var time = new ManualTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(time);
        var request = AdmissionRequest(time, "handoff-expired-before-callback");
        var callbackCount = 0;
        time.Advance(TimeSpan.FromSeconds(6));

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            admissions.AdmitAsync(
                    request,
                    "target-spot",
                    _ =>
                    {
                        Interlocked.Increment(ref callbackCount);
                        return ValueTask.FromResult(
                            new ZLinkRemoteActorAdmissionReply(
                                true,
                                "application/json",
                                [],
                                request.DeadlineUnixTimeMilliseconds));
                    },
                    CancellationToken.None)
                .AsTask());

        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
        Assert.Equal(0, callbackCount);
        Assert.True(admissions.SnapshotDrain().IsSafe);
    }

    [Fact]
    public async Task TargetReservationValidatesExactCommitShrinksAndReleases()
    {
        var time = new ManualTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(time);
        var permits = new ZLinkRelocationPermitPool(new ZLinkLocationOptions
        {
            MaxRelocationPayloadInFlightBytes = 100
        });
        var request = AdmissionRequest(time, "handoff-reserved") with
        {
            ActorGeneration = 1,
            ActorAuthorityOwnerGeneration = 1,
            PredictedPayloadBytes = 80,
            TargetSpotGeneration = 5,
            TargetSpotAuthorityOwnerGeneration = 3
        };
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Inbound(80, restore: true),
            out var reservationLease));
        var reservation = new ZLinkActorRelocationReservation(
            "reservation-1",
            80,
            RoutingId.From("target-node"),
            7,
            5,
            2,
            3);
        const string target = "target-spot";

        _ = await admissions.AdmitReservedAsync(
            request,
            target,
            _ => ValueTask.FromResult(
                new ZLinkActorHandoffAdmissionDecision(
                    ZLinkRemoteActorJoinPackets.CreateAdmissionReply(
                        true,
                        ZLinkMessage.Empty,
                        new ZLinkCodecRegistryBuilder(),
                        request.DeadlineUnixTimeMilliseconds,
                        reservation),
                    reservationLease)),
            CancellationToken.None);
        var commit = CommitRequest(request.HandoffId, []) with
        {
            ReservationToken = reservation.Token,
            ReservedPayloadBytes = reservation.ReservedPayloadBytes,
            TargetNodeRid = reservation.TargetNodeRid.ToBytes().ToArray(),
            TargetNodeGeneration = reservation.TargetNodeGeneration,
            TargetSpotGeneration = reservation.TargetSpotGeneration,
            TargetAuthorityOwnerGeneration =
                reservation.TargetAuthorityOwnerGeneration,
            TargetSpotAuthorityOwnerGeneration =
                reservation.TargetSpotAuthorityOwnerGeneration
        };

        Assert.Throws<InvalidOperationException>(() =>
            admissions.BeginCommit(
                commit with { ReservationToken = "other" },
                target,
                actualPayloadBytes: 30));
        admissions.BeginCommit(commit, target, actualPayloadBytes: 30);
        Assert.Equal(
            new ZLinkRelocationPermitSnapshot(0, 1, 0, 1, 30, false),
            permits.Snapshot());

        admissions.Complete(request.HandoffId);
        Assert.Equal(default, permits.Snapshot());
    }

    [Fact]
    public async Task TargetReservationAbortAndDeadlineReleaseExactLease()
    {
        var time = new ManualTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(time);
        var permits = new ZLinkRelocationPermitPool(new ZLinkLocationOptions
        {
            MaxRelocationPayloadInFlightBytes = 100
        });
        const string target = "target-spot";

        var abortRequest = AdmissionRequest(time, "handoff-abort");
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Inbound(40, restore: true),
            out var abortLease));
        _ = await admissions.AdmitReservedAsync(
            abortRequest,
            target,
            _ => ValueTask.FromResult(Decision(
                abortRequest,
                "reservation-abort",
                abortLease)),
            CancellationToken.None);
        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            admissions.AbortReservationAsync(
                new ZLinkRemoteActorAdmissionAbortRequest(
                    abortRequest.ActorId,
                    abortRequest.HandoffId,
                    "wrong"),
                target).AsTask());
        await admissions.AbortReservationAsync(
            new ZLinkRemoteActorAdmissionAbortRequest(
                abortRequest.ActorId,
                abortRequest.HandoffId,
                "reservation-abort"),
            target);
        Assert.Equal(default, permits.Snapshot());

        var expiringRequest = AdmissionRequest(time, "handoff-expire");
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Inbound(40, restore: true),
            out var expiringLease));
        _ = await admissions.AdmitReservedAsync(
            expiringRequest,
            target,
            _ => ValueTask.FromResult(Decision(
                expiringRequest,
                "reservation-expire",
                expiringLease)),
            CancellationToken.None);
        time.Advance(TimeSpan.FromSeconds(6));
        Assert.False(admissions.TryGetReply(expiringRequest, target, out _));
        Assert.Equal(default, permits.Snapshot());

        static ZLinkActorHandoffAdmissionDecision Decision(
            ZLinkRemoteActorAdmissionRequest request,
            string token,
            ZLinkRelocationPermitPool.ZLinkRelocationPermitLease lease)
        {
            return new ZLinkActorHandoffAdmissionDecision(
                new ZLinkRemoteActorAdmissionReply(
                    true,
                    "application/json",
                    [],
                    request.DeadlineUnixTimeMilliseconds,
                    token,
                    40,
                    RoutingId.From("target-node").ToBytes().ToArray(),
                    7,
                    5,
                    2,
                    3),
                lease);
        }
    }

    [Fact]
    public async Task Drain_Waits_For_InProgress_And_Accepted_Handoff_Admission()
    {
        var time = new ManualTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(time);
        var request = AdmissionRequest(time, "handoff-drain");
        const string target = "target-spot";
        var decisionStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var decision = new TaskCompletionSource<ZLinkRemoteActorAdmissionReply>(
            TaskCreationOptions.RunContinuationsAsynchronously);

        var admission = admissions.AdmitAsync(
            request,
            target,
            _ =>
            {
                decisionStarted.TrySetResult();
                return new ValueTask<ZLinkRemoteActorAdmissionReply>(decision.Task);
            },
            CancellationToken.None).AsTask();
        await decisionStarted.Task;
        var drainSafe = admissions.WaitUntilDrainSafeAsync(CancellationToken.None);
        Assert.False(drainSafe.IsCompleted);

        decision.SetResult(new ZLinkRemoteActorAdmissionReply(
            true,
            "application/json",
            [],
            request.DeadlineUnixTimeMilliseconds));
        await admission;
        Assert.False(drainSafe.IsCompleted);

        admissions.Complete(request.HandoffId);
        await drainSafe.WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task Drain_Rejects_New_Admission_But_Allows_An_Already_Accepted_Commit()
    {
        var time = new ManualTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(time);
        var gate = new ZLinkDrainAdmissionGate();
        var request = AdmissionRequest(time, "handoff-accepted-before-drain");
        const string target = "target-spot";

        Assert.True(gate.TryEnterActorAdmission(out var admissionLease));
        var reply = await admissions.AdmitAsync(
            request,
            target,
            _ => ValueTask.FromResult(new ZLinkRemoteActorAdmissionReply(
                true,
                "application/json",
                [],
                request.DeadlineUnixTimeMilliseconds)),
            CancellationToken.None);
        admissionLease.Dispose();
        Assert.True(reply.Accepted);

        Assert.True(gate.BeginDrain());
        Assert.False(gate.TryEnterActorAdmission(out var rejectedAdmission));
        rejectedAdmission.Dispose();

        var commit = CommitRequest(request.HandoffId, []);
        admissions.BeginCommit(commit, target);
        admissions.Complete(request.HandoffId);

        await admissions.WaitUntilDrainSafeAsync(CancellationToken.None)
            .WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task TerminalHandoffOutcome_SurvivesAdmissionCleanup_AndRejectsChangedRetry()
    {
        var admissions = new ZLinkActorHandoffAdmissions();
        var request = CommitRequest("handoff-1", []);
        const string targetSpot = "spot-1";
        var reply = ZLinkRemoteActorJoinPackets.CreateJoinReply(true, ActorRef("node-b", 2));

        admissions.RecordJoinOutcome(request, targetSpot, reply);
        await admissions.AbortAsync(request.HandoffId);

        Assert.True(admissions.TryGetJoinOutcome(request, targetSpot, out var stored));
        Assert.Same(reply, stored);
        Assert.False(admissions.TryGetJoinOutcome(
            request with { RelocationReference = "changed-root" },
            targetSpot,
            out _));
        var completion = new ZLinkRemoteActorHandoffCompletionRequest(
            request.ActorId,
            request.HandoffId,
            request.SourceSpotId,
            request.SourceNodeRid,
            targetSpot,
            []);
        Assert.True(admissions.TryBeginCompletion(completion, targetSpot));
        admissions.CancelCompletion(completion, targetSpot);
        Assert.Throws<InvalidOperationException>(() =>
            admissions.TryBeginCompletion(
                completion with
                {
                    Frames =
                    [
                        new ZLinkActorHandoffFrame([], 0, [], [], 1, 0, [], [], 0)
                    ]
                },
                targetSpot));
        Assert.True(admissions.TryBeginCompletion(completion, targetSpot));
        admissions.RecordCompletion(completion, targetSpot);
        Assert.False(admissions.TryBeginCompletion(completion, targetSpot));
        // A completion this target no longer honors is terminal for the
        // source's reconciliation (RequestRejected), never retried.
        var changedTarget = Assert.Throws<ZLinkFrameworkException>(() =>
            admissions.TryBeginCompletion(
                completion with { TargetSpotId = "spot-other" },
                targetSpot));
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, changedTarget.Kind);
    }

    [Fact]
    public void HandoffCompletion_RejectsAChangedBoundSessionRoute()
    {
        var admissions = new ZLinkActorHandoffAdmissions();
        const string targetSpot = "spot-1";
        var request = CommitRequest("handoff-bound-session", []) with
        {
            BoundSessionNodeRid = RoutingId.From("session-node").ToBytes().ToArray(),
            BoundSessionRid = RoutingId.From("session").ToBytes().ToArray(),
            BoundSessionBindingToken = "binding-token",
            BoundSessionBindingGeneration = 7,
            BoundSessionObjectGeneration = 11,
            BoundSessionAuthorityOwnerGeneration = 13,
            BoundSessionMeshName = "mesh-b",
            BoundSessionTargetNodeGeneration = 17,
            BoundSessionOwnerLeaseGeneration = 19,
            BoundSessionOwnerNodeGeneration = 23,
            BoundSessionAcceptedHighWater = 29
        };
        admissions.RecordJoinOutcome(
            request,
            targetSpot,
            ZLinkRemoteActorJoinPackets.CreateJoinReply(true, ActorRef("node-b", 2)));
        var completion = new ZLinkRemoteActorHandoffCompletionRequest(
            request.ActorId,
            request.HandoffId,
            request.SourceSpotId,
            request.SourceNodeRid,
            targetSpot,
            [],
            BoundSessionNodeRid: request.BoundSessionNodeRid,
            BoundSessionRid: request.BoundSessionRid,
            BoundSessionBindingToken: request.BoundSessionBindingToken,
            BoundSessionBindingGeneration: request.BoundSessionBindingGeneration,
            BoundSessionObjectGeneration: request.BoundSessionObjectGeneration,
            BoundSessionAuthorityOwnerGeneration: request.BoundSessionAuthorityOwnerGeneration,
            BoundSessionMeshName: request.BoundSessionMeshName,
            BoundSessionTargetNodeGeneration: request.BoundSessionTargetNodeGeneration,
            BoundSessionOwnerLeaseGeneration: request.BoundSessionOwnerLeaseGeneration,
            BoundSessionOwnerNodeGeneration: request.BoundSessionOwnerNodeGeneration,
            BoundSessionAcceptedHighWater: request.BoundSessionAcceptedHighWater);

        Assert.True(admissions.TryBeginCompletion(completion, targetSpot));
        admissions.CancelCompletion(completion, targetSpot);
        var changedRoute = Assert.Throws<ZLinkFrameworkException>(() =>
            admissions.TryBeginCompletion(
                completion with { BoundSessionOwnerLeaseGeneration = 31 },
                targetSpot));
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, changedRoute.Kind);
    }

    [Fact]
    public void ActiveTerminalOutcomes_AreNotEvictedByTheCompletedResultCapacity()
    {
        var admissions = new ZLinkActorHandoffAdmissions();
        const string targetSpot = "spot-1";
        var first = CommitRequest("handoff-0", []);
        for (var index = 0; index < 1025; index++)
        {
            var request = CommitRequest($"handoff-{index}", []);
            admissions.RecordJoinOutcome(
                request,
                targetSpot,
                ZLinkRemoteActorJoinPackets.CreateJoinReply(true, ActorRef("node-b", (ulong)index + 1)));
        }

        Assert.True(admissions.TryGetJoinOutcome(first, targetSpot, out _));
    }

    [Fact]
    public void PreparedCommit_ExpiresToRejectedOutcome_AndRejectsLateCompletion()
    {
        var time = new ManualTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(time);
        const string targetSpot = "spot-1";
        var request = CommitRequest("handoff-expiring", []);
        var accepted = ZLinkRemoteActorJoinPackets.CreateJoinReply(true, ActorRef("node-b", 2));
        var rejected = ZLinkRemoteActorJoinPackets.CreateJoinReply(false, ActorRef("rejected", 0));
        admissions.RecordJoinOutcome(request, targetSpot, accepted, TimeSpan.FromSeconds(5));

        time.Advance(TimeSpan.FromSeconds(5));

        Assert.True(admissions.TryExpirePreparedCommit(request, targetSpot, rejected));
        Assert.True(admissions.TryGetJoinOutcome(request, targetSpot, out var outcome));
        Assert.False(outcome.Accepted);
        var completion = new ZLinkRemoteActorHandoffCompletionRequest(
            request.ActorId,
            request.HandoffId,
            request.SourceSpotId,
            request.SourceNodeRid,
            targetSpot,
            []);
        // A completion the target no longer honors is terminal for the
        // source's reconciliation (RequestRejected), never retried.
        var lateCompletion = Assert.Throws<ZLinkFrameworkException>(() =>
            admissions.TryBeginCompletion(completion, targetSpot));
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, lateCompletion.Kind);
    }

    [Fact]
    public void PreparedAcceptance_CanBeAtomicallyCompensatedToRejection()
    {
        var admissions = new ZLinkActorHandoffAdmissions();
        const string targetSpot = "spot-1";
        var request = CommitRequest("handoff-compensated", []);
        var accepted = ZLinkRemoteActorJoinPackets.CreateJoinReply(true, ActorRef("node-b", 2));
        var rejected = ZLinkRemoteActorJoinPackets.CreateJoinReply(false, ActorRef("rejected", 0));

        admissions.RecordJoinOutcome(request, targetSpot, accepted, TimeSpan.FromSeconds(5));
        admissions.RejectPreparedJoinOutcome(request, targetSpot, rejected);

        Assert.True(admissions.TryGetJoinOutcome(request, targetSpot, out var outcome));
        Assert.False(outcome.Accepted);
    }

    [Fact]
    public async Task ExpiredAdmission_IsAtomicallyReplacedByTheNewDecision()
    {
        var time = new ManualTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(time);
        const string targetSpot = "spot-1";
        var expired = AdmissionRequest(time, "handoff-1");
        admissions.Register(
            expired,
            targetSpot,
            new ZLinkRemoteActorAdmissionReply(true, "application/json", [1], expired.DeadlineUnixTimeMilliseconds));
        time.Advance(TimeSpan.FromSeconds(6));
        var replacement = AdmissionRequest(time, "handoff-1");
        var calls = 0;

        var reply = await admissions.AdmitAsync(
            replacement,
            targetSpot,
            _ =>
            {
                calls++;
                return ValueTask.FromResult(new ZLinkRemoteActorAdmissionReply(
                    true,
                    "application/json",
                    [2],
                    replacement.DeadlineUnixTimeMilliseconds));
            },
            CancellationToken.None);

        Assert.Equal(1, calls);
        Assert.Equal([2], reply.Reply);
        Assert.True(admissions.TryGetReply(replacement, targetSpot, out var stored));
        Assert.Same(reply, stored);
    }

    [Fact]
    public async Task Abort_WaitsForDurableCapacityCleanup()
    {
        var cleanupStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var allowCleanup = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var time = new ManualTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(
            time,
            abortCapacityReservation: async (_, _) =>
            {
                cleanupStarted.TrySetResult();
                await allowCleanup.Task;
                return ZLinkRelocationCapacityAbortResult.Aborted;
            });
        var request = AdmissionRequest(time, "handoff-cleanup");
        const string targetSpot = "spot-1";
        _ = await admissions.AdmitReservedAsync(
            request,
            targetSpot,
            _ => ValueTask.FromResult(
                new ZLinkActorHandoffAdmissionDecision(
                    new ZLinkRemoteActorAdmissionReply(
                        true,
                        "application/json",
                        [1],
                        request.DeadlineUnixTimeMilliseconds),
                    default,
                    new ZLinkRelocationCapacityFence("capacity-cleanup"))),
            CancellationToken.None);

        var abort = admissions.AbortAsync(request.HandoffId).AsTask();
        await cleanupStarted.Task.WaitAsync(TimeSpan.FromSeconds(2));
        Assert.False(abort.IsCompleted);

        allowCleanup.TrySetResult();
        await abort.WaitAsync(TimeSpan.FromSeconds(2));
    }

    [Fact]
    public async Task StaleCapacityCleanup_KeepsAdmissionOwnedUntilRetryConverges()
    {
        var time = new ManualTimeProvider();
        var result = ZLinkRelocationCapacityAbortResult.Stale;
        var calls = 0;
        var admissions = new ZLinkActorHandoffAdmissions(
            time,
            abortCapacityReservation: (_, _) =>
            {
                calls++;
                return ValueTask.FromResult(result);
            });
        var request = AdmissionRequest(time, "handoff-stale-cleanup");
        const string targetSpot = "spot-1";
        _ = await admissions.AdmitReservedAsync(
            request,
            targetSpot,
            _ => ValueTask.FromResult(
                new ZLinkActorHandoffAdmissionDecision(
                    new ZLinkRemoteActorAdmissionReply(
                        true,
                        "application/json",
                        [1],
                        request.DeadlineUnixTimeMilliseconds),
                    default,
                    new ZLinkRelocationCapacityFence("capacity-stale"))),
            CancellationToken.None);

        await Assert.ThrowsAsync<InvalidOperationException>(
            () => admissions.AbortAsync(request.HandoffId).AsTask());
        Assert.Equal(3, calls);
        Assert.True(
            admissions.TryGetReply(request, targetSpot, out _));

        result = ZLinkRelocationCapacityAbortResult.Aborted;
        await admissions.AbortAsync(request.HandoffId);
        Assert.False(
            admissions.TryGetReply(request, targetSpot, out _));
    }

    [Fact]
    public async Task ShutdownCleanup_UsesBoundedCancellationAndRetainsOwner()
    {
        var time = new ManualTimeProvider();
        var admissions = new ZLinkActorHandoffAdmissions(
            time,
            abortCapacityReservation: async (_, cancellationToken) =>
            {
                await Task.Delay(
                    Timeout.InfiniteTimeSpan,
                    cancellationToken);
                return ZLinkRelocationCapacityAbortResult.Aborted;
            });
        var request = AdmissionRequest(time, "handoff-shutdown-deadline");
        const string targetSpot = "spot-1";
        _ = await admissions.AdmitReservedAsync(
            request,
            targetSpot,
            _ => ValueTask.FromResult(
                new ZLinkActorHandoffAdmissionDecision(
                    new ZLinkRemoteActorAdmissionReply(
                        true,
                        "application/json",
                        [1],
                        request.DeadlineUnixTimeMilliseconds),
                    default,
                    new ZLinkRelocationCapacityFence(
                        "capacity-shutdown"))),
            CancellationToken.None);
        using var deadline = new CancellationTokenSource(
            TimeSpan.FromMilliseconds(50));

        var error = await Assert.ThrowsAsync<AggregateException>(
            () => admissions.ResetGenerationAsync(deadline.Token)
                .AsTask());

        Assert.Contains(
            error.Flatten().InnerExceptions,
            exception => exception is OperationCanceledException);
        Assert.True(
            admissions.TryGetReply(request, targetSpot, out _));
        using var drainDeadline = new CancellationTokenSource(
            TimeSpan.FromMilliseconds(50));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => admissions.WaitUntilDrainSafeAsync(
                drainDeadline.Token));
    }

    [Fact]
    public async Task CallbackAfterDeadline_RetainsCleanupOwnerUntilStoreConverges()
    {
        var time = new ManualTimeProvider();
        var result = ZLinkRelocationCapacityAbortResult.Stale;
        var admissions = new ZLinkActorHandoffAdmissions(
            time,
            abortCapacityReservation: (_, _) =>
                ValueTask.FromResult(result));
        var request = AdmissionRequest(
            time,
            "handoff-callback-after-deadline");
        const string targetSpot = "spot-1";

        await Assert.ThrowsAsync<ZLinkFrameworkException>(
            () => admissions.AdmitReservedAsync(
                    request,
                    targetSpot,
                    _ =>
                    {
                        time.Advance(TimeSpan.FromSeconds(6));
                        return ValueTask.FromResult(
                            new ZLinkActorHandoffAdmissionDecision(
                                new ZLinkRemoteActorAdmissionReply(
                                    true,
                                    "application/json",
                                    [1],
                                    request.DeadlineUnixTimeMilliseconds),
                                default,
                                new ZLinkRelocationCapacityFence(
                                    "capacity-after-deadline")));
                    },
                    CancellationToken.None)
                .AsTask());

        using (var drainDeadline = new CancellationTokenSource(
                   TimeSpan.FromMilliseconds(50)))
            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => admissions.WaitUntilDrainSafeAsync(
                    drainDeadline.Token));

        result = ZLinkRelocationCapacityAbortResult.Aborted;
        time.Advance(TimeSpan.FromSeconds(1));
        await admissions.WaitUntilDrainSafeAsync(
            new CancellationTokenSource(TimeSpan.FromSeconds(1)).Token);
    }

    private static void Capture(
        ZLinkActorRuntimeState state,
        string bodyText,
        string sessionRid,
        ZlinkStreamMessageKind kind = ZlinkStreamMessageKind.Send,
        ulong requestId = 1,
        uint flags = 0)
    {
        using var body = Message.From(System.Text.Encoding.UTF8.GetBytes(bodyText));
        using var frame = Frame(body, ActorRef("node-a", 1), sessionRid, kind, requestId, flags);

        Assert.Equal(
            ZLinkActorHandoffCaptureResult.Captured,
            state.Handoff.TryCapture(frame));
    }

    private static ZLinkSpotActorFrame Frame(
        Message body,
        ZLinkBackendActorRef actor,
        string sessionRid,
        ZlinkStreamMessageKind kind = ZlinkStreamMessageKind.Send,
        ulong requestId = 1,
        uint flags = 0)
    {
        var sourceNodeRid = RoutingId.From("session-node");
        const ulong sourceNodeGeneration = 7;
        var frame = new ZLinkSpotActorFrame(
            actor,
            actor,
            sourceNodeRid,
            RoutingId.From(sessionRid),
            requestId,
            flags,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(11, requestId),
                MessageFollowHopCount: 1,
                TargetNodeGeneration: 13,
                AuthorityOwnerGeneration: 17,
                OwnerLeaseGeneration: 19,
                ReplyRequestId: requestId,
                ReplyFlags: flags),
            new ZlinkStreamHeader(
                kind,
                ZlinkStreamCodec.Raw,
                kind == ZlinkStreamMessageKind.Request
                    ? ZlinkStreamHeaderFlags.HasRequestSeq
                    : ZlinkStreamHeaderFlags.None,
                kind == ZlinkStreamMessageKind.Request ? new ZlinkStreamRequestSeq(42) : null,
                "Packet",
                ZlinkStreamMetadata.Empty),
            Message.From(body.AsReadOnlySpan()),
            sourceNodeGeneration,
            new ZLinkServiceWireCodec.RequestSourceFence(
                "actor-handoff-source",
                23,
                sourceNodeRid,
                sourceNodeGeneration));
        if (kind == ZlinkStreamMessageKind.Request)
            frame.BindRelocationReplyRoute(requestId);
        return frame;
    }

    private static string DecodeBody(ZLinkActorHandoffFrame frame)
        => System.Text.Encoding.UTF8.GetString(frame.Body);

    private static ZLinkActorHandoffFrame HandoffFrame(
        ulong requestId,
        long arrivalIndex)
        => new([], 0, [], [], requestId, 0, [], [], arrivalIndex);

    private static ZlinkStreamHeader Header(string packetName)
        => new(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Raw,
            ZlinkStreamHeaderFlags.None,
            null,
            packetName,
            ZlinkStreamMetadata.Empty);

    private static ZLinkBackendActorRef ActorRef(string nodeRid, ulong generation)
        => new(RoutingId.From(nodeRid), "actor-1", generation);

    private static IReadOnlyList<ZLinkActorHandoffFrame> Cutover(
        ZLinkActorRuntimeState state,
        int committedFrameCount,
        ZLinkBackendActorRef source,
        ZLinkBackendActorRef target)
        => state.Handoff.CutoverCaptureToMessageFollow(
            committedFrameCount,
            source,
            target,
            "mesh-a",
            sourceNodeGeneration: 1,
            targetNodeGeneration: 1,
            sourceAuthorityOwnerGeneration: 1,
            targetAuthorityOwnerGeneration: 2,
            sourceOwnerLeaseGeneration: 1,
            targetOwnerLeaseGeneration: 2);

    private static ZLinkRemoteActorJoinRequest CommitRequest(
        string handoffId,
        IReadOnlyList<ZLinkActorHandoffFrame> frames)
        => new(
            "actor-1",
            "warrior",
            handoffId,
            null,
            null,
            "application/json",
            "root-1",
            7,
            Guid.Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
            1,
            new byte[32],
            "application/json",
            [],
            frames,
            "source-spot",
            [2],
            1,
            1);

    private static ZLinkRemoteActorAdmissionRequest AdmissionRequest(
        TimeProvider timeProvider,
        string handoffId)
        => new(
            "actor-1",
            "warrior",
            "source-spot",
            [2],
            "application/json",
            [],
            handoffId,
            (timeProvider.GetUtcNow() + TimeSpan.FromSeconds(5)).ToUnixTimeMilliseconds());

}
