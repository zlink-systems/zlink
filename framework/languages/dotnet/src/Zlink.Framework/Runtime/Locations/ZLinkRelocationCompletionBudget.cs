namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkRelocationCompletionBudget
{
    private readonly IReadOnlyDictionary<
        (ZLinkCanonicalRequestSourceFence Source, string OperationId),
        (ZLinkRelocationCompletionEntry Entry, ulong Size)
    > _fallbacks;
    private readonly ulong _initialSize;
    private readonly ulong _maximumBytes;

    internal ZLinkRelocationCompletionBudget(
        Guid relocationId,
        IEnumerable<ZLinkRelocationCompletionEntry> fallbacks,
        ulong maximumBytes = ZLinkRelocationTreeStore.MaxLogicalBytes
    )
    {
        _maximumBytes = maximumBytes;
        var reservations =
            new Dictionary<
                (ZLinkCanonicalRequestSourceFence Source, string OperationId),
                (ZLinkRelocationCompletionEntry Entry, ulong Size)
            >();
        foreach (var entry in fallbacks)
            if (
                !reservations.TryAdd(
                    (entry.Source, entry.OperationId),
                    (entry, ZLinkRelocationCompletionRecord.ReservedEntrySize(entry))
                )
            )
                throw new ZLinkRelocationDataLostException(
                    "Relocation completion has a duplicate accepted request."
                );
        _fallbacks = reservations;
        _initialSize = (ulong)new ZLinkRelocationCompletionRecord(relocationId, []).Encode().Length;
        if (_fallbacks.Count > 0)
        {
            // The empty record includes both brackets. Each entry adds one
            // separator except the first.
            _initialSize--;
            foreach (var fallback in _fallbacks.Values)
                _initialSize = checked(_initialSize + fallback.Size);
        }
    }

    internal ulong AcceptedRequests => (ulong)_fallbacks.Count;

    internal void EnsureFitsBeforeCommit()
    {
        if (_fallbacks.Count != 0 && _initialSize > _maximumBytes)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InternalFailure,
                "Relocation completion cannot reserve the failed replies for all accepted requests."
            );
    }

    internal ZLinkRelocationCompletionEntry SelectReply(
        IReadOnlyList<ZLinkRelocationCompletionEntry> entries,
        ZLinkRelocationCompletionEntry candidate
    )
    {
        var length = _initialSize;
        foreach (var entry in entries)
            length = ReplaceReservation(length, entry);
        return ReplaceReservation(length, candidate) <= _maximumBytes
            ? candidate
            : GetFallback(candidate);
    }

    private ulong ReplaceReservation(ulong total, ZLinkRelocationCompletionEntry entry) =>
        checked(
            total
            - GetReservation(entry).Size
            + ZLinkRelocationCompletionRecord.ReservedEntrySize(entry)
        );

    internal ZLinkRelocationCompletionEntry GetFallback(ZLinkRelocationCompletionEntry entry) =>
        GetReservation(entry).Entry;

    private (ZLinkRelocationCompletionEntry Entry, ulong Size) GetReservation(
        ZLinkRelocationCompletionEntry entry
    ) =>
        _fallbacks.TryGetValue((entry.Source, entry.OperationId), out var fallback)
        && fallback.Entry.ObjectKey == entry.ObjectKey
        && fallback.Entry.ReplyRouteId == entry.ReplyRouteId
            ? fallback
            : throw new ZLinkRelocationDataLostException(
                "The relocation completion has no size reservation for its accepted request."
            );
}
