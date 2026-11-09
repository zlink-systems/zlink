using System.Diagnostics;
using System.Text;
using Etcdserverpb;
using Google.Protobuf;
using Mvccpb;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.Locations.Etcd;

public sealed partial class ZLinkEtcdLocationStore
{
    private readonly EtcdCursor cursors = new();

    private async Task<ZLinkStoreScanResult> Scan(
        ZLinkStoreScanRequest request,
        EtcdOperation operation
    )
    {
        ArgumentNullException.ThrowIfNull(request);
        var prefix = EtcdEncoding.ValidateKey(request.Prefix, true);
        if (request.Limit is < 1 or > 1000)
            throw new ArgumentException("Invalid Store page limit.");
        EtcdSnapshot? snapshot = null;
        if (request.Cursor is { } cursor)
        {
            if (
                string.IsNullOrEmpty(cursor.Value)
                || EtcdEncoding.Utf8.GetByteCount(cursor.Value) > 4096
            )
                throw new ArgumentException("A Store cursor exceeds its UTF-8 bound.");
            snapshot = cursors.Decode(cursor.Value, storeNamespace, prefix);
            if (snapshot is null)
                return new ZLinkStoreScanResult.Expired();
        }
        var started = Stopwatch.GetTimestamp();
        var first = new TxnRequest();
        first.Success.Add(Read(clock));
        var observed = await client.Txn(first, operation).ConfigureAwait(false);
        var now = StoreNow(observed);
        if (snapshot is not null && snapshot.Cluster != observed.Header.ClusterId)
            return new ZLinkStoreScanResult.Expired();
        snapshot ??= new(
            observed.Header.ClusterId,
            observed.Header.Revision,
            now,
            started,
            ByteString.Empty
        );
        var metadataPrefix = MetadataKey(prefix);
        var rangeEnd = EtcdEncoding.PrefixEnd(metadataPrefix);
        var last = snapshot.Last;
        var selected = new List<(ByteString Key, KeyValue Metadata, EtcdMetadata Info)>();
        var size = 0;
        var hasMore = false;
        try
        {
            while (true)
            {
                if (Stopwatch.GetElapsedTime(snapshot.Started) >= TimeSpan.FromMinutes(5))
                    return new ZLinkStoreScanResult.Expired();
                var range = await client
                    .Range(
                        new RangeRequest
                        {
                            Key = last.IsEmpty
                                ? metadataPrefix
                                : ByteString.CopyFrom(
                                    MetadataKey(last).Concat(new byte[] { 0 }).ToArray()
                                ),
                            RangeEnd = rangeEnd,
                            Revision = snapshot.Revision,
                            Limit = 1000,
                            SortOrder = RangeRequest.Types.SortOrder.Ascend,
                            SortTarget = RangeRequest.Types.SortTarget.Key,
                        },
                        operation
                    )
                    .ConfigureAwait(false);
                if (range.Header.ClusterId != snapshot.Cluster)
                    return new ZLinkStoreScanResult.Expired();
                var stopped = false;
                foreach (var metadata in range.Kvs)
                {
                    var key = ByteString.CopyFrom(
                        metadata.Key.Span[MetadataKey(ByteString.Empty).Length..]
                    );
                    // Stored keys use the same strict UTF-8 contract as incoming SPI keys.
                    try
                    {
                        EtcdEncoding.ValidateKey(EtcdEncoding.Utf8.GetString(key.Span));
                    }
                    catch (ArgumentException)
                    {
                        throw EtcdEncoding.Failure();
                    }
                    var info = EtcdEncoding.DecodeMetadata(metadata.Value);
                    if (info.IsExpired(snapshot.Now))
                    {
                        last = key;
                        continue;
                    }
                    var itemSize =
                        key.Length
                        + info.PayloadLength
                        + EtcdEncoding.Version(snapshot.Cluster, metadata.ModRevision).Length
                        + 128;
                    if (
                        selected.Count == request.Limit
                        || size + itemSize > EtcdEncoding.MaxPageBytes
                    )
                    {
                        stopped = true;
                        break;
                    }
                    size += itemSize;
                    selected.Add((key, metadata, info));
                    last = key;
                }
                if (stopped)
                {
                    hasMore = true;
                    break;
                }
                if (!range.More)
                    break;
            }
            var payloadRequest = new TxnRequest();
            payloadRequest.Success.Add(
                selected.Select(item => Read(PayloadKey(item.Key), snapshot.Revision))
            );
            var items = new List<KeyValuePair<ZLinkStoreKey, ZLinkStoreValue>>(selected.Count);
            if (selected.Count != 0)
            {
                var response = await client.Txn(payloadRequest, operation).ConfigureAwait(false);
                if (response.Header.ClusterId != snapshot.Cluster)
                    return new ZLinkStoreScanResult.Expired();
                for (var i = 0; i < selected.Count; i++)
                {
                    var item = selected[i];
                    var payload = One(response.Responses[i].ResponseRange);
                    Record(item.Metadata, payload);
                    items.Add(
                        new(
                            new(EtcdEncoding.Utf8.GetString(item.Key.Span)),
                            Value(
                                item.Metadata,
                                payload!,
                                item.Info,
                                snapshot.Cluster,
                                snapshot.Now
                            )
                        )
                    );
                }
            }
            if (Stopwatch.GetElapsedTime(snapshot.Started) >= TimeSpan.FromMinutes(5))
                return new ZLinkStoreScanResult.Expired();
            ZLinkStoreScanCursor? next = hasMore
                ? new(cursors.Encode(snapshot with { Last = last }, storeNamespace, prefix))
                : null;
            return new ZLinkStoreScanResult.Page(
                new(items, next, DateTimeOffset.FromUnixTimeMilliseconds(snapshot.Now))
            );
        }
        catch (EtcdCallException error) when (error.Kind == EtcdErrorKind.Compaction)
        {
            return new ZLinkStoreScanResult.Expired();
        }
        catch (DecoderFallbackException)
        {
            throw EtcdEncoding.Failure();
        }
    }
}
