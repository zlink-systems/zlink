using Systems.Zlink;
using Zlink.Framework.LocationProvider;

internal sealed class ActorDestroyStore(
    ZLinkRedisLocationStore inner,
    ActorDestroyEvents events,
    string actorId,
    string meshName
) : IZLinkLocationStore, IAsyncDisposable
{
    private readonly ZLinkStoreKey _actorKey = OutageCanonicalStoreReader.ActorKey(actorId);
    private readonly Hold _delete = new();
    private readonly Hold _closingCas = new();
    private readonly SemaphoreSlim _leaseWrites = new(1);
    private OutageCanonicalStoreReader.Authority? _original;
    private ZLinkStoreKey? _endedLease;
    private long _validUntilTicks;
    private readonly AsyncLocal<bool> _createInProgress = new();
    private int _createObservedClosing;
    private int _observedValidLease;
    private int _observedInvalidLease;

    public void HoldDelete() => _delete.Arm();

    public void ResumeDelete() => _delete.Release(() => events.Write("ownerDeleteResumed"));

    public void HoldCas() => _closingCas.Arm();

    public void FailCas() => _closingCas.Release(() => events.Write("closingCasFailed"));

    public void StopHolding()
    {
        _delete.Cancel();
        _closingCas.Cancel();
    }

    public void BeginCreate() => _createInProgress.Value = true;

    public void EndCreate() => _createInProgress.Value = false;

    public async ValueTask<ZLinkStoreReadResult> ReadAsync(
        ZLinkStoreKey key,
        CancellationToken cancellationToken = default
    )
    {
        var result = await inner.ReadAsync(key, cancellationToken);
        if (events.Owner || key != _actorKey || result is not ZLinkStoreReadResult.Found found)
            return result;

        var authority = OutageCanonicalStoreReader.ReadAuthority(found.Value.Bytes);
        var payload = OutageCanonicalStoreReader.ReadActorPayload(authority.Payload);
        if (
            payload is { State: 2, OperationKind: 3 }
            && authority.ObjectGeneration == Volatile.Read(ref _original)?.ObjectGeneration
        )
        {
            if (_createInProgress.Value && Interlocked.Exchange(ref _createObservedClosing, 1) == 0)
                events.Write("createObservedClosing");

            if (
                events.Scenario == "f"
                && Volatile.Read(ref _observedValidLease) == 0
                && await ReadLeaseAsync(cancellationToken) == "valid"
                && Interlocked.Exchange(ref _observedValidLease, 1) == 0
            )
                events.Write("callerObservedClosingWithValidLease");
        }
        return result;
    }

    public ValueTask<ZLinkStoreScanResult> ScanAsync(
        ZLinkStoreScanRequest request,
        CancellationToken cancellationToken = default
    ) => inner.ScanAsync(request, cancellationToken);

    public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
        ZLinkStoreWriteRequest request,
        CancellationToken cancellationToken = default
    )
    {
        if (
            events.Owner
            && events.Scenario == "f"
            && request
                .Mutations.OfType<ZLinkStoreMutation.Put>()
                .Any(put => put.Key.Value.StartsWith("owner-lease\0", StringComparison.Ordinal))
        )
        {
            await _leaseWrites.WaitAsync(cancellationToken);
            try
            {
                if (
                    _endedLease is { } ended
                    && request
                        .Mutations.OfType<ZLinkStoreMutation.Put>()
                        .Any(put => put.Key == ended)
                )
                    throw new InvalidOperationException(
                        "Injected owner lease termination blocks renewal and reclaim."
                    );
                return await inner.WriteAsync(request, cancellationToken);
            }
            finally
            {
                _leaseWrites.Release();
            }
        }

        var original = Volatile.Read(ref _original);
        var put = request
            .Mutations.OfType<ZLinkStoreMutation.Put>()
            .SingleOrDefault(mutation => mutation.Key == _actorKey);
        var deleting = request
            .Mutations.OfType<ZLinkStoreMutation.Delete>()
            .Any(mutation => mutation.Key == _actorKey);
        if (put is null && !deleting)
            return await inner.WriteAsync(request, cancellationToken);

        var written = put is null ? null : OutageCanonicalStoreReader.ReadAuthority(put.Bytes);
        var payload = written is null
            ? null
            : OutageCanonicalStoreReader.ReadActorPayload(written.Payload);
        var closing =
            original is not null
            && written?.ObjectGeneration == original.ObjectGeneration
            && payload is { State: 2, OperationKind: 3 };
        if (
            events.Owner
            && closing
            && _closingCas.Pending(() => events.Write("closingCasHeld"), out var cas)
        )
        {
            await cas.WaitAsync(cancellationToken);
            throw new ActorDestroyClosingCasFailure();
        }
        if (
            events.Owner
            && deleting
            && original is not null
            && _delete.Pending(() => events.Write("ownerDeleteHeld"), out var delete)
        )
        {
            await delete.WaitAsync(cancellationToken);
        }
        // The caller's release decision is this write, so observe the lease here, before
        // submission, as the Java and Node hosts do. An authority read can precede the
        // Framework's own lease read and miss the expiry.
        if (
            !events.Owner
            && deleting
            && original is not null
            && events.Scenario == "f"
            && await ReadLeaseAsync(cancellationToken) != "valid"
            && Interlocked.Exchange(ref _observedInvalidLease, 1) == 0
        )
            events.Write("callerObservedClosingForRelease");

        var result = await inner.WriteAsync(request, cancellationToken);
        if (result is ZLinkStoreWriteResult.Applied committed)
        {
            if (events.Owner && closing)
                events.Write("closingCommitted");
            if (events.Owner && deleting && original is not null)
                events.WriteAt(committed.StoreNow, "ownerDeleteCommitted");
            if (!events.Owner && deleting && original is not null && events.Scenario is "d" or "f")
                RecordRelease(request, committed, original);
            if (
                !events.Owner
                && original is not null
                && written is not null
                && written.ObjectGeneration > original.ObjectGeneration
                && payload!.SteadyReady
            )
                events.Write("newReadyCommitted");
            events.Write("authorityWritten");
        }
        return result;
    }

    public async Task CaptureOriginalAsync(ActorRef actor, CancellationToken cancellationToken)
    {
        if (Volatile.Read(ref _original) is not null)
            return;
        var row = await inner.ReadAsync(_actorKey, cancellationToken);
        if (row is not ZLinkStoreReadResult.Found found)
            throw new InvalidOperationException("Initial authority row is missing.");
        var authority = OutageCanonicalStoreReader.ReadAuthority(found.Value.Bytes);
        if (authority.ObjectGeneration != actor.ObjectGeneration)
            throw new InvalidOperationException(
                "Initial authority generation differs from the public ActorRef."
            );
        Volatile.Write(ref _original, authority);
        await ReadLeaseAsync(cancellationToken);
    }

    public async Task AuthorityAsync(string label, CancellationToken cancellationToken)
    {
        var row = await inner.ReadAsync(_actorKey, cancellationToken);
        if (row is not ZLinkStoreReadResult.Found found)
        {
            events.Write("authority", ("label", label), ("read", "Missing"));
            return;
        }
        var authority = OutageCanonicalStoreReader.ReadAuthority(found.Value.Bytes);
        var payload = OutageCanonicalStoreReader.ReadActorPayload(authority.Payload);
        events.Write(
            "authority",
            ("label", label),
            ("read", "Found"),
            ("version", found.Value.Version.Value),
            ("generation", authority.ObjectGeneration),
            ("state", payload.State),
            ("operationKind", payload.OperationKind),
            ("owner", payload.OwnerRid),
            ("ownerId", authority.OwnerId),
            ("ownerGeneration", authority.AuthorityOwnerGeneration),
            ("leaseGeneration", authority.OwnerLeaseGeneration),
            ("nodeGeneration", payload.NodeGeneration),
            ("actorId", payload.ActorId),
            ("actorType", payload.ActorType),
            ("spot", payload.Spot),
            (
                "spotKind",
                payload.SpotKind switch
                {
                    1 => "entry",
                    2 => "user",
                    _ => payload.SpotKind.ToString(
                        System.Globalization.CultureInfo.InvariantCulture
                    ),
                }
            )
        );
    }

    public async Task WaitForPlacementAsync(
        RoutingId nodeRid,
        int weight,
        CancellationToken cancellationToken
    )
    {
        var key = OutageCanonicalStoreReader.MeshKey(meshName, nodeRid);
        while (true)
        {
            var row = await inner.ReadAsync(key, cancellationToken);
            if (
                row is ZLinkStoreReadResult.Found found
                && OutageCanonicalStoreReader.ReadPlacementWeight(found.Value.Bytes) == weight
            )
                return;
            await Task.Delay(10, cancellationToken);
        }
    }

    public async Task<string> ReadLeaseAsync(CancellationToken cancellationToken)
    {
        var original =
            Volatile.Read(ref _original)
            ?? throw new InvalidOperationException("Original authority has not been observed.");
        var row = await inner.ReadAsync(
            OutageCanonicalStoreReader.OwnerKey(original.OwnerId),
            cancellationToken
        );
        if (row is ZLinkStoreReadResult.Missing missing)
        {
            var deadline = Interlocked.Read(ref _validUntilTicks);
            return deadline > 0 && missing.StoreNow.UtcTicks >= deadline ? "expired" : "missing";
        }
        var value = ((ZLinkStoreReadResult.Found)row).Value;
        if (OutageCanonicalStoreReader.ReadOwnerLease(value.Bytes) != original.OwnerLeaseGeneration)
            return "replaced";
        var expires =
            value.ExpiresAt ?? throw new InvalidDataException("Owner lease has no expiry.");
        Interlocked.Exchange(ref _validUntilTicks, expires.UtcTicks);
        return value.StoreNow < expires ? "valid" : "expired";
    }

    public async Task EndLeaseAsync(CancellationToken cancellationToken)
    {
        var original =
            Volatile.Read(ref _original)
            ?? throw new InvalidOperationException("Original authority has not been observed.");
        var key = OutageCanonicalStoreReader.OwnerKey(original.OwnerId);
        await _leaseWrites.WaitAsync(cancellationToken);
        try
        {
            var row = await inner.ReadAsync(key, cancellationToken);
            if (
                row is not ZLinkStoreReadResult.Found found
                || OutageCanonicalStoreReader.ReadOwnerLease(found.Value.Bytes)
                    != original.OwnerLeaseGeneration
            )
                throw new InvalidOperationException("Original owner lease is no longer present.");
            _endedLease = key;
            var result = await inner.WriteAsync(
                new(
                    [new ZLinkStoreCondition.Version(key, found.Value.Version)],
                    [new ZLinkStoreMutation.Delete(key)]
                ),
                cancellationToken
            );
            if (result is not ZLinkStoreWriteResult.Applied committed)
                throw new InvalidOperationException(
                    $"Owner lease termination returned {result.GetType().Name}."
                );
            events.WriteAt(committed.StoreNow, "ownerLeaseEnded");
        }
        finally
        {
            _leaseWrites.Release();
        }
    }

    private void RecordRelease(
        ZLinkStoreWriteRequest request,
        ZLinkStoreWriteResult.Applied committed,
        OutageCanonicalStoreReader.Authority original
    )
    {
        var ownerKey = OutageCanonicalStoreReader.OwnerKey(original.OwnerId);
        var rid = OutageCanonicalStoreReader.ReadActorPayload(original.Payload).OwnerRid;
        var descriptorKey = OutageCanonicalStoreReader.MeshKey(meshName, rid);
        var capacityKey = new ZLinkStoreKey(
            $"zlink:v12:capacity\0{meshName}\0{rid.ToHex()}\0actors\0active"
        );
        var versions = request
            .Conditions.OfType<ZLinkStoreCondition.Version>()
            .ToDictionary(condition => condition.Key);
        var missing = request
            .Conditions.OfType<ZLinkStoreCondition.Missing>()
            .Select(condition => condition.Key)
            .ToHashSet();
        // An Entry Spot Actor's membership is carried by its authority row.
        // Redis checks the counter and rejects a negative result within the Add batch.
        var delta = request
            .Mutations.OfType<ZLinkStoreMutation.Add>()
            .Where(add => add.Key == capacityKey)
            .Sum(add => add.Delta);
        events.Write(
            "releaseCommitted",
            ("version", versions.GetValueOrDefault(_actorKey)?.Expected.Value),
            ("invalidLease", Flag(missing.Contains(ownerKey) || versions.ContainsKey(ownerKey))),
            (
                "descriptorCondition",
                Flag(missing.Contains(descriptorKey) || versions.ContainsKey(descriptorKey))
            ),
            (
                "capacityCondition",
                Flag(delta < 0 && committed.PutVersions.ContainsKey(capacityKey))
            ),
            ("membershipRemoved", "true"),
            ("capacityDelta", delta),
            (
                "relocationPolicyCondition",
                Flag(
                    versions
                        .Keys.Concat(missing)
                        .Any(key =>
                            key != descriptorKey
                            && key.Value.StartsWith("mesh-node\0", StringComparison.Ordinal)
                        )
                )
            )
        );
    }

    private static string Flag(bool value) => value ? "true" : "false";

    public ValueTask DisposeAsync() => inner.DisposeAsync();

    private sealed class Hold
    {
        private TaskCompletionSource? _signal;
        private int _observed;

        public void Arm()
        {
            var signal = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            Volatile.Write(ref _observed, 0);
            if (Interlocked.CompareExchange(ref _signal, signal, null) is not null)
                throw new InvalidOperationException("A provider hold is already armed.");
        }

        public bool Pending(Action mark, out Task task)
        {
            var signal = Volatile.Read(ref _signal);
            task = signal?.Task ?? Task.CompletedTask;
            if (signal is null)
                return false;
            if (Interlocked.Exchange(ref _observed, 1) == 0)
                mark();
            return true;
        }

        public void Release(Action mark)
        {
            var signal =
                Interlocked.Exchange(ref _signal, null)
                ?? throw new InvalidOperationException("No provider hold is armed.");
            mark();
            signal.SetResult();
        }

        public void Cancel() => Volatile.Read(ref _signal)?.TrySetCanceled();
    }
}

internal sealed class ActorDestroyClosingCasFailure() : IOException("injectedClosingCasFailure");
