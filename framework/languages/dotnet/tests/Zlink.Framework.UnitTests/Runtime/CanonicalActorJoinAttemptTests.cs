using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class CanonicalActorJoinAttemptTests
{
    [Fact]
    public void Accepted_admission_is_not_a_public_join_completion()
    {
        var attempt = CreateAttempt();

        attempt.RecordAdmissionAccepted(Accepted(chunkLimit: 8192));

        Assert.Equal(ZLinkCanonicalActorJoinAttemptPhase.AdmissionAccepted,
            attempt.Phase);
        Assert.False(attempt.IsPublicCompletionTerminal);
        Assert.Equal<ulong>(8192, attempt.TargetReceiveChunkLimitBytes);
    }

    [Fact]
    public void Canonical_correlation_is_separate_from_public_completion_identity()
    {
        var attempt = CreateAttempt(correlation: 0x8000000000000001UL);

        Assert.Equal<ulong>(0x8000000000000001UL,
            attempt.WireAttemptKey.Correlation);
        Assert.NotEqual(attempt.WireAttemptKey.Correlation,
            attempt.PublicCompletionId.Low);
        Assert.Throws<ArgumentException>(() => ZLinkCanonicalActorJoinAttempt.Create(
            "actor-1", 7, SourceFence(), 41,
            new ZLinkActorJoinOperationId(0, 41),
            Authority("source"), Authority("target")));
    }

    [Fact]
    public void Relocation_and_reservation_identities_are_local_to_the_attempt()
    {
        var attempt = CreateAttempt();
        attempt.RecordAdmissionAccepted(Accepted(chunkLimit: 1));

        var prepareKey = attempt.GetPrepareBindingKey();

        Assert.Equal("11111111222243338444555555555555", attempt.HandoffId);
        Assert.Equal(Guid.Parse("11111111-2222-4333-8444-555555555555"),
            attempt.RelocationId);
        Assert.Equal(attempt.WireAttemptKey, prepareKey.WireAttemptKey);
        Assert.NotEqual(attempt.PublicCompletionId.Low,
            attempt.WireAttemptKey.Correlation);
    }

    [Fact]
    public void Pre_commit_failure_aborts_attempt_and_requests_source_seal_rollback()
    {
        var attempt = CreateAttempt();
        attempt.RecordAdmissionAccepted(Accepted(chunkLimit: 1));
        attempt.MarkSourceSealed();

        var abort = attempt.AbortBeforeCommit();

        Assert.Equal(ZLinkCanonicalActorJoinAttemptPhase.AbortedBeforeCommit,
            attempt.Phase);
        Assert.Equal(ZLinkCanonicalActorJoinSourceSealState.RollbackRequired,
            attempt.SourceSealState);
        Assert.True(abort.AbortTargetAdmission);
        Assert.True(abort.RollbackSourceSeal);
    }

    [Fact]
    public void Post_cas_failure_requires_reconciliation_without_source_replay()
    {
        var attempt = CreateAttempt();
        attempt.RecordAdmissionAccepted(Accepted(chunkLimit: 1));
        attempt.MarkTargetOnlyCasCommitted();

        var reconciliation = attempt.RequirePostCasReconciliation();

        Assert.Equal(ZLinkCanonicalActorJoinAttemptPhase.ReconciliationRequired,
            attempt.Phase);
        Assert.True(reconciliation.ReconcileCurrentAuthority);
        Assert.False(reconciliation.ReplaySource);
    }

    [Fact]
    public void Accepted_tail_only_projects_its_receive_chunk_limit_as_chunk_cap()
    {
        var attempt = CreateAttempt();
        var accepted = Accepted(chunkLimit: 4096);

        attempt.RecordAdmissionAccepted(accepted);

        Assert.Equal<ulong>(4096, attempt.TargetReceiveChunkLimitBytes);
        Assert.Equal(accepted.Spot, attempt.Admission!.Spot);
        Assert.Equal<ulong>(accepted.MembershipEpoch,
            attempt.Admission.MembershipEpoch);
    }

    private static ZLinkCanonicalActorJoinAttempt CreateAttempt(
        ulong correlation = 41) => ZLinkCanonicalActorJoinAttempt.Create(
        "actor-1",
        7,
        SourceFence(),
        correlation,
        new ZLinkActorJoinOperationId(17, 23),
        Authority("source"),
        Authority("target"),
        Guid.Parse("11111111-2222-4333-8444-555555555555"));

    private static ZLinkCanonicalActorJoinAdmissionAccepted Accepted(
        ulong chunkLimit) => new(
        new ZLinkSpotHandleSnapshot(
            "mesh",
            RoutingId.From("target-node"),
            "target-spot",
            13,
            ZLinkSpotKind.Entry,
            17,
            19,
            23),
        MembershipEpoch: 29,
        ReceiveChunkLimitBytes: chunkLimit,
        new ZLinkCanonicalActorJoinApplicationReply(
            "application/json", new byte[] { 1, 2, 3 }));

    private static ZLinkServiceWireCodec.RequestSourceFence SourceFence() => new(
        "source-owner",
        31,
        RoutingId.From("source-node"),
        37);

    private static ZLinkAuthoritySnapshot Authority(string node) => new(
        $"store-{node}",
        ReadOnlyMemory<byte>.Empty,
        7,
        11,
        $"{node}-owner",
        13,
        new ZLinkPlacementAllocation(
            ZLinkPlacementAllocationState.Active,
            ZLinkPlacementObjectKind.Actor,
            "player",
            new ZLinkMeshNodeDescriptorKey("mesh", RoutingId.From(node)),
            17,
            new ZLinkCapacityVector(1, 0, null)),
        null,
        DateTimeOffset.UnixEpoch);
}
