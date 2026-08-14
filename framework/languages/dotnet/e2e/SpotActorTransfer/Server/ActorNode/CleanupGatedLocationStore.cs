using System.Collections.Concurrent;
using Zlink.Framework.LocationProvider;

namespace SpotActorTransfer.ActorNode;

internal sealed class ActorCleanupGateStore(EvidenceStore evidence)
{
    private const int ExpectedParticipantCount = 2;
    private readonly ConcurrentDictionary<string, CleanupGate> _gates =
        new(StringComparer.Ordinal);
    private readonly object _armLock = new();

    public bool Arm(string actorId, string scenario)
    {
        lock (_armLock)
        {
            if (!_gates.IsEmpty)
                return false;

            return _gates.TryAdd(
                actorId,
                new CleanupGate(
                    scenario,
                    actorId,
                    "source_cleanup_attempt",
                    "source_cleanup_completed",
                    CleanupGateMode.SourceCleanup,
                    allowAttempt: false));
        }
    }

    public bool ArmTargetPublication(string actorId, string scenario)
    {
        lock (_armLock)
        {
            if (!_gates.IsEmpty)
                return false;

            return _gates.TryAdd(
                actorId,
                new CleanupGate(
                    scenario,
                    actorId,
                    "target_publication_gate",
                    "target_publication_gate_released",
                    CleanupGateMode.TargetPublication,
                    allowAttempt: true));
        }
    }

    public bool ActivateTargetPublicationAfterRestore(string actorId) =>
        _gates.TryGetValue(actorId, out var gate)
        && gate.ActivateTargetPublication();

    public bool AllowAttempt(string actorId) =>
        _gates.TryGetValue(actorId, out var gate)
        && gate.AllowAttempt.TrySetResult();

    public bool Release(string actorId) =>
        _gates.TryGetValue(actorId, out var gate)
        && gate.Release.TrySetResult();

    public void ObserveAppliedWrite(
        ZLinkStoreWriteRequest request,
        ZLinkStoreWriteResult result)
    {
        if (result is not ZLinkStoreWriteResult.Applied)
            return;

        var gate = _gates.Values.SingleOrDefault();
        if (gate?.Mode != CleanupGateMode.TargetPublication)
            return;

        gate.ObserveAppliedTargetWrite(request);
    }

    public async ValueTask WaitBeforeWriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken)
    {
        var gate = _gates.Values.SingleOrDefault();
        if (gate is null)
            return;

        if (gate.Mode == CleanupGateMode.SourceCleanup)
        {
            if (!IsSourceCleanupDelete(request))
                return;

            await gate.AllowAttempt.Task.WaitAsync(cancellationToken);
        }
        else if (!IsTargetPublicationDelete(request)
                 || !gate.TryBeginTargetDeleteBlock())
        {
            return;
        }

        if (Interlocked.Exchange(ref gate.AttemptObserved, 1) == 0)
            evidence.Add(
                gate.Scenario,
                gate.ActorId,
                gate.AttemptEvidenceKind,
                "opaque-delete-batch");

        await gate.Release.Task.WaitAsync(cancellationToken);
        if (TryRemove(gate))
            evidence.Add(
                gate.Scenario,
                gate.ActorId,
                gate.CompletionEvidenceKind,
                "opaque-delete-batch");
    }

    private bool TryRemove(CleanupGate gate)
    {
        lock (_armLock)
        {
            return _gates.TryGetValue(gate.ActorId, out var activeGate)
                && ReferenceEquals(activeGate, gate)
                && _gates.TryRemove(gate.ActorId, out _);
        }
    }

    private static bool IsSourceCleanupDelete(
        ZLinkStoreWriteRequest request) =>
        request.Conditions.Count == 0
        && request.Mutations.Count > 0
        && request.Mutations.All(static mutation =>
            mutation is ZLinkStoreMutation.Delete);

    private static bool IsTargetPublicationDelete(
        ZLinkStoreWriteRequest request) =>
        request.Conditions.Count == 0
        && request.Mutations.Count == ExpectedParticipantCount
        && request.Mutations.All(static mutation =>
            mutation is ZLinkStoreMutation.Delete);

    private enum CleanupGateMode
    {
        SourceCleanup,
        TargetPublication
    }

    private enum TargetPublicationStage
    {
        Inactive,
        AwaitingNewOwnerParticipants,
        AwaitingNormalizedMarker,
        AwaitingDelete,
        BlockingDelete
    }

    private sealed class CleanupGate(
        string scenario,
        string actorId,
        string attemptEvidenceKind,
        string completionEvidenceKind,
        CleanupGateMode mode,
        bool allowAttempt)
    {
        private readonly object _targetStateLock = new();

        public string Scenario { get; } = scenario;
        public string ActorId { get; } = actorId;
        public string AttemptEvidenceKind { get; } = attemptEvidenceKind;
        public string CompletionEvidenceKind { get; } =
            completionEvidenceKind;
        public CleanupGateMode Mode { get; } = mode;
        public TaskCompletionSource AllowAttempt { get; } =
            CreateAllowAttempt(allowAttempt);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public int AttemptObserved;
        public int NewOwnerParticipantWritesObserved { get; private set; }
        public TargetPublicationStage TargetStage { get; private set; } =
            TargetPublicationStage.Inactive;

        public bool ActivateTargetPublication()
        {
            if (Mode != CleanupGateMode.TargetPublication)
                return false;

            lock (_targetStateLock)
            {
                if (TargetStage != TargetPublicationStage.Inactive)
                    return false;

                TargetStage =
                    TargetPublicationStage.AwaitingNewOwnerParticipants;
                return true;
            }
        }

        public void ObserveAppliedTargetWrite(
            ZLinkStoreWriteRequest request)
        {
            lock (_targetStateLock)
            {
                if (TargetStage
                        == TargetPublicationStage.AwaitingNewOwnerParticipants
                    && IsNewOwnerParticipantWrite(request))
                {
                    NewOwnerParticipantWritesObserved++;
                    if (NewOwnerParticipantWritesObserved == 2)
                        TargetStage =
                            TargetPublicationStage.AwaitingNormalizedMarker;
                    return;
                }

                if (TargetStage
                        == TargetPublicationStage.AwaitingNormalizedMarker
                    && IsNormalizedMarkerWrite(request))
                    TargetStage = TargetPublicationStage.AwaitingDelete;
            }
        }

        public bool TryBeginTargetDeleteBlock()
        {
            lock (_targetStateLock)
            {
                if (TargetStage == TargetPublicationStage.AwaitingDelete)
                    TargetStage = TargetPublicationStage.BlockingDelete;

                return TargetStage == TargetPublicationStage.BlockingDelete;
            }
        }

        private static bool IsNewOwnerParticipantWrite(
            ZLinkStoreWriteRequest request) =>
            request.Conditions.Count == 2
            && request.Conditions[0] is ZLinkStoreCondition.Version
            && request.Conditions[1] is ZLinkStoreCondition.Missing
                or ZLinkStoreCondition.Version
            && request.Mutations.Count == 3
            && request.Mutations.All(static mutation =>
                mutation is ZLinkStoreMutation.Put);

        private static bool IsNormalizedMarkerWrite(
            ZLinkStoreWriteRequest request) =>
            request.Conditions.Count == 1
            && request.Conditions[0] is ZLinkStoreCondition.Version
            && request.Mutations.Count == 1
            && request.Mutations[0] is ZLinkStoreMutation.Put;

        private static TaskCompletionSource CreateAllowAttempt(bool allowed)
        {
            var completion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            if (allowed)
                completion.SetResult();
            return completion;
        }
    }
}

/// <summary>
/// Delays the next isolated unconditional delete-only batch without
/// interpreting Framework-owned keys or payloads.
/// </summary>
internal sealed class CleanupGatedLocationStore(
    IZLinkLocationStore inner,
    ActorCleanupGateStore cleanupGates) :
    IZLinkLocationStore,
    IAsyncDisposable
{
    public ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default) =>
        inner.ReadAsync(key, cancellationToken);

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default)
    {
        await cleanupGates.WaitBeforeWriteAsync(request, cancellationToken);
        var result = await inner.WriteAsync(request, cancellationToken);
        cleanupGates.ObserveAppliedWrite(request, result);
        return result;
    }

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default) =>
        inner.ScanAsync(request, cancellationToken);

    public async ValueTask DisposeAsync()
    {
        switch (inner)
        {
            case IAsyncDisposable asyncDisposable:
                await asyncDisposable.DisposeAsync();
                break;
            case IDisposable disposable:
                disposable.Dispose();
                break;
        }
    }
}
