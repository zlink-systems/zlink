using System.Text;
using Zlink.Framework.Internal;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.Runtime.Locations;

internal sealed partial class ZLinkProviderLocationRepository
{
    private static void AddAcceptedReplyRows(
        Guid relocationId,
        ZLinkLocationOwnerToken writer,
        IReadOnlyList<ZLinkRelocationCompletionEntry> entries,
        List<ZLinkStoreCondition> conditions,
        List<ZLinkStoreMutation> mutations
    )
    {
        var keys = new HashSet<ZLinkStoreKey>();
        foreach (var entry in entries)
        {
            var row = new ZLinkRelocationReplyRow(
                relocationId,
                writer,
                entry with
                {
                    Delivery = ZLinkRelocationCompletionDelivery.Accepted,
                    Reply = "",
                }
            );
            if (!keys.Add(row.Key))
                throw new ZLinkRelocationDataLostException(
                    "Relocation contains a duplicate captured request."
                );

            var encoded = row.Encode();
            if (
                encoded.Length > ZLinkRelocationReplyRow.MaximumBytes
                || Encoding.UTF8.GetByteCount(row.Key.Value)
                    > ZLinkLocationStoreLimits.MaximumKeyBytes
            )
                throw new ZLinkRelocationReplyBatchLimitException();
            conditions.Add(new ZLinkStoreCondition.Missing(row.Key));
            mutations.Add(
                new ZLinkStoreMutation.Put(
                    row.Key,
                    encoded,
                    ZLinkRelocationReplyRow.ActiveRetention
                )
            );
        }
    }

    private ValueTask<ZLinkStoreWriteResult> WriteRelocationCommitAsync(
        List<ZLinkStoreCondition> conditions,
        List<ZLinkStoreMutation> mutations,
        bool hasReplyRows,
        CancellationToken cancellationToken
    )
    {
        var batch = new ZLinkStoreWriteRequest(conditions, mutations);
        if (hasReplyRows)
        {
            var conditionKeys = conditions.SelectMany(ZLinkStoreBatchKeys.ForCondition).ToArray();
            var mutationKeys = mutations.Select(ZLinkStoreBatchKeys.ForMutation).ToArray();
            if (
                conditionKeys.Union(mutationKeys).Count()
                    > ZLinkLocationStoreLimits.MaximumUniqueKeys
                || ZLinkStoreBatchKeys.EncodedBytes(batch, conditionKeys, mutationKeys)
                    > ZLinkLocationStoreLimits.MaximumEncodedBatchBytes
            )
                throw new ZLinkRelocationReplyBatchLimitException();
        }
        return _provider.WriteAsync(batch, cancellationToken);
    }

    public async ValueTask<ZLinkRelocationReplyRow?> UpdateRelocationReplyAsync(
        ZLinkRelocationReplyRow identity,
        Func<ZLinkRelocationReplyRow, ZLinkRelocationReplyRow> update,
        CancellationToken cancellationToken
    )
    {
        var key = identity.Key;
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var read = await _provider.ReadAsync(key, cancellationToken).ConfigureAwait(false);
            if (read is not ZLinkStoreReadResult.Found found)
                return null;
            var row = ZLinkRelocationReplyRow.Decode(key, found.Value.Bytes);
            if (
                row.Writer != identity.Writer
                || row.Entry.ObjectKey != identity.Entry.ObjectKey
                || row.Entry.ReplyRouteId != identity.Entry.ReplyRouteId
            )
                throw new ZLinkRelocationDataLostException(
                    "Relocation reply row identity changed."
                );

            var writer = await ReadLiveOwnerAsync(row.Writer, cancellationToken)
                .ConfigureAwait(false);
            if (writer is null)
                return null;

            var next = update(row);
            if (
                next == row
                && (
                    !row.IsActive
                    || found.Value.ExpiresAt - found.Value.StoreNow
                        > ZLinkRelocationReplyRow.RenewThreshold
                )
            )
                return row;

            try
            {
                var result = await _provider
                    .WriteAsync(
                        new ZLinkStoreWriteRequest(
                            [
                                new ZLinkStoreCondition.Version(key, found.Value.Version),
                                new ZLinkStoreCondition.Value(
                                    OwnerKey(row.Writer.OwnerId),
                                    writer.Value
                                ),
                            ],
                            [
                                new ZLinkStoreMutation.Put(
                                    key,
                                    next.Encode(),
                                    next.IsActive
                                        ? ZLinkRelocationReplyRow.ActiveRetention
                                        : ZLinkRelocationReplyRow.TerminalRetention
                                ),
                            ]
                        ),
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (result is ZLinkStoreWriteResult.Applied)
                    return next;
                if (result is ZLinkStoreWriteResult.Corrupt)
                    throw new ZLinkRelocationDataLostException(
                        "Relocation reply row CAS found corrupt state."
                    );
            }
            catch (Exception error)
                when (ZLinkLocationStoreFailure.IsIndeterminate(error, cancellationToken))
            {
                // A lost CAS response is resolved from the same row version and state.
            }
        }
    }

    public ValueTask<IReadOnlyList<ZLinkRelocationReplyRow>> ReadRelocationRepliesAsync(
        Guid relocationId,
        CancellationToken cancellationToken
    ) => ReadRelocationRepliesAsync(relocationId, dueForRenewal: false, cancellationToken);

    public async ValueTask RenewAcceptedRelocationRepliesAsync(
        Guid relocationId,
        ZLinkLocationOwnerToken writer,
        CancellationToken cancellationToken
    )
    {
        var store = new ZLinkRelocationCompletionStore(this);
        foreach (
            var row in await ReadRelocationRepliesAsync(
                    relocationId,
                    dueForRenewal: true,
                    cancellationToken
                )
                .ConfigureAwait(false)
        )
            if (
                row.Writer == writer
                && row.Entry.Delivery == ZLinkRelocationCompletionDelivery.Accepted
            )
                await store.RenewAsync(row, cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask<IReadOnlyList<ZLinkRelocationReplyRow>> ReadRelocationRepliesAsync(
        Guid relocationId,
        bool dueForRenewal,
        CancellationToken cancellationToken
    )
    {
        var prefix = ZLinkRelocationReplyRow.Prefix(relocationId);
        var rows = new List<ZLinkRelocationReplyRow>();
        ZLinkStoreScanCursor? cursor = null;
        do
        {
            var scan = await _provider
                .ScanAsync(
                    new ZLinkStoreScanRequest(
                        prefix,
                        cursor,
                        ZLinkLocationStoreLimits.MaximumPageItems
                    ),
                    cancellationToken
                )
                .ConfigureAwait(false);
            if (scan is ZLinkStoreScanResult.Expired)
            {
                rows.Clear();
                cursor = null;
                continue;
            }
            var page = ((ZLinkStoreScanResult.Page)scan).Value;
            foreach (var item in page.Items)
                if (
                    !dueForRenewal
                    || item.Value.ExpiresAt - page.StoreNow
                        <= ZLinkRelocationReplyRow.RenewThreshold
                )
                    rows.Add(ZLinkRelocationReplyRow.Decode(item.Key, item.Value.Bytes));
            cursor = page.NextCursor;
            if (cursor is null)
                return rows;
        } while (true);
    }
}

internal sealed class ZLinkRelocationReplyBatchLimitException()
    : InvalidOperationException(
        "Relocation accepted replies exceed the Location Store commit batch limits."
    )
{
    internal ZLinkFrameworkException ToFrameworkException() =>
        new(ZLinkFrameworkErrorKind.InternalFailure, Message, innerException: this);
}
