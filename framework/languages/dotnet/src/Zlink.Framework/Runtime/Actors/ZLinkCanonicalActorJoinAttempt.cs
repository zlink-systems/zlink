using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Actors;

// This is intentionally language-internal state.  Command 28 remains a
// closed schema message: its correlation is an admission idempotency key, not
// a relocation or public-completion identity (51-internal-service-wire-protocol
// and service-wire-v1.schema.json).
internal readonly record struct ZLinkCanonicalActorJoinWireAttemptKey(
    string ActorId,
    ulong ActorGeneration,
    ZLinkServiceWireCodec.RequestSourceFence Source,
    ulong Correlation);

internal sealed record ZLinkCanonicalActorJoinApplicationReply(
    string ContentType,
    ReadOnlyMemory<byte> Payload);

internal sealed record ZLinkCanonicalActorJoinAdmissionAccepted(
    ZLinkSpotHandleSnapshot Spot,
    ulong MembershipEpoch,
    ulong ReceiveChunkLimitBytes,
    ZLinkCanonicalActorJoinApplicationReply? ApplicationReply);

internal enum ZLinkCanonicalActorJoinAttemptPhase
{
    Created = 0,
    AdmissionAccepted = 1,
    TargetOnlyCasCommitted = 2,
    AbortedBeforeCommit = 3,
    ReconciliationRequired = 4,
    PublicCompletionDelivered = 5
}

internal enum ZLinkCanonicalActorJoinSourceSealState
{
    NotSealed = 0,
    Sealed = 1,
    RollbackRequired = 2
}

internal readonly record struct ZLinkCanonicalActorJoinPreCommitAbort(
    bool RollbackSourceSeal);

internal readonly record struct ZLinkCanonicalActorJoinPostCasReconciliation(
    bool ReconcileCurrentAuthority,
    bool ReplaySource);

/// <summary>
/// Local continuity context between canonical actorJoin admission and the
/// existing relocation chain.  It does not send a frame or change that chain.
/// </summary>
internal sealed class ZLinkCanonicalActorJoinAttempt
{
    private ZLinkCanonicalActorJoinAttempt(
        ZLinkCanonicalActorJoinWireAttemptKey wireAttemptKey,
        Guid relocationId,
        ZLinkActorJoinOperationId publicCompletionId,
        ZLinkAuthoritySnapshot sourceAuthority,
        ZLinkAuthoritySnapshot targetAuthority)
    {
        WireAttemptKey = wireAttemptKey;
        RelocationId = relocationId;
        HandoffId = relocationId.ToString("N");
        PublicCompletionId = publicCompletionId;
        SourceAuthority = sourceAuthority;
        TargetAuthority = targetAuthority;
    }

    internal ZLinkCanonicalActorJoinWireAttemptKey WireAttemptKey { get; }
    internal Guid RelocationId { get; }
    internal string HandoffId { get; }
    internal ZLinkActorJoinOperationId PublicCompletionId { get; }
    internal ZLinkAuthoritySnapshot SourceAuthority { get; }
    internal ZLinkAuthoritySnapshot TargetAuthority { get; }
    internal ZLinkCanonicalActorJoinAdmissionAccepted? Admission { get; private set; }
    internal ZLinkCanonicalActorJoinAttemptPhase Phase { get; private set; }
        = ZLinkCanonicalActorJoinAttemptPhase.Created;
    internal ZLinkCanonicalActorJoinSourceSealState SourceSealState { get; private set; }
        = ZLinkCanonicalActorJoinSourceSealState.NotSealed;
    internal bool IsPublicCompletionTerminal { get; private set; }

    // The cap is deliberately projected only from the canonical accepted tail.
    // The effective payload chunk size is calculated later with the local
    // server and in-flight limits (28-relocation-flow.md §4.2).
    internal ulong TargetReceiveChunkLimitBytes =>
        Admission?.ReceiveChunkLimitBytes ?? 0;

    internal static ZLinkCanonicalActorJoinAttempt Create(
        string actorId,
        ulong actorGeneration,
        ZLinkServiceWireCodec.RequestSourceFence source,
        ulong canonicalCorrelation,
        ZLinkActorJoinOperationId publicCompletionId,
        ZLinkAuthoritySnapshot sourceAuthority,
        ZLinkAuthoritySnapshot targetAuthority,
        Guid? relocationId = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actorId);
        ArgumentNullException.ThrowIfNull(sourceAuthority);
        ArgumentNullException.ThrowIfNull(targetAuthority);
        if (actorGeneration == 0
            || source.NodeRid.IsEmpty
            || source.NodeGeneration == 0
            || string.IsNullOrWhiteSpace(source.OwnerId)
            || source.LeaseGeneration == 0
            || canonicalCorrelation == 0
            || publicCompletionId == default)
            throw new ArgumentOutOfRangeException(nameof(canonicalCorrelation));

        // Do not collapse a one-word wire correlation into the two-word public
        // completion identity.  Both are opaque equality tokens.
        if (publicCompletionId.High == 0
            && publicCompletionId.Low == canonicalCorrelation)
            throw new ArgumentException(
                "The canonical correlation cannot be the public completion identity.",
                nameof(publicCompletionId));

        var id = relocationId ?? Guid.NewGuid();
        if (id == Guid.Empty)
            throw new ArgumentOutOfRangeException(nameof(relocationId));
        return new ZLinkCanonicalActorJoinAttempt(
            new ZLinkCanonicalActorJoinWireAttemptKey(
                actorId,
                actorGeneration,
                source,
                canonicalCorrelation),
            id,
            publicCompletionId,
            sourceAuthority,
            targetAuthority);
    }

    internal void RecordAdmissionAccepted(
        ZLinkCanonicalActorJoinAdmissionAccepted accepted)
    {
        ArgumentNullException.ThrowIfNull(accepted);
        if (Phase != ZLinkCanonicalActorJoinAttemptPhase.Created)
            throw new InvalidOperationException(
                "Canonical Actor Join admission can only be recorded once.");
        Admission = accepted;
        Phase = ZLinkCanonicalActorJoinAttemptPhase.AdmissionAccepted;
    }

    internal void MarkSourceSealed()
    {
        if (Phase != ZLinkCanonicalActorJoinAttemptPhase.AdmissionAccepted)
            throw new InvalidOperationException(
                "The source can only seal after admission is accepted.");
        SourceSealState = ZLinkCanonicalActorJoinSourceSealState.Sealed;
    }

    // Command 28 owns no target relocation state. Before target-only CAS a
    // failed source attempt rolls back only its own seal; command 40 owns any
    // target-stage abort under its separate relocation identity.
    internal ZLinkCanonicalActorJoinPreCommitAbort AbortBeforeCommit()
    {
        if (Phase is not ZLinkCanonicalActorJoinAttemptPhase.AdmissionAccepted)
            throw new InvalidOperationException(
                "Only an admitted, pre-commit attempt can be aborted.");
        var rollbackSourceSeal =
            SourceSealState == ZLinkCanonicalActorJoinSourceSealState.Sealed;
        Phase = ZLinkCanonicalActorJoinAttemptPhase.AbortedBeforeCommit;
        if (rollbackSourceSeal)
            SourceSealState = ZLinkCanonicalActorJoinSourceSealState.RollbackRequired;
        return new ZLinkCanonicalActorJoinPreCommitAbort(rollbackSourceSeal);
    }

    internal void MarkTargetOnlyCasCommitted()
    {
        if (Phase != ZLinkCanonicalActorJoinAttemptPhase.AdmissionAccepted)
            throw new InvalidOperationException(
                "Target-only CAS can only follow accepted admission.");
        Phase = ZLinkCanonicalActorJoinAttemptPhase.TargetOnlyCasCommitted;
    }

    // After target-only CAS, the source must never replay.  The caller uses
    // this signal to re-read/reconcile current authority instead.
    internal ZLinkCanonicalActorJoinPostCasReconciliation
        RequirePostCasReconciliation()
    {
        if (Phase != ZLinkCanonicalActorJoinAttemptPhase.TargetOnlyCasCommitted)
            throw new InvalidOperationException(
                "Post-CAS reconciliation requires a committed target authority.");
        Phase = ZLinkCanonicalActorJoinAttemptPhase.ReconciliationRequired;
        return new ZLinkCanonicalActorJoinPostCasReconciliation(
            ReconcileCurrentAuthority: true,
            ReplaySource: false);
    }

    internal void DeliverPublicCompletion()
    {
        if (Phase != ZLinkCanonicalActorJoinAttemptPhase.TargetOnlyCasCommitted)
            throw new InvalidOperationException(
                "Public completion requires the relocation commit path to finish.");
        Phase = ZLinkCanonicalActorJoinAttemptPhase.PublicCompletionDelivered;
        IsPublicCompletionTerminal = true;
    }
}
